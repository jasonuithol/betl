/* mssql.bulkinsert — SINK that writes Arrow record batches into SQL
 * Server via unixODBC using bulk-array parameter binding.
 *
 * Config (per SPEC §6.4-bulk, mirroring mssql.upsert minus the merge
 * semantics):
 *   connection  string,       required — name of a connection in BetlContext
 *   table       string,       required — schema-qualified target
 *   columns     list[string], optional — explicit column list to write
 *                                        (defaults to the input schema)
 *   batch_size  int,          optional, default 1000 — rows per SQLExecute
 *                                        (capped at 65535 by ODBC)
 *
 * Wire path: SQLPrepare an INSERT once, set SQL_ATTR_PARAMSET_SIZE to
 * batch_size, bind every parameter to a per-column array of that
 * length, then execute one batch at a time. The final partial batch
 * lowers SQL_ATTR_PARAMSET_SIZE to the actual row count before its
 * SQLExecute. Everything wrapped in a single transaction.
 *
 * Throughput target: a SQLExecute amortises across batch_size rows,
 * so the per-row TCP RTT cost is divided down. Expected ~10× over
 * mssql.upsert's row-by-row MERGE on the same data path.
 *
 * NB: lots of code in here mirrors mssql_upsert.c (type marshalling,
 * JSON parsing, connection setup). A future cleanup should pull the
 * shared bits into a common header — for now the duplication is the
 * tradeoff against an over-engineered shared layer. */

#include "runtime/mssql_bulkinsert.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "betl/provider.h"
#include "runtime/binary_util.h"
#include "runtime/date_util.h"
#include "runtime/decimal_util.h"
#include "runtime/uuid_util.h"
#include "runtime/mssql_sql.h"
#include "runtime/mssql_bulk_common.h"

/* Default ODBC parameter array size — 1000 rows / SQLExecute. The
 * ODBC max is 65535 (SQL_UINTEGER); driver-side limits are typically
 * lower (memory cost per batch grows linearly). 1000 is the sweet
 * spot in practice for INSERT into SQL Server. */
/* Mode-dependent default batch sizes:
 *   array: 1000 — bigger doesn't help (the ODBC driver flattens
 *          SQL_ATTR_PARAMSET_SIZE arrays to per-row INSERTs server-
 *          side) and costs memory proportional to batch_size.
 *   bcp:   10000 — TDS bulk-load throughput keeps climbing through
 *          ~20k before flattening. Memory cost in bcp mode is
 *          negligible because we only allocate one row of scalar
 *          buffers regardless of batch_size; batch_size only
 *          controls the bcp_batch commit-grouping cadence.
 * Both can be overridden via the `batch_size:` config. */
#define BI_DEFAULT_BATCH_ARRAY 1000
#define BI_DEFAULT_BATCH_BCP   10000
#define BI_MAX_BATCH           65535

/* Per-column max-width caps for variable-length types. Rows that
 * exceed these limits return BETL_ERR_INVALID at fill time rather
 * than silently truncating. The cap can be raised by widening these
 * constants — they're the tradeoff between memory and the maximum
 * VARCHAR/VARBINARY width we'll bulk-load. */
#define BI_UTF8_MAX_WIDTH   4096
#define BI_BINARY_MAX_WIDTH 8192
#define BI_TSTZ_MAX_WIDTH   48
#define BI_DECIMAL_MAX_WIDTH 48
#define BI_UUID_WIDTH       37
#define BI_TIME_MAX_WIDTH   20

/* ============================================================== *
 *  JSON value extractor                                            *
 *  (same shape as mssql_upsert.c — flat JSON only)                 *
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

static int json_int(const char *json, const char *key, int64_t *out) {
    const char *v = json_value_after(json, key);
    if (!v) return -1;
    char *end = NULL;
    long long ll = strtoll(v, &end, 10);
    if (end == v) return -1;
    *out = (int64_t)ll;
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

/* Pull the `dsn` field out of the named connection. Returns a malloc'd
 * NUL-terminated string on success or NULL with `out_err` populated.
 * Shared with the BCP TU via mssql_bulk_common.h. */
char *ms_bulk_get_dsn(BetlContext *ctx, const char *connection_name,
                      char *out_err, size_t out_err_cap) {
    const char *conn_json = betl_get_connection(ctx, connection_name);
    if (!conn_json) {
        snprintf(out_err, out_err_cap,
                 "connection '%s' not declared", connection_name);
        return NULL;
    }
    char *dsn = NULL;
    if (json_string(conn_json, "dsn", &dsn) != 0 || !dsn) {
        snprintf(out_err, out_err_cap,
                 "connection '%s' missing `dsn` field", connection_name);
        free(dsn);
        return NULL;
    }
    return dsn;
}

/* ============================================================== *
 *  Arrow → ODBC type mapping (mirrors mssql_upsert.c)              *
 * ============================================================== */

/* MsColType lives in mssql_bulk_common.h so the BCP TU can see it
 * too. The mapping function moved here is exported via the same
 * header for the BCP TU to reuse. */

MsColType ms_bulk_arrow_to_ms(const char *fmt) {
    if (!fmt) return MS_UNSUPPORTED;
    if (strcmp(fmt, "l")        == 0) return MS_INT64;
    if (strcmp(fmt, "L")        == 0) return MS_UINT64;
    if (strcmp(fmt, "c")        == 0) return MS_INT8;
    if (strcmp(fmt, "C")        == 0) return MS_UINT8;
    if (strcmp(fmt, "s")        == 0) return MS_INT16;
    if (strcmp(fmt, "S")        == 0) return MS_UINT16;
    if (strcmp(fmt, "i")        == 0) return MS_INT32;
    if (strcmp(fmt, "I")        == 0) return MS_UINT32;
    if (strcmp(fmt, "g")        == 0) return MS_FLOAT64;
    if (strcmp(fmt, "f")        == 0) return MS_FLOAT32;
    if (strcmp(fmt, "u")        == 0) return MS_UTF8;
    if (strcmp(fmt, "b")        == 0) return MS_BOOL;
    if (strcmp(fmt, "tdD")      == 0) return MS_DATE32;
    if (strcmp(fmt, "tsu:")     == 0) return MS_TIMESTAMP_US;
    if (strcmp(fmt, "tsu:UTC")  == 0) return MS_TIMESTAMP_TZ;
    if (strcmp(fmt, "w:16")     == 0) return MS_UUID;
    if (strcmp(fmt, "ttu")      == 0) return MS_TIME_US;
    if (strcmp(fmt, "z")        == 0) return MS_BINARY;
    if (strncmp(fmt, "d:", 2)   == 0) return MS_DECIMAL128;
    return MS_UNSUPPORTED;
}

int ms_bulk_decimal_pscale(const char *fmt, int *p, int *s) {
    return sscanf(fmt + 2, "%d,%d", p, s) == 2 ? 0 : -1;
}

/* ============================================================== *
 *  Per-column bulk buffer.                                         *
 *                                                                  *
 *  Fixed-width types (ints, floats, bit, date, ts) use a packed    *
 *  array of N elements. Variable-length types (utf8, binary,       *
 *  decimal-as-text, tstz-as-text, uuid, time) use an array of N    *
 *  fixed-width slots of `slot_size` bytes each, with the actual    *
 *  byte length per row carried in ind_array[i].                    *
 * ============================================================== */
typedef struct {
    MsColType type;
    int       dec_precision;
    int       dec_scale;

    /* For fixed-width: one of these is the bound array.
     * For variable-length: str_data + slot_size (row stride). */
    SQLBIGINT             *i64_array;     /* MS_INT* / MS_UINT* */
    SQLDOUBLE             *f64_array;     /* MS_FLOAT* */
    SQLCHAR               *bool_array;    /* MS_BOOL — SQL_C_BIT */
    SQL_DATE_STRUCT       *date_array;    /* MS_DATE32 */
    SQL_TIMESTAMP_STRUCT  *ts_array;      /* MS_TIMESTAMP_US */
    char                  *str_data;      /* variable-width staging */
    size_t                 slot_size;     /* bytes per row in str_data */
    SQLLEN                *ind_array;     /* len/SQL_NULL_DATA per row */
} MsBulkCol;

static void ms_bulk_col_free(MsBulkCol *c) {
    free(c->i64_array);
    free(c->f64_array);
    free(c->bool_array);
    free(c->date_array);
    free(c->ts_array);
    free(c->str_data);
    free(c->ind_array);
}

/* Allocate per-column arrays sized for `n_rows`. Returns 0 / -1. */
static int ms_bulk_col_alloc(MsBulkCol *c, size_t n_rows) {
    c->ind_array = calloc(n_rows, sizeof *c->ind_array);
    if (!c->ind_array) return -1;
    switch (c->type) {
    case MS_INT64: case MS_UINT64:
    case MS_INT8:  case MS_UINT8:
    case MS_INT16: case MS_UINT16:
    case MS_INT32: case MS_UINT32:
        c->i64_array = calloc(n_rows, sizeof *c->i64_array);
        return c->i64_array ? 0 : -1;
    case MS_FLOAT64: case MS_FLOAT32:
        c->f64_array = calloc(n_rows, sizeof *c->f64_array);
        return c->f64_array ? 0 : -1;
    case MS_BOOL:
        c->bool_array = calloc(n_rows, sizeof *c->bool_array);
        return c->bool_array ? 0 : -1;
    case MS_DATE32:
        c->date_array = calloc(n_rows, sizeof *c->date_array);
        return c->date_array ? 0 : -1;
    case MS_TIMESTAMP_US:
        c->ts_array = calloc(n_rows, sizeof *c->ts_array);
        return c->ts_array ? 0 : -1;
    case MS_UTF8:           c->slot_size = BI_UTF8_MAX_WIDTH;    break;
    case MS_BINARY:         c->slot_size = BI_BINARY_MAX_WIDTH;  break;
    case MS_TIMESTAMP_TZ:   c->slot_size = BI_TSTZ_MAX_WIDTH;    break;
    case MS_DECIMAL128:     c->slot_size = BI_DECIMAL_MAX_WIDTH; break;
    case MS_UUID:           c->slot_size = BI_UUID_WIDTH;        break;
    case MS_TIME_US:        c->slot_size = BI_TIME_MAX_WIDTH;    break;
    case MS_UNSUPPORTED:
    default:
        return -1;
    }
    /* Variable-length path. */
    c->str_data = calloc(n_rows, c->slot_size);
    return c->str_data ? 0 : -1;
}

/* ============================================================== *
 *  State                                                           *
 * ============================================================== */

/* MsBulkMode and MsColType live in mssql_bulk_common.h since the BCP
 * TU needs them too. */

typedef struct {
    BetlContext *ctx;

    /* Configured */
    char       *connection_name;
    char       *table;
    char      **explicit_cols;
    size_t      n_explicit_cols;
    size_t      batch_size;
    MsBulkMode  mode;

    /* ODBC handles — only opened when mode == MS_MODE_ARRAY. */
    SQLHENV henv;
    SQLHDBC hdbc;

    /* Input stream */
    int                     have_input;
    struct ArrowArrayStream input;
} MsBulkState;

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
        betl_set_error(ctx, "mssql.bulkinsert: connection '%s' not declared", name);
        return BETL_ERR_NOT_FOUND;
    }
    char *dsn = NULL;
    if (json_string(conn_json, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(ctx,
            "mssql.bulkinsert: connection '%s' is missing a `dsn` field", name);
        free(dsn);
        return BETL_ERR_INVALID;
    }

    SQLHENV henv = SQL_NULL_HENV;
    SQLHDBC hdbc = SQL_NULL_HDBC;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv))) {
        betl_set_error(ctx, "mssql.bulkinsert: SQLAllocHandle(ENV) failed");
        free(dsn);
        return BETL_ERR_INTERNAL;
    }
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc))) {
        betl_set_error(ctx, "mssql.bulkinsert: SQLAllocHandle(DBC) failed");
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
        betl_set_error(ctx, "mssql.bulkinsert: connect failed: %s", msg);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
        return BETL_ERR_AUTH;
    }
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
    MsBulkState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->henv = SQL_NULL_HENV;
    s->hdbc = SQL_NULL_HDBC;

    if (json_string(cfg, "connection", &s->connection_name) != 0
        || !s->connection_name)
    {
        betl_set_error(ctx, "mssql.bulkinsert: missing required `connection`");
        goto fail;
    }
    if (json_string(cfg, "table", &s->table) != 0 || !s->table) {
        betl_set_error(ctx, "mssql.bulkinsert: missing required `table`");
        goto fail;
    }
    if (json_string_array(cfg, "columns",
                          &s->explicit_cols, &s->n_explicit_cols) != 0)
    {
        s->explicit_cols   = NULL;
        s->n_explicit_cols = 0;
    }

    /* mode: array (default) | bcp (requires libsybdb at compile time).
     * Parse first so the batch_size default can be mode-dependent. */
    char *mode_str = NULL;
    json_string(cfg, "mode", &mode_str);
    if (mode_str && *mode_str) {
        if (strcmp(mode_str, "array") == 0) {
            s->mode = MS_MODE_ARRAY;
        } else if (strcmp(mode_str, "bcp") == 0) {
#ifdef BETL_HAVE_SYBDB
            s->mode = MS_MODE_BCP;
#else
            betl_set_error(ctx,
                "mssql.bulkinsert: mode=bcp requested but this build "
                "lacks libsybdb (BETL_HAVE_SYBDB undefined). Use "
                "mode=array, or rebuild with FreeTDS db-lib in deps/.");
            free(mode_str);
            goto fail;
#endif
        } else {
            betl_set_error(ctx,
                "mssql.bulkinsert: unknown mode '%s' (want array|bcp)",
                mode_str);
            free(mode_str);
            goto fail;
        }
    }
    free(mode_str);

    /* Default batch_size depends on mode (see BI_DEFAULT_BATCH_*).
     * Explicit batch_size: overrides. */
    s->batch_size = (s->mode == MS_MODE_BCP)
        ? BI_DEFAULT_BATCH_BCP : BI_DEFAULT_BATCH_ARRAY;
    int64_t bs = 0;
    if (json_int(cfg, "batch_size", &bs) == 0) {
        if (bs < 1 || bs > BI_MAX_BATCH) {
            betl_set_error(ctx,
                "mssql.bulkinsert: batch_size %" PRId64
                " out of range (1..%d)", bs, BI_MAX_BATCH);
            goto fail;
        }
        s->batch_size = (size_t)bs;
    }

    /* ODBC connection is only needed in array mode. BCP mode opens
     * its own dblib connection in ms_bcp_run() when the data starts
     * flowing. */
    if (s->mode == MS_MODE_ARRAY) {
        int conn_rc = ms_open_conn(ctx, s->connection_name,
                                   &s->henv, &s->hdbc);
        if (conn_rc != BETL_OK) goto fail;
    }

    *state = s;
    return BETL_OK;

fail:
    free(s->connection_name);
    free(s->table);
    free_string_array(s->explicit_cols, s->n_explicit_cols);
    ms_close_conn(s->henv, s->hdbc);
    free(s);
    return BETL_ERR_INVALID;
}

static void ms_destroy(void *state) {
    MsBulkState *s = state;
    if (!s) return;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    ms_close_conn(s->henv, s->hdbc);
    free(s->connection_name);
    free(s->table);
    free_string_array(s->explicit_cols, s->n_explicit_cols);
    free(s);
}

static int ms_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    MsBulkState *s = state;
    s->input = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  Cell fill / param binding                                       *
 * ============================================================== */

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Write one Arrow cell into slot `i` of `b`'s array. Returns 0 / BETL_*. */
static int ms_fill_cell(BetlContext *ctx,
                        const struct ArrowArray *col,
                        int64_t row,
                        MsBulkCol *b,
                        size_t i,
                        const char *col_name) {
    if (validity_is_null(col, row)) {
        b->ind_array[i] = SQL_NULL_DATA;
        return BETL_OK;
    }
    int64_t off = col->offset + row;
    switch (b->type) {
    case MS_INT64:
    case MS_UINT64:
    case MS_INT8:  case MS_UINT8:
    case MS_INT16: case MS_UINT16:
    case MS_INT32: case MS_UINT32: {
        int64_t v = 0;
        switch (b->type) {
            case MS_INT64:  v = ((const int64_t  *)col->buffers[1])[off]; break;
            case MS_UINT64: v = (int64_t)((const uint64_t *)col->buffers[1])[off]; break;
            case MS_INT8:   v = ((const int8_t   *)col->buffers[1])[off]; break;
            case MS_UINT8:  v = ((const uint8_t  *)col->buffers[1])[off]; break;
            case MS_INT16:  v = ((const int16_t  *)col->buffers[1])[off]; break;
            case MS_UINT16: v = ((const uint16_t *)col->buffers[1])[off]; break;
            case MS_INT32:  v = ((const int32_t  *)col->buffers[1])[off]; break;
            case MS_UINT32: v = (int64_t)((const uint32_t *)col->buffers[1])[off]; break;
            default: break;
        }
        b->i64_array[i] = (SQLBIGINT)v;
        b->ind_array[i] = 0;
        return BETL_OK;
    }
    case MS_FLOAT64:
    case MS_FLOAT32: {
        double v = (b->type == MS_FLOAT32)
                       ? (double)((const float *)col->buffers[1])[off]
                       : ((const double *)col->buffers[1])[off];
        b->f64_array[i] = (SQLDOUBLE)v;
        b->ind_array[i] = 0;
        return BETL_OK;
    }
    case MS_BOOL: {
        const uint8_t *bits = col->buffers[1];
        b->bool_array[i] = (SQLCHAR)((bits[off / 8] >> (off % 8)) & 1);
        b->ind_array[i] = 0;
        return BETL_OK;
    }
    case MS_UTF8: {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t  len   = (size_t)(end - start);
        if (len > b->slot_size) {
            betl_set_error(ctx,
                "mssql.bulkinsert: utf8 value in column '%s' (%zu bytes) "
                "exceeds slot width %zu — raise BI_UTF8_MAX_WIDTH",
                col_name, len, b->slot_size);
            return BETL_ERR_INVALID;
        }
        if (len) memcpy(b->str_data + i * b->slot_size, data + start, len);
        b->ind_array[i] = (SQLLEN)len;
        return BETL_OK;
    }
    case MS_BINARY: {
        const int32_t *offs = col->buffers[1];
        const uint8_t *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t  len   = (size_t)(end - start);
        if (len > b->slot_size) {
            betl_set_error(ctx,
                "mssql.bulkinsert: binary value in column '%s' (%zu bytes) "
                "exceeds slot width %zu — raise BI_BINARY_MAX_WIDTH",
                col_name, len, b->slot_size);
            return BETL_ERR_INVALID;
        }
        if (len) memcpy(b->str_data + i * b->slot_size, data + start, len);
        b->ind_array[i] = (SQLLEN)len;
        return BETL_OK;
    }
    case MS_DATE32: {
        const int32_t *vals = col->buffers[1];
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(vals[off], &y, &m, &d);
        b->date_array[i].year  = (SQLSMALLINT)y;
        b->date_array[i].month = (SQLUSMALLINT)m;
        b->date_array[i].day   = (SQLUSMALLINT)d;
        b->ind_array[i] = (SQLLEN)sizeof b->date_array[i];
        return BETL_OK;
    }
    case MS_TIMESTAMP_US: {
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        b->ts_array[i].year     = (SQLSMALLINT)y;
        b->ts_array[i].month    = (SQLUSMALLINT)m;
        b->ts_array[i].day      = (SQLUSMALLINT)d;
        b->ts_array[i].hour     = (SQLUSMALLINT)(us_of_day / 3600000000LL);
        b->ts_array[i].minute   = (SQLUSMALLINT)((us_of_day / 60000000LL) % 60);
        b->ts_array[i].second   = (SQLUSMALLINT)((us_of_day / 1000000LL) % 60);
        b->ts_array[i].fraction = (SQLUINTEGER)((us_of_day % 1000000LL) * 1000);
        b->ind_array[i] = (SQLLEN)sizeof b->ts_array[i];
        return BETL_OK;
    }
    case MS_TIMESTAMP_TZ: {
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        int hh   = (int)(us_of_day / 3600000000LL);
        int mm   = (int)((us_of_day / 60000000LL) % 60);
        int ss   = (int)((us_of_day / 1000000LL) % 60);
        int frac = (int)(us_of_day % 1000000LL);
        char tmp[BI_TSTZ_MAX_WIDTH];
        int n;
        if (frac == 0) {
            n = snprintf(tmp, sizeof tmp,
                         "%04d-%02u-%02u %02d:%02d:%02d +00:00",
                         y, m, d, hh, mm, ss);
        } else {
            n = snprintf(tmp, sizeof tmp,
                         "%04d-%02u-%02u %02d:%02d:%02d.%06d +00:00",
                         y, m, d, hh, mm, ss, frac);
        }
        if (n < 0 || (size_t)n >= sizeof tmp) {
            betl_set_error(ctx, "mssql.bulkinsert: tstz format overflow");
            return BETL_ERR_INTERNAL;
        }
        memcpy(b->str_data + i * b->slot_size, tmp, (size_t)n);
        b->ind_array[i] = (SQLLEN)n;
        return BETL_OK;
    }
    case MS_TIME_US: {
        const int64_t *vals = col->buffers[1];
        char tmp[BI_TIME_MAX_WIDTH];
        int n = betl_format_iso_time(vals[off], tmp, sizeof tmp);
        if (n < 0) {
            betl_set_error(ctx,
                "mssql.bulkinsert: time format failed for col '%s'", col_name);
            return BETL_ERR_INTERNAL;
        }
        memcpy(b->str_data + i * b->slot_size, tmp, (size_t)n);
        b->ind_array[i] = (SQLLEN)n;
        return BETL_OK;
    }
    case MS_UUID: {
        const uint8_t *vals = col->buffers[1];
        char tmp[BI_UUID_WIDTH];
        if (betl_uuid_format(&vals[off * 16], tmp, 36) < 0) {
            betl_set_error(ctx,
                "mssql.bulkinsert: uuid format failed for col '%s'", col_name);
            return BETL_ERR_INTERNAL;
        }
        memcpy(b->str_data + i * b->slot_size, tmp, 36);
        b->ind_array[i] = 36;
        return BETL_OK;
    }
    case MS_DECIMAL128: {
        const betl_dec128 *vals = col->buffers[1];
        char tmp[BI_DECIMAL_MAX_WIDTH];
        int n = betl_dec128_format(vals[off], b->dec_scale, tmp, sizeof tmp);
        if (n < 0) {
            betl_set_error(ctx,
                "mssql.bulkinsert: decimal format failed for col '%s'",
                col_name);
            return BETL_ERR_INTERNAL;
        }
        memcpy(b->str_data + i * b->slot_size, tmp, (size_t)n);
        b->ind_array[i] = (SQLLEN)n;
        return BETL_OK;
    }
    case MS_UNSUPPORTED:
    default:
        betl_set_error(ctx,
            "mssql.bulkinsert: column '%s' has unsupported Arrow type",
            col_name);
        return BETL_ERR_UNSUPPORTED;
    }
}

/* Bind one parameter to its column array. SQL_ATTR_PARAMSET_SIZE
 * tells the driver how many rows live behind each pointer; the
 * BufferLength here is the per-element stride. */
static int ms_bind_param(BetlContext *ctx, SQLHSTMT hstmt,
                         SQLUSMALLINT slot, MsBulkCol *b,
                         const char *col_name) {
    SQLRETURN rc;
    switch (b->type) {
    case MS_INT64:
    case MS_UINT64:
    case MS_INT8:  case MS_UINT8:
    case MS_INT16: case MS_UINT16:
    case MS_INT32: case MS_UINT32:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                              b->i64_array, sizeof *b->i64_array, b->ind_array);
        break;
    case MS_FLOAT64:
    case MS_FLOAT32:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_DOUBLE, SQL_DOUBLE, 0, 0,
                              b->f64_array, sizeof *b->f64_array, b->ind_array);
        break;
    case MS_BOOL:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_BIT, SQL_BIT, 0, 0,
                              b->bool_array, sizeof *b->bool_array, b->ind_array);
        break;
    case MS_UTF8:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_VARCHAR, b->slot_size, 0,
                              b->str_data, (SQLLEN)b->slot_size, b->ind_array);
        break;
    case MS_DATE32:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_TYPE_DATE, SQL_TYPE_DATE, 10, 0,
                              b->date_array, sizeof *b->date_array,
                              b->ind_array);
        break;
    case MS_TIMESTAMP_US:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6,
                              b->ts_array, sizeof *b->ts_array, b->ind_array);
        break;
    case MS_DECIMAL128:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_DECIMAL,
                              (SQLULEN)b->dec_precision, b->dec_scale,
                              b->str_data, (SQLLEN)b->slot_size, b->ind_array);
        break;
    case MS_TIMESTAMP_TZ:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_VARCHAR, 34, 7,
                              b->str_data, (SQLLEN)b->slot_size, b->ind_array);
        break;
    case MS_UUID:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_GUID, 36, 0,
                              b->str_data, (SQLLEN)b->slot_size, b->ind_array);
        break;
    case MS_TIME_US:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_CHAR, SQL_TYPE_TIME, 16, 6,
                              b->str_data, (SQLLEN)b->slot_size, b->ind_array);
        break;
    case MS_BINARY:
        rc = SQLBindParameter(hstmt, slot, SQL_PARAM_INPUT,
                              SQL_C_BINARY, SQL_VARBINARY, b->slot_size, 0,
                              b->str_data, (SQLLEN)b->slot_size, b->ind_array);
        break;
    case MS_UNSUPPORTED:
    default:
        betl_set_error(ctx,
            "mssql.bulkinsert: cannot bind unsupported type for column '%s'",
            col_name);
        return BETL_ERR_UNSUPPORTED;
    }
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(ctx,
            "mssql.bulkinsert: SQLBindParameter for column '%s' failed: %s",
            col_name, msg);
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

/* Flush the accumulated `n_pending` rows via one SQLExecute. */
static int ms_flush(BetlContext *ctx, SQLHSTMT hstmt, size_t n_pending,
                    int64_t *rows_written) {
    SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMSET_SIZE,
                   (SQLPOINTER)(SQLULEN)n_pending, 0);
    SQLRETURN rc = SQLExecute(hstmt);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(ctx,
            "mssql.bulkinsert: SQLExecute batch (%zu rows) failed: %s",
            n_pending, msg);
        return BETL_ERR_IO;
    }
    /* SQL Server's INSERT batches need the cursor closed between
     * executes so the row-count meta doesn't carry over. */
    SQLFreeStmt(hstmt, SQL_CLOSE);
    *rows_written += (int64_t)n_pending;
    return BETL_OK;
}

/* ============================================================== *
 *  sink_run                                                        *
 * ============================================================== */

static int ms_sink_run(void *state) {
    MsBulkState *s = state;
    if (!s->have_input) {
        betl_set_error(s->ctx,
            "mssql.bulkinsert: sink_run without attached input");
        return BETL_ERR_INVALID;
    }

    struct ArrowSchema schema = {0};
    if (s->input.get_schema(&s->input, &schema) != 0) {
        betl_set_error(s->ctx, "mssql.bulkinsert: get_schema failed");
        return BETL_ERR_IO;
    }
    if (!schema.format || strcmp(schema.format, "+s") != 0) {
        betl_set_error(s->ctx,
            "mssql.bulkinsert: input must be a struct stream (got '%s')",
            schema.format ? schema.format : "(null)");
        if (schema.release) schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    int64_t n_cols_in = schema.n_children;
    if (n_cols_in <= 0) {
        betl_set_error(s->ctx, "mssql.bulkinsert: input has no columns");
        schema.release(&schema);
        return BETL_ERR_TYPE;
    }

    /* Resolve column list. */
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
            out_cols[i] = (char *)schema.children[i]->name;
        }
        n_out_cols = (size_t)n_cols_in;
    }

    int rc = BETL_OK;
    int64_t   *col_to_child = malloc(n_out_cols * sizeof *col_to_child);
    MsBulkCol *bufs         = calloc(n_out_cols, sizeof *bufs);
    if (!col_to_child || !bufs) { rc = BETL_ERR_INTERNAL; goto cleanup_pre; }

    /* Pass 1: resolve each `out_cols[i]` to a child index in the
     * input schema, capture its Arrow type, and pull decimal p/scale
     * if present. No buffer allocation yet — BCP mode delegates that
     * to the BCP TU which manages its own driver-typed buffers. */
    for (size_t i = 0; i < n_out_cols; ++i) {
        col_to_child[i] = -1;
        for (int64_t j = 0; j < n_cols_in; ++j) {
            if (strcmp(out_cols[i], schema.children[j]->name) == 0) {
                col_to_child[i] = j;
                bufs[i].type = ms_bulk_arrow_to_ms(schema.children[j]->format);
                if (bufs[i].type == MS_DECIMAL128) {
                    int p = 0, sc = 0;
                    if (ms_bulk_decimal_pscale(schema.children[j]->format,
                                               &p, &sc) != 0) {
                        betl_set_error(s->ctx,
                            "mssql.bulkinsert: column '%s' has malformed decimal format",
                            out_cols[i]);
                        rc = BETL_ERR_TYPE;
                        goto cleanup_pre;
                    }
                    bufs[i].dec_precision = p;
                    bufs[i].dec_scale     = sc;
                }
                break;
            }
        }
        if (col_to_child[i] < 0) {
            betl_set_error(s->ctx,
                "mssql.bulkinsert: column '%s' not present in input schema",
                out_cols[i]);
            rc = BETL_ERR_INVALID;
            goto cleanup_pre;
        }
        if (bufs[i].type == MS_UNSUPPORTED) {
            betl_set_error(s->ctx,
                "mssql.bulkinsert: column '%s' has unsupported Arrow type",
                out_cols[i]);
            rc = BETL_ERR_UNSUPPORTED;
            goto cleanup_pre;
        }
    }

    if (s->mode == MS_MODE_BCP) {
#ifdef BETL_HAVE_SYBDB
        /* Marshal per-column metadata into flat arrays for the BCP TU
         * (it doesn't see our MsBulkCol layout — different headers). */
        MsColType *col_types = malloc(n_out_cols * sizeof *col_types);
        int       *col_prec  = malloc(n_out_cols * sizeof *col_prec);
        int       *col_scale = malloc(n_out_cols * sizeof *col_scale);
        if (!col_types || !col_prec || !col_scale) {
            free(col_types); free(col_prec); free(col_scale);
            rc = BETL_ERR_INTERNAL;
            goto cleanup_pre;
        }
        for (size_t i = 0; i < n_out_cols; ++i) {
            col_types[i] = bufs[i].type;
            col_prec[i]  = bufs[i].dec_precision;
            col_scale[i] = bufs[i].dec_scale;
        }
        rc = ms_bcp_run(s->ctx, s->connection_name, s->table,
                        out_cols, n_out_cols, s->batch_size,
                        col_to_child, col_types, col_prec, col_scale,
                        &s->input);
        free(col_types); free(col_prec); free(col_scale);
        goto cleanup_pre;
#else
        betl_set_error(s->ctx,
            "mssql.bulkinsert: mode=bcp not compiled in (libsybdb absent)");
        rc = BETL_ERR_UNSUPPORTED;
        goto cleanup_pre;
#endif
    }

    /* Pass 2 (array mode only): allocate the bulk-array buffers. */
    for (size_t i = 0; i < n_out_cols; ++i) {
        if (ms_bulk_col_alloc(&bufs[i], s->batch_size) != 0) {
            betl_set_error(s->ctx,
                "mssql.bulkinsert: OOM allocating bulk buffers for col '%s'",
                out_cols[i]);
            rc = BETL_ERR_INTERNAL;
            goto cleanup_pre;
        }
    }

    /* --- Build SQL --- */
    BetlBuf sql_buf = {0};
    int br = betl_build_mssql_insert_sql(&sql_buf, s->table,
                                         out_cols, n_out_cols);
    if (br != 0) {
        const char *why = (br == -1) ? "embedded `]` in identifier"
                                     : "out of memory";
        betl_set_error(s->ctx, "mssql.bulkinsert: build_sql: %s", why);
        rc = (br == -2) ? BETL_ERR_INTERNAL : BETL_ERR_INVALID;
        free(sql_buf.data);
        goto cleanup_pre;
    }

    /* --- PREPARE + bind once, set bulk attrs --- */
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s->hdbc, &hstmt))) {
        betl_set_error(s->ctx, "mssql.bulkinsert: SQLAllocHandle(STMT) failed");
        free(sql_buf.data);
        rc = BETL_ERR_INTERNAL;
        goto cleanup_pre;
    }
    if (!SQL_SUCCEEDED(SQLPrepare(hstmt, (SQLCHAR *)sql_buf.data, SQL_NTS))) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(s->ctx, "mssql.bulkinsert: PREPARE failed: %s", msg);
        free(sql_buf.data);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        rc = BETL_ERR_IO;
        goto cleanup_pre;
    }
    free(sql_buf.data);

    /* Column-wise parameter binding semantics. Default is column-wise
     * when SQL_ATTR_PARAM_BIND_TYPE is SQL_PARAM_BIND_BY_COLUMN (the
     * default); we set it explicitly to be defensive against drivers
     * that flipped the default. */
    SQLSetStmtAttr(hstmt, SQL_ATTR_PARAM_BIND_TYPE,
                   (SQLPOINTER)SQL_PARAM_BIND_BY_COLUMN, 0);
    /* PARAMSET_SIZE is set per-flush — full batches use s->batch_size,
     * the trailing partial uses its actual row count. */

    for (size_t i = 0; i < n_out_cols; ++i) {
        rc = ms_bind_param(s->ctx, hstmt, (SQLUSMALLINT)(i + 1),
                           &bufs[i], out_cols[i]);
        if (rc != BETL_OK) {
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            goto cleanup_pre;
        }
    }

    /* --- Main loop --- */
    int64_t n_rows_total = 0;
    size_t  n_pending    = 0;

    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            betl_set_error(s->ctx, "mssql.bulkinsert: cancelled by host");
            rc = BETL_ERR_CANCELLED;
            break;
        }
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            const char *up = s->input.get_last_error
                ? s->input.get_last_error(&s->input) : NULL;
            betl_set_error(s->ctx,
                "mssql.bulkinsert: get_next failed: %s",
                up ? up : "(no detail)");
            rc = BETL_ERR_IO;
            break;
        }
        if (!batch.release) break; /* end of stream */

        for (int64_t r = 0; r < batch.length; ++r) {
            for (size_t c = 0; c < n_out_cols; ++c) {
                int frc = ms_fill_cell(s->ctx,
                                       batch.children[col_to_child[c]],
                                       r, &bufs[c], n_pending, out_cols[c]);
                if (frc != BETL_OK) {
                    rc = frc;
                    goto break_batch;
                }
            }
            ++n_pending;
            if (n_pending == s->batch_size) {
                int frc = ms_flush(s->ctx, hstmt, n_pending, &n_rows_total);
                if (frc != BETL_OK) { rc = frc; goto break_batch; }
                n_pending = 0;
            }
        }
        batch.release(&batch);
        continue;

    break_batch:
        if (batch.release) batch.release(&batch);
        break;
    }

    if (rc == BETL_OK && n_pending > 0) {
        rc = ms_flush(s->ctx, hstmt, n_pending, &n_rows_total);
    }

    if (rc == BETL_OK) {
        if (!SQL_SUCCEEDED(SQLEndTran(SQL_HANDLE_DBC, s->hdbc, SQL_COMMIT))) {
            char msg[512] = {0};
            copy_diag(SQL_HANDLE_DBC, s->hdbc, msg, sizeof msg);
            betl_set_error(s->ctx, "mssql.bulkinsert: COMMIT failed: %s", msg);
            rc = BETL_ERR_IO;
        }
    } else {
        SQLEndTran(SQL_HANDLE_DBC, s->hdbc, SQL_ROLLBACK);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);

    if (rc == BETL_OK) {
        betl_log(s->ctx, BETL_LOG_INFO,
                 "mssql.bulkinsert: wrote %" PRId64 " rows to %s "
                 "(batch_size=%zu)",
                 n_rows_total, s->table, s->batch_size);
    }

cleanup_pre:
    if (bufs) {
        for (size_t i = 0; i < n_out_cols; ++i) ms_bulk_col_free(&bufs[i]);
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
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows to bulk-insert" },
};

static const BetlComponentDef ms_components[] = {
    { .name               = "mssql.bulkinsert",
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
    .name            = "betl-builtins-mssql-bulkinsert",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = ms_components,
    .component_count = sizeof ms_components / sizeof ms_components[0],
};

int betl_register_mssql_bulkinsert(BetlRegistry *r) {
    return betl_registry_register(r, &ms_provider, "<builtin:mssql.bulkinsert>");
}
