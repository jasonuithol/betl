/* Standard data-flow transforms (SPEC §4.3): `filter`, `map`.
 *
 * These are first-party components that compile expressions through
 * the betl_get_expr_engine() / BetlExprEngine ABI from §7. Registered
 * in-process by betl_register_builtins.
 */

#ifndef BETL_RUNTIME_TRANSFORMS_H
#define BETL_RUNTIME_TRANSFORMS_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register filter + map as in-process providers. Returns BETL_OK on
 * success or the registry error otherwise. */
int betl_register_transforms(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
