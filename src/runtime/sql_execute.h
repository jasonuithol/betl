/* sql.execute — control-flow TASK that runs a single SQL statement
 * against a declared connection. Dispatches by the connection's
 * `type:` field (postgres / mssql). SSIS Execute SQL Task parity:
 * one-shot, no row trigger, no result rows captured. */

#ifndef BETL_RUNTIME_SQL_EXECUTE_H
#define BETL_RUNTIME_SQL_EXECUTE_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_sql_execute(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
