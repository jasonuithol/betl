/* postgres.copy — SINK that bulk-loads Arrow batches into Postgres via
 * libpq's binary COPY FROM STDIN protocol. ~10-40× faster than
 * postgres.upsert's row-by-row INSERTs; insert-only (no MERGE / ON
 * CONFLICT support). Use postgres.upsert when you need conflict
 * handling.
 *
 * Config:
 *   connection  string, required
 *   table       string, required — schema-qualified
 *   columns     list[string], optional — defaults to the input schema
 *                                        column order.
 *   truncate    bool, optional, default false — TRUNCATE the target
 *                                                before loading.
 *
 * Type coverage v0.1: int16 (`s`), int32 (`i`), int64 (`l`), float32
 * (`f`), float64 (`g`), utf8 (`u`), bool (`b`). All emitted as network-
 * order binary, matching libpq's BINARY COPY wire format. Other Arrow
 * types return BETL_ERR_UNSUPPORTED with a clear message — use
 * postgres.upsert for full type coverage.
 *
 * Wire path: BEGIN → optional TRUNCATE → COPY ... FROM STDIN (BINARY)
 * → PQputCopyData per row (one full row per call, buffered into the
 * libpq send queue) → PQputCopyEnd → COMMIT/ROLLBACK. */

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
#include "runtime/postgres_copy.h"
#include "runtime/transforms_internal.h"

typedef enum {
    PC_INT16   = 1,
    PC_INT32   = 2,
    PC_INT64   = 3,
    PC_FLOAT32 = 4,
    PC_FLOAT64 = 5,
    PC_UTF8    = 6,
    PC_BOOL    = 7,
} PcColType;

typedef struct {
    PcColType type;
    int64_t   child_idx;
} PcCol;

typedef struct {
    BetlContext *ctx;

    char        *connection_name;
    char        *table;
    char       **out_cols;
    size_t       n_out_cols;
    int          truncate;

    PGconn      *conn;

    struct ArrowArrayStream input;
    int                     have_input;

    char         last_err[400];
} PcState;

static void pcset_err(PcState *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->last_err, sizeof p->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(p->ctx, "%s", p->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { PcState *p; char ***out; size_t *n_out; int err; } StrArrCtx;

static int str_visit(const char *value, size_t value_len, void *user) {
    StrArrCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        pcset_err(c->p, "postgres.copy: `columns:` entries must be strings");
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

static int parse_cols(PcState *p, const char *cfg) {
    const char *pos = betl_tx_json_value_after(cfg, "columns");
    if (!pos) return 0;
    if (*pos != '[') {
        pcset_err(p, "postgres.copy: `columns:` must be a list");
        return -1;
    }
    StrArrCtx c = { .p = p, .out = &p->out_cols, .n_out = &p->n_out_cols,
                    .err = 0 };
    if (betl_tx_json_walk_array(pos, str_visit, &c) != 0 || c.err) return -1;
    return 0;
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

static int open_conn(PcState *p) {
    const char *conn_json = betl_get_connection(p->ctx, p->connection_name);
    if (!conn_json) {
        pcset_err(p, "postgres.copy: connection '%s' not declared",
                  p->connection_name);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (extract_dsn(conn_json, &dsn) != 0 || !dsn) {
        pcset_err(p, "postgres.copy: connection '%s' missing `dsn` field",
                  p->connection_name);
        free(dsn);
        return BETL_ERR_INVALID;
    }
    p->conn = PQconnectdb(dsn);
    free(dsn);
    if (PQstatus(p->conn) != CONNECTION_OK) {
        pcset_err(p, "postgres.copy: connect failed: %s",
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

static int pc_init(BetlContext *ctx, const char *cfg, void **state) {
    PcState *p = calloc(1, sizeof *p);
    if (!p) return BETL_ERR_INTERNAL;
    p->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "connection", &p->connection_name) != 0
        || !p->connection_name) {
        pcset_err(p, "postgres.copy: missing required `connection`");
        free(p); return BETL_ERR_INVALID;
    }
    if (betl_tx_json_string_at(cfg, "table", &p->table) != 0 || !p->table) {
        pcset_err(p, "postgres.copy: missing required `table`");
        free(p->connection_name); free(p);
        return BETL_ERR_INVALID;
    }
    if (parse_cols(p, cfg) != 0) {
        free(p->connection_name); free(p->table); free(p);
        return BETL_ERR_INVALID;
    }
    {
        char *trunc = NULL;
        if (betl_tx_json_value_to_string(cfg, "truncate", &trunc) == 0 && trunc) {
            p->truncate = (strcmp(trunc, "true") == 0);
            free(trunc);
        }
    }

    int rc = open_conn(p);
    if (rc != BETL_OK) {
        for (size_t i = 0; i < p->n_out_cols; ++i) free(p->out_cols[i]);
        free(p->out_cols);
        free(p->table); free(p->connection_name); free(p);
        return rc;
    }

    *state = p;
    return BETL_OK;
}

static int pc_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    PcState *p = state;
    p->input      = *in;
    p->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void pc_destroy(void *state) {
    if (!state) return;
    PcState *p = state;
    if (p->have_input && p->input.release) p->input.release(&p->input);
    if (p->conn) PQfinish(p->conn);
    for (size_t i = 0; i < p->n_out_cols; ++i) free(p->out_cols[i]);
    free(p->out_cols);
    free(p->table);
    free(p->connection_name);
    free(p);
}

/* ============================================================== *
 *  Type mapping + per-row binary encoder                          *
 * ============================================================== */

static PcColType fmt_to_pctype(const char *fmt) {
    if (!fmt) return 0;
    if (strcmp(fmt, "s") == 0) return PC_INT16;
    if (strcmp(fmt, "i") == 0) return PC_INT32;
    if (strcmp(fmt, "l") == 0) return PC_INT64;
    if (strcmp(fmt, "f") == 0) return PC_FLOAT32;
    if (strcmp(fmt, "g") == 0) return PC_FLOAT64;
    if (strcmp(fmt, "u") == 0) return PC_UTF8;
    if (strcmp(fmt, "b") == 0) return PC_BOOL;
    return 0;
}

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Network-order serialisers — pack into caller-supplied buffer at
 * offset *off, growing *cap as needed. Returns 0 on success, -1 OOM. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} PcBuf;

static int pc_reserve(PcBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *n = realloc(b->buf, nc);
    if (!n) return -1;
    b->buf = n;
    b->cap = nc;
    return 0;
}

static int pc_put16(PcBuf *b, uint16_t v) {
    if (pc_reserve(b, 2) != 0) return -1;
    b->buf[b->len + 0] = (uint8_t)(v >> 8);
    b->buf[b->len + 1] = (uint8_t)(v & 0xff);
    b->len += 2;
    return 0;
}

static int pc_put32(PcBuf *b, uint32_t v) {
    if (pc_reserve(b, 4) != 0) return -1;
    b->buf[b->len + 0] = (uint8_t)(v >> 24);
    b->buf[b->len + 1] = (uint8_t)(v >> 16);
    b->buf[b->len + 2] = (uint8_t)(v >>  8);
    b->buf[b->len + 3] = (uint8_t)(v >>  0);
    b->len += 4;
    return 0;
}

static int pc_put64(PcBuf *b, uint64_t v) {
    if (pc_reserve(b, 8) != 0) return -1;
    for (int i = 0; i < 8; ++i) {
        b->buf[b->len + i] = (uint8_t)(v >> (56 - i * 8));
    }
    b->len += 8;
    return 0;
}

static int pc_put_bytes(PcBuf *b, const uint8_t *src, size_t n) {
    if (pc_reserve(b, n) != 0) return -1;
    if (n) memcpy(b->buf + b->len, src, n);
    b->len += n;
    return 0;
}

/* ============================================================== *
 *  sink_run                                                        *
 * ============================================================== */

static int pg_exec_simple(PcState *p, const char *sql, const char *label) {
    PGresult *r = PQexec(p->conn, sql);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        pcset_err(p, "postgres.copy: %s failed: %s",
                  label, PQerrorMessage(p->conn));
        PQclear(r);
        return BETL_ERR_IO;
    }
    PQclear(r);
    return BETL_OK;
}

/* Quote an identifier ("table" or "column") for safe interpolation
 * into the COPY statement. Doubles any embedded `"`. */
static int append_quoted_ident(PcBuf *b, const char *ident) {
    if (pc_reserve(b, 1) != 0) return -1;
    b->buf[b->len++] = '"';
    for (const char *c = ident; *c; ++c) {
        if (*c == '"') {
            if (pc_reserve(b, 2) != 0) return -1;
            b->buf[b->len++] = '"';
            b->buf[b->len++] = '"';
        } else {
            if (pc_reserve(b, 1) != 0) return -1;
            b->buf[b->len++] = (uint8_t)*c;
        }
    }
    if (pc_reserve(b, 1) != 0) return -1;
    b->buf[b->len++] = '"';
    return 0;
}

/* Build the COPY statement string. The table identifier may be
 * "schema.table" — split on the first dot only and quote each side. */
static int build_copy_sql(PcState *p, char **out) {
    PcBuf b = {0};
    const char *kw = "COPY ";
    if (pc_put_bytes(&b, (const uint8_t *)kw, strlen(kw)) != 0) goto oom;

    const char *dot = strchr(p->table, '.');
    if (dot) {
        size_t slen = (size_t)(dot - p->table);
        char *schema = malloc(slen + 1);
        if (!schema) goto oom;
        memcpy(schema, p->table, slen);
        schema[slen] = '\0';
        int rc1 = append_quoted_ident(&b, schema);
        free(schema);
        if (rc1 != 0) goto oom;
        if (pc_reserve(&b, 1) != 0) goto oom;
        b.buf[b.len++] = '.';
        if (append_quoted_ident(&b, dot + 1) != 0) goto oom;
    } else {
        if (append_quoted_ident(&b, p->table) != 0) goto oom;
    }

    if (pc_put_bytes(&b, (const uint8_t *)" (", 2) != 0) goto oom;
    for (size_t i = 0; i < p->n_out_cols; ++i) {
        if (i > 0 && pc_put_bytes(&b, (const uint8_t *)",", 1) != 0) goto oom;
        if (append_quoted_ident(&b, p->out_cols[i]) != 0) goto oom;
    }
    const char *tail = ") FROM STDIN (FORMAT BINARY)";
    if (pc_put_bytes(&b, (const uint8_t *)tail, strlen(tail)) != 0) goto oom;
    if (pc_put_bytes(&b, (const uint8_t *)"", 1) != 0) goto oom;   /* NUL */
    *out = (char *)b.buf;
    return 0;
oom:
    free(b.buf);
    pcset_err(p, "postgres.copy: out of memory building COPY statement");
    return -1;
}

/* Pack one row into `tuple_buf` in binary COPY format. */
static int pc_encode_row(PcState *p, PcBuf *tuple,
                         const struct ArrowArray *batch,
                         int64_t row,
                         const PcCol *cols,
                         size_t n_cols) {
    tuple->len = 0;
    if (pc_put16(tuple, (uint16_t)n_cols) != 0) return -1;
    for (size_t c = 0; c < n_cols; ++c) {
        const struct ArrowArray *col = batch->children[cols[c].child_idx];
        if (validity_is_null(col, row)) {
            if (pc_put32(tuple, 0xFFFFFFFFu) != 0) return -1;
            continue;
        }
        int64_t off = col->offset + row;
        switch (cols[c].type) {
        case PC_INT16: {
            int16_t v = ((const int16_t *)col->buffers[1])[off];
            if (pc_put32(tuple, 2) != 0) return -1;
            if (pc_put16(tuple, (uint16_t)v) != 0) return -1;
            break;
        }
        case PC_INT32: {
            int32_t v = ((const int32_t *)col->buffers[1])[off];
            if (pc_put32(tuple, 4) != 0) return -1;
            if (pc_put32(tuple, (uint32_t)v) != 0) return -1;
            break;
        }
        case PC_INT64: {
            int64_t v = ((const int64_t *)col->buffers[1])[off];
            if (pc_put32(tuple, 8) != 0) return -1;
            if (pc_put64(tuple, (uint64_t)v) != 0) return -1;
            break;
        }
        case PC_FLOAT32: {
            uint32_t wire;
            float fv = ((const float *)col->buffers[1])[off];
            memcpy(&wire, &fv, 4);
            if (pc_put32(tuple, 4) != 0) return -1;
            if (pc_put32(tuple, wire) != 0) return -1;
            break;
        }
        case PC_FLOAT64: {
            uint64_t wire;
            double dv = ((const double *)col->buffers[1])[off];
            memcpy(&wire, &dv, 8);
            if (pc_put32(tuple, 8) != 0) return -1;
            if (pc_put64(tuple, wire) != 0) return -1;
            break;
        }
        case PC_BOOL: {
            const uint8_t *bits = col->buffers[1];
            uint8_t v = (uint8_t)((bits[off / 8] >> (off % 8)) & 1);
            if (pc_put32(tuple, 1) != 0) return -1;
            if (pc_put_bytes(tuple, &v, 1) != 0) return -1;
            break;
        }
        case PC_UTF8: {
            const int32_t *offs = col->buffers[1];
            const uint8_t *data = col->buffers[2];
            int32_t start = offs[off];
            int32_t end   = offs[off + 1];
            size_t len = (size_t)(end - start);
            if (pc_put32(tuple, (uint32_t)len) != 0) return -1;
            if (pc_put_bytes(tuple, data + start, len) != 0) return -1;
            break;
        }
        }
    }
    return 0;
}

static int pc_sink_run(void *state) {
    PcState *p = state;
    if (!p->have_input) {
        pcset_err(p, "postgres.copy: sink_run without attached input");
        return BETL_ERR_INVALID;
    }
    int rc = BETL_OK;

    /* --- Schema --- */
    struct ArrowSchema schema = {0};
    if (p->input.get_schema(&p->input, &schema) != 0) {
        pcset_err(p, "postgres.copy: get_schema failed");
        return BETL_ERR_IO;
    }
    if (!schema.format || strcmp(schema.format, "+s") != 0
        || schema.n_children <= 0) {
        pcset_err(p, "postgres.copy: input must be a non-empty struct stream");
        if (schema.release) schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    int64_t n_in = schema.n_children;

    /* Resolve columns. */
    int cols_owned = 0;
    if (p->n_out_cols == 0) {
        p->out_cols = calloc((size_t)n_in, sizeof *p->out_cols);
        if (!p->out_cols) { schema.release(&schema); return BETL_ERR_INTERNAL; }
        for (int64_t i = 0; i < n_in; ++i) {
            p->out_cols[i] = strdup(schema.children[i]->name);
            if (!p->out_cols[i]) {
                for (int64_t k = 0; k < i; ++k) free(p->out_cols[k]);
                free(p->out_cols); p->out_cols = NULL;
                schema.release(&schema);
                return BETL_ERR_INTERNAL;
            }
        }
        p->n_out_cols = (size_t)n_in;
        cols_owned = 1;
    }
    (void)cols_owned;

    PcCol *cols = calloc(p->n_out_cols, sizeof *cols);
    if (!cols) { schema.release(&schema); return BETL_ERR_INTERNAL; }
    for (size_t i = 0; i < p->n_out_cols; ++i) {
        int64_t child = -1;
        for (int64_t j = 0; j < n_in; ++j) {
            if (strcmp(p->out_cols[i], schema.children[j]->name) == 0) {
                child = j; break;
            }
        }
        if (child < 0) {
            pcset_err(p, "postgres.copy: column '%s' not in input schema",
                      p->out_cols[i]);
            free(cols); schema.release(&schema);
            return BETL_ERR_INVALID;
        }
        const char *fmt = schema.children[child]->format;
        PcColType t = fmt_to_pctype(fmt);
        if (t == 0) {
            pcset_err(p, "postgres.copy: column '%s' has unsupported Arrow "
                         "type '%s' (v0.1 supports s/i/l/f/g/u/b — for full "
                         "type coverage use postgres.upsert)",
                      p->out_cols[i], fmt ? fmt : "(none)");
            free(cols); schema.release(&schema);
            return BETL_ERR_UNSUPPORTED;
        }
        cols[i].type      = t;
        cols[i].child_idx = child;
    }

    /* --- BEGIN, optional TRUNCATE, COPY ... FROM STDIN BINARY. --- */
    rc = pg_exec_simple(p, "BEGIN", "BEGIN");
    if (rc != BETL_OK) goto out;

    if (p->truncate) {
        PcBuf tsql = {0};
        const char *prefix = "TRUNCATE TABLE ";
        if (pc_put_bytes(&tsql, (const uint8_t *)prefix, strlen(prefix)) != 0) {
            free(tsql.buf);
            pcset_err(p, "postgres.copy: out of memory");
            rc = BETL_ERR_INTERNAL;
            goto rollback;
        }
        const char *dot = strchr(p->table, '.');
        int qrc = 0;
        if (dot) {
            size_t slen = (size_t)(dot - p->table);
            char *sch = malloc(slen + 1);
            if (!sch) { qrc = -1; }
            else {
                memcpy(sch, p->table, slen);
                sch[slen] = '\0';
                qrc = append_quoted_ident(&tsql, sch);
                free(sch);
                if (qrc == 0) {
                    if (pc_reserve(&tsql, 1) == 0) tsql.buf[tsql.len++] = '.';
                    else qrc = -1;
                }
                if (qrc == 0) qrc = append_quoted_ident(&tsql, dot + 1);
            }
        } else {
            qrc = append_quoted_ident(&tsql, p->table);
        }
        if (qrc != 0 || pc_put_bytes(&tsql, (const uint8_t *)"", 1) != 0) {
            free(tsql.buf);
            pcset_err(p, "postgres.copy: out of memory");
            rc = BETL_ERR_INTERNAL;
            goto rollback;
        }
        rc = pg_exec_simple(p, (const char *)tsql.buf, "TRUNCATE");
        free(tsql.buf);
        if (rc != BETL_OK) goto rollback;
    }

    char *copy_sql = NULL;
    if (build_copy_sql(p, &copy_sql) != 0) { rc = BETL_ERR_INTERNAL; goto rollback; }
    PGresult *cr = PQexec(p->conn, copy_sql);
    free(copy_sql);
    ExecStatusType st = PQresultStatus(cr);
    if (st != PGRES_COPY_IN) {
        pcset_err(p, "postgres.copy: COPY did not enter COPY_IN: %s",
                  PQresultErrorMessage(cr));
        PQclear(cr);
        rc = BETL_ERR_IO;
        goto rollback;
    }
    PQclear(cr);

    /* Header: PGCOPY\n\377\r\n\0 (11) + flags(0) (4) + ext_len(0) (4). */
    static const uint8_t HDR[] = {
        'P','G','C','O','P','Y','\n', 0xFF, '\r','\n', 0,
        0,0,0,0,
        0,0,0,0,
    };
    if (PQputCopyData(p->conn, (const char *)HDR, sizeof HDR) != 1) {
        pcset_err(p, "postgres.copy: PQputCopyData(header) failed: %s",
                  PQerrorMessage(p->conn));
        rc = BETL_ERR_IO;
        goto end_copy;
    }

    PcBuf tuple = {0};
    int64_t n_rows = 0;
    for (;;) {
        if (betl_should_cancel(p->ctx)) {
            pcset_err(p, "postgres.copy: cancelled by host");
            rc = BETL_ERR_CANCELLED;
            free(tuple.buf);
            goto end_copy;
        }
        struct ArrowArray batch = {0};
        if (p->input.get_next(&p->input, &batch) != 0) {
            const char *up = p->input.get_last_error
                                ? p->input.get_last_error(&p->input) : NULL;
            pcset_err(p, "postgres.copy: upstream get_next failed: %s",
                      up ? up : "(no detail)");
            free(tuple.buf);
            rc = BETL_ERR_IO;
            goto end_copy;
        }
        if (!batch.release) break;
        for (int64_t r = 0; r < batch.length; ++r) {
            if (pc_encode_row(p, &tuple, &batch, r, cols, p->n_out_cols) != 0) {
                pcset_err(p, "postgres.copy: encode failed at row %" PRId64,
                          n_rows + r);
                batch.release(&batch);
                free(tuple.buf);
                rc = BETL_ERR_INTERNAL;
                goto end_copy;
            }
            if (PQputCopyData(p->conn, (const char *)tuple.buf,
                              (int)tuple.len) != 1) {
                pcset_err(p, "postgres.copy: PQputCopyData(row %" PRId64
                             ") failed: %s",
                          n_rows + r, PQerrorMessage(p->conn));
                batch.release(&batch);
                free(tuple.buf);
                rc = BETL_ERR_IO;
                goto end_copy;
            }
        }
        n_rows += batch.length;
        batch.release(&batch);
    }
    free(tuple.buf);

    /* Trailer: int16 = -1 (0xFFFF). */
    {
        uint8_t tr[2] = { 0xFF, 0xFF };
        if (PQputCopyData(p->conn, (const char *)tr, 2) != 1) {
            pcset_err(p, "postgres.copy: PQputCopyData(trailer) failed: %s",
                      PQerrorMessage(p->conn));
            rc = BETL_ERR_IO;
            goto end_copy;
        }
    }

end_copy:
    {
        const char *end_msg = (rc == BETL_OK) ? NULL : "betl error";
        if (PQputCopyEnd(p->conn, end_msg) != 1) {
            if (rc == BETL_OK) {
                pcset_err(p, "postgres.copy: PQputCopyEnd failed: %s",
                          PQerrorMessage(p->conn));
                rc = BETL_ERR_IO;
            }
        }
        /* Drain results from the COPY command itself. */
        PGresult *res;
        while ((res = PQgetResult(p->conn)) != NULL) {
            ExecStatusType s = PQresultStatus(res);
            if (rc == BETL_OK
                && s != PGRES_COMMAND_OK && s != PGRES_TUPLES_OK)
            {
                pcset_err(p, "postgres.copy: COPY result error: %s",
                          PQresultErrorMessage(res));
                rc = BETL_ERR_IO;
            }
            PQclear(res);
        }
    }

    if (rc == BETL_OK) {
        rc = pg_exec_simple(p, "COMMIT", "COMMIT");
        if (rc == BETL_OK) {
            betl_log(p->ctx, BETL_LOG_INFO,
                     "postgres.copy: wrote %" PRId64 " rows to %s",
                     n_rows, p->table);
        }
    }

rollback:
    if (rc != BETL_OK) {
        PGresult *r2 = PQexec(p->conn, "ROLLBACK");
        PQclear(r2);
    }
out:
    free(cols);
    if (schema.release) schema.release(&schema);
    return rc;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef pc_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows to bulk-load" },
};

static const BetlComponentDef pc_components[] = {
    { .name               = "postgres.copy",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_TRANSACTIONAL,
      .inputs             = pc_inputs,
      .input_count        = 1,
      .init               = pc_init,
      .destroy            = pc_destroy,
      .attach_input       = pc_attach_input,
      .sink_run           = pc_sink_run },
};

static const BetlProvider pc_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-postgres-copy",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = pc_components,
    .component_count = sizeof pc_components / sizeof pc_components[0],
};

int betl_register_postgres_copy(BetlRegistry *r) {
    return betl_registry_register(r, &pc_provider, "<builtin:postgres-copy>");
}
