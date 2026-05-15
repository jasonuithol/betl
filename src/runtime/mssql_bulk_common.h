/* Shared internal types for mssql.bulkinsert.
 *
 * The Phase 1 (ODBC bulk-array) and Phase 2 (FreeTDS BCP) code paths
 * live in different translation units because unixODBC's <sqltypes.h>
 * and FreeTDS' <sybdb.h> define `RETCODE` and `BOOL` incompatibly
 * (signed short vs int). This header carries the bits both files
 * need that are driver-free: config struct, mode enum, type-mapping
 * enum, and the function the main TU calls into the BCP TU through.
 *
 * Per-column buffer structs and the Arrow→driver marshalling stay
 * private to each TU — they need driver-typed fields, and the two
 * drivers' structs aren't layout-compatible. */

#ifndef BETL_RUNTIME_MSSQL_BULK_COMMON_H
#define BETL_RUNTIME_MSSQL_BULK_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "betl/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MS_MODE_ARRAY = 0,
    MS_MODE_BCP   = 1,
} MsBulkMode;

typedef enum {
    MS_INT64,
    MS_INT8,  MS_UINT8,
    MS_INT16, MS_UINT16,
    MS_INT32, MS_UINT32,
    MS_UINT64,
    MS_FLOAT64,
    MS_FLOAT32,
    MS_UTF8,
    MS_BOOL,
    MS_DATE32,
    MS_TIMESTAMP_US,
    MS_TIMESTAMP_TZ,
    MS_DECIMAL128,
    MS_UUID,
    MS_TIME_US,
    MS_BINARY,
    MS_UNSUPPORTED
} MsColType;

MsColType ms_bulk_arrow_to_ms(const char *fmt);
int       ms_bulk_decimal_pscale(const char *fmt, int *p, int *s);

/* Look up the connection's `dsn` field, returning a malloc'd copy on
 * success or NULL with `out_err` populated. Caller frees. */
char     *ms_bulk_get_dsn(BetlContext *ctx, const char *connection_name,
                          char *out_err, size_t out_err_cap);

/* BCP entry points — defined only when BETL_HAVE_SYBDB is on. The
 * main TU calls these through this header; the BCP TU implements
 * them and includes the FreeTDS db-lib headers privately. */
#ifdef BETL_HAVE_SYBDB
int  ms_bcp_run(BetlContext         *ctx,
                const char          *connection_name,
                const char          *table,
                char               **out_cols,
                size_t               n_out_cols,
                size_t               batch_size,
                int64_t             *col_to_child,
                MsColType           *col_types,
                const int           *col_dec_precision,
                const int           *col_dec_scale,
                struct ArrowArrayStream *input);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_MSSQL_BULK_COMMON_H */
