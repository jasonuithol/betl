/* postgres.copy — SINK that bulk-loads via libpq's binary COPY FROM
 * STDIN protocol. Insert-only (no MERGE / ON CONFLICT). Conditionally
 * compiled when libpq is available. */

#ifndef BETL_RUNTIME_POSTGRES_COPY_H
#define BETL_RUNTIME_POSTGRES_COPY_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_postgres_copy(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
