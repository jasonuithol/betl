/* postgres-backed `lookup` TRANSFORM (SPEC §4.3).
 *
 * Conditionally compiled when libpq is available — same gate as
 * postgres.upsert. Registered as its own provider so builtins.c can
 * pull it in alongside the upsert sink.
 */

#ifndef BETL_RUNTIME_POSTGRES_LOOKUP_H
#define BETL_RUNTIME_POSTGRES_LOOKUP_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_postgres_lookup(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
