/* Built-in `literal` expression engine — SPEC §7.
 *
 * Replicates a single user-supplied value across an output column. No
 * code execution; the engine does not look at row data. The host
 * registers it as an in-process provider via betl_register_builtins.
 */

#ifndef BETL_RUNTIME_LITERAL_EXPR_H
#define BETL_RUNTIME_LITERAL_EXPR_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the literal engine as an in-process provider. Returns
 * BETL_OK on success or the registry error otherwise. */
int betl_register_literal_engine(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
