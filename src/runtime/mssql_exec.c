/* mssql.exec — TRANSFORM that executes a user SQL statement once per
 * input row, then forwards the row unchanged. SSIS OLE DB Command
 * parity.
 *
 * Config:
 *   connection  string,         required
 *   sql         string,         required — text with `?` placeholders
 *   parameters  list[string],   optional — input column names to bind to
 *                                          the placeholders, in order.
 *                                          Defaults to the full input
 *                                          column list in schema order.
 *
 * Type coverage v0.1: int64 (`l`), float64 (`g`), utf8 (`u`), bool (`b`).
 * Other types return BETL_ERR_UNSUPPORTED at run time — stringify via
 * `map` first if you need to bind dates / decimals / binaries.
 *
 * Wire path: SQLPrepare once, SQLBindParameter once, SQLExecute per row.
 * All executes run inside a single transaction; on error the txn rolls
 * back and the component returns BETL_ERR_IO. */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/mssql_exec.h"
#include "runtime/transforms_internal.h"

typedef enum {
    EX_INT64   = 1,
    EX_FLOAT64 = 2,
    EX_UTF8    = 3,
    EX_BOOL    = 4,
} ExColType;

typedef struct {
    ExColType type;
    int64_t   child_idx;
    SQLBIGINT i64;
    SQLDOUBLE f64;
    SQLCHAR   b8;       /* SQL_BIT */
    char     *str;
    size_t    str_cap;
    SQLLEN    ind;
} ExParam;

typedef struct {
    BetlContext *ctx;

    char        *connection_name;
    char        *sql;
    char       **param_cols;
    size_t       n_param_cols;     /* 0 → resolve from schema at run time */

    SQLHENV      henv;
    SQLHDBC      hdbc;
    SQLHSTMT     hstmt;
    int          prepared;
    int          schema_cached;
    int          saw_eof_cleanly;   /* true when upstream EOF reached without error */
    ExParam     *params;
    size_t       n_params;

    struct ArrowArrayStream input;
    int                     have_input;

    char         last_err[400];
} ExState;

static void exset_err(ExState *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->last_err, sizeof e->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(e->ctx, "%s", e->last_err);
}

/* ============================================================== *
 *  Config parsing — reuses the JSON helpers in transforms_common  *
 * ============================================================== */

typedef struct { ExState *e; char ***out; size_t *n_out; int err; } ParamArrCtx;

static int param_visit(const char *value, size_t value_len, void *user) {
    ParamArrCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        exset_err(c->e, "mssql.exec: `parameters:` entries must be strings");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *name = NULL;
    if (betl_tx_json_decode_str(vbuf, &name) != 0 || !name) {
        free(vbuf); c->err = 1; return -1;
    }
    free(vbuf);

    size_t n = *c->n_out;
    char **grow = realloc(*c->out, (n + 1) * sizeof *grow);
    if (!grow) { free(name); c->err = 1; return -1; }
    *c->out = grow;
    grow[n] = name;
    *c->n_out = n + 1;
    return 0;
}

static int parse_param_list(ExState *e, const char *cfg,
                            char ***out, size_t *n_out) {
    *out = NULL; *n_out = 0;
    const char *p = betl_tx_json_value_after(cfg, "parameters");
    if (!p) return 0;
    if (*p != '[') {
        exset_err(e, "mssql.exec: `parameters:` must be a list");
        return -1;
    }
    ParamArrCtx c = { .e = e, .out = out, .n_out = n_out, .err = 0 };
    if (betl_tx_json_walk_array(p, param_visit, &c) != 0 || c.err) return -1;
    return 0;
}

/* ============================================================== *
 *  Diagnostics & connection                                       *
 * ============================================================== */

static void copy_diag(SQLSMALLINT type, SQLHANDLE h, char *out, size_t cap) {
    out[0] = '\0';
    SQLCHAR state[6] = {0}, msg[400] = {0};
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRec(type, h, 1, state, &native,
                                    msg, sizeof msg, &msg_len))) {
        snprintf(out, cap, "SQLSTATE=%s native=%d %s",
                 state, (int)native, msg);
    }
}

static int extract_dsn(const char *conn_json, char **out_dsn) {
    *out_dsn = NULL;
    const char *p = strstr(conn_json, "\"dsn\":");
    if (!p) return -1;
    p += 6;
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (*p != '"') return -1;
    ++p;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, p, len);
    s[len] = '\0';
    *out_dsn = s;
    return 0;
}

static int open_conn(ExState *e) {
    const char *conn_json = betl_get_connection(e->ctx, e->connection_name);
    if (!conn_json) {
        exset_err(e, "mssql.exec: connection '%s' not declared",
                  e->connection_name);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (extract_dsn(conn_json, &dsn) != 0 || !dsn) {
        exset_err(e, "mssql.exec: connection '%s' missing `dsn` field",
                  e->connection_name);
        free(dsn);
        return BETL_ERR_INVALID;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &e->henv))) {
        exset_err(e, "mssql.exec: SQLAllocHandle(ENV) failed");
        free(dsn);
        return BETL_ERR_INTERNAL;
    }
    SQLSetEnvAttr(e->henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, e->henv, &e->hdbc))) {
        exset_err(e, "mssql.exec: SQLAllocHandle(DBC) failed");
        free(dsn);
        return BETL_ERR_INTERNAL;
    }
    SQLCHAR out[1024]; SQLSMALLINT ol = 0;
    SQLRETURN rc = SQLDriverConnect(e->hdbc, NULL,
                                    (SQLCHAR *)dsn, SQL_NTS,
                                    out, sizeof out, &ol,
                                    SQL_DRIVER_NOPROMPT);
    free(dsn);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[400] = {0};
        copy_diag(SQL_HANDLE_DBC, e->hdbc, msg, sizeof msg);
        exset_err(e, "mssql.exec: connect failed: %s", msg);
        return BETL_ERR_AUTH;
    }
    SQLSetConnectAttr(e->hdbc, SQL_ATTR_AUTOCOMMIT,
                      (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
    return BETL_OK;
}

static void close_conn(ExState *e) {
    if (e->hstmt) { SQLFreeHandle(SQL_HANDLE_STMT, e->hstmt); e->hstmt = 0; }
    if (e->hdbc) {
        SQLDisconnect(e->hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, e->hdbc);
        e->hdbc = 0;
    }
    if (e->henv) { SQLFreeHandle(SQL_HANDLE_ENV, e->henv); e->henv = 0; }
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int ex_init(BetlContext *ctx, const char *cfg, void **state) {
    ExState *e = calloc(1, sizeof *e);
    if (!e) return BETL_ERR_INTERNAL;
    e->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "connection", &e->connection_name) != 0
        || !e->connection_name) {
        exset_err(e, "mssql.exec: missing required `connection`");
        free(e); return BETL_ERR_INVALID;
    }
    if (betl_tx_json_string_at(cfg, "sql", &e->sql) != 0 || !e->sql) {
        exset_err(e, "mssql.exec: missing required `sql`");
        free(e->connection_name); free(e);
        return BETL_ERR_INVALID;
    }
    if (parse_param_list(e, cfg, &e->param_cols, &e->n_param_cols) != 0) {
        free(e->connection_name); free(e->sql); free(e);
        return BETL_ERR_INVALID;
    }

    int rc = open_conn(e);
    if (rc != BETL_OK) {
        close_conn(e);
        for (size_t i = 0; i < e->n_param_cols; ++i) free(e->param_cols[i]);
        free(e->param_cols);
        free(e->connection_name); free(e->sql); free(e);
        return rc;
    }

    *state = e;
    return BETL_OK;
}

static int ex_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    ExState *e = state;
    e->input      = *in;
    e->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void ex_destroy(void *state) {
    if (!state) return;
    ExState *e = state;
    if (e->have_input && e->input.release) e->input.release(&e->input);
    close_conn(e);
    if (e->params) {
        for (size_t i = 0; i < e->n_params; ++i) free(e->params[i].str);
        free(e->params);
    }
    for (size_t i = 0; i < e->n_param_cols; ++i) free(e->param_cols[i]);
    free(e->param_cols);
    free(e->connection_name);
    free(e->sql);
    free(e);
}

/* ============================================================== *
 *  Schema resolution + bind                                       *
 * ============================================================== */

static ExColType fmt_to_extype(const char *fmt) {
    if (!fmt) return 0;
    if (strcmp(fmt, "l") == 0) return EX_INT64;
    if (strcmp(fmt, "g") == 0) return EX_FLOAT64;
    if (strcmp(fmt, "u") == 0) return EX_UTF8;
    if (strcmp(fmt, "b") == 0) return EX_BOOL;
    return 0;
}

static int bind_one(ExState *e, SQLUSMALLINT slot, ExParam *p) {
    SQLRETURN rc = SQL_ERROR;
    switch (p->type) {
    case EX_INT64:
        rc = SQLBindParameter(e->hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                              &p->i64, 0, &p->ind);
        break;
    case EX_FLOAT64:
        rc = SQLBindParameter(e->hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_DOUBLE, SQL_DOUBLE, 0, 0,
                              &p->f64, 0, &p->ind);
        break;
    case EX_UTF8:
        rc = SQLBindParameter(e->hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_VARCHAR, 4000, 0,
                              p->str, (SQLLEN)p->str_cap, &p->ind);
        break;
    case EX_BOOL:
        rc = SQLBindParameter(e->hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_BIT, SQL_BIT, 0, 0,
                              &p->b8, 0, &p->ind);
        break;
    }
    if (!SQL_SUCCEEDED(rc)) {
        char msg[400] = {0};
        copy_diag(SQL_HANDLE_STMT, e->hstmt, msg, sizeof msg);
        exset_err(e, "mssql.exec: SQLBindParameter slot %u failed: %s",
                  (unsigned)slot, msg);
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

static int ensure_str_cap(ExParam *p, size_t need) {
    if (p->str_cap >= need) return 0;
    size_t nc = p->str_cap ? p->str_cap : 64;
    while (nc < need) nc *= 2;
    char *q = realloc(p->str, nc);
    if (!q) return -1;
    p->str = q;
    p->str_cap = nc;
    return 0;
}

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

static int fill_cell(ExState *e, const struct ArrowArray *col,
                     int64_t row, ExParam *p,
                     const char *col_name) {
    if (validity_is_null(col, row)) {
        p->ind = SQL_NULL_DATA;
        return BETL_OK;
    }
    int64_t off = col->offset + row;
    switch (p->type) {
    case EX_INT64:
        p->i64 = ((const int64_t *)col->buffers[1])[off];
        p->ind = 0;
        return BETL_OK;
    case EX_FLOAT64:
        p->f64 = ((const double *)col->buffers[1])[off];
        p->ind = 0;
        return BETL_OK;
    case EX_BOOL: {
        const uint8_t *bits = col->buffers[1];
        p->b8 = (SQLCHAR)((bits[off / 8] >> (off % 8)) & 1);
        p->ind = 0;
        return BETL_OK;
    }
    case EX_UTF8: {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t len = (size_t)(end - start);
        if (ensure_str_cap(p, len ? len : 1) != 0) {
            exset_err(e, "mssql.exec: OOM staging utf8 column '%s'", col_name);
            return BETL_ERR_INTERNAL;
        }
        if (len) memcpy(p->str, data + start, len);
        p->ind = (SQLLEN)len;
        return BETL_OK;
    }
    }
    return BETL_ERR_INTERNAL;
}

static int prepare_and_bind(ExState *e, const struct ArrowSchema *sch) {
    if (e->prepared) return BETL_OK;

    /* Resolve param column names (default = all input cols). */
    size_t n = e->n_param_cols;
    char **names = e->param_cols;
    int names_owned_by_us = 0;
    if (n == 0) {
        n = (size_t)sch->n_children;
        names = calloc(n, sizeof *names);
        if (!names) { exset_err(e, "mssql.exec: out of memory"); return BETL_ERR_INTERNAL; }
        for (size_t i = 0; i < n; ++i) {
            names[i] = (char *)(sch->children[i]->name);
        }
        names_owned_by_us = 1;
    }

    e->params = calloc(n, sizeof *e->params);
    if (!e->params) {
        if (names_owned_by_us) free(names);
        exset_err(e, "mssql.exec: out of memory");
        return BETL_ERR_INTERNAL;
    }
    e->n_params = n;

    for (size_t i = 0; i < n; ++i) {
        int64_t child = -1;
        for (int64_t j = 0; j < sch->n_children; ++j) {
            if (strcmp(names[i], sch->children[j]->name) == 0) {
                child = j; break;
            }
        }
        if (child < 0) {
            exset_err(e, "mssql.exec: parameter column '%s' not in input schema",
                      names[i]);
            if (names_owned_by_us) free(names);
            return BETL_ERR_INVALID;
        }
        const char *fmt = sch->children[child]->format;
        ExColType t = fmt_to_extype(fmt);
        if (t == 0) {
            exset_err(e, "mssql.exec: parameter '%s' has unsupported Arrow "
                         "type '%s' (v0.1 supports l/g/u/b)",
                      names[i], fmt ? fmt : "(none)");
            if (names_owned_by_us) free(names);
            return BETL_ERR_UNSUPPORTED;
        }
        e->params[i].type      = t;
        e->params[i].child_idx = child;
        if (t == EX_UTF8 && ensure_str_cap(&e->params[i], 64) != 0) {
            exset_err(e, "mssql.exec: out of memory");
            if (names_owned_by_us) free(names);
            return BETL_ERR_INTERNAL;
        }
    }
    if (names_owned_by_us) free(names);

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, e->hdbc, &e->hstmt))) {
        exset_err(e, "mssql.exec: SQLAllocHandle(STMT) failed");
        return BETL_ERR_INTERNAL;
    }
    if (!SQL_SUCCEEDED(SQLPrepare(e->hstmt, (SQLCHAR *)e->sql, SQL_NTS))) {
        char msg[400] = {0};
        copy_diag(SQL_HANDLE_STMT, e->hstmt, msg, sizeof msg);
        exset_err(e, "mssql.exec: PREPARE failed: %s", msg);
        return BETL_ERR_IO;
    }
    for (size_t i = 0; i < n; ++i) {
        int rc = bind_one(e, (SQLUSMALLINT)(i + 1), &e->params[i]);
        if (rc != BETL_OK) return rc;
    }
    e->prepared = 1;
    return BETL_OK;
}

/* ============================================================== *
 *  Stream — pass-through with per-row SQLExecute                  *
 * ============================================================== */

static int ex_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    ExState *e = st->private_data;
    if (!e || !e->have_input) return EINVAL;
    return e->input.get_schema(&e->input, out) == 0 ? 0 : EIO;
}

static int ex_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    ExState *e = st->private_data;
    memset(out, 0, sizeof *out);
    if (!e || !e->have_input) return EINVAL;

    if (!e->schema_cached) {
        struct ArrowSchema sch = {0};
        if (e->input.get_schema(&e->input, &sch) != 0) {
            exset_err(e, "mssql.exec: upstream get_schema failed");
            return EIO;
        }
        int rc = prepare_and_bind(e, &sch);
        if (sch.release) sch.release(&sch);
        if (rc != BETL_OK) return EIO;
        e->schema_cached = 1;
    }

    struct ArrowArray batch = {0};
    if (e->input.get_next(&e->input, &batch) != 0) {
        const char *up = e->input.get_last_error
                            ? e->input.get_last_error(&e->input) : NULL;
        exset_err(e, "mssql.exec: upstream get_next failed: %s",
                  up ? up : "(no detail)");
        return EIO;
    }
    if (!batch.release) {
        e->saw_eof_cleanly = 1;
        return 0;                  /* EOF */
    }

    /* Per-row fill + execute. */
    for (int64_t r = 0; r < batch.length; ++r) {
        if (betl_should_cancel(e->ctx)) {
            batch.release(&batch);
            exset_err(e, "mssql.exec: cancelled");
            return EIO;
        }
        for (size_t i = 0; i < e->n_params; ++i) {
            ExParam *p = &e->params[i];
            int frc = fill_cell(e, batch.children[p->child_idx], r, p,
                                "(param)");
            if (frc != BETL_OK) {
                batch.release(&batch);
                return EIO;
            }
            /* utf8 scratch may have grown — rebind so the driver sees
             * the new address. */
            if (p->type == EX_UTF8) {
                int brc = bind_one(e, (SQLUSMALLINT)(i + 1), p);
                if (brc != BETL_OK) {
                    batch.release(&batch);
                    return EIO;
                }
            }
        }
        SQLRETURN er = SQLExecute(e->hstmt);
        /* Tolerate SQL_NO_DATA (returned for some DDL/sproc shapes — see
         * the FreeTDS-on-DDL gotcha noted in the project memory). */
        if (er != SQL_NO_DATA && !SQL_SUCCEEDED(er)) {
            char msg[400] = {0};
            copy_diag(SQL_HANDLE_STMT, e->hstmt, msg, sizeof msg);
            exset_err(e, "mssql.exec: row %" PRId64 " execute failed: %s",
                      r, msg);
            batch.release(&batch);
            return EIO;
        }
        SQLFreeStmt(e->hstmt, SQL_CLOSE);
    }

    *out = batch;
    return 0;
}

static const char *ex_get_last_error(struct ArrowArrayStream *st) {
    ExState *e = st->private_data;
    return (e && e->last_err[0]) ? e->last_err : NULL;
}

static void ex_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int ex_attach_output(void *state, int port,
                            struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = ex_get_schema;
    out->get_next       = ex_get_next;
    out->get_last_error = ex_get_last_error;
    out->release        = ex_release;
    out->private_data   = state;
    return BETL_OK;
}

/* End-of-stream commit hook: betl streams don't (yet) expose a clean
 * "done" callback to transforms, so we COMMIT lazily inside the same
 * stream's release path. Tracked via state->prepared. The host pulls
 * once more past EOF, gets a release(=NULL) batch, but we still need
 * to flush the transaction before destroy() closes the handle. */

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef ex_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows" },
};
static const BetlPortDef ex_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "same rows, pass-through" },
};

/* Commit happens in destroy() — by the time the host calls destroy
 * the downstream sink has finished pulling. This also matches how
 * mssql.upsert handles its commit (synchronously inside sink_run). */
static void ex_destroy_commit(void *state) {
    if (!state) return;
    ExState *e = state;
    if (e->hdbc && e->prepared) {
        SQLEndTran(SQL_HANDLE_DBC, e->hdbc,
                   e->saw_eof_cleanly ? SQL_COMMIT : SQL_ROLLBACK);
    }
    ex_destroy(state);
}

static const BetlComponentDef ex_components[] = {
    { .name               = "mssql.exec",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_TRANSACTIONAL,
      .inputs             = ex_inputs,
      .input_count        = 1,
      .outputs            = ex_outputs,
      .output_count       = 1,
      .init               = ex_init,
      .destroy            = ex_destroy_commit,
      .attach_input       = ex_attach_input,
      .attach_output      = ex_attach_output },
};

static const BetlProvider ex_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-mssql-exec",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = ex_components,
    .component_count = sizeof ex_components / sizeof ex_components[0],
};

int betl_register_mssql_exec(BetlRegistry *r) {
    return betl_registry_register(r, &ex_provider, "<builtin:mssql-exec>");
}
