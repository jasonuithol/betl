/* Built-in providers compiled into the engine binary. These are
 * registered against a registry without going through dlopen.
 *
 * Two are shipped as part of v0.1, both for testing the executor:
 *
 *   betl.gen_int64    — SOURCE; emits N rows of (id int64, ...).
 *   betl.count_rows   — SINK;   pulls all batches, counts rows,
 *                       optionally fails if count != configured expect.
 *
 * Real first-class providers (csv.read, postgres.upsert, lua.map) live
 * outside this set; they have their own dependencies and tests. */

#ifndef BETL_RUNTIME_BUILTINS_H
#define BETL_RUNTIME_BUILTINS_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register every builtin provider into `r`. Returns BETL_OK on success
 * or the first error from betl_registry_register. */
int betl_register_builtins(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_BUILTINS_H */
