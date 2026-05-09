/* Provider loader / component registry.
 *
 * The registry owns dlopen handles for every loaded provider and a flat
 * (component_name -> definition) lookup table. Component names are
 * global; loading a provider whose component name collides with one
 * already registered is an error.
 */

#ifndef BETL_LOADER_REGISTRY_H
#define BETL_LOADER_REGISTRY_H

#include <stddef.h>

#include "betl/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BetlRegistry BetlRegistry;

/* Allocate an empty registry. Returns NULL on OOM. */
BetlRegistry *betl_registry_create(void);

/* Tear everything down: call each provider's `provider_shutdown` (if set),
 * dlclose every handle, free internal storage. Safe to pass NULL. */
void betl_registry_destroy(BetlRegistry *r);

/* Load a provider shared library and register its components.
 *
 * Returns BETL_OK on success, BetlStatus on failure. On failure the
 * reason is available via `betl_registry_last_error`. The registry's
 * state is left unchanged on failure (no partial registration). */
int betl_registry_load(BetlRegistry *r, const char *path);

/* Register an in-process provider (no dlopen). `prov` must remain
 * valid for the lifetime of the registry. `label` is a short string
 * used in diagnostics in lieu of a file path, e.g. "<builtin>".
 *
 * Same validation rules as betl_registry_load: ABI version match,
 * non-NULL name/version/license, no component-name collisions. */
int betl_registry_register(BetlRegistry *r,
                           const BetlProvider *prov,
                           const char *label);

/* Most recent error message from a failed call on this registry.
 * Returns "" if there has been no error. The string is owned by the
 * registry and remains valid until the next call that may set an error. */
const char *betl_registry_last_error(const BetlRegistry *r);

/* Find a component definition by global name (e.g. "csv.read").
 * Returns NULL if not registered. */
const BetlComponentDef *betl_registry_find(const BetlRegistry *r,
                                           const char *component_name);

/* Find an expression engine by language tag (e.g. "lua", "python"). The
 * tag is matched verbatim. Returns NULL if no loaded provider claims it. */
const BetlExprEngine *betl_registry_find_expr(const BetlRegistry *r,
                                              const char *lang);

/* Number of providers currently loaded. */
size_t betl_registry_provider_count(const BetlRegistry *r);

/* Number of components currently registered (sum across providers). */
size_t betl_registry_component_count(const BetlRegistry *r);

/* Number of expression engines currently registered (one per provider
 * that advertises one). */
size_t betl_registry_expr_count(const BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif /* BETL_LOADER_REGISTRY_H */
