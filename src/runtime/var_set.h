/* var.set — control-flow TASK that writes a value to the runtime's
 * `${vars.NAME}` kv-store. Two modes:
 *
 *   value:                — set the literal text (after ${...} subst)
 *   connection: + sql:    — run a SELECT, take the first column of the
 *                           first row, store its text. NULL → unset.
 *
 * SSIS Variable analogue (the `SET @[User::var]` pattern). Required to
 * make foreach iteration over runtime-discovered state actually useful. */

#ifndef BETL_RUNTIME_VAR_SET_H
#define BETL_RUNTIME_VAR_SET_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_var_set(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
