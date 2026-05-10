/* postgres.read — SOURCE that runs a SELECT against Postgres via libpq
 * and emits the result set as Arrow batches. Conditionally compiled
 * when libpq is available. */

#ifndef BETL_RUNTIME_POSTGRES_READ_H
#define BETL_RUNTIME_POSTGRES_READ_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_postgres_read(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
