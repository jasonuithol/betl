/* mssql.bulkinsert — Phase 2 BCP path.
 *
 * Lives in its own translation unit because FreeTDS db-lib's
 * <sybdb.h> conflicts with unixODBC's <sqltypes.h> on `RETCODE` and
 * `BOOL`. The main TU (mssql_bulkinsert.c) handles config parsing,
 * column resolution, and the ODBC array path, then calls
 * `ms_bcp_run` here for mode=bcp.
 *
 * Wire path: dblib opens a DBPROCESS, bcp_init names the target
 * table, bcp_bind ties each column to a host scalar, the main loop
 * fills the scalars per row and calls bcp_sendrow. FreeTDS holds a
 * client-side buffer and flushes a TDS bulk-load frame whenever it
 * fills; bcp_batch is a "commit this group" hint we issue every
 * batch_size rows. */

#define _GNU_SOURCE         /* strncasecmp */
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sybfront.h>
#include <sybdb.h>

#include "betl/provider.h"
#include "runtime/binary_util.h"
#include "runtime/date_util.h"
#include "runtime/decimal_util.h"
#include "runtime/uuid_util.h"
#include "runtime/mssql_bulk_common.h"

/* Per-column scalar buffer. One row's worth — bcp_sendrow reads the
 * bound addresses each call, so we just overwrite them per row. */
typedef struct {
    MsColType  type;
    int        dec_precision;
    int        dec_scale;
    DBBIGINT   i64;
    DBFLT8     f64;
    DBBIT      bit;
    DBDATEREC  date_rec;       /* (host pre-formatted struct) */
    /* For SYBMSDATE / SYBMSDATETIME2 / SYBMSDATETIMEOFFSET we use the
     * driver's own wire structs. The simplest cross-version idiom is
     * to format dates/timestamps as ISO strings and bind as SYBCHAR. */
    char      *str_buf;
    size_t     str_cap;
    DBINT      varlen;         /* per-row actual length for var-types */
} BcpCol;

#define BCP_STR_DEFAULT_CAP 64
#define BCP_STR_MAX_CAP     8192

static int bcp_col_ensure_str_cap(BcpCol *b, size_t n) {
    if (b->str_cap >= n) return 0;
    size_t nc = b->str_cap ? b->str_cap : BCP_STR_DEFAULT_CAP;
    while (nc < n) nc *= 2;
    if (nc > BCP_STR_MAX_CAP) nc = n;
    char *p = realloc(b->str_buf, nc);
    if (!p) return -1;
    b->str_buf = p;
    b->str_cap = nc;
    return 0;
}

static void bcp_col_free(BcpCol *b) { free(b->str_buf); }

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Write one Arrow cell into the BCP column scalar. Returns 0/BETL_*.
 * NULLs are signalled to bcp_sendrow via bcp_collen(-1) at the caller. */
static int bcp_fill_cell(BetlContext *ctx, const struct ArrowArray *col,
                         int64_t row, BcpCol *b, const char *col_name,
                         int *is_null_out) {
    if (validity_is_null(col, row)) {
        *is_null_out = 1;
        b->varlen = 0;
        return BETL_OK;
    }
    *is_null_out = 0;
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
        b->i64 = (DBBIGINT)v;
        b->varlen = (DBINT)sizeof b->i64;
        return BETL_OK;
    }
    case MS_FLOAT64:
    case MS_FLOAT32: {
        double v = (b->type == MS_FLOAT32)
                       ? (double)((const float *)col->buffers[1])[off]
                       : ((const double *)col->buffers[1])[off];
        b->f64 = (DBFLT8)v;
        b->varlen = (DBINT)sizeof b->f64;
        return BETL_OK;
    }
    case MS_BOOL: {
        const uint8_t *bits = col->buffers[1];
        b->bit = (DBBIT)((bits[off / 8] >> (off % 8)) & 1);
        b->varlen = (DBINT)sizeof b->bit;
        return BETL_OK;
    }
    case MS_UTF8:
    case MS_BINARY: {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        size_t  len   = (size_t)(end - start);
        if (bcp_col_ensure_str_cap(b, len ? len : 1) != 0) {
            betl_set_error(ctx,
                "mssql.bulkinsert(bcp): OOM staging column '%s'", col_name);
            return BETL_ERR_INTERNAL;
        }
        if (len) memcpy(b->str_buf, data + start, len);
        b->varlen = (DBINT)len;
        return BETL_OK;
    }
    case MS_DATE32: {
        /* Format as 'YYYY-MM-DD' and bind via SYBCHAR — most reliable
         * across FreeTDS versions; SYBMSDATE wire format varies. */
        const int32_t *vals = col->buffers[1];
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(vals[off], &y, &m, &d);
        char tmp[16];
        int n = snprintf(tmp, sizeof tmp, "%04d-%02u-%02u", y, m, d);
        if (n < 0) return BETL_ERR_INTERNAL;
        if (bcp_col_ensure_str_cap(b, (size_t)n + 1) != 0) return BETL_ERR_INTERNAL;
        memcpy(b->str_buf, tmp, (size_t)n);
        b->varlen = (DBINT)n;
        return BETL_OK;
    }
    case MS_TIMESTAMP_US: {
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        char tmp[40];
        int n = snprintf(tmp, sizeof tmp,
                         "%04d-%02u-%02u %02" PRId64 ":%02" PRId64
                         ":%02" PRId64 ".%06" PRId64,
                         y, m, d,
                         (int64_t)(us_of_day / 3600000000LL),
                         (int64_t)((us_of_day / 60000000LL) % 60),
                         (int64_t)((us_of_day / 1000000LL) % 60),
                         (int64_t)(us_of_day % 1000000LL));
        if (n < 0 || (size_t)n >= sizeof tmp) return BETL_ERR_INTERNAL;
        if (bcp_col_ensure_str_cap(b, (size_t)n + 1) != 0) return BETL_ERR_INTERNAL;
        memcpy(b->str_buf, tmp, (size_t)n);
        b->varlen = (DBINT)n;
        return BETL_OK;
    }
    case MS_TIMESTAMP_TZ: {
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        char tmp[48];
        int n = snprintf(tmp, sizeof tmp,
                         "%04d-%02u-%02u %02" PRId64 ":%02" PRId64
                         ":%02" PRId64 ".%06" PRId64 " +00:00",
                         y, m, d,
                         (int64_t)(us_of_day / 3600000000LL),
                         (int64_t)((us_of_day / 60000000LL) % 60),
                         (int64_t)((us_of_day / 1000000LL) % 60),
                         (int64_t)(us_of_day % 1000000LL));
        if (n < 0 || (size_t)n >= sizeof tmp) return BETL_ERR_INTERNAL;
        if (bcp_col_ensure_str_cap(b, (size_t)n + 1) != 0) return BETL_ERR_INTERNAL;
        memcpy(b->str_buf, tmp, (size_t)n);
        b->varlen = (DBINT)n;
        return BETL_OK;
    }
    case MS_TIME_US: {
        const int64_t *vals = col->buffers[1];
        char tmp[20];
        int n = betl_format_iso_time(vals[off], tmp, sizeof tmp);
        if (n < 0) return BETL_ERR_INTERNAL;
        if (bcp_col_ensure_str_cap(b, (size_t)n + 1) != 0) return BETL_ERR_INTERNAL;
        memcpy(b->str_buf, tmp, (size_t)n);
        b->varlen = (DBINT)n;
        return BETL_OK;
    }
    case MS_UUID: {
        const uint8_t *vals = col->buffers[1];
        char tmp[37];
        if (betl_uuid_format(&vals[off * 16], tmp, 36) < 0)
            return BETL_ERR_INTERNAL;
        if (bcp_col_ensure_str_cap(b, 37) != 0) return BETL_ERR_INTERNAL;
        memcpy(b->str_buf, tmp, 36);
        b->varlen = 36;
        return BETL_OK;
    }
    case MS_DECIMAL128: {
        const betl_dec128 *vals = col->buffers[1];
        char tmp[48];
        int n = betl_dec128_format(vals[off], b->dec_scale, tmp, sizeof tmp);
        if (n < 0) return BETL_ERR_INTERNAL;
        if (bcp_col_ensure_str_cap(b, (size_t)n + 1) != 0) return BETL_ERR_INTERNAL;
        memcpy(b->str_buf, tmp, (size_t)n);
        b->varlen = (DBINT)n;
        return BETL_OK;
    }
    case MS_UNSUPPORTED:
    default:
        betl_set_error(ctx,
            "mssql.bulkinsert(bcp): column '%s' has unsupported Arrow type",
            col_name);
        return BETL_ERR_UNSUPPORTED;
    }
}

/* Sybase host type for each Arrow column. Strings/dates/etc all go
 * across as SYBCHAR — the simplest cross-FreeTDS-version idiom. */
static int ms_to_sybtype(MsColType t) {
    switch (t) {
    case MS_INT64: case MS_UINT64:
    case MS_INT8:  case MS_UINT8:
    case MS_INT16: case MS_UINT16:
    case MS_INT32: case MS_UINT32:
        return SYBINT8;
    case MS_FLOAT64: case MS_FLOAT32:
        return SYBFLT8;
    case MS_BOOL:
        return SYBBIT;
    case MS_UTF8:
    case MS_DATE32:
    case MS_TIMESTAMP_US:
    case MS_TIMESTAMP_TZ:
    case MS_TIME_US:
    case MS_DECIMAL128:
    case MS_UUID:
        return SYBCHAR;
    case MS_BINARY:
        return SYBBINARY;
    case MS_UNSUPPORTED:
    default:
        return -1;
    }
}

static int bcp_col_alloc_for(BcpCol *b, MsColType t,
                             int dec_precision, int dec_scale)
{
    b->type          = t;
    b->dec_precision = dec_precision;
    b->dec_scale     = dec_scale;
    switch (t) {
    case MS_UTF8:
    case MS_BINARY:
    case MS_DATE32:
    case MS_TIMESTAMP_US:
    case MS_TIMESTAMP_TZ:
    case MS_TIME_US:
    case MS_DECIMAL128:
    case MS_UUID:
        return bcp_col_ensure_str_cap(b, BCP_STR_DEFAULT_CAP);
    default:
        return 0;
    }
}

/* ============================================================== *
 *  dblib lifecycle                                                 *
 * ============================================================== */

static char dblib_last_msg[512];

static int dblib_msg_handler(DBPROCESS *dbproc, DBINT msgno, int msgstate,
                             int severity, char *msgtext, char *srvname,
                             char *procname, int line)
{
    (void)dbproc; (void)msgstate; (void)srvname; (void)procname; (void)line;
    if (severity > 10 && msgtext) {
        snprintf(dblib_last_msg, sizeof dblib_last_msg,
                 "dblib msg %ld sev=%d: %s",
                 (long)msgno, severity, msgtext);
    }
    return 0;
}

static int dblib_err_handler(DBPROCESS *dbproc, int severity, int dberr,
                             int oserr, char *dberrstr, char *oserrstr)
{
    (void)dbproc;
    if (dberrstr) {
        snprintf(dblib_last_msg, sizeof dblib_last_msg,
                 "dblib err %d sev=%d: %s%s%s",
                 dberr, severity, dberrstr,
                 (oserrstr && oserr) ? " — os: " : "",
                 (oserrstr && oserr) ? oserrstr : "");
    }
    return INT_CANCEL;
}

static int dblib_inited = 0;
static int dblib_init_once(BetlContext *ctx) {
    if (dblib_inited) return BETL_OK;
    if (dbinit() == FAIL) {
        betl_set_error(ctx, "mssql.bulkinsert(bcp): dbinit() failed");
        return BETL_ERR_INTERNAL;
    }
    dberrhandle(dblib_err_handler);
    dbmsghandle(dblib_msg_handler);
    dblib_inited = 1;
    return BETL_OK;
}

/* DSN key=value lookup — case-insensitive, handles {braced} values. */
static int dsn_lookup(const char *dsn, const char *key,
                      char *out, size_t out_cap)
{
    if (!dsn || !key || !out || out_cap < 2) return -1;
    size_t key_len = strlen(key);
    const char *p = dsn;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') ++p;
        if (!*p) break;
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t this_key_len = (size_t)(eq - p);
        while (this_key_len > 0
               && (p[this_key_len - 1] == ' ' || p[this_key_len - 1] == '\t'))
            --this_key_len;
        int match = (this_key_len == key_len)
                 && strncasecmp(p, key, key_len) == 0;
        const char *v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        const char *v_end;
        if (*v == '{') {
            ++v;
            v_end = strchr(v, '}');
            if (!v_end) v_end = v + strlen(v);
        } else {
            v_end = strchr(v, ';');
            if (!v_end) v_end = v + strlen(v);
        }
        if (match) {
            size_t v_len = (size_t)(v_end - v);
            if (v_len >= out_cap) v_len = out_cap - 1;
            memcpy(out, v, v_len);
            out[v_len] = '\0';
            return 0;
        }
        p = (*v_end == '}') ? v_end + 1 : v_end;
    }
    return -1;
}

static DBPROCESS *bcp_open(BetlContext *ctx, const char *connection_name) {
    if (dblib_init_once(ctx) != BETL_OK) return NULL;

    char err_buf[256];
    char *dsn = ms_bulk_get_dsn(ctx, connection_name,
                                err_buf, sizeof err_buf);
    if (!dsn) {
        betl_set_error(ctx, "mssql.bulkinsert(bcp): %s", err_buf);
        return NULL;
    }
    char server[256] = {0}, port[16] = {0}, user[128] = {0};
    char pwd[128] = {0}, dbname[128] = {0};
    if (dsn_lookup(dsn, "Server",      server, sizeof server) != 0 &&
        dsn_lookup(dsn, "Host",        server, sizeof server) != 0 &&
        dsn_lookup(dsn, "Data Source", server, sizeof server) != 0)
    {
        betl_set_error(ctx,
            "mssql.bulkinsert(bcp): DSN has no Server/Host/Data Source");
        free(dsn);
        return NULL;
    }
    dsn_lookup(dsn, "Port", port, sizeof port);
    if (dsn_lookup(dsn, "UID", user, sizeof user) != 0)
        dsn_lookup(dsn, "User ID", user, sizeof user);
    if (dsn_lookup(dsn, "PWD", pwd, sizeof pwd) != 0)
        dsn_lookup(dsn, "Password", pwd, sizeof pwd);
    if (dsn_lookup(dsn, "Database", dbname, sizeof dbname) != 0)
        dsn_lookup(dsn, "Initial Catalog", dbname, sizeof dbname);
    free(dsn);

    LOGINREC *login = dblogin();
    if (!login) {
        betl_set_error(ctx, "mssql.bulkinsert(bcp): dblogin() failed");
        return NULL;
    }
    if (user[0])   DBSETLUSER(login, user);
    if (pwd[0])    DBSETLPWD(login, pwd);
    if (dbname[0]) DBSETLDBNAME(login, dbname);
    BCP_SETL(login, TRUE);   /* BCP must be enabled on the login */

    char server_arg[300];
    if (port[0]) snprintf(server_arg, sizeof server_arg,
                          "%s:%s", server, port);
    else         snprintf(server_arg, sizeof server_arg, "%s", server);

    dblib_last_msg[0] = '\0';
    DBPROCESS *dbproc = dbopen(login, server_arg);
    dbloginfree(login);
    if (!dbproc) {
        betl_set_error(ctx,
            "mssql.bulkinsert(bcp): dbopen('%s') failed: %s",
            server_arg,
            dblib_last_msg[0] ? dblib_last_msg : "(no detail)");
        return NULL;
    }
    if (dbname[0]) dbuse(dbproc, dbname);
    return dbproc;
}

/* ============================================================== *
 *  ms_bcp_run — entry point called from the array TU              *
 * ============================================================== */

int ms_bcp_run(BetlContext              *ctx,
               const char               *connection_name,
               const char               *table,
               char                    **out_cols,
               size_t                    n_out_cols,
               size_t                    batch_size,
               int64_t                  *col_to_child,
               MsColType                *col_types,
               const int                *col_dec_precision,
               const int                *col_dec_scale,
               struct ArrowArrayStream  *input)
{
    DBPROCESS *dbproc = bcp_open(ctx, connection_name);
    if (!dbproc) return BETL_ERR_AUTH;

    int rc = BETL_OK;

    BcpCol *cols = calloc(n_out_cols, sizeof *cols);
    if (!cols) { rc = BETL_ERR_INTERNAL; goto done; }

    for (size_t i = 0; i < n_out_cols; ++i) {
        if (bcp_col_alloc_for(&cols[i], col_types[i],
                              col_dec_precision[i],
                              col_dec_scale[i]) != 0) {
            betl_set_error(ctx, "mssql.bulkinsert(bcp): OOM allocating col '%s'",
                           out_cols[i]);
            rc = BETL_ERR_INTERNAL;
            goto done;
        }
    }

    dblib_last_msg[0] = '\0';
    if (bcp_init(dbproc, table, NULL, NULL, DB_IN) == FAIL) {
        betl_set_error(ctx,
            "mssql.bulkinsert(bcp): bcp_init('%s') failed: %s",
            table, dblib_last_msg[0] ? dblib_last_msg : "(no detail)");
        rc = BETL_ERR_IO;
        goto done;
    }

    /* Bind each column. For variable-width types we pass varlen=0 and
     * supply per-row actual length via bcp_collen. The scalar address
     * (and str_buf pointer for var-width) stays stable for the
     * lifetime of the bind — bcp_sendrow re-reads on each call. */
    for (size_t i = 0; i < n_out_cols; ++i) {
        BYTE *varaddr = NULL;
        DBINT varlen = 0;
        int sybtype = ms_to_sybtype(cols[i].type);
        if (sybtype < 0) {
            betl_set_error(ctx,
                "mssql.bulkinsert(bcp): col '%s' has no Sybase mapping — "
                "use mode=array", out_cols[i]);
            rc = BETL_ERR_UNSUPPORTED;
            goto done;
        }
        switch (cols[i].type) {
        case MS_INT64: case MS_UINT64:
        case MS_INT8:  case MS_UINT8:
        case MS_INT16: case MS_UINT16:
        case MS_INT32: case MS_UINT32:
            /* varlen=-1 → Sybase BCP "default length for this host
             * type" (8 for SYBINT8). Passing an explicit sizeof here
             * trips FreeTDS' SYBEBCIT check, which mis-interprets
             * non-zero varlen as "this is varying-length data, you
             * must supply a terminator". -1 is the right idiom. */
            varaddr = (BYTE *)&cols[i].i64;  varlen = -1; break;
        case MS_FLOAT64: case MS_FLOAT32:
            varaddr = (BYTE *)&cols[i].f64;  varlen = -1; break;
        case MS_BOOL:
            varaddr = (BYTE *)&cols[i].bit;  varlen = -1; break;
        default:
            varaddr = (BYTE *)cols[i].str_buf;
            varlen = 0;   /* var-width: bcp_collen sets actual per row */
            break;
        }
        dblib_last_msg[0] = '\0';
        if (bcp_bind(dbproc, varaddr, 0, varlen, NULL, 0, sybtype,
                     (int)(i + 1)) == FAIL)
        {
            betl_set_error(ctx,
                "mssql.bulkinsert(bcp): bcp_bind col '%s' (1-based pos %zu) "
                "failed: %s",
                out_cols[i], i + 1,
                dblib_last_msg[0] ? dblib_last_msg : "(no detail)");
            rc = BETL_ERR_IO;
            goto done;
        }
    }

    int64_t n_rows_total = 0;
    size_t  n_in_group   = 0;

    for (;;) {
        if (betl_should_cancel(ctx)) {
            betl_set_error(ctx, "mssql.bulkinsert(bcp): cancelled");
            rc = BETL_ERR_CANCELLED;
            break;
        }
        struct ArrowArray batch = {0};
        if (input->get_next(input, &batch) != 0) {
            const char *up = input->get_last_error
                ? input->get_last_error(input) : NULL;
            betl_set_error(ctx,
                "mssql.bulkinsert(bcp): get_next failed: %s",
                up ? up : "(no detail)");
            rc = BETL_ERR_IO;
            break;
        }
        if (!batch.release) break;

        for (int64_t r = 0; r < batch.length; ++r) {
            for (size_t c = 0; c < n_out_cols; ++c) {
                int is_null = 0;
                int frc = bcp_fill_cell(ctx,
                                        batch.children[col_to_child[c]],
                                        r, &cols[c], out_cols[c],
                                        &is_null);
                if (frc != BETL_OK) { rc = frc; goto break_batch; }
                /* Re-set the colptr for str_data (in case ensure_cap
                 * reallocated since the last bcp_bind) and the per-row
                 * length. For fixed-width types varlen stays fixed. */
                int sybtype = ms_to_sybtype(cols[c].type);
                if (sybtype == SYBCHAR || sybtype == SYBBINARY) {
                    if (bcp_colptr(dbproc, (BYTE *)cols[c].str_buf,
                                   (int)(c + 1)) == FAIL) {
                        betl_set_error(ctx,
                            "mssql.bulkinsert(bcp): bcp_colptr col '%s': %s",
                            out_cols[c],
                            dblib_last_msg[0] ? dblib_last_msg : "?");
                        rc = BETL_ERR_IO;
                        goto break_batch;
                    }
                    DBINT len = is_null ? -1 : cols[c].varlen;
                    if (bcp_collen(dbproc, len, (int)(c + 1)) == FAIL) {
                        betl_set_error(ctx,
                            "mssql.bulkinsert(bcp): bcp_collen col '%s': %s",
                            out_cols[c],
                            dblib_last_msg[0] ? dblib_last_msg : "?");
                        rc = BETL_ERR_IO;
                        goto break_batch;
                    }
                } else if (is_null) {
                    if (bcp_collen(dbproc, -1, (int)(c + 1)) == FAIL) {
                        rc = BETL_ERR_IO;
                        goto break_batch;
                    }
                } else {
                    /* Fixed-width non-null: restore the typed length
                     * in case a previous row was NULL. */
                    bcp_collen(dbproc, cols[c].varlen, (int)(c + 1));
                }
            }
            dblib_last_msg[0] = '\0';
            if (bcp_sendrow(dbproc) == FAIL) {
                betl_set_error(ctx,
                    "mssql.bulkinsert(bcp): bcp_sendrow row %" PRId64
                    ": %s",
                    n_rows_total + r,
                    dblib_last_msg[0] ? dblib_last_msg : "(no detail)");
                rc = BETL_ERR_IO;
                goto break_batch;
            }
            ++n_in_group;
            if (n_in_group >= batch_size) {
                DBINT batched = bcp_batch(dbproc);
                if (batched < 0) {
                    betl_set_error(ctx,
                        "mssql.bulkinsert(bcp): bcp_batch failed: %s",
                        dblib_last_msg[0] ? dblib_last_msg : "?");
                    rc = BETL_ERR_IO;
                    goto break_batch;
                }
                n_in_group = 0;
            }
        }
        n_rows_total += batch.length;
        batch.release(&batch);
        continue;

    break_batch:
        if (batch.release) batch.release(&batch);
        break;
    }

    if (rc == BETL_OK) {
        DBINT done_rows = bcp_done(dbproc);
        if (done_rows < 0) {
            betl_set_error(ctx,
                "mssql.bulkinsert(bcp): bcp_done failed: %s",
                dblib_last_msg[0] ? dblib_last_msg : "?");
            rc = BETL_ERR_IO;
        } else {
            betl_log(ctx, BETL_LOG_INFO,
                     "mssql.bulkinsert: wrote %" PRId64 " rows to %s "
                     "(mode=bcp batch_size=%zu)",
                     n_rows_total, table, batch_size);
        }
    } else {
        (void)bcp_done(dbproc);
    }

done:
    if (cols) {
        for (size_t i = 0; i < n_out_cols; ++i) bcp_col_free(&cols[i]);
        free(cols);
    }
    if (dbproc) dbclose(dbproc);
    return rc;
}
