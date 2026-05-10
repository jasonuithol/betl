/* mssql.read — SOURCE that runs a SELECT against SQL Server via
 * unixODBC and emits rows as Arrow batches. Conditionally compiled
 * when ODBC is available. */

#ifndef BETL_RUNTIME_MSSQL_READ_H
#define BETL_RUNTIME_MSSQL_READ_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_mssql_read(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
