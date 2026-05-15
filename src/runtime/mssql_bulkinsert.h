/* mssql.bulkinsert — SINK that writes Arrow batches into SQL Server
 * via unixODBC using bulk-array parameter binding
 * (SQL_ATTR_PARAMSET_SIZE). Insert-only — no MERGE / on_conflict
 * semantics. Pairs with mssql.upsert: pick this one when you need
 * raw throughput against an empty (or append-only) target.
 *
 * Compiled only when ODBC was found at configure time. */

#ifndef BETL_RUNTIME_MSSQL_BULKINSERT_H
#define BETL_RUNTIME_MSSQL_BULKINSERT_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_mssql_bulkinsert(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_MSSQL_BULKINSERT_H */
