/* postgres.exec — TRANSFORM that runs a user SQL statement per input
 * row against Postgres via libpq, then forwards the row unchanged.
 * SSIS OLE DB Command parity (PG flavour). Conditionally compiled
 * when libpq is available. */

#ifndef BETL_RUNTIME_POSTGRES_EXEC_H
#define BETL_RUNTIME_POSTGRES_EXEC_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_postgres_exec(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
