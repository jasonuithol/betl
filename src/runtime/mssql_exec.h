/* mssql.exec — TRANSFORM that runs a user SQL statement per input row
 * against SQL Server via unixODBC, then forwards the row unchanged.
 * SSIS OLE DB Command parity. Conditionally compiled when ODBC is
 * available. */

#ifndef BETL_RUNTIME_MSSQL_EXEC_H
#define BETL_RUNTIME_MSSQL_EXEC_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_mssql_exec(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
