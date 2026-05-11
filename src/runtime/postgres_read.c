/* postgres.read — SOURCE that runs a SELECT against Postgres via libpq
 * and emits the result set as Arrow record batches.
 *
 * Config:
 *   connection  string,  required  — name of a connection in BetlContext
 *   query       string,  required  — SELECT to run
 *   batch_size  int,     optional  — rows per emitted batch (default 1024)
 *
 * Wire model: open the connection, start a transaction, declare a
 * server-side cursor for the user's query, then FETCH `batch_size`
 * rows per get_next call. The cursor is necessary for true streaming
 * — PQexec would buffer the entire result set in client memory before
 * we ever saw a row.
 *
 * Type coverage v0.1: int64 ('l') and utf8 ('u'), discovered from the
 * libpq column OIDs (same set as postgres.lookup). NULL cells are
 * honored: a SQL NULL becomes a cleared bit in the Arrow leaf's
 * validity bitmap. */

#include "runtime/postgres_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "runtime/date_util.h"
#include "runtime/decimal_util.h"
#include "runtime/uuid_util.h"

#include "betl/provider.h"

/* ============================================================== *
 *  JSON helpers (same shape as the rest of the runtime)            *
 * ============================================================== */

static const char *pgr_json_value_after(const char *json, const char *key) {
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

static int pgr_json_decode_str(const char *p, char **out) {
    *out = NULL;
    if (!p || *p != '"') return -1;
    ++p;
    size_t cap = strlen(p) + 1;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p != '\\') { buf[i++] = *p++; continue; }
        ++p;
        if (!*p) { free(buf); return -1; }
        switch (*p) {
            case '"':  buf[i++] = '"';  ++p; break;
            case '\\': buf[i++] = '\\'; ++p; break;
            case '/':  buf[i++] = '/';  ++p; break;
            case 'n':  buf[i++] = '\n'; ++p; break;
            case 't':  buf[i++] = '\t'; ++p; break;
            case 'r':  buf[i++] = '\r'; ++p; break;
            case 'b':  buf[i++] = '\b'; ++p; break;
            case 'f':  buf[i++] = '\f'; ++p; break;
            default: free(buf); return -1;
        }
    }
    if (*p != '"') { free(buf); return -1; }
    buf[i] = '\0';
    *out = buf;
    return 0;
}

static int pgr_json_string(const char *json, const char *key, char **out) {
    return pgr_json_decode_str(pgr_json_value_after(json, key), out);
}

static int pgr_json_int64(const char *json, const char *key, int64_t *out) {
    const char *v = pgr_json_value_after(json, key);
    if (!v) return -1;
    char *end = NULL;
    long long ll = strtoll(v, &end, 10);
    if (end == v) return -1;
    *out = (int64_t)ll;
    return 0;
}

/* ============================================================== *
 *  Type mapping + Arrow leaf releases                              *
 * ============================================================== */

#define PG_OID_INT2        21
#define PG_OID_INT4        23
#define PG_OID_INT8        20
#define PG_OID_TEXT        25
#define PG_OID_VARCHAR     1043
#define PG_OID_BPCHAR      1042
#define PG_OID_DATE        1082
#define PG_OID_TIMESTAMP   1114
#define PG_OID_TIMESTAMPTZ 1184
#define PG_OID_NUMERIC     1700
#define PG_OID_UUID        2950
#define PG_OID_FLOAT4      700
#define PG_OID_FLOAT8      701

/* Internal column-format tags. Map to Arrow leaf formats:
 *   'l' → "l"      'u' → "u"      'D' → "tdD"
 *   'T' → "tsu:"   'Z' → "tsu:UTC"
 *   'N' → "d:p,s"  (per-column format string assembled separately)
 * `out_is_tztz` is no longer set — TIMESTAMPTZ is accepted now. */
static int pg_oid_to_fmt(Oid t, char *out, int *out_is_tztz) {
    *out_is_tztz = 0;
    if (t == PG_OID_INT8 || t == PG_OID_INT4 || t == PG_OID_INT2) {
        *out = 'l'; return 0;
    }
    if (t == PG_OID_FLOAT4 || t == PG_OID_FLOAT8) {
        *out = 'g'; return 0;
    }
    if (t == PG_OID_TEXT || t == PG_OID_VARCHAR || t == PG_OID_BPCHAR) {
        *out = 'u'; return 0;
    }
    if (t == PG_OID_DATE)        { *out = 'D'; return 0; }
    if (t == PG_OID_TIMESTAMP)   { *out = 'T'; return 0; }
    if (t == PG_OID_TIMESTAMPTZ) { *out = 'Z'; return 0; }
    if (t == PG_OID_NUMERIC)     { *out = 'N'; return 0; }
    if (t == PG_OID_UUID)        { *out = 'U'; return 0; }
    return -1;
}

/* Forward decl: lives further down with the rest of the leaf builders. */
static uint8_t *pgr_build_validity(const uint8_t *nulls, size_t n,
                                   int64_t *out_null_count);

static void pgr_release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgr_release_date32_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgr_release_decimal128_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgr_release_uuid_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgr_release_float64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static int pgr_build_float64_leaf(struct ArrowArray *out,
                                  const double *vals,
                                  const uint8_t *nulls,
                                  size_t n) {
    double *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0.0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = pgr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = pgr_release_float64_leaf;
    return 0;
}

static void pgr_release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgr_release_struct(struct ArrowArray *arr) {
    for (int64_t i = 0; i < arr->n_children; ++i) {
        if (arr->children[i] && arr->children[i]->release) {
            arr->children[i]->release(arr->children[i]);
        }
        free(arr->children[i]);
    }
    free(arr->children);
    free(arr->buffers);
    arr->release = NULL;
}

static void pgr_release_schema_named(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->release = NULL;
}

static void pgr_release_schema_named_owned_format(struct ArrowSchema *sch) {
    free((void *)sch->name);
    free((void *)sch->format);
    sch->release = NULL;
}

static void pgr_release_schema_struct(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i] && sch->children[i]->release) {
            sch->children[i]->release(sch->children[i]);
        }
        free(sch->children[i]);
    }
    free(sch->children);
    sch->release = NULL;
}

/* ============================================================== *
 *  State                                                           *
 * ============================================================== */

typedef struct {
    BetlContext *ctx;

    char  *connection;
    char  *query;
    size_t batch_size;

    PGconn *conn;
    int     in_txn;
    int     have_cursor;
    char    cursor_name[32];

    /* Schema (resolved on first get_schema or get_next). */
    int      schema_resolved;
    int      n_cols;
    char   **col_names;
    char    *col_fmts;
    int     *col_precisions;     /* decimal columns only */
    int     *col_scales;         /* decimal columns only */
    char   **col_fmt_strings;    /* heap-owned "d:p,s" for decimal cols */

    int      eof;
} PgReadState;

/* ============================================================== *
 *  libpq helpers                                                   *
 * ============================================================== */

static int pgr_exec_simple(PgReadState *s, const char *sql,
                           const char *step) {
    PGresult *r = PQexec(s->conn, sql);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        betl_set_error(s->ctx, "postgres.read: %s failed: %s",
                       step, PQerrorMessage(s->conn));
        PQclear(r);
        return -1;
    }
    PQclear(r);
    return 0;
}

static int pgr_open_conn(PgReadState *s) {
    const char *cjson = betl_get_connection(s->ctx, s->connection);
    if (!cjson) {
        betl_set_error(s->ctx, "postgres.read: connection '%s' not declared",
                       s->connection);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (pgr_json_string(cjson, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(s->ctx,
            "postgres.read: connection '%s' is missing a `dsn` field",
            s->connection);
        return BETL_ERR_INVALID;
    }
    s->conn = PQconnectdb(dsn);
    free(dsn);
    if (PQstatus(s->conn) != CONNECTION_OK) {
        betl_set_error(s->ctx, "postgres.read: connect failed: %s",
                       PQerrorMessage(s->conn));
        PQfinish(s->conn);
        s->conn = NULL;
        return BETL_ERR_AUTH;
    }
    return BETL_OK;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int pgr_init(BetlContext *ctx, const char *cfg, void **state) {
    PgReadState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx        = ctx;
    s->batch_size = 1024;
    cfg = cfg ? cfg : "{}";

    if (pgr_json_string(cfg, "connection", &s->connection) != 0
        || !s->connection)
    {
        betl_set_error(ctx, "postgres.read: missing required `connection`");
        goto fail;
    }
    if (pgr_json_string(cfg, "query", &s->query) != 0 || !s->query) {
        betl_set_error(ctx, "postgres.read: missing required `query`");
        goto fail;
    }
    int64_t bs = 0;
    if (pgr_json_int64(cfg, "batch_size", &bs) == 0 && bs > 0) {
        s->batch_size = (size_t)bs;
    }

    int rc = pgr_open_conn(s);
    if (rc != BETL_OK) goto fail;

    /* BEGIN, then DECLARE the cursor. The cursor name is per-state so
     * concurrent steps in the same pipeline don't collide on one
     * shared connection (we only use our own conn, but the unique name
     * is cheap insurance and shows up nicely in pg_stat_activity). */
    snprintf(s->cursor_name, sizeof s->cursor_name,
             "betl_read_%p", (void *)s);
    /* Replace any chars the server might dislike in an identifier; the
     * pointer-derived name is normally hex digits but %p is impl-defined. */
    for (char *p = s->cursor_name; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
              || (*p >= '0' && *p <= '9') || *p == '_')) *p = '_';
    }

    if (pgr_exec_simple(s, "BEGIN", "BEGIN") != 0) goto fail;
    s->in_txn = 1;

    /* Pin the session to UTC so TIMESTAMPTZ values come back as
     * "YYYY-MM-DD HH:MM:SS+00", not the server's local TZ. The decode
     * path then strips the offset deterministically. */
    if (pgr_exec_simple(s, "SET LOCAL TIME ZONE 'UTC'",
                        "SET LOCAL TIME ZONE 'UTC'") != 0) goto fail;

    /* DECLARE betl_read_xxx CURSOR FOR <user query>. We pass the query
     * verbatim — user is trusted (the SPEC framing has the pipeline
     * author writing the SELECT). */
    size_t qlen = strlen(s->query) + strlen(s->cursor_name) + 32;
    char *decl = malloc(qlen);
    if (!decl) goto fail;
    snprintf(decl, qlen, "DECLARE %s CURSOR FOR %s",
             s->cursor_name, s->query);
    int dc = pgr_exec_simple(s, decl, "DECLARE CURSOR");
    free(decl);
    if (dc != 0) goto fail;
    s->have_cursor = 1;

    *state = s;
    return BETL_OK;

fail:
    if (s->have_cursor) {
        char close_sql[64];
        snprintf(close_sql, sizeof close_sql, "CLOSE %s", s->cursor_name);
        (void)pgr_exec_simple(s, close_sql, "CLOSE");
    }
    if (s->in_txn) (void)pgr_exec_simple(s, "ROLLBACK", "ROLLBACK");
    if (s->conn) PQfinish(s->conn);
    free(s->connection);
    free(s->query);
    free(s);
    return BETL_ERR_INVALID;
}

static void pgr_destroy(void *state) {
    PgReadState *s = state;
    if (!s) return;
    if (s->conn) {
        if (s->have_cursor) {
            char close_sql[64];
            snprintf(close_sql, sizeof close_sql, "CLOSE %s", s->cursor_name);
            PGresult *r = PQexec(s->conn, close_sql);
            PQclear(r);
        }
        if (s->in_txn) {
            PGresult *r = PQexec(s->conn, "COMMIT");
            PQclear(r);
        }
        PQfinish(s->conn);
    }
    if (s->col_names) {
        for (int c = 0; c < s->n_cols; ++c) free(s->col_names[c]);
        free(s->col_names);
    }
    if (s->col_fmt_strings) {
        for (int c = 0; c < s->n_cols; ++c) free(s->col_fmt_strings[c]);
        free(s->col_fmt_strings);
    }
    free(s->col_fmts);
    free(s->col_precisions);
    free(s->col_scales);
    free(s->connection);
    free(s->query);
    free(s);
}

/* Resolve column metadata once. We FETCH 0 from the cursor — libpq
 * still returns a PGresult populated with field names + OIDs even
 * when no rows came back. */
static int pgr_resolve_schema(PgReadState *s) {
    if (s->schema_resolved) return 0;
    char fetch0[80];
    snprintf(fetch0, sizeof fetch0, "FETCH FORWARD 0 FROM %s",
             s->cursor_name);
    PGresult *r = PQexec(s->conn, fetch0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        betl_set_error(s->ctx, "postgres.read: schema FETCH 0 failed: %s",
                       PQerrorMessage(s->conn));
        PQclear(r);
        return -1;
    }
    int n = PQnfields(r);
    if (n <= 0) {
        betl_set_error(s->ctx, "postgres.read: query returned no columns");
        PQclear(r);
        return -1;
    }
    s->n_cols = n;
    s->col_names       = calloc((size_t)n, sizeof *s->col_names);
    s->col_fmts        = malloc((size_t)n);
    s->col_precisions  = calloc((size_t)n, sizeof *s->col_precisions);
    s->col_scales      = calloc((size_t)n, sizeof *s->col_scales);
    s->col_fmt_strings = calloc((size_t)n, sizeof *s->col_fmt_strings);
    if (!s->col_names || !s->col_fmts || !s->col_precisions
        || !s->col_scales || !s->col_fmt_strings) {
        betl_set_error(s->ctx, "postgres.read: out of memory");
        PQclear(r);
        return -1;
    }
    for (int c = 0; c < n; ++c) {
        Oid t = PQftype(r, c);
        char fmt;
        int is_tztz = 0;
        if (pg_oid_to_fmt(t, &fmt, &is_tztz) != 0) {
            betl_set_error(s->ctx,
                "postgres.read: column '%s' has unsupported OID %u "
                "(supported: int*, text/varchar/bpchar, date, timestamp, "
                "timestamptz, numeric)",
                PQfname(r, c), (unsigned)t);
            PQclear(r);
            return -1;
        }
        s->col_fmts[c]  = fmt;
        if (fmt == 'N') {
            /* NUMERIC: typmod-4 encodes (precision<<16) | scale. -1 means
             * "no constraint" — we need a defined precision/scale to
             * emit Arrow's `d:p,s` format string, so reject it with a
             * helpful hint. */
            int typmod = PQfmod(r, c);
            if (typmod < (int)sizeof(int32_t)) {
                betl_set_error(s->ctx,
                    "postgres.read: column '%s' is unconstrained NUMERIC "
                    "(no precision/scale). Cast it in your query, "
                    "e.g. `CAST(%s AS NUMERIC(12, 2))`",
                    PQfname(r, c), PQfname(r, c));
                PQclear(r);
                return -1;
            }
            int p = (typmod - 4) >> 16;
            int sc = (typmod - 4) & 0xFFFF;
            if (p < 1 || p > 38 || sc < 0 || sc > p) {
                betl_set_error(s->ctx,
                    "postgres.read: column '%s' NUMERIC(%d,%d) is outside the "
                    "supported [1,38] precision range", PQfname(r, c), p, sc);
                PQclear(r);
                return -1;
            }
            s->col_precisions[c] = p;
            s->col_scales[c]     = sc;
            char buf[24];
            snprintf(buf, sizeof buf, "d:%d,%d", p, sc);
            s->col_fmt_strings[c] = strdup(buf);
            if (!s->col_fmt_strings[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                PQclear(r);
                return -1;
            }
        }
        s->col_names[c] = strdup(PQfname(r, c));
        if (!s->col_names[c]) {
            betl_set_error(s->ctx, "postgres.read: out of memory");
            PQclear(r);
            return -1;
        }
    }
    PQclear(r);
    s->schema_resolved = 1;
    return 0;
}

/* ============================================================== *
 *  Arrow leaf builders                                             *
 * ============================================================== */

static uint8_t *pgr_build_validity(const uint8_t *nulls, size_t n,
                                   int64_t *out_null_count) {
    *out_null_count = 0;
    int64_t nc = 0;
    for (size_t i = 0; i < n; ++i) if (nulls[i]) ++nc;
    if (nc == 0) return NULL;
    size_t bytes = (n + 7) / 8;
    uint8_t *bm = malloc(bytes ? bytes : 1);
    if (!bm) return NULL;
    memset(bm, 0xFF, bytes ? bytes : 1);
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i]) bm[i / 8] &= (uint8_t)~(1u << (i % 8));
    }
    *out_null_count = nc;
    return bm;
}

static int pgr_build_int64_leaf(struct ArrowArray *out,
                                const int64_t *vals,
                                const uint8_t *nulls,
                                size_t n) {
    int64_t *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = pgr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = pgr_release_int64_leaf;
    return 0;
}

static int pgr_build_date32_leaf(struct ArrowArray *out,
                                 const int32_t *vals,
                                 const uint8_t *nulls,
                                 size_t n) {
    int32_t *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = pgr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = pgr_release_date32_leaf;
    return 0;
}

static int pgr_build_uuid_leaf(struct ArrowArray *out,
                               const uint8_t *vals,  /* n*16 bytes */
                               const uint8_t *nulls,
                               size_t n) {
    size_t bytes = (n ? n : 1) * 16;
    uint8_t *vbuf = malloc(bytes);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i]) memset(vbuf + i * 16, 0, 16);
        else          memcpy(vbuf + i * 16, vals + i * 16, 16);
    }
    int64_t null_count = 0;
    uint8_t *vmap = pgr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = pgr_release_uuid_leaf;
    return 0;
}

static int pgr_build_decimal128_leaf(struct ArrowArray *out,
                                     const betl_dec128 *vals,
                                     const uint8_t *nulls,
                                     size_t n) {
    betl_dec128 *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = pgr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = pgr_release_decimal128_leaf;
    return 0;
}

static int pgr_build_utf8_leaf(struct ArrowArray *out,
                               const char *const *strs, const size_t *lens,
                               const uint8_t *nulls, size_t n) {
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0;
    offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t le = nulls[i] ? 0 : lens[i];
        total += le;
        if (total > (size_t)INT32_MAX) { free(offs); return -1; }
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i]) continue;
        if (lens[i]) memcpy(data + pos, strs[i], lens[i]);
        pos += lens[i];
    }
    int64_t null_count = 0;
    uint8_t *vmap = pgr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(offs); free(data); return -1; }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = offs; bufs[2] = data;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 3;
    out->buffers    = bufs;
    out->release    = pgr_release_utf8_leaf;
    return 0;
}

/* ============================================================== *
 *  Stream — get_schema + get_next                                  *
 * ============================================================== */

static struct ArrowSchema *pgr_new_leaf(const char *name, const char *fmt) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    char *nm = strdup(name);
    if (!c || !nm) { free(c); free(nm); return NULL; }
    c->format  = fmt;
    c->name    = nm;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = pgr_release_schema_named;
    return c;
}

static int pgr_stream_get_schema(struct ArrowArrayStream *st,
                                 struct ArrowSchema *out) {
    PgReadState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (pgr_resolve_schema(s) != 0) return EIO;

    struct ArrowSchema **kids = calloc((size_t)s->n_cols, sizeof *kids);
    if (!kids) return ENOMEM;
    for (int i = 0; i < s->n_cols; ++i) {
        const char *fmt = "u";
        int owned_format = 0;
        switch (s->col_fmts[i]) {
            case 'l': fmt = "l";    break;
            case 'g': fmt = "g";    break;
            case 'D': fmt = "tdD";  break;
            case 'T': fmt = "tsu:"; break;
            case 'Z': fmt = "tsu:UTC"; break;
            case 'U': fmt = "w:16"; break;
            case 'N':
                /* Duplicate so the schema can outlive the source's state. */
                fmt = strdup(s->col_fmt_strings[i]);
                owned_format = 1;
                if (!fmt) {
                    for (int k = 0; k < i; ++k) {
                        if (kids[k]->release) kids[k]->release(kids[k]);
                        free(kids[k]);
                    }
                    free(kids);
                    return ENOMEM;
                }
                break;
            default:  fmt = "u";    break;
        }
        kids[i] = pgr_new_leaf(s->col_names[i], fmt);
        if (!kids[i]) {
            if (owned_format) free((void *)fmt);
            for (int k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            return ENOMEM;
        }
        if (owned_format) {
            kids[i]->release = pgr_release_schema_named_owned_format;
        }
        if (!kids[i]) {
            for (int k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            return ENOMEM;
        }
    }
    out->format     = "+s";
    out->n_children = (int64_t)s->n_cols;
    out->children   = kids;
    out->release    = pgr_release_schema_struct;
    return 0;
}

static int pgr_stream_get_next(struct ArrowArrayStream *st,
                               struct ArrowArray *out) {
    PgReadState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (pgr_resolve_schema(s) != 0) return EIO;
    if (s->eof) return 0;

    if (betl_should_cancel(s->ctx)) {
        betl_set_error(s->ctx, "postgres.read: cancelled by host");
        return EIO;
    }

    char fetch_sql[80];
    snprintf(fetch_sql, sizeof fetch_sql, "FETCH FORWARD %zu FROM %s",
             s->batch_size, s->cursor_name);
    PGresult *r = PQexec(s->conn, fetch_sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        betl_set_error(s->ctx, "postgres.read: FETCH failed: %s",
                       PQerrorMessage(s->conn));
        PQclear(r);
        return EIO;
    }
    int n_rows = PQntuples(r);
    if (n_rows == 0) {
        s->eof = 1;
        PQclear(r);
        return 0;
    }
    if (PQnfields(r) != s->n_cols) {
        betl_set_error(s->ctx,
            "postgres.read: cursor column count changed mid-stream "
            "(was %d, now %d)", s->n_cols, PQnfields(r));
        PQclear(r);
        return EIO;
    }

    /* Build per-column staging from the PGresult, then materialize
     * Arrow leaves. We allocate a small set of scratch arrays per
     * column and free them at the end. utf8 cells point into the
     * PGresult while we read; the Arrow leaf builder copies them into
     * its data buffer, so we can PQclear before returning. */
    int rc = EIO;
    /* i64 staging is shared between 'l' (int64) and 'T' (timestamp_us)
     * — both are int64 under the hood. */
    int64_t **i64_cols  = calloc((size_t)s->n_cols, sizeof *i64_cols);
    int32_t **d32_cols  = calloc((size_t)s->n_cols, sizeof *d32_cols);
    double  **f64_cols  = calloc((size_t)s->n_cols, sizeof *f64_cols);
    betl_dec128 **d128_cols = calloc((size_t)s->n_cols, sizeof *d128_cols);
    uint8_t **uuid_cols = calloc((size_t)s->n_cols, sizeof *uuid_cols);
    const char ***u8s_cols = calloc((size_t)s->n_cols, sizeof *u8s_cols);
    size_t  **u8l_cols = calloc((size_t)s->n_cols, sizeof *u8l_cols);
    uint8_t **null_cols = calloc((size_t)s->n_cols, sizeof *null_cols);
    if (!i64_cols || !d32_cols || !f64_cols || !d128_cols || !uuid_cols
        || !u8s_cols || !u8l_cols || !null_cols) {
        betl_set_error(s->ctx, "postgres.read: out of memory");
        goto cleanup;
    }
    for (int c = 0; c < s->n_cols; ++c) {
        null_cols[c] = calloc((size_t)n_rows, 1);
        if (!null_cols[c]) {
            betl_set_error(s->ctx, "postgres.read: out of memory");
            goto cleanup;
        }
        if (s->col_fmts[c] == 'l' || s->col_fmts[c] == 'T' || s->col_fmts[c] == 'Z') {
            i64_cols[c] = malloc((size_t)n_rows * sizeof(int64_t));
            if (!i64_cols[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                goto cleanup;
            }
        } else if (s->col_fmts[c] == 'D') {
            d32_cols[c] = malloc((size_t)n_rows * sizeof(int32_t));
            if (!d32_cols[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                goto cleanup;
            }
        } else if (s->col_fmts[c] == 'N') {
            d128_cols[c] = malloc((size_t)n_rows * sizeof(betl_dec128));
            if (!d128_cols[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                goto cleanup;
            }
        } else if (s->col_fmts[c] == 'U') {
            uuid_cols[c] = malloc((size_t)n_rows * 16);
            if (!uuid_cols[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                goto cleanup;
            }
        } else if (s->col_fmts[c] == 'g') {
            f64_cols[c] = malloc((size_t)n_rows * sizeof(double));
            if (!f64_cols[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                goto cleanup;
            }
        } else {
            u8s_cols[c] = malloc((size_t)n_rows * sizeof(char *));
            u8l_cols[c] = malloc((size_t)n_rows * sizeof(size_t));
            if (!u8s_cols[c] || !u8l_cols[c]) {
                betl_set_error(s->ctx, "postgres.read: out of memory");
                goto cleanup;
            }
        }
    }

    for (int row = 0; row < n_rows; ++row) {
        for (int c = 0; c < s->n_cols; ++c) {
            if (PQgetisnull(r, row, c)) {
                null_cols[c][row] = 1;
                continue;
            }
            null_cols[c][row] = 0;
            const char *v = PQgetvalue(r, row, c);
            if (s->col_fmts[c] == 'l') {
                char *end = NULL;
                long long iv = strtoll(v, &end, 10);
                if (end == v || *end != '\0') {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a "
                        "valid int", row, s->col_names[c], v);
                    goto cleanup;
                }
                i64_cols[c][row] = (int64_t)iv;
            } else if (s->col_fmts[c] == 'D') {
                int32_t days;
                if (betl_parse_iso_date(v, (size_t)PQgetlength(r, row, c),
                                        &days) != 0) {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a "
                        "valid YYYY-MM-DD date", row, s->col_names[c], v);
                    goto cleanup;
                }
                d32_cols[c][row] = days;
            } else if (s->col_fmts[c] == 'T') {
                int64_t us;
                if (betl_parse_iso_ts(v, (size_t)PQgetlength(r, row, c),
                                      &us) != 0) {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a "
                        "valid YYYY-MM-DD HH:MM:SS[.uuuuuu] timestamp",
                        row, s->col_names[c], v);
                    goto cleanup;
                }
                i64_cols[c][row] = us;
            } else if (s->col_fmts[c] == 'Z') {
                int64_t us;
                if (betl_parse_iso_tstz(v, (size_t)PQgetlength(r, row, c),
                                        &us) != 0) {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a "
                        "valid timestamptz", row, s->col_names[c], v);
                    goto cleanup;
                }
                i64_cols[c][row] = us;
            } else if (s->col_fmts[c] == 'N') {
                betl_dec128 dv;
                if (betl_dec128_parse(v, (size_t)PQgetlength(r, row, c),
                                      s->col_scales[c], &dv) != 0) {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a valid "
                        "NUMERIC(%d,%d)", row, s->col_names[c], v,
                        s->col_precisions[c], s->col_scales[c]);
                    goto cleanup;
                }
                d128_cols[c][row] = dv;
            } else if (s->col_fmts[c] == 'U') {
                if (betl_uuid_parse(v, (size_t)PQgetlength(r, row, c),
                                    &uuid_cols[c][row * 16]) != 0) {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a "
                        "valid UUID", row, s->col_names[c], v);
                    goto cleanup;
                }
            } else if (s->col_fmts[c] == 'g') {
                char *end = NULL;
                double dv = strtod(v, &end);
                if (end == v || *end != '\0') {
                    betl_set_error(s->ctx,
                        "postgres.read: row %d col '%s': '%s' is not a "
                        "valid float", row, s->col_names[c], v);
                    goto cleanup;
                }
                f64_cols[c][row] = dv;
            } else {
                u8s_cols[c][row] = v;       /* borrowed from PGresult */
                u8l_cols[c][row] = (size_t)PQgetlength(r, row, c);
            }
        }
    }

    /* Build Arrow leaves. utf8 builder copies bytes, so PGresult can
     * be freed afterwards. */
    struct ArrowArray **kids = calloc((size_t)s->n_cols, sizeof *kids);
    if (!kids) { betl_set_error(s->ctx, "postgres.read: out of memory");
                 goto cleanup; }
    int build_failed = 0;
    for (int c = 0; c < s->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) { build_failed = 1; break; }
        int brc;
        if (s->col_fmts[c] == 'l' || s->col_fmts[c] == 'T' || s->col_fmts[c] == 'Z') {
            brc = pgr_build_int64_leaf(kids[c], i64_cols[c], null_cols[c],
                                       (size_t)n_rows);
        } else if (s->col_fmts[c] == 'D') {
            brc = pgr_build_date32_leaf(kids[c], d32_cols[c], null_cols[c],
                                        (size_t)n_rows);
        } else if (s->col_fmts[c] == 'N') {
            brc = pgr_build_decimal128_leaf(kids[c], d128_cols[c],
                                            null_cols[c], (size_t)n_rows);
        } else if (s->col_fmts[c] == 'U') {
            brc = pgr_build_uuid_leaf(kids[c], uuid_cols[c],
                                      null_cols[c], (size_t)n_rows);
        } else if (s->col_fmts[c] == 'g') {
            brc = pgr_build_float64_leaf(kids[c], f64_cols[c],
                                         null_cols[c], (size_t)n_rows);
        } else {
            brc = pgr_build_utf8_leaf(kids[c], u8s_cols[c], u8l_cols[c],
                                      null_cols[c], (size_t)n_rows);
        }
        if (brc != 0) { build_failed = 1; break; }
    }
    if (build_failed) {
        for (int c = 0; c < s->n_cols; ++c) {
            if (kids[c]) {
                if (kids[c]->release) kids[c]->release(kids[c]);
                free(kids[c]);
            }
        }
        free(kids);
        betl_set_error(s->ctx, "postgres.read: failed to build output column");
        goto cleanup;
    }

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (int c = 0; c < s->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        betl_set_error(s->ctx, "postgres.read: out of memory");
        goto cleanup;
    }
    outer[0] = NULL;
    out->length     = (int64_t)n_rows;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = pgr_release_struct;
    rc = 0;

cleanup:
    if (i64_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(i64_cols[c]);
        free(i64_cols);
    }
    if (d32_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(d32_cols[c]);
        free(d32_cols);
    }
    if (d128_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(d128_cols[c]);
        free(d128_cols);
    }
    if (uuid_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(uuid_cols[c]);
        free(uuid_cols);
    }
    if (f64_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(f64_cols[c]);
        free(f64_cols);
    }
    if (u8s_cols) {
        for (int c = 0; c < s->n_cols; ++c) free((void *)u8s_cols[c]);
        free(u8s_cols);
    }
    if (u8l_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(u8l_cols[c]);
        free(u8l_cols);
    }
    if (null_cols) {
        for (int c = 0; c < s->n_cols; ++c) free(null_cols[c]);
        free(null_cols);
    }
    PQclear(r);
    return rc;
}

static const char *pgr_stream_get_last_error(struct ArrowArrayStream *st) {
    (void)st; return NULL;
}

static void pgr_stream_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int pgr_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = pgr_stream_get_schema;
    out->get_next       = pgr_stream_get_next;
    out->get_last_error = pgr_stream_get_last_error;
    out->release        = pgr_stream_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef pgr_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DERIVED, .doc = "query rows" },
};

static const BetlComponentDef pgr_components[] = {
    { .name               = "postgres.read",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = 0,
      .outputs            = pgr_outputs,
      .output_count       = 1,
      .init               = pgr_init,
      .destroy            = pgr_destroy,
      .attach_output      = pgr_attach_output },
};

static const BetlProvider pgr_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-postgres-read",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = pgr_components,
    .component_count = sizeof pgr_components / sizeof pgr_components[0],
};

int betl_register_postgres_read(BetlRegistry *r) {
    return betl_registry_register(r, &pgr_provider, "<builtin:postgres-read>");
}
