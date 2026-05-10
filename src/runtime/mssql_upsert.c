/* mssql.upsert — SINK that writes Arrow record batches into SQL Server
 * via unixODBC, using a MERGE statement.
 *
 * Config (per SPEC §6.4, mirroring postgres.upsert):
 *   connection      string,  required — name of a connection in BetlContext
 *   table           string,  required — schema-qualified target
 *   key             list[string], required — columns that uniquely id a row
 *   on_conflict     enum,    default "update"
 *                     update | update_if_changed | ignore | error
 *   columns         list[string], optional — explicit column list to write
 *                                            (defaults to the input schema)
 *
 * Wire path: SQLPrepare once with `?` placeholders, then SQLExecute per
 * row, all inside a single transaction (autocommit OFF + SQL_COMMIT /
 * SQL_ROLLBACK at the end). NULL is encoded as SQL_NULL_DATA in each
 * column's indicator. For utf8, we copy the Arrow data into a per-
 * column scratch buffer that grows on demand — keeps the SQLBindParameter
 * call stable across rows. */

#include "runtime/mssql_upsert.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "betl/provider.h"
#include "runtime/mssql_sql.h"

/* ============================================================== *
 *  JSON value extractor                                            *
 *                                                                  *
 *  Same pattern as builtins.c / postgres_upsert.c — flat JSON      *
 *  objects only. Replaced when we adopt a real parser.             *
 * ============================================================== */

static const char *json_value_after(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof needle) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

static int json_string(const char *json, const char *key, char **out) {
    const char *v = json_value_after(json, key);
    if (!v || *v != '"') return -1;
    ++v;
    const char *end = strchr(v, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - v);
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, v, len);
    s[len] = '\0';
    *out = s;
    return 0;
}

static int json_string_array(const char *json, const char *key,
                             char ***out, size_t *n_out) {
    const char *p = json_value_after(json, key);
    if (!p || *p != '[') return -1;
    ++p;
    char **arr = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
            ++p;
        if (*p == ']') break;
        if (*p != '"') goto fail;
        ++p;
        const char *end = strchr(p, '"');
        if (!end) goto fail;
        size_t len = (size_t)(end - p);
        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            char **new_arr = realloc(arr, new_cap * sizeof *arr);
            if (!new_arr) goto fail;
            arr = new_arr;
            cap = new_cap;
        }
        char *s = malloc(len + 1);
        if (!s) goto fail;
        memcpy(s, p, len);
        s[len] = '\0';
        arr[n++] = s;
        p = end + 1;
    }
    *out = arr;
    *n_out = n;
    return 0;
fail:
    for (size_t i = 0; i < n; ++i) free(arr[i]);
    free(arr);
    return -1;
}

static void free_string_array(char **arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; ++i) free(arr[i]);
    free(arr);
}

/* ============================================================== *
 *  Arrow → ODBC type mapping                                       *
 * ============================================================== */

typedef enum {
    MS_INT64,
    MS_FLOAT64,
    MS_UTF8,
    MS_BOOL,
    MS_UNSUPPORTED
} MsColType;

static MsColType arrow_to_ms(const char *fmt) {
    if (!fmt) return MS_UNSUPPORTED;
    if (strcmp(fmt, "l") == 0) return MS_INT64;
    if (strcmp(fmt, "g") == 0) return MS_FLOAT64;
    if (strcmp(fmt, "u") == 0) return MS_UTF8;
    if (strcmp(fmt, "b") == 0) return MS_BOOL;
    return MS_UNSUPPORTED;
}

/* Per-column scratch the bound parameter points at. Bound once — the
 * addresses below stay stable for the lifetime of the prepared
 * statement, only the contents (and `ind`) change per row. */
typedef struct {
    MsColType type;
    SQLBIGINT i64_val;
    SQLDOUBLE f64_val;
    SQLCHAR   bool_val;        /* SQL_BIT — 0/1 */
    char     *str_buf;         /* utf8 scratch */
    size_t    str_cap;
    SQLLEN    ind;             /* SQL_NULL_DATA or column length */
} MsColBuf;

/* ============================================================== *
 *  State                                                           *
 * ============================================================== */

typedef struct {
    BetlContext *ctx;

    /* Configured */
    char        *connection_name;
    char        *table;
    char       **key_cols;        size_t n_keys;
    char       **explicit_cols;   size_t n_explicit_cols;  /* may be 0 */
    BetlOnConflict on_conflict;

    /* ODBC handles — opened in init, closed in destroy. */
    SQLHENV  henv;
    SQLHDBC  hdbc;

    /* Input stream. */
    int                       have_input;
    struct ArrowArrayStream   input;
} MsUpsertState;

/* ============================================================== *
 *  Diagnostics                                                     *
 * ============================================================== */

static void copy_diag(SQLSMALLINT type, SQLHANDLE h,
                      char *out, size_t out_cap) {
    out[0] = '\0';
    SQLCHAR state[6] = {0}, msg[400] = {0};
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRec(type, h, 1, state, &native,
                                    msg, sizeof msg, &msg_len))) {
        snprintf(out, out_cap, "SQLSTATE=%s native=%d %s",
                 state, (int)native, msg);
    }
}

/* ============================================================== *
 *  Connection setup                                                *
 * ============================================================== */

static int ms_open_conn(BetlContext *ctx, const char *name,
                        SQLHENV *out_env, SQLHDBC *out_dbc) {
    const char *conn_json = betl_get_connection(ctx, name);
    if (!conn_json) {
        betl_set_error(ctx, "mssql.upsert: connection '%s' not declared", name);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (json_string(conn_json, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(ctx,
            "mssql.upsert: connection '%s' is missing a `dsn` field", name);
        free(dsn);
        return BETL_ERR_INVALID;
    }

    SQLHENV henv = SQL_NULL_HENV;
    SQLHDBC hdbc = SQL_NULL_HDBC;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv))) {
        betl_set_error(ctx, "mssql.upsert: SQLAllocHandle(ENV) failed");
        free(dsn);
        return BETL_ERR_INTERNAL;
    }
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc))) {
        betl_set_error(ctx, "mssql.upsert: SQLAllocHandle(DBC) failed");
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
        free(dsn);
        return BETL_ERR_INTERNAL;
    }

    SQLCHAR out_dsn[1024];
    SQLSMALLINT out_dsn_len = 0;
    SQLRETURN rc = SQLDriverConnect(hdbc, NULL,
                                    (SQLCHAR *)dsn, SQL_NTS,
                                    out_dsn, sizeof out_dsn, &out_dsn_len,
                                    SQL_DRIVER_NOPROMPT);
    free(dsn);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_DBC, hdbc, msg, sizeof msg);
        betl_set_error(ctx, "mssql.upsert: connect failed: %s", msg);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
        return BETL_ERR_AUTH;
    }
    /* Manual transaction control. */
    SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT,
                      (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);

    *out_env = henv;
    *out_dbc = hdbc;
    return BETL_OK;
}

static void ms_close_conn(SQLHENV henv, SQLHDBC hdbc) {
    if (hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    }
    if (henv != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, henv);
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int ms_init(BetlContext *ctx, const char *cfg, void **state) {
    MsUpsertState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->henv = SQL_NULL_HENV;
    s->hdbc = SQL_NULL_HDBC;

    if (json_string(cfg, "connection", &s->connection_name) != 0
        || !s->connection_name)
    {
        betl_set_error(ctx, "mssql.upsert: missing required `connection`");
        goto fail;
    }
    if (json_string(cfg, "table", &s->table) != 0 || !s->table) {
        betl_set_error(ctx, "mssql.upsert: missing required `table`");
        goto fail;
    }
    if (json_string_array(cfg, "key", &s->key_cols, &s->n_keys) != 0
        || s->n_keys == 0)
    {
        betl_set_error(ctx,
            "mssql.upsert: `key` must be a non-empty list of column names");
        goto fail;
    }
    if (json_string_array(cfg, "columns",
                          &s->explicit_cols, &s->n_explicit_cols) != 0)
    {
        s->explicit_cols   = NULL;
        s->n_explicit_cols = 0;
    }

    char *on_conflict_str = NULL;
    json_string(cfg, "on_conflict", &on_conflict_str);
    if (betl_parse_on_conflict(on_conflict_str, &s->on_conflict) != 0) {
        betl_set_error(ctx,
            "mssql.upsert: invalid on_conflict '%s'", on_conflict_str);
        free(on_conflict_str);
        goto fail;
    }
    free(on_conflict_str);

    int conn_rc = ms_open_conn(ctx, s->connection_name, &s->henv, &s->hdbc);
    if (conn_rc != BETL_OK) goto fail;

    *state = s;
    return BETL_OK;

fail:
    free(s->connection_name);
    free(s->table);
    free_string_array(s->key_cols, s->n_keys);
    free_string_array(s->explicit_cols, s->n_explicit_cols);
    ms_close_conn(s->henv, s->hdbc);
    free(s);
    return BETL_ERR_INVALID;
}

static void ms_destroy(void *state) {
    MsUpsertState *s = state;
    if (!s) return;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    ms_close_conn(s->henv, s->hdbc);
    free(s->connection_name);
    free(s->table);
    free_string_array(s->key_cols, s->n_keys);
    free_string_array(s->explicit_cols, s->n_explicit_cols);
    free(s);
}

static int ms_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    MsUpsertState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  Validity helper                                                 *
 * ============================================================== */

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Ensure a column buffer's utf8 scratch is at least `n` bytes. Returns
 * 0 on success, -1 on OOM. */
static int ensure_str_cap(MsColBuf *b, size_t n) {
    if (b->str_cap >= n) return 0;
    size_t nc = b->str_cap ? b->str_cap : 64;
    while (nc < n) nc *= 2;
    char *p = realloc(b->str_buf, nc);
    if (!p) return -1;
    b->str_buf = p;
    b->str_cap = nc;
    return 0;
}

/* Per-row fill: read the Arrow cell, write into the scratch buf, set
 * `ind` for SQL_NULL_DATA / column length. Returns 0 on success,
 * BETL_ERR_* on failure. */
static int ms_fill_cell(BetlContext *ctx,
                        const struct ArrowArray *col,
                        int64_t row,
                        MsColBuf *b,
                        const char *col_name) {
    if (validity_is_null(col, row)) {
        b->ind = SQL_NULL_DATA;
        return BETL_OK;
    }
    int64_t off = col->offset + row;
    switch (b->type) {
    case MS_INT64: {
        const int64_t *vals = col->buffers[1];
        b->i64_val = (SQLBIGINT)vals[off];
        b->ind = 0;
        return BETL_OK;
    }
    case MS_FLOAT64: {
        const double *vals = col->buffers[1];
        b->f64_val = (SQLDOUBLE)vals[off];
        b->ind = 0;
        return BETL_OK;
    }
    case MS_BOOL: {
        const uint8_t *bits = col->buffers[1];
        int v = (bits[off / 8] >> (off % 8)) & 1;
        b->bool_val = (SQLCHAR)v;
        b->ind = 0;
        return BETL_OK;
    }
    case MS_UTF8: {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t  len   = (size_t)(end - start);
        if (ensure_str_cap(b, len ? len : 1) != 0) {
            betl_set_error(ctx, "mssql.upsert: OOM staging utf8 column '%s'",
                           col_name);
            return BETL_ERR_INTERNAL;
        }
        if (len) memcpy(b->str_buf, data + start, len);
        b->ind = (SQLLEN)len;
        return BETL_OK;
    }
    case MS_UNSUPPORTED:
    default:
        betl_set_error(ctx,
            "mssql.upsert: column '%s' has unsupported Arrow type", col_name);
        return BETL_ERR_UNSUPPORTED;
    }
}

/* Bind a parameter to its scratch buffer. Bound once after SQLPrepare;
 * the addresses stay live for as long as the statement does. */
static int ms_bind_param(BetlContext *ctx, SQLHSTMT hstmt,
                         SQLUSMALLINT slot, MsColBuf *b,
                         const char *col_name) {
    SQLRETURN rc;
    switch (b->type) {
    case MS_INT64:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                              &b->i64_val, 0, &b->ind);
        break;
    case MS_FLOAT64:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_DOUBLE, SQL_DOUBLE, 0, 0,
                              &b->f64_val, 0, &b->ind);
        break;
    case MS_BOOL:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_BIT, SQL_BIT, 0, 0,
                              &b->bool_val, 0, &b->ind);
        break;
    case MS_UTF8:
        /* Use a generous column-size hint. SQL Server NVARCHAR/VARCHAR
         * will accept up to its declared length, so 4000 covers typical
         * columns; the driver still reads the actual byte count from
         * `ind`. For columns wider than that the user's table needs
         * to be NVARCHAR(MAX) — the driver handles that transparently. */
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_VARCHAR, 4000, 0,
                              b->str_buf, (SQLLEN)b->str_cap, &b->ind);
        break;
    case MS_UNSUPPORTED:
    default:
        betl_set_error(ctx,
            "mssql.upsert: cannot bind unsupported type for column '%s'",
            col_name);
        return BETL_ERR_UNSUPPORTED;
    }
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(ctx,
            "mssql.upsert: SQLBindParameter for column '%s' failed: %s",
            col_name, msg);
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

/* ============================================================== *
 *  sink_run                                                        *
 * ============================================================== */

static int ms_sink_run(void *state) {
    MsUpsertState *s = state;
    if (!s->have_input) {
        betl_set_error(s->ctx, "mssql.upsert: sink_run without attached input");
        return BETL_ERR_INVALID;
    }

    /* --- Schema --- */
    struct ArrowSchema schema = {0};
    if (s->input.get_schema(&s->input, &schema) != 0) {
        betl_set_error(s->ctx, "mssql.upsert: get_schema failed");
        return BETL_ERR_IO;
    }
    if (!schema.format || strcmp(schema.format, "+s") != 0) {
        betl_set_error(s->ctx,
            "mssql.upsert: input must be a struct stream (got '%s')",
            schema.format ? schema.format : "(null)");
        if (schema.release) schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    int64_t n_cols_in = schema.n_children;
    if (n_cols_in <= 0) {
        betl_set_error(s->ctx, "mssql.upsert: input stream has no columns");
        schema.release(&schema);
        return BETL_ERR_TYPE;
    }

    /* Resolve column list (use explicit_cols if set, else schema names). */
    char **out_cols = NULL;
    size_t n_out_cols = 0;
    if (s->n_explicit_cols > 0) {
        out_cols   = s->explicit_cols;
        n_out_cols = s->n_explicit_cols;
    } else {
        out_cols = calloc((size_t)n_cols_in, sizeof *out_cols);
        if (!out_cols) {
            schema.release(&schema);
            return BETL_ERR_INTERNAL;
        }
        for (int64_t i = 0; i < n_cols_in; ++i) {
            const char *nm = schema.children[i]->name;
            out_cols[i] = (char *)nm;
        }
        n_out_cols = (size_t)n_cols_in;
    }

    int64_t  *col_to_child = malloc(n_out_cols * sizeof *col_to_child);
    MsColBuf *bufs         = calloc(n_out_cols, sizeof *bufs);
    int rc = BETL_OK;
    if (!col_to_child || !bufs) { rc = BETL_ERR_INTERNAL; goto cleanup_pre; }

    for (size_t i = 0; i < n_out_cols; ++i) {
        col_to_child[i] = -1;
        for (int64_t j = 0; j < n_cols_in; ++j) {
            if (strcmp(out_cols[i], schema.children[j]->name) == 0) {
                col_to_child[i] = j;
                bufs[i].type = arrow_to_ms(schema.children[j]->format);
                break;
            }
        }
        if (col_to_child[i] < 0) {
            betl_set_error(s->ctx,
                "mssql.upsert: column '%s' not present in input schema",
                out_cols[i]);
            rc = BETL_ERR_INVALID;
            goto cleanup_pre;
        }
        if (bufs[i].type == MS_UNSUPPORTED) {
            betl_set_error(s->ctx,
                "mssql.upsert: column '%s' has unsupported Arrow type", out_cols[i]);
            rc = BETL_ERR_UNSUPPORTED;
            goto cleanup_pre;
        }
        /* Pre-allocate utf8 scratch so SQLBindParameter has a stable
         * non-NULL pointer. ms_fill_cell will grow it on demand. */
        if (bufs[i].type == MS_UTF8 && ensure_str_cap(&bufs[i], 64) != 0) {
            rc = BETL_ERR_INTERNAL;
            goto cleanup_pre;
        }
    }

    /* --- Build the SQL --- */
    BetlBuf sql_buf = {0};
    int br = betl_build_mssql_merge_sql(&sql_buf, s->table,
                                        out_cols, n_out_cols,
                                        s->key_cols, s->n_keys,
                                        s->on_conflict);
    if (br != 0) {
        const char *why =
            br == -1 ? "embedded `]` in identifier" :
            br == -3 ? "key column not in column list" : "out of memory";
        betl_set_error(s->ctx, "mssql.upsert: build_sql: %s", why);
        rc = (br == -2) ? BETL_ERR_INTERNAL : BETL_ERR_INVALID;
        free(sql_buf.data);
        goto cleanup_pre;
    }

    /* --- PREPARE + bind once + per-row execute, all under a txn --- */
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s->hdbc, &hstmt))) {
        betl_set_error(s->ctx, "mssql.upsert: SQLAllocHandle(STMT) failed");
        free(sql_buf.data);
        rc = BETL_ERR_INTERNAL;
        goto cleanup_pre;
    }
    if (!SQL_SUCCEEDED(SQLPrepare(hstmt,
                                  (SQLCHAR *)sql_buf.data, SQL_NTS))) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(s->ctx, "mssql.upsert: PREPARE failed: %s", msg);
        free(sql_buf.data);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        rc = BETL_ERR_IO;
        goto cleanup_pre;
    }
    free(sql_buf.data);

    /* For MERGE the SELECT clause uses `?` once per column, and the
     * WHEN-MATCHED / WHEN-NOT-MATCHED clauses reuse the S.* aliases —
     * so the placeholder count equals n_out_cols regardless of mode.
     * (Plain INSERT in the OC_ERROR path also has exactly n_out_cols
     * placeholders.) */
    for (size_t i = 0; i < n_out_cols; ++i) {
        rc = ms_bind_param(s->ctx, hstmt, (SQLUSMALLINT)(i + 1),
                           &bufs[i], out_cols[i]);
        if (rc != BETL_OK) {
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            goto cleanup_pre;
        }
    }
    /* utf8 scratch is realloc-able; record the bound pointer so we can
     * detect a move in the row loop and rebind. */
    char **bound_str_ptr = calloc(n_out_cols, sizeof *bound_str_ptr);
    if (!bound_str_ptr) {
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        rc = BETL_ERR_INTERNAL;
        goto cleanup_pre;
    }
    for (size_t i = 0; i < n_out_cols; ++i) {
        if (bufs[i].type == MS_UTF8) bound_str_ptr[i] = bufs[i].str_buf;
    }

    int64_t n_rows_total = 0;
    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            betl_set_error(s->ctx, "mssql.upsert: cancelled by host");
            rc = BETL_ERR_CANCELLED;
            break;
        }
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            const char *up = s->input.get_last_error
                ? s->input.get_last_error(&s->input) : NULL;
            betl_set_error(s->ctx,
                "mssql.upsert: get_next failed: %s",
                up ? up : "(no detail)");
            rc = BETL_ERR_IO;
            break;
        }
        if (!batch.release) break;       /* end of stream */

        for (int64_t r = 0; r < batch.length; ++r) {
            for (size_t c = 0; c < n_out_cols; ++c) {
                int frc = ms_fill_cell(s->ctx,
                                       batch.children[col_to_child[c]],
                                       r, &bufs[c], out_cols[c]);
                if (frc != BETL_OK) {
                    rc = frc;
                    goto break_batch;
                }
                /* If realloc moved the utf8 buffer, the bound address
                 * is stale — rebind. Same if cap changed (driver may
                 * cache BufferLength). */
                if (bufs[c].type == MS_UTF8
                    && bufs[c].str_buf != bound_str_ptr[c])
                {
                    int brc = ms_bind_param(s->ctx, hstmt,
                                            (SQLUSMALLINT)(c + 1),
                                            &bufs[c], out_cols[c]);
                    if (brc != BETL_OK) { rc = brc; goto break_batch; }
                    bound_str_ptr[c] = bufs[c].str_buf;
                }
            }
            SQLRETURN er = SQLExecute(hstmt);
            if (!SQL_SUCCEEDED(er)) {
                char msg[512] = {0};
                copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
                betl_set_error(s->ctx,
                    "mssql.upsert: execute row %" PRId64 " failed: %s",
                    n_rows_total + r, msg);
                rc = BETL_ERR_IO;
                goto break_batch;
            }
            /* SQL Server's MERGE sets a row count we don't otherwise
             * consume; close the cursor between executes so the driver
             * doesn't carry it forward. */
            SQLFreeStmt(hstmt, SQL_CLOSE);
        }
        n_rows_total += batch.length;
        batch.release(&batch);
        continue;

    break_batch:
        if (batch.release) batch.release(&batch);
        break;
    }

    if (rc == BETL_OK) {
        if (!SQL_SUCCEEDED(SQLEndTran(SQL_HANDLE_DBC, s->hdbc, SQL_COMMIT))) {
            char msg[512] = {0};
            copy_diag(SQL_HANDLE_DBC, s->hdbc, msg, sizeof msg);
            betl_set_error(s->ctx, "mssql.upsert: COMMIT failed: %s", msg);
            rc = BETL_ERR_IO;
        }
    } else {
        SQLEndTran(SQL_HANDLE_DBC, s->hdbc, SQL_ROLLBACK);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    free(bound_str_ptr);

    if (rc == BETL_OK) {
        betl_log(s->ctx, BETL_LOG_INFO,
                 "mssql.upsert: wrote %" PRId64 " rows to %s",
                 n_rows_total, s->table);
    }

cleanup_pre:
    if (bufs) {
        for (size_t i = 0; i < n_out_cols; ++i) free(bufs[i].str_buf);
        free(bufs);
    }
    free(col_to_child);
    if (s->n_explicit_cols == 0) free(out_cols);
    if (schema.release) schema.release(&schema);
    return rc;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef ms_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to upsert" },
};

static const BetlComponentDef ms_components[] = {
    { .name               = "mssql.upsert",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_TRANSACTIONAL,
      .inputs             = ms_inputs,
      .input_count        = 1,
      .init               = ms_init,
      .destroy            = ms_destroy,
      .attach_input       = ms_attach_input,
      .sink_run           = ms_sink_run },
};

static const BetlProvider ms_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-mssql",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = ms_components,
    .component_count = sizeof ms_components / sizeof ms_components[0],
};

int betl_register_mssql(BetlRegistry *r) {
    return betl_registry_register(r, &ms_provider, "<builtin:mssql>");
}
