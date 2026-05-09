/* postgres.upsert — built-in SINK component, conditionally available
 * when libpq was found at configure time. Exposed as a separate
 * registration entry point so that builtins.c can call it under the
 * BETL_HAVE_LIBPQ guard without itself including libpq-fe.h. */

#ifndef BETL_RUNTIME_POSTGRES_UPSERT_H
#define BETL_RUNTIME_POSTGRES_UPSERT_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_postgres(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_POSTGRES_UPSERT_H */
