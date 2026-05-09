/* `${X}` placeholder substitution.
 *
 * Walks a string looking for `${env.NAME}` / `${params.NAME}` patterns
 * and produces a freshly-allocated string with each one replaced by
 * its resolved value. Used both for connection JSON (before storing on
 * the context) and for component config JSON (before handing to init).
 *
 * Recognized prefixes:
 *   ${env.NAME}     — getenv(NAME); missing => error
 *   ${params.NAME}  — betl_get_param(ctx, NAME); missing => error
 *
 * Other `${...}` forms (notably `${secret.X}`) pass through unchanged.
 * They will be resolved by their own layers in later versions.
 *
 * The context pointer may be NULL — in that case `${params.X}` is
 * treated as an error ("no context"). Callers that don't expose
 * parameters can use the env-only helper. */

#ifndef BETL_RUNTIME_SUBSTITUTE_H
#define BETL_RUNTIME_SUBSTITUTE_H

#include <stddef.h>

#include "betl/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate a new string containing `src` with `${env.X}` and
 * `${params.X}` references resolved. Returns NULL on error and writes
 * a description into `err_buf` (capacity >= 2). The caller frees the
 * returned string. */
char *betl_substitute_refs(const char *src,
                           BetlContext *ctx,
                           char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_SUBSTITUTE_H */
