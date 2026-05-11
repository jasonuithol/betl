/* mssql.read — SOURCE that runs a SELECT against SQL Server via ODBC
 * and emits the result set as Arrow record batches.
 *
 * Config:
 *   connection  string,  required  — name of a connection in BetlContext
 *   query       string,  required  — SELECT to run
 *   batch_size  int,     optional  — rows per emitted batch (default 1024)
 *
 * Type coverage v0.1: int64 ('l') and utf8 ('u') — same set as
 * mssql.lookup. Other SQL types are rejected at first get_schema time
 * with a useful message.
 *
 * NULLs are honored: a SQL NULL becomes a cleared bit in the Arrow
 * leaf's validity bitmap. */

#include "runtime/mssql_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "runtime/date_util.h"
#include "runtime/decimal_util.h"
#include "runtime/uuid_util.h"

#include "betl/provider.h"

/* ============================================================== *
 *  JSON helpers (same shape as the rest of the runtime)            *
 * ============================================================== */

static const char *msr_json_value_after(const char *json, const char *key) {
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

static int msr_json_decode_str(const char *p, char **out) {
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

static int msr_json_string(const char *json, const char *key, char **out) {
    return msr_json_decode_str(msr_json_value_after(json, key), out);
}

static int msr_json_int64(const char *json, const char *key, int64_t *out) {
    const char *v = msr_json_value_after(json, key);
    if (!v) return -1;
    char *end = NULL;
    long long ll = strtoll(v, &end, 10);
    if (end == v) return -1;
    *out = (int64_t)ll;
    return 0;
}

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

/* ============================================================== *
 *  Type mapping + Arrow leaf releases                              *
 * ============================================================== */

static int sql_type_to_fmt(SQLSMALLINT t, char *out) {
    switch (t) {
    case SQL_BIGINT:
    case SQL_INTEGER:
    case SQL_SMALLINT:
    case SQL_TINYINT:
        *out = 'l'; return 0;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
        *out = 'u'; return 0;
    case SQL_TYPE_DATE:          /* MSSQL DATE */
    case SQL_DATE:               /* older ODBC alias */
        *out = 'D'; return 0;
    case SQL_TYPE_TIMESTAMP:     /* MSSQL DATETIME / DATETIME2 */
    case SQL_TIMESTAMP:          /* older ODBC alias */
        *out = 'T'; return 0;
    case -155:                   /* SQL_SS_TIMESTAMPOFFSET (MS extension) */
        *out = 'Z'; return 0;
    case SQL_GUID:               /* UNIQUEIDENTIFIER */
        *out = 'U'; return 0;
    case SQL_TYPE_TIME:          /* MSSQL TIME */
    case SQL_TIME:               /* older alias */
        *out = 'M'; return 0;
    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
        *out = 'g'; return 0;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
        *out = 'N'; return 0;
    default:
        return -1;
    }
}

static void msr_release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msr_release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msr_release_struct(struct ArrowArray *arr) {
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

static void msr_release_schema_named_owned_format(struct ArrowSchema *sch) {
    free((void *)sch->name);
    free((void *)sch->format);
    sch->release = NULL;
}

static void msr_release_schema_named(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->release = NULL;
}

static void msr_release_schema_struct(struct ArrowSchema *sch) {
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
    /* Per-batch staging — capacity = batch_size; current row count
     * tracked at the state level. i64_vals is used for both 'l' and
     * 'T' (timestamp_us); d32_vals for 'D'; d128_vals for 'N';
     * uuid_vals for 'U' (16 bytes per row, packed); f64_vals for 'g'. */
    int64_t      *i64_vals;
    int32_t      *d32_vals;
    double       *f64_vals;
    betl_dec128  *d128_vals;
    uint8_t      *uuid_vals;
    char        **u8_strs;
    size_t       *u8_lens;
    uint8_t      *nulls;       /* 1 = null, 0 = valid */
} MsReadCol;

typedef struct {
    BetlContext *ctx;

    char  *connection;
    char  *query;
    size_t batch_size;

    SQLHENV  henv;
    SQLHDBC  hdbc;
    SQLHSTMT hstmt;
    int      hstmt_executed;

    /* Schema (resolved on first get_schema). */
    int          schema_resolved;
    SQLSMALLINT  n_cols;
    char       **col_names;
    char        *col_fmts;        /* per-col 'l'/'u'/'D'/'T'/'N' */
    int         *col_precisions;  /* decimal columns only */
    int         *col_scales;      /* decimal columns only */
    char       **col_fmt_strings; /* heap-owned "d:p,s" for decimal cols */

    /* Per-batch staging. */
    MsReadCol *cols;
    size_t     cur_rows;          /* rows in the current batch */

    int eof;
} MsReadState;

/* ============================================================== *
 *  Connection setup                                                *
 * ============================================================== */

static int msr_open_conn(MsReadState *s) {
    const char *cjson = betl_get_connection(s->ctx, s->connection);
    if (!cjson) {
        betl_set_error(s->ctx, "mssql.read: connection '%s' not declared",
                       s->connection);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (msr_json_string(cjson, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(s->ctx,
            "mssql.read: connection '%s' is missing a `dsn` field",
            s->connection);
        return BETL_ERR_INVALID;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &s->henv))) {
        betl_set_error(s->ctx, "mssql.read: SQLAllocHandle(ENV) failed");
        free(dsn);
        return BETL_ERR_INTERNAL;
    }
    SQLSetEnvAttr(s->henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, s->henv, &s->hdbc))) {
        betl_set_error(s->ctx, "mssql.read: SQLAllocHandle(DBC) failed");
        free(dsn);
        return BETL_ERR_INTERNAL;
    }
    SQLCHAR out_dsn[1024];
    SQLSMALLINT out_dsn_len = 0;
    SQLRETURN rc = SQLDriverConnect(s->hdbc, NULL,
                                    (SQLCHAR *)dsn, SQL_NTS,
                                    out_dsn, sizeof out_dsn, &out_dsn_len,
                                    SQL_DRIVER_NOPROMPT);
    free(dsn);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_DBC, s->hdbc, msg, sizeof msg);
        betl_set_error(s->ctx, "mssql.read: connect failed: %s", msg);
        return BETL_ERR_AUTH;
    }
    return BETL_OK;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int msr_init(BetlContext *ctx, const char *cfg, void **state) {
    MsReadState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx        = ctx;
    s->batch_size = 1024;
    s->henv = SQL_NULL_HENV;
    s->hdbc = SQL_NULL_HDBC;
    s->hstmt = SQL_NULL_HSTMT;
    cfg = cfg ? cfg : "{}";

    if (msr_json_string(cfg, "connection", &s->connection) != 0
        || !s->connection)
    {
        betl_set_error(ctx, "mssql.read: missing required `connection`");
        goto fail;
    }
    if (msr_json_string(cfg, "query", &s->query) != 0 || !s->query) {
        betl_set_error(ctx, "mssql.read: missing required `query`");
        goto fail;
    }
    int64_t bs = 0;
    if (msr_json_int64(cfg, "batch_size", &bs) == 0 && bs > 0) {
        s->batch_size = (size_t)bs;
    }

    int rc = msr_open_conn(s);
    if (rc != BETL_OK) goto fail;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s->hdbc, &s->hstmt))) {
        betl_set_error(ctx, "mssql.read: SQLAllocHandle(STMT) failed");
        goto fail;
    }
    SQLRETURN er = SQLExecDirect(s->hstmt, (SQLCHAR *)s->query, SQL_NTS);
    if (!SQL_SUCCEEDED(er)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, s->hstmt, msg, sizeof msg);
        betl_set_error(ctx, "mssql.read: SELECT failed: %s", msg);
        goto fail;
    }
    s->hstmt_executed = 1;

    *state = s;
    return BETL_OK;

fail:
    if (s->hstmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, s->hstmt);
    if (s->hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(s->hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, s->hdbc);
    }
    if (s->henv != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, s->henv);
    free(s->connection);
    free(s->query);
    free(s);
    return BETL_ERR_INVALID;
}

static void msr_free_batch_strings(MsReadState *s) {
    if (!s->cols) return;
    for (SQLSMALLINT c = 0; c < s->n_cols; ++c) {
        if (s->col_fmts[c] != 'u') continue;
        if (!s->cols[c].u8_strs) continue;
        for (size_t r = 0; r < s->cur_rows; ++r) {
            free(s->cols[c].u8_strs[r]);
            s->cols[c].u8_strs[r] = NULL;
        }
    }
}

static void msr_destroy(void *state) {
    MsReadState *s = state;
    if (!s) return;
    if (s->cols) {
        msr_free_batch_strings(s);
        for (SQLSMALLINT c = 0; c < s->n_cols; ++c) {
            free(s->cols[c].i64_vals);
            free(s->cols[c].d32_vals);
            free(s->cols[c].d128_vals);
            free(s->cols[c].uuid_vals);
            free(s->cols[c].f64_vals);
            free(s->cols[c].u8_strs);
            free(s->cols[c].u8_lens);
            free(s->cols[c].nulls);
        }
        free(s->cols);
    }
    if (s->col_names) {
        for (SQLSMALLINT c = 0; c < s->n_cols; ++c) free(s->col_names[c]);
        free(s->col_names);
    }
    if (s->col_fmt_strings) {
        for (SQLSMALLINT c = 0; c < s->n_cols; ++c) free(s->col_fmt_strings[c]);
        free(s->col_fmt_strings);
    }
    free(s->col_fmts);
    free(s->col_precisions);
    free(s->col_scales);
    if (s->hstmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, s->hstmt);
    if (s->hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(s->hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, s->hdbc);
    }
    if (s->henv != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, s->henv);
    free(s->connection);
    free(s->query);
    free(s);
}

/* Resolve the result-set schema once. Discovers each column's name,
 * SQL type, and Arrow format; allocates per-column staging at
 * batch_size capacity. */
static int msr_resolve_schema(MsReadState *s) {
    if (s->schema_resolved) return 0;
    SQLSMALLINT n = 0;
    SQLNumResultCols(s->hstmt, &n);
    if (n <= 0) {
        betl_set_error(s->ctx, "mssql.read: query returned no columns");
        return -1;
    }
    s->n_cols = n;
    s->col_names       = calloc((size_t)n, sizeof *s->col_names);
    s->col_fmts        = malloc((size_t)n);
    s->col_precisions  = calloc((size_t)n, sizeof *s->col_precisions);
    s->col_scales      = calloc((size_t)n, sizeof *s->col_scales);
    s->col_fmt_strings = calloc((size_t)n, sizeof *s->col_fmt_strings);
    s->cols            = calloc((size_t)n, sizeof *s->cols);
    if (!s->col_names || !s->col_fmts || !s->col_precisions
        || !s->col_scales || !s->col_fmt_strings || !s->cols) {
        betl_set_error(s->ctx, "mssql.read: out of memory");
        return -1;
    }
    for (SQLSMALLINT c = 0; c < n; ++c) {
        SQLCHAR colname[256];
        SQLSMALLINT name_len = 0, sql_type = 0, decimal = 0, nullable = 0;
        SQLULEN col_size = 0;
        if (!SQL_SUCCEEDED(SQLDescribeCol(s->hstmt, (SQLUSMALLINT)(c + 1),
                                          colname, sizeof colname, &name_len,
                                          &sql_type, &col_size,
                                          &decimal, &nullable))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLDescribeCol(%d) failed", (int)(c + 1));
            return -1;
        }
        char fmt;
        if (sql_type_to_fmt(sql_type, &fmt) != 0) {
            betl_set_error(s->ctx,
                "mssql.read: column '%s' has unsupported SQL type %d "
                "(supported: int family, char/varchar family, date, "
                "datetime/datetime2, numeric/decimal)",
                colname, (int)sql_type);
            return -1;
        }
        s->col_fmts[c]  = fmt;
        s->col_names[c] = strdup((const char *)colname);
        if (!s->col_names[c]) {
            betl_set_error(s->ctx, "mssql.read: out of memory");
            return -1;
        }
        if (fmt == 'N') {
            int p  = (int)col_size;
            int sc = (int)decimal;
            if (p < 1 || p > 38 || sc < 0 || sc > p) {
                betl_set_error(s->ctx,
                    "mssql.read: column '%s' DECIMAL(%d,%d) outside supported "
                    "[1,38] precision", s->col_names[c], p, sc);
                return -1;
            }
            s->col_precisions[c] = p;
            s->col_scales[c]     = sc;
            char buf[24];
            snprintf(buf, sizeof buf, "d:%d,%d", p, sc);
            s->col_fmt_strings[c] = strdup(buf);
            if (!s->col_fmt_strings[c]) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        }
        /* Pre-allocate staging at batch_size capacity. */
        s->cols[c].nulls = calloc(s->batch_size, 1);
        if (!s->cols[c].nulls) {
            betl_set_error(s->ctx, "mssql.read: out of memory");
            return -1;
        }
        if (fmt == 'l' || fmt == 'T' || fmt == 'Z' || fmt == 'M') {
            s->cols[c].i64_vals = malloc(s->batch_size * sizeof(int64_t));
            if (!s->cols[c].i64_vals) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        } else if (fmt == 'D') {
            s->cols[c].d32_vals = malloc(s->batch_size * sizeof(int32_t));
            if (!s->cols[c].d32_vals) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        } else if (fmt == 'N') {
            s->cols[c].d128_vals = malloc(s->batch_size * sizeof(betl_dec128));
            if (!s->cols[c].d128_vals) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        } else if (fmt == 'U') {
            s->cols[c].uuid_vals = malloc(s->batch_size * 16);
            if (!s->cols[c].uuid_vals) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        } else if (fmt == 'g') {
            s->cols[c].f64_vals = malloc(s->batch_size * sizeof(double));
            if (!s->cols[c].f64_vals) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        } else {
            s->cols[c].u8_strs = calloc(s->batch_size, sizeof(char *));
            s->cols[c].u8_lens = calloc(s->batch_size, sizeof(size_t));
            if (!s->cols[c].u8_strs || !s->cols[c].u8_lens) {
                betl_set_error(s->ctx, "mssql.read: out of memory");
                return -1;
            }
        }
    }
    s->schema_resolved = 1;
    return 0;
}

/* ============================================================== *
 *  Arrow leaf builders                                             *
 * ============================================================== */

static uint8_t *msr_build_validity(const uint8_t *nulls, size_t n,
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

static int msr_build_int64_leaf(struct ArrowArray *out,
                                const int64_t *vals,
                                const uint8_t *nulls,
                                size_t n) {
    int64_t *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) {
        vbuf[i] = nulls[i] ? 0 : vals[i];
    }
    int64_t null_count = 0;
    uint8_t *vmap = msr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = msr_release_int64_leaf;
    return 0;
}

static void msr_release_date32_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msr_release_decimal128_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msr_release_uuid_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msr_release_float64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static int msr_build_float64_leaf(struct ArrowArray *out,
                                  const double *vals,
                                  const uint8_t *nulls,
                                  size_t n) {
    double *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0.0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = msr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = msr_release_float64_leaf;
    return 0;
}

static int msr_build_uuid_leaf(struct ArrowArray *out,
                               const uint8_t *vals, /* n*16 bytes */
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
    uint8_t *vmap = msr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = msr_release_uuid_leaf;
    return 0;
}

static int msr_build_decimal128_leaf(struct ArrowArray *out,
                                     const betl_dec128 *vals,
                                     const uint8_t *nulls,
                                     size_t n) {
    betl_dec128 *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = msr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = msr_release_decimal128_leaf;
    return 0;
}

static int msr_build_date32_leaf(struct ArrowArray *out,
                                 const int32_t *vals,
                                 const uint8_t *nulls,
                                 size_t n) {
    int32_t *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) vbuf[i] = nulls[i] ? 0 : vals[i];
    int64_t null_count = 0;
    uint8_t *vmap = msr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(vbuf); return -1; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = msr_release_date32_leaf;
    return 0;
}

static int msr_build_utf8_leaf(struct ArrowArray *out,
                               char *const *strs, const size_t *lens,
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
    uint8_t *vmap = msr_build_validity(nulls, n, &null_count);
    if (null_count > 0 && !vmap) { free(offs); free(data); return -1; }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = offs; bufs[2] = data;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 3;
    out->buffers    = bufs;
    out->release    = msr_release_utf8_leaf;
    return 0;
}

/* ============================================================== *
 *  Stream — get_schema + get_next                                  *
 * ============================================================== */

static struct ArrowSchema *msr_new_leaf(const char *name, const char *fmt) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    char *nm = strdup(name);
    if (!c || !nm) { free(c); free(nm); return NULL; }
    c->format  = fmt;
    c->name    = nm;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = msr_release_schema_named;
    return c;
}

static int msr_stream_get_schema(struct ArrowArrayStream *st,
                                 struct ArrowSchema *out) {
    MsReadState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (msr_resolve_schema(s) != 0) return EIO;

    struct ArrowSchema **kids = calloc((size_t)s->n_cols, sizeof *kids);
    if (!kids) return ENOMEM;
    for (SQLSMALLINT i = 0; i < s->n_cols; ++i) {
        const char *fmt = "u";
        int owned = 0;
        switch (s->col_fmts[i]) {
            case 'l': fmt = "l";    break;
            case 'g': fmt = "g";    break;
            case 'D': fmt = "tdD";  break;
            case 'T': fmt = "tsu:"; break;
            case 'Z': fmt = "tsu:UTC"; break;
            case 'M': fmt = "ttu"; break;
            case 'U': fmt = "w:16"; break;
            case 'N':
                fmt = strdup(s->col_fmt_strings[i]);
                owned = 1;
                if (!fmt) {
                    for (SQLSMALLINT k = 0; k < i; ++k) {
                        if (kids[k]->release) kids[k]->release(kids[k]);
                        free(kids[k]);
                    }
                    free(kids);
                    return ENOMEM;
                }
                break;
            default:  fmt = "u";    break;
        }
        kids[i] = msr_new_leaf(s->col_names[i], fmt);
        if (!kids[i]) {
            if (owned) free((void *)fmt);
            for (SQLSMALLINT k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            return ENOMEM;
        }
        if (owned) {
            /* msr_new_leaf used the default release that frees only the
             * name; switch to the variant that also frees format. */
            kids[i]->release = msr_release_schema_named_owned_format;
        }
        if (!kids[i]) {
            for (SQLSMALLINT k = 0; k < i; ++k) {
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
    out->release    = msr_release_schema_struct;
    return 0;
}

/* Read one cell from the current row into the column's staging slot. */
static int msr_read_cell(MsReadState *s, SQLSMALLINT c, size_t row) {
    MsReadCol *col = &s->cols[c];
    if (s->col_fmts[c] == 'Z') {
        /* DATETIMEOFFSET via SQL_C_CHAR: comes back as e.g.
         * "2026-05-11 10:30:00.123456 +05:30". Strip the offset and
         * normalize to UTC micros. */
        SQLCHAR buf[64];
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_CHAR, buf, sizeof buf, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(datetimeoffset) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]    = 1;
            col->i64_vals[row] = 0;
        } else {
            /* SQL Server emits " " before the offset; trim trailing spaces
             * and embed-`+` as a tz separator. The shared parser handles
             * the space-then-sign layout too if we collapse to one space. */
            size_t blen = (size_t)ind;
            int64_t us;
            if (betl_parse_iso_tstz((const char *)buf, blen, &us) != 0) {
                /* Try after stripping the space before the offset. */
                char tmp[80];
                size_t out = 0;
                int saw_sign = 0;
                for (size_t i = 0; i < blen && out + 1 < sizeof tmp; ++i) {
                    char ch = (char)buf[i];
                    if (!saw_sign && (ch == '+' || ch == '-') && i >= 19) {
                        /* drop any whitespace just before */
                        while (out > 0 && tmp[out - 1] == ' ') --out;
                        saw_sign = 1;
                    }
                    tmp[out++] = ch;
                }
                tmp[out] = '\0';
                if (betl_parse_iso_tstz(tmp, out, &us) != 0) {
                    betl_set_error(s->ctx,
                        "mssql.read: row %zu col '%s': '%.*s' is not a "
                        "valid DATETIMEOFFSET",
                        row, s->col_names[c], (int)blen, (const char *)buf);
                    return -1;
                }
            }
            col->nulls[row]    = 0;
            col->i64_vals[row] = us;
        }
        return 0;
    }
    if (s->col_fmts[c] == 'M') {
        /* Fetch as text — "HH:MM:SS[.uuuuuu]". */
        SQLCHAR buf[24];
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_CHAR, buf, sizeof buf, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(time) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]    = 1;
            col->i64_vals[row] = 0;
        } else {
            int64_t us;
            if (betl_parse_iso_time((const char *)buf, (size_t)ind, &us) != 0) {
                betl_set_error(s->ctx,
                    "mssql.read: row %zu col '%s': '%.*s' is not a valid time",
                    row, s->col_names[c], (int)ind, (const char *)buf);
                return -1;
            }
            col->nulls[row]    = 0;
            col->i64_vals[row] = us;
        }
        return 0;
    }
    if (s->col_fmts[c] == 'g') {
        SQLDOUBLE v = 0;
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_DOUBLE, &v, sizeof v, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(float) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]     = 1;
            col->f64_vals[row]  = 0.0;
        } else {
            col->nulls[row]     = 0;
            col->f64_vals[row]  = (double)v;
        }
        return 0;
    }
    if (s->col_fmts[c] == 'U') {
        /* UNIQUEIDENTIFIER as text: standard 36-char form (sometimes
         * upper-case). Our parser is case-insensitive. */
        SQLCHAR buf[64];
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_CHAR, buf, sizeof buf, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(uuid) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row] = 1;
            memset(&col->uuid_vals[row * 16], 0, 16);
        } else {
            if (betl_uuid_parse((const char *)buf, (size_t)ind,
                                &col->uuid_vals[row * 16]) != 0) {
                betl_set_error(s->ctx,
                    "mssql.read: row %zu col '%s': '%.*s' is not a valid UUID",
                    row, s->col_names[c], (int)ind, (const char *)buf);
                return -1;
            }
            col->nulls[row] = 0;
        }
        return 0;
    }
    if (s->col_fmts[c] == 'N') {
        /* Fetch as text — driver formats as "[-]ddd[.ddd...]" with the
         * column's scale. Parse with our shared decimal helper. */
        SQLCHAR buf[64];
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_CHAR, buf, sizeof buf, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(decimal) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]     = 1;
            col->d128_vals[row] = 0;
        } else {
            betl_dec128 v;
            if (betl_dec128_parse((const char *)buf, (size_t)ind,
                                  s->col_scales[c], &v) != 0) {
                betl_set_error(s->ctx,
                    "mssql.read: row %zu col '%s': '%s' is not a valid "
                    "DECIMAL(%d,%d)", row, s->col_names[c], (const char *)buf,
                    s->col_precisions[c], s->col_scales[c]);
                return -1;
            }
            col->nulls[row]     = 0;
            col->d128_vals[row] = v;
        }
        return 0;
    }
    if (s->col_fmts[c] == 'l') {
        SQLBIGINT v = 0;
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_SBIGINT, &v, sizeof v, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(int) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]    = 1;
            col->i64_vals[row] = 0;
        } else {
            col->nulls[row]    = 0;
            col->i64_vals[row] = (int64_t)v;
        }
        return 0;
    }
    if (s->col_fmts[c] == 'D') {
        SQL_DATE_STRUCT ds = {0};
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_TYPE_DATE, &ds, sizeof ds, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(date) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]    = 1;
            col->d32_vals[row] = 0;
        } else {
            col->nulls[row]    = 0;
            col->d32_vals[row] = betl_days_from_civil(
                (int)ds.year, (unsigned)ds.month, (unsigned)ds.day);
        }
        return 0;
    }
    if (s->col_fmts[c] == 'T') {
        SQL_TIMESTAMP_STRUCT ts = {0};
        SQLLEN ind = 0;
        if (!SQL_SUCCEEDED(SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                      SQL_C_TYPE_TIMESTAMP, &ts, sizeof ts, &ind))) {
            betl_set_error(s->ctx,
                "mssql.read: SQLGetData(timestamp) col %d row %zu failed",
                (int)(c + 1), row);
            return -1;
        }
        if (ind == SQL_NULL_DATA) {
            col->nulls[row]    = 1;
            col->i64_vals[row] = 0;
        } else {
            int32_t days = betl_days_from_civil(
                (int)ts.year, (unsigned)ts.month, (unsigned)ts.day);
            /* ODBC ts.fraction is in nanoseconds; truncate to micros. */
            int64_t us_of_day = (int64_t)ts.hour   * 3600000000LL
                              + (int64_t)ts.minute *   60000000LL
                              + (int64_t)ts.second *    1000000LL
                              + (int64_t)ts.fraction / 1000LL;
            col->nulls[row]    = 0;
            col->i64_vals[row] = (int64_t)days * 86400000000LL + us_of_day;
        }
        return 0;
    }
    /* utf8 — handle truncation by re-reading the tail when the first
     * call only fit a prefix (SQL_SUCCESS_WITH_INFO with ind > buffer). */
    SQLCHAR small[1024];
    SQLLEN ind = 0;
    SQLRETURN gr = SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                              SQL_C_CHAR, small, sizeof small, &ind);
    if (!SQL_SUCCEEDED(gr)) {
        betl_set_error(s->ctx,
            "mssql.read: SQLGetData(str) col %d row %zu failed",
            (int)(c + 1), row);
        return -1;
    }
    if (ind == SQL_NULL_DATA) {
        col->nulls[row]   = 1;
        col->u8_strs[row] = NULL;
        col->u8_lens[row] = 0;
        return 0;
    }
    size_t total_len = (size_t)ind;
    char *dup = malloc(total_len + 1);
    if (!dup) {
        betl_set_error(s->ctx, "mssql.read: out of memory");
        return -1;
    }
    if (gr == SQL_SUCCESS) {
        memcpy(dup, small, total_len);
        dup[total_len] = '\0';
    } else {
        /* SUCCESS_WITH_INFO: small was truncated. Driver NUL-terminated
         * the prefix at sizeof small - 1. Pull the rest with a sized
         * buffer. */
        size_t got = sizeof small - 1;
        if (got > total_len) got = total_len;
        memcpy(dup, small, got);
        if (got < total_len) {
            SQLLEN ind2 = 0;
            SQLRETURN gr2 = SQLGetData(s->hstmt, (SQLUSMALLINT)(c + 1),
                                       SQL_C_CHAR, dup + got,
                                       (SQLLEN)(total_len - got + 1),
                                       &ind2);
            if (!SQL_SUCCEEDED(gr2)) {
                free(dup);
                betl_set_error(s->ctx,
                    "mssql.read: SQLGetData(str cont) col %d row %zu failed",
                    (int)(c + 1), row);
                return -1;
            }
        }
        dup[total_len] = '\0';
    }
    col->nulls[row]   = 0;
    col->u8_strs[row] = dup;
    col->u8_lens[row] = total_len;
    return 0;
}

static int msr_stream_get_next(struct ArrowArrayStream *st,
                               struct ArrowArray *out) {
    MsReadState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (msr_resolve_schema(s) != 0) return EIO;
    if (s->eof) return 0;

    s->cur_rows = 0;
    while (s->cur_rows < s->batch_size) {
        if (betl_should_cancel(s->ctx)) {
            betl_set_error(s->ctx, "mssql.read: cancelled by host");
            return EIO;
        }
        SQLRETURN fr = SQLFetch(s->hstmt);
        if (fr == SQL_NO_DATA) { s->eof = 1; break; }
        if (!SQL_SUCCEEDED(fr)) {
            char msg[512] = {0};
            copy_diag(SQL_HANDLE_STMT, s->hstmt, msg, sizeof msg);
            betl_set_error(s->ctx, "mssql.read: SQLFetch failed: %s", msg);
            return EIO;
        }
        for (SQLSMALLINT c = 0; c < s->n_cols; ++c) {
            if (msr_read_cell(s, c, s->cur_rows) != 0) return EIO;
        }
        s->cur_rows++;
    }
    if (s->cur_rows == 0) return 0;     /* clean EOF */

    int64_t n = (int64_t)s->cur_rows;

    struct ArrowArray **kids = calloc((size_t)s->n_cols, sizeof *kids);
    if (!kids) {
        betl_set_error(s->ctx, "mssql.read: out of memory");
        return EIO;
    }
    int build_failed = 0;
    for (SQLSMALLINT c = 0; c < s->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) { build_failed = 1; break; }
        int rc;
        if (s->col_fmts[c] == 'l' || s->col_fmts[c] == 'T'
            || s->col_fmts[c] == 'Z' || s->col_fmts[c] == 'M') {
            rc = msr_build_int64_leaf(kids[c], s->cols[c].i64_vals,
                                      s->cols[c].nulls, (size_t)n);
        } else if (s->col_fmts[c] == 'D') {
            rc = msr_build_date32_leaf(kids[c], s->cols[c].d32_vals,
                                       s->cols[c].nulls, (size_t)n);
        } else if (s->col_fmts[c] == 'N') {
            rc = msr_build_decimal128_leaf(kids[c], s->cols[c].d128_vals,
                                           s->cols[c].nulls, (size_t)n);
        } else if (s->col_fmts[c] == 'U') {
            rc = msr_build_uuid_leaf(kids[c], s->cols[c].uuid_vals,
                                     s->cols[c].nulls, (size_t)n);
        } else if (s->col_fmts[c] == 'g') {
            rc = msr_build_float64_leaf(kids[c], s->cols[c].f64_vals,
                                        s->cols[c].nulls, (size_t)n);
        } else {
            rc = msr_build_utf8_leaf(kids[c], s->cols[c].u8_strs,
                                     s->cols[c].u8_lens,
                                     s->cols[c].nulls, (size_t)n);
        }
        if (rc != 0) { build_failed = 1; break; }
    }
    if (build_failed) {
        for (SQLSMALLINT c = 0; c < s->n_cols; ++c) {
            if (kids[c]) {
                if (kids[c]->release) kids[c]->release(kids[c]);
                free(kids[c]);
            }
        }
        free(kids);
        betl_set_error(s->ctx, "mssql.read: failed to build output column");
        return EIO;
    }

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (SQLSMALLINT c = 0; c < s->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        betl_set_error(s->ctx, "mssql.read: out of memory");
        return EIO;
    }
    outer[0] = NULL;
    out->length     = n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = msr_release_struct;

    /* The leaf builders memcpy'd each row's utf8 bytes into the Arrow
     * data buffer, so the per-row scratch can be released ahead of the
     * next batch. */
    msr_free_batch_strings(s);
    return 0;
}

static const char *msr_stream_get_last_error(struct ArrowArrayStream *st) {
    (void)st; return NULL;
}

static void msr_stream_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int msr_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = msr_stream_get_schema;
    out->get_next       = msr_stream_get_next;
    out->get_last_error = msr_stream_get_last_error;
    out->release        = msr_stream_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef msr_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DERIVED, .doc = "query rows" },
};

static const BetlComponentDef msr_components[] = {
    { .name               = "mssql.read",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = 0,
      .outputs            = msr_outputs,
      .output_count       = 1,
      .init               = msr_init,
      .destroy            = msr_destroy,
      .attach_output      = msr_attach_output },
};

static const BetlProvider msr_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-mssql-read",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = msr_components,
    .component_count = sizeof msr_components / sizeof msr_components[0],
};

int betl_register_mssql_read(BetlRegistry *r) {
    return betl_registry_register(r, &msr_provider, "<builtin:mssql-read>");
}
