/* postgres.exec — TRANSFORM that executes a user SQL statement once per
 * input row via libpq, then forwards the row unchanged. SSIS OLE DB
 * Command parity (PG flavour).
 *
 * Config:
 *   connection  string,         required
 *   sql         string,         required — text with $1, $2, ... placeholders
 *   parameters  list[string],   optional — input column names to bind to
 *                                          $1, $2, ..., in order. Defaults
 *                                          to the full input column list
 *                                          in schema order.
 *
 * Type coverage v0.1: int64 (`l`), float64 (`g`), utf8 (`u`), bool (`b`).
 * Other types return BETL_ERR_UNSUPPORTED — stringify via `map` first.
 *
 * Wire path: PQprepare once, then PQexecPrepared per row with text-format
 * params, all inside a single transaction (BEGIN / COMMIT or ROLLBACK).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/postgres_exec.h"
#include "runtime/transforms_internal.h"

typedef enum {
    PE_INT64   = 1,
    PE_FLOAT64 = 2,
    PE_UTF8    = 3,
    PE_BOOL    = 4,
    PE_INT8    = 5,
    PE_INT16   = 6,
    PE_INT32   = 7,
    PE_FLOAT32 = 8,
} PeColType;

typedef struct {
    PeColType type;
    int64_t   child_idx;
} PeParam;

typedef struct {
    BetlContext *ctx;

    char        *connection_name;
    char        *sql;
    char       **param_cols;
    size_t       n_param_cols;
    int          params_explicit;   /* true iff `parameters:` was supplied
                                     * (even as empty list) — when set, do
                                     * NOT auto-fill from input schema */

    PGconn      *conn;
    int          in_txn;
    int          prepared;
    int          schema_cached;
    int          saw_eof_cleanly;
    PeParam     *params;
    size_t       n_params;

    struct ArrowArrayStream input;
    int                     have_input;

    char         last_err[400];
} PeState;

static void peset_err(PeState *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->last_err, sizeof p->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(p->ctx, "%s", p->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { PeState *p; char ***out; size_t *n_out; int err; } ParamArrCtx;

static int param_visit(const char *value, size_t value_len, void *user) {
    ParamArrCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        peset_err(c->p, "postgres.exec: `parameters:` entries must be strings");
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

static int parse_param_list(PeState *p, const char *cfg,
                            char ***out, size_t *n_out) {
    *out = NULL; *n_out = 0;
    const char *pos = betl_tx_json_value_after(cfg, "parameters");
    if (!pos) return 0;
    if (*pos != '[') {
        peset_err(p, "postgres.exec: `parameters:` must be a list");
        return -1;
    }
    p->params_explicit = 1;
    ParamArrCtx c = { .p = p, .out = out, .n_out = n_out, .err = 0 };
    if (betl_tx_json_walk_array(pos, param_visit, &c) != 0 || c.err) return -1;
    return 0;
}

/* ============================================================== *
 *  Connection                                                       *
 * ============================================================== */

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

static int open_conn(PeState *p) {
    const char *conn_json = betl_get_connection(p->ctx, p->connection_name);
    if (!conn_json) {
        peset_err(p, "postgres.exec: connection '%s' not declared",
                  p->connection_name);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (extract_dsn(conn_json, &dsn) != 0 || !dsn) {
        peset_err(p, "postgres.exec: connection '%s' missing `dsn` field",
                  p->connection_name);
        free(dsn);
        return BETL_ERR_INVALID;
    }
    p->conn = PQconnectdb(dsn);
    free(dsn);
    if (PQstatus(p->conn) != CONNECTION_OK) {
        peset_err(p, "postgres.exec: connect failed: %s",
                  PQerrorMessage(p->conn));
        PQfinish(p->conn);
        p->conn = NULL;
        return BETL_ERR_AUTH;
    }
    return BETL_OK;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int pe_init(BetlContext *ctx, const char *cfg, void **state) {
    PeState *p = calloc(1, sizeof *p);
    if (!p) return BETL_ERR_INTERNAL;
    p->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "connection", &p->connection_name) != 0
        || !p->connection_name) {
        peset_err(p, "postgres.exec: missing required `connection`");
        free(p); return BETL_ERR_INVALID;
    }
    if (betl_tx_json_string_at(cfg, "sql", &p->sql) != 0 || !p->sql) {
        peset_err(p, "postgres.exec: missing required `sql`");
        free(p->connection_name); free(p);
        return BETL_ERR_INVALID;
    }
    if (parse_param_list(p, cfg, &p->param_cols, &p->n_param_cols) != 0) {
        free(p->connection_name); free(p->sql); free(p);
        return BETL_ERR_INVALID;
    }

    int rc = open_conn(p);
    if (rc != BETL_OK) {
        for (size_t i = 0; i < p->n_param_cols; ++i) free(p->param_cols[i]);
        free(p->param_cols);
        free(p->connection_name); free(p->sql); free(p);
        return rc;
    }

    *state = p;
    return BETL_OK;
}

static int pe_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    PeState *p = state;
    p->input      = *in;
    p->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void pe_destroy(void *state) {
    if (!state) return;
    PeState *p = state;
    if (p->have_input && p->input.release) p->input.release(&p->input);
    if (p->conn) {
        if (p->in_txn) {
            PGresult *r = PQexec(p->conn,
                                 p->saw_eof_cleanly ? "COMMIT" : "ROLLBACK");
            PQclear(r);
        }
        PQfinish(p->conn);
    }
    free(p->params);
    for (size_t i = 0; i < p->n_param_cols; ++i) free(p->param_cols[i]);
    free(p->param_cols);
    free(p->connection_name);
    free(p->sql);
    free(p);
}

/* ============================================================== *
 *  Schema + prepare                                                *
 * ============================================================== */

static PeColType fmt_to_petype(const char *fmt) {
    if (!fmt) return 0;
    if (strcmp(fmt, "l") == 0) return PE_INT64;
    if (strcmp(fmt, "i") == 0) return PE_INT32;
    if (strcmp(fmt, "s") == 0) return PE_INT16;
    if (strcmp(fmt, "c") == 0) return PE_INT8;
    if (strcmp(fmt, "g") == 0) return PE_FLOAT64;
    if (strcmp(fmt, "f") == 0) return PE_FLOAT32;
    if (strcmp(fmt, "u") == 0) return PE_UTF8;
    if (strcmp(fmt, "b") == 0) return PE_BOOL;
    return 0;
}

static int prepare_stmt(PeState *p, const struct ArrowSchema *sch) {
    if (p->prepared) return BETL_OK;

    size_t n = p->n_param_cols;
    char **names = p->param_cols;
    int names_owned = 0;
    if (n == 0 && !p->params_explicit) {
        n = (size_t)sch->n_children;
        names = calloc(n, sizeof *names);
        if (!names) { peset_err(p, "postgres.exec: out of memory"); return BETL_ERR_INTERNAL; }
        for (size_t i = 0; i < n; ++i) {
            names[i] = (char *)(sch->children[i]->name);
        }
        names_owned = 1;
    }

    p->params = calloc(n ? n : 1, sizeof *p->params);
    if (!p->params) {
        if (names_owned) free(names);
        peset_err(p, "postgres.exec: out of memory");
        return BETL_ERR_INTERNAL;
    }
    p->n_params = n;

    for (size_t i = 0; i < n; ++i) {
        int64_t child = -1;
        for (int64_t j = 0; j < sch->n_children; ++j) {
            if (strcmp(names[i], sch->children[j]->name) == 0) {
                child = j; break;
            }
        }
        if (child < 0) {
            peset_err(p, "postgres.exec: parameter column '%s' not in input schema",
                      names[i]);
            if (names_owned) free(names);
            return BETL_ERR_INVALID;
        }
        const char *fmt = sch->children[child]->format;
        PeColType t = fmt_to_petype(fmt);
        if (t == 0) {
            peset_err(p, "postgres.exec: parameter '%s' has unsupported Arrow "
                         "type '%s' (supported: l/i/s/c/g/f/u/b)",
                      names[i], fmt ? fmt : "(none)");
            if (names_owned) free(names);
            return BETL_ERR_UNSUPPORTED;
        }
        p->params[i].type      = t;
        p->params[i].child_idx = child;
    }
    if (names_owned) free(names);

    /* BEGIN. */
    PGresult *r = PQexec(p->conn, "BEGIN");
    ExecStatusType st = PQresultStatus(r);
    PQclear(r);
    if (st != PGRES_COMMAND_OK) {
        peset_err(p, "postgres.exec: BEGIN failed: %s",
                  PQerrorMessage(p->conn));
        return BETL_ERR_IO;
    }
    p->in_txn = 1;

    /* PREPARE the user statement. */
    r = PQprepare(p->conn, "betl_exec", p->sql, (int)n, NULL);
    st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK) {
        peset_err(p, "postgres.exec: PREPARE failed: %s",
                  PQresultErrorMessage(r));
        PQclear(r);
        return BETL_ERR_IO;
    }
    PQclear(r);
    p->prepared = 1;
    return BETL_OK;
}

/* ============================================================== *
 *  Per-row text encoder (subset)                                  *
 * ============================================================== */

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Returns a malloc'd text representation (caller frees), or NULL if the
 * cell is SQL NULL. Returns -1 in *err_out on encode failure. */
static char *encode_cell(PeState *p, const struct ArrowArray *col,
                         int64_t row, PeColType t, int *err_out) {
    *err_out = 0;
    if (validity_is_null(col, row)) return NULL;
    int64_t off = col->offset + row;
    switch (t) {
    case PE_INT8: case PE_INT16: case PE_INT32: case PE_INT64: {
        int64_t v;
        switch (t) {
        case PE_INT8:  v = ((const int8_t  *)col->buffers[1])[off]; break;
        case PE_INT16: v = ((const int16_t *)col->buffers[1])[off]; break;
        case PE_INT32: v = ((const int32_t *)col->buffers[1])[off]; break;
        default:       v = ((const int64_t *)col->buffers[1])[off]; break;
        }
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%" PRId64, v);
        if (n < 0 || (size_t)n >= sizeof buf) { *err_out = 1; return NULL; }
        char *s = malloc((size_t)n + 1);
        if (!s) { *err_out = 1; return NULL; }
        memcpy(s, buf, (size_t)n + 1);
        return s;
    }
    case PE_FLOAT32: case PE_FLOAT64: {
        double v = (t == PE_FLOAT32)
            ? (double)((const float  *)col->buffers[1])[off]
            :         ((const double *)col->buffers[1])[off];
        char buf[40];
        int n = snprintf(buf, sizeof buf, "%.17g", v);
        if (n < 0 || (size_t)n >= sizeof buf) { *err_out = 1; return NULL; }
        char *s = malloc((size_t)n + 1);
        if (!s) { *err_out = 1; return NULL; }
        memcpy(s, buf, (size_t)n + 1);
        return s;
    }
    case PE_BOOL: {
        const uint8_t *bits = col->buffers[1];
        int v = (bits[off / 8] >> (off % 8)) & 1;
        char *s = malloc(2);
        if (!s) { *err_out = 1; return NULL; }
        s[0] = v ? 't' : 'f';
        s[1] = '\0';
        return s;
    }
    case PE_UTF8: {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t len = (size_t)(end - start);
        char *s = malloc(len + 1);
        if (!s) { *err_out = 1; return NULL; }
        if (len) memcpy(s, data + start, len);
        s[len] = '\0';
        return s;
    }
    }
    *err_out = 1;
    (void)p;
    return NULL;
}

/* ============================================================== *
 *  Stream                                                          *
 * ============================================================== */

static int pe_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    PeState *p = st->private_data;
    if (!p || !p->have_input) return EINVAL;
    return p->input.get_schema(&p->input, out) == 0 ? 0 : EIO;
}

static int pe_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    PeState *p = st->private_data;
    memset(out, 0, sizeof *out);
    if (!p || !p->have_input) return EINVAL;

    if (!p->schema_cached) {
        struct ArrowSchema sch = {0};
        if (p->input.get_schema(&p->input, &sch) != 0) {
            peset_err(p, "postgres.exec: upstream get_schema failed");
            return EIO;
        }
        int rc = prepare_stmt(p, &sch);
        if (sch.release) sch.release(&sch);
        if (rc != BETL_OK) return EIO;
        p->schema_cached = 1;
    }

    struct ArrowArray batch = {0};
    if (p->input.get_next(&p->input, &batch) != 0) {
        const char *up = p->input.get_last_error
                            ? p->input.get_last_error(&p->input) : NULL;
        peset_err(p, "postgres.exec: upstream get_next failed: %s",
                  up ? up : "(no detail)");
        return EIO;
    }
    if (!batch.release) {
        p->saw_eof_cleanly = 1;
        return 0;
    }

    /* Per-row encode + PQexecPrepared. */
    char **pvals = calloc(p->n_params ? p->n_params : 1, sizeof *pvals);
    if (!pvals) {
        batch.release(&batch);
        peset_err(p, "postgres.exec: out of memory");
        return EIO;
    }

    for (int64_t r = 0; r < batch.length; ++r) {
        if (betl_should_cancel(p->ctx)) {
            for (size_t i = 0; i < p->n_params; ++i) free(pvals[i]);
            free(pvals);
            batch.release(&batch);
            peset_err(p, "postgres.exec: cancelled");
            return EIO;
        }
        int enc_err = 0;
        for (size_t i = 0; i < p->n_params; ++i) {
            free(pvals[i]);
            pvals[i] = encode_cell(p, batch.children[p->params[i].child_idx], r,
                                   p->params[i].type, &enc_err);
            if (enc_err) {
                for (size_t k = 0; k < p->n_params; ++k) free(pvals[k]);
                free(pvals);
                batch.release(&batch);
                peset_err(p, "postgres.exec: encode failed at row %" PRId64
                             " param %zu", r, i);
                return EIO;
            }
        }
        PGresult *res = PQexecPrepared(p->conn, "betl_exec",
                                       (int)p->n_params,
                                       (const char *const *)pvals,
                                       NULL, NULL, 0);
        ExecStatusType st_res = PQresultStatus(res);
        if (st_res != PGRES_COMMAND_OK && st_res != PGRES_TUPLES_OK) {
            char errbuf[400];
            snprintf(errbuf, sizeof errbuf, "%s", PQresultErrorMessage(res));
            PQclear(res);
            for (size_t i = 0; i < p->n_params; ++i) free(pvals[i]);
            free(pvals);
            batch.release(&batch);
            peset_err(p, "postgres.exec: row %" PRId64 " execute failed: %s",
                      r, errbuf);
            return EIO;
        }
        PQclear(res);
    }
    for (size_t i = 0; i < p->n_params; ++i) free(pvals[i]);
    free(pvals);

    *out = batch;
    return 0;
}

static const char *pe_get_last_error(struct ArrowArrayStream *st) {
    PeState *p = st->private_data;
    return (p && p->last_err[0]) ? p->last_err : NULL;
}

static void pe_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int pe_attach_output(void *state, int port,
                            struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = pe_get_schema;
    out->get_next       = pe_get_next;
    out->get_last_error = pe_get_last_error;
    out->release        = pe_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef pe_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows" },
};
static const BetlPortDef pe_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "same rows, pass-through" },
};

static const BetlComponentDef pe_components[] = {
    { .name               = "postgres.exec",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_TRANSACTIONAL,
      .inputs             = pe_inputs,
      .input_count        = 1,
      .outputs            = pe_outputs,
      .output_count       = 1,
      .init               = pe_init,
      .destroy            = pe_destroy,
      .attach_input       = pe_attach_input,
      .attach_output      = pe_attach_output },
};

static const BetlProvider pe_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-postgres-exec",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = pe_components,
    .component_count = sizeof pe_components / sizeof pe_components[0],
};

int betl_register_postgres_exec(BetlRegistry *r) {
    return betl_registry_register(r, &pe_provider, "<builtin:postgres-exec>");
}
