/* mssql `mssql.lookup` TRANSFORM (SPEC §4.3) — sibling of postgres.lookup,
 * built on top of unixODBC. Conditionally compiled when ODBC is available. */

#ifndef BETL_RUNTIME_MSSQL_LOOKUP_H
#define BETL_RUNTIME_MSSQL_LOOKUP_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_mssql_lookup(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
