/* mssql.upsert — SINK that writes Arrow batches into SQL Server via
 * unixODBC, using a MERGE statement. Compiled only when ODBC was found
 * at configure time. */

#ifndef BETL_RUNTIME_MSSQL_UPSERT_H
#define BETL_RUNTIME_MSSQL_UPSERT_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_mssql(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_MSSQL_UPSERT_H */
