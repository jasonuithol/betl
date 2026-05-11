/* postgres.upsert — SINK that writes Arrow record batches into Postgres
 * via libpq, using INSERT ... ON CONFLICT semantics.
 *
 * Config (per SPEC §6.4):
 *   connection      string,  required — name of a connection in BetlContext
 *   table           string,  required — schema-qualified target
 *   key             list[string], required — columns that uniquely id a row
 *   on_conflict     enum,    default "update"
 *                     update | update_if_changed | ignore | error
 *   columns         list[string], optional — explicit column list to write
 *                                            (defaults to the input schema)
 *   batch_size      int,     optional — currently a hint, not honoured
 *
 * Type coverage v0.1: int64, float64, utf8, bool. Other Arrow types
 * return BETL_ERR_UNSUPPORTED at run time with a clear message.
 *
 * Wire path: PQprepare once with `$1, $2, ...` placeholders, then
 * PQexecPrepared per row, all inside a single transaction. SQL NULL is
 * encoded as a NULL paramValues[i]. Text format for everything (no
 * binary marshalling for v0.1 — text is adequate for the supported set
 * and lets Postgres do the input parsing).
 */

#include "runtime/postgres_upsert.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "betl/provider.h"
#include "runtime/date_util.h"
#include "runtime/binary_util.h"
#include "runtime/decimal_util.h"
#include "runtime/pg_sql.h"
#include "runtime/uuid_util.h"

/* ============================================================== *
 *  JSON value extractor                                            *
 *                                                                  *
 *  builtins.c has a private copy of the same helpers; duplicating  *
 *  them here keeps this file standalone and avoids cross-file      *
 *  coupling on what is meant to be a v0.1 stopgap. Both copies go  *
 *  away when we adopt a real JSON parser.                          *
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

/* Parse a top-level JSON array of strings, e.g. `"key":["a","b"]`.
 * Returns 0 + sets *out / *n_out on success, -1 if absent or shape-bad.
 * Caller frees each element and the array. Tolerates whitespace inside
 * the array but does not handle escaped quotes inside strings. */
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

    /* Open libpq connection — opened in init, closed in destroy. */
    PGconn      *conn;

    /* Input stream. */
    int                       have_input;
    struct ArrowArrayStream   input;
} PgUpsertState;

/* ============================================================== *
 *  Connection setup                                                *
 * ============================================================== */

static int pg_open_conn(BetlContext *ctx, const char *name,
                        PGconn **out_conn) {
    const char *conn_json = betl_get_connection(ctx, name);
    if (!conn_json) {
        betl_set_error(ctx, "postgres.upsert: connection '%s' not declared",
                       name);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (json_string(conn_json, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(ctx,
            "postgres.upsert: connection '%s' is missing a `dsn` field",
            name);
        free(dsn);
        return BETL_ERR_INVALID;
    }
    PGconn *c = PQconnectdb(dsn);
    free(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        betl_set_error(ctx, "postgres.upsert: connect failed: %s",
                       PQerrorMessage(c));
        PQfinish(c);
        return BETL_ERR_AUTH;
    }
    *out_conn = c;
    return BETL_OK;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int pg_init(BetlContext *ctx, const char *cfg, void **state) {
    PgUpsertState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;

    if (json_string(cfg, "connection", &s->connection_name) != 0
        || !s->connection_name)
    {
        betl_set_error(ctx, "postgres.upsert: missing required `connection`");
        goto fail;
    }
    if (json_string(cfg, "table", &s->table) != 0 || !s->table) {
        betl_set_error(ctx, "postgres.upsert: missing required `table`");
        goto fail;
    }
    if (json_string_array(cfg, "key", &s->key_cols, &s->n_keys) != 0
        || s->n_keys == 0)
    {
        betl_set_error(ctx,
            "postgres.upsert: `key` must be a non-empty list of column names");
        goto fail;
    }
    /* `columns` is optional; tolerate absence. */
    if (json_string_array(cfg, "columns",
                          &s->explicit_cols, &s->n_explicit_cols) != 0)
    {
        s->explicit_cols    = NULL;
        s->n_explicit_cols  = 0;
    }

    char *on_conflict_str = NULL;
    json_string(cfg, "on_conflict", &on_conflict_str);
    if (betl_parse_on_conflict(on_conflict_str, &s->on_conflict) != 0) {
        betl_set_error(ctx,
            "postgres.upsert: invalid on_conflict '%s'", on_conflict_str);
        free(on_conflict_str);
        goto fail;
    }
    free(on_conflict_str);

    int conn_rc = pg_open_conn(ctx, s->connection_name, &s->conn);
    if (conn_rc != BETL_OK) goto fail;

    *state = s;
    return BETL_OK;

fail:
    free(s->connection_name);
    free(s->table);
    free_string_array(s->key_cols, s->n_keys);
    free_string_array(s->explicit_cols, s->n_explicit_cols);
    if (s->conn) PQfinish(s->conn);
    free(s);
    return BETL_ERR_INVALID;
}

static void pg_destroy(void *state) {
    PgUpsertState *s = state;
    if (!s) return;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    if (s->conn) PQfinish(s->conn);
    free(s->connection_name);
    free(s->table);
    free_string_array(s->key_cols, s->n_keys);
    free_string_array(s->explicit_cols, s->n_explicit_cols);
    free(s);
}

static int pg_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    PgUpsertState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  Arrow → text-format param marshalling                           *
 * ============================================================== */

typedef enum {
    PG_INT64,
    PG_FLOAT64,
    PG_UTF8,
    PG_BOOL,
    PG_DATE32,
    PG_TIMESTAMP_US,
    PG_DECIMAL128,
    PG_UUID,
    PG_TIME_US,
    PG_BINARY,
    PG_UNSUPPORTED
} PgColType;

static PgColType arrow_to_pg(const char *fmt) {
    if (!fmt) return PG_UNSUPPORTED;
    if (strcmp(fmt, "l")    == 0) return PG_INT64;
    if (strcmp(fmt, "g")    == 0) return PG_FLOAT64;
    if (strcmp(fmt, "u")    == 0) return PG_UTF8;
    if (strcmp(fmt, "b")    == 0) return PG_BOOL;
    if (strcmp(fmt, "tdD")     == 0) return PG_DATE32;
    if (strcmp(fmt, "tsu:")    == 0) return PG_TIMESTAMP_US;
    if (strcmp(fmt, "tsu:UTC") == 0) return PG_TIMESTAMP_US;  /* UTC-pinned */
    if (strcmp(fmt, "w:16")    == 0) return PG_UUID;
    if (strcmp(fmt, "ttu")     == 0) return PG_TIME_US;
    if (strcmp(fmt, "z")       == 0) return PG_BINARY;
    if (strncmp(fmt, "d:", 2)  == 0) return PG_DECIMAL128;
    return PG_UNSUPPORTED;
}

/* Extract scale from a "d:precision,scale" format string. -1 on malform. */
static int pg_decimal_scale(const char *fmt) {
    int p = 0, s = 0;
    if (sscanf(fmt + 2, "%d,%d", &p, &s) != 2) return -1;
    return s;
}

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    /* buffers[0] is the validity bitmap; NULL means "all valid". */
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Render row `row` of leaf `col` into `out`, as text. Returns 0 on
 * success, -1 on type-unsupported, -2 on OOM. Sets *out_is_null = 1
 * if the value is NULL, in which case `out` is not touched.
 * `dec_scale` is consulted only for PG_DECIMAL128 cells. */
static int render_cell(const struct ArrowArray *col, PgColType type,
                       int dec_scale,
                       int64_t row, char **out, int *out_is_null) {
    *out_is_null = 0;
    *out         = NULL;
    if (validity_is_null(col, row)) {
        *out_is_null = 1;
        return 0;
    }
    int64_t off = col->offset + row;
    switch (type) {
    case PG_INT64: {
        const int64_t *vals = col->buffers[1];
        char  buf[32];
        int n = snprintf(buf, sizeof buf, "%" PRId64, vals[off]);
        if (n < 0 || (size_t)n >= sizeof buf) return -2;
        char *s = malloc((size_t)n + 1);
        if (!s) return -2;
        memcpy(s, buf, (size_t)n + 1);
        *out = s;
        return 0;
    }
    case PG_FLOAT64: {
        const double *vals = col->buffers[1];
        char  buf[64];
        int n = snprintf(buf, sizeof buf, "%.17g", vals[off]);
        if (n < 0 || (size_t)n >= sizeof buf) return -2;
        char *s = malloc((size_t)n + 1);
        if (!s) return -2;
        memcpy(s, buf, (size_t)n + 1);
        *out = s;
        return 0;
    }
    case PG_BOOL: {
        const uint8_t *bits = col->buffers[1];
        int v = (bits[off / 8] >> (off % 8)) & 1;
        char *s = malloc(2);
        if (!s) return -2;
        s[0] = v ? 't' : 'f';
        s[1] = '\0';
        *out = s;
        return 0;
    }
    case PG_UTF8: {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t  len   = (size_t)(end - start);
        char *s = malloc(len + 1);
        if (!s) return -2;
        memcpy(s, data + start, len);
        s[len] = '\0';
        *out = s;
        return 0;
    }
    case PG_DATE32: {
        const int32_t *vals = col->buffers[1];
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(vals[off], &y, &m, &d);
        char buf[16];
        int n = snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
        if (n < 0 || (size_t)n >= sizeof buf) return -2;
        char *s = malloc((size_t)n + 1);
        if (!s) return -2;
        memcpy(s, buf, (size_t)n + 1);
        *out = s;
        return 0;
    }
    case PG_DECIMAL128: {
        const betl_dec128 *vals = col->buffers[1];
        char buf[48];
        int n = betl_dec128_format(vals[off], dec_scale, buf, sizeof buf);
        if (n < 0) return -2;
        char *str = malloc((size_t)n + 1);
        if (!str) return -2;
        memcpy(str, buf, (size_t)n + 1);
        *out = str;
        return 0;
    }
    case PG_UUID: {
        const uint8_t *vals = col->buffers[1];
        char buf[37];
        if (betl_uuid_format(&vals[off * 16], buf, sizeof buf - 1) < 0) return -2;
        buf[36] = '\0';
        char *str = malloc(37);
        if (!str) return -2;
        memcpy(str, buf, 37);
        *out = str;
        return 0;
    }
    case PG_TIME_US: {
        const int64_t *vals = col->buffers[1];
        char buf[20];
        int n = betl_format_iso_time(vals[off], buf, sizeof buf);
        if (n < 0) return -2;
        char *str = malloc((size_t)n + 1);
        if (!str) return -2;
        memcpy(str, buf, (size_t)n + 1);
        *out = str;
        return 0;
    }
    case PG_BINARY: {
        /* "\xHEX..." text form for BYTEA params. */
        const int32_t *offs = col->buffers[1];
        const uint8_t *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t  len   = (size_t)(end - start);
        char *str = malloc(2 + len * 2 + 1);
        if (!str) return -2;
        str[0] = '\\';
        str[1] = 'x';
        if (len > 0) betl_hex_encode(data + start, len, str + 2, len * 2);
        str[2 + len * 2] = '\0';
        *out = str;
        return 0;
    }
    case PG_TIMESTAMP_US: {
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        int hh   = (int)(us_of_day / 3600000000LL);
        int mm   = (int)((us_of_day / 60000000LL) % 60);
        int ss   = (int)((us_of_day / 1000000LL) % 60);
        int frac = (int)(us_of_day % 1000000LL);
        char buf[40];
        int n;
        if (frac == 0) {
            n = snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d:%02d",
                         y, m, d, hh, mm, ss);
        } else {
            n = snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d:%02d.%06d",
                         y, m, d, hh, mm, ss, frac);
        }
        if (n < 0 || (size_t)n >= sizeof buf) return -2;
        char *s = malloc((size_t)n + 1);
        if (!s) return -2;
        memcpy(s, buf, (size_t)n + 1);
        *out = s;
        return 0;
    }
    default:
        return -1;
    }
}

/* ============================================================== *
 *  sink_run                                                        *
 * ============================================================== */

static int pg_exec_simple(PGconn *conn, const char *sql, BetlContext *ctx,
                          const char *step_label) {
    PGresult *r = PQexec(conn, sql);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        betl_set_error(ctx, "postgres.upsert: %s failed: %s",
                       step_label, PQerrorMessage(conn));
        PQclear(r);
        return BETL_ERR_IO;
    }
    PQclear(r);
    return BETL_OK;
}

static int pg_sink_run(void *state) {
    PgUpsertState *s = state;
    if (!s->have_input) {
        betl_set_error(s->ctx, "postgres.upsert: sink_run without attached input");
        return BETL_ERR_INVALID;
    }

    /* --- Schema --- */
    struct ArrowSchema schema = {0};
    if (s->input.get_schema(&s->input, &schema) != 0) {
        betl_set_error(s->ctx, "postgres.upsert: get_schema failed");
        return BETL_ERR_IO;
    }
    if (!schema.format || strcmp(schema.format, "+s") != 0) {
        betl_set_error(s->ctx,
            "postgres.upsert: input must be a struct stream (got '%s')",
            schema.format ? schema.format : "(null)");
        if (schema.release) schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    int64_t n_cols_in = schema.n_children;
    if (n_cols_in <= 0) {
        betl_set_error(s->ctx,
            "postgres.upsert: input stream has no columns");
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
            out_cols[i] = (char *)nm; /* not freed: aliased into schema */
        }
        n_out_cols = (size_t)n_cols_in;
    }

    /* For each output column, find the matching schema child index and
     * resolve its Arrow type. */
    int64_t   *col_to_child = malloc(n_out_cols * sizeof *col_to_child);
    PgColType *col_types    = malloc(n_out_cols * sizeof *col_types);
    int       *col_dec_scales = calloc(n_out_cols, sizeof *col_dec_scales);
    int rc = BETL_OK;
    if (!col_to_child || !col_types || !col_dec_scales) {
        rc = BETL_ERR_INTERNAL; goto cleanup_pre;
    }

    for (size_t i = 0; i < n_out_cols; ++i) {
        col_to_child[i] = -1;
        for (int64_t j = 0; j < n_cols_in; ++j) {
            if (strcmp(out_cols[i], schema.children[j]->name) == 0) {
                col_to_child[i] = j;
                col_types[i]    = arrow_to_pg(schema.children[j]->format);
                if (col_types[i] == PG_DECIMAL128) {
                    int sc = pg_decimal_scale(schema.children[j]->format);
                    if (sc < 0) {
                        betl_set_error(s->ctx,
                            "postgres.upsert: column '%s' has malformed decimal format '%s'",
                            out_cols[i], schema.children[j]->format);
                        rc = BETL_ERR_TYPE;
                        goto cleanup_pre;
                    }
                    col_dec_scales[i] = sc;
                }
                break;
            }
        }
        if (col_to_child[i] < 0) {
            betl_set_error(s->ctx,
                "postgres.upsert: column '%s' not in input schema",
                out_cols[i]);
            rc = BETL_ERR_TYPE;
            goto cleanup_pre;
        }
        if (col_types[i] == PG_UNSUPPORTED) {
            betl_set_error(s->ctx,
                "postgres.upsert: unsupported Arrow type '%s' for column '%s'",
                schema.children[col_to_child[i]]->format, out_cols[i]);
            rc = BETL_ERR_UNSUPPORTED;
            goto cleanup_pre;
        }
    }

    /* --- Build SQL --- */
    BetlBuf sql_buf = {0};
    int br = betl_build_upsert_sql(&sql_buf, s->table,
                                   out_cols, n_out_cols,
                                   s->key_cols, s->n_keys,
                                   s->on_conflict);
    if (br != 0) {
        const char *why =
            br == -1 ? "embedded `\"` in identifier" :
            br == -3 ? "key column not in column list" : "out of memory";
        betl_set_error(s->ctx, "postgres.upsert: build_sql: %s", why);
        rc = (br == -2) ? BETL_ERR_INTERNAL : BETL_ERR_INVALID;
        free(sql_buf.data);
        goto cleanup_pre;
    }

    /* --- BEGIN, PREPARE, then per-row PQexecPrepared, then COMMIT --- */
    rc = pg_exec_simple(s->conn, "BEGIN", s->ctx, "BEGIN");
    if (rc == BETL_OK) {
        /* Pin the transaction's TZ to UTC so text-mode TIMESTAMP /
         * TIMESTAMPTZ parameters are interpreted deterministically. */
        rc = pg_exec_simple(s->conn, "SET LOCAL TIME ZONE 'UTC'",
                            s->ctx, "SET LOCAL TIME ZONE 'UTC'");
    }
    if (rc != BETL_OK) { free(sql_buf.data); goto cleanup_pre; }

    {
        PGresult *p = PQprepare(s->conn, "betl_upsert", sql_buf.data,
                                (int)n_out_cols, NULL);
        if (PQresultStatus(p) != PGRES_COMMAND_OK) {
            betl_set_error(s->ctx, "postgres.upsert: PREPARE failed: %s",
                           PQerrorMessage(s->conn));
            PQclear(p);
            free(sql_buf.data);
            (void)pg_exec_simple(s->conn, "ROLLBACK", s->ctx, "ROLLBACK");
            rc = BETL_ERR_IO;
            goto cleanup_pre;
        }
        PQclear(p);
    }
    free(sql_buf.data);

    int64_t n_rows_total = 0;
    char **param_values = malloc(n_out_cols * sizeof *param_values);
    if (!param_values) { rc = BETL_ERR_INTERNAL; goto cleanup_post; }

    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            betl_set_error(s->ctx, "postgres.upsert: cancelled by host");
            rc = BETL_ERR_CANCELLED;
            break;
        }
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            betl_set_error(s->ctx, "postgres.upsert: get_next failed: %s",
                           s->input.get_last_error
                               ? s->input.get_last_error(&s->input)
                               : "(no detail)");
            rc = BETL_ERR_IO;
            break;
        }
        if (!batch.release) break;     /* end of stream */

        for (int64_t r = 0; r < batch.length; ++r) {
            int row_rc = 0;
            for (size_t c = 0; c < n_out_cols; ++c) {
                int is_null = 0;
                row_rc = render_cell(batch.children[col_to_child[c]],
                                     col_types[c], col_dec_scales[c], r,
                                     &param_values[c], &is_null);
                if (row_rc != 0) {
                    betl_set_error(s->ctx,
                        "postgres.upsert: render col '%s' row %" PRId64 ": %s",
                        out_cols[c], r,
                        row_rc == -1 ? "type unsupported" : "out of memory");
                    rc = (row_rc == -1)
                            ? BETL_ERR_UNSUPPORTED : BETL_ERR_INTERNAL;
                    /* free already-rendered cells in this row */
                    for (size_t k = 0; k < c; ++k) free(param_values[k]);
                    goto break_batch;
                }
                if (is_null) param_values[c] = NULL;
            }

            PGresult *res = PQexecPrepared(s->conn, "betl_upsert",
                                           (int)n_out_cols,
                                           (const char *const *)param_values,
                                           NULL, NULL, 0);
            ExecStatusType st = PQresultStatus(res);
            for (size_t c = 0; c < n_out_cols; ++c) free(param_values[c]);

            if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
                betl_set_error(s->ctx,
                    "postgres.upsert: row %" PRId64 ": %s",
                    n_rows_total + r, PQerrorMessage(s->conn));
                PQclear(res);
                rc = BETL_ERR_IO;
                goto break_batch;
            }
            PQclear(res);
        }
        n_rows_total += batch.length;
        batch.release(&batch);
        continue;

    break_batch:
        if (batch.release) batch.release(&batch);
        break;
    }
    free(param_values);

cleanup_post:
    if (rc == BETL_OK) {
        rc = pg_exec_simple(s->conn, "COMMIT", s->ctx, "COMMIT");
    } else {
        (void)pg_exec_simple(s->conn, "ROLLBACK", s->ctx, "ROLLBACK");
    }
    if (rc == BETL_OK) {
        betl_log(s->ctx, BETL_LOG_INFO,
                 "postgres.upsert: wrote %" PRId64 " rows to %s",
                 n_rows_total, s->table);
        betl_set_error(s->ctx,
            "postgres.upsert: wrote %" PRId64 " rows", n_rows_total);
    }

cleanup_pre:
    free(col_to_child);
    free(col_types);
    free(col_dec_scales);
    if (s->n_explicit_cols == 0) free(out_cols);
    if (schema.release) schema.release(&schema);
    return rc;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef pg_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to upsert" },
};

static const BetlComponentDef pg_components[] = {
    { .name               = "postgres.upsert",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_TRANSACTIONAL,
      .inputs             = pg_inputs,
      .input_count        = 1,
      .init               = pg_init,
      .destroy            = pg_destroy,
      .attach_input       = pg_attach_input,
      .sink_run           = pg_sink_run },
};

static const BetlProvider pg_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-postgres",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = pg_components,
    .component_count = sizeof pg_components / sizeof pg_components[0],
};

int betl_register_postgres(BetlRegistry *r) {
    return betl_registry_register(r, &pg_provider, "<builtin:postgres>");
}
