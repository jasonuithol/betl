/* Host-side BetlContext API.
 *
 * The struct is opaque to plugins (they only see `BetlContext *`). The
 * host owns it, populates parameters and connections, sets logging
 * options, and hands the same pointer to every component's `init`. The
 * five `betl_*` extern functions declared in betl/provider.h resolve
 * back into the host binary at dlopen time — see the ENABLE_EXPORTS
 * setting in CMakeLists.txt.
 */

#ifndef BETL_RUNTIME_CONTEXT_H
#define BETL_RUNTIME_CONTEXT_H

#include <stdio.h>

#include "betl/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

BetlContext *betl_context_create(void);
void         betl_context_destroy(BetlContext *ctx);

/* Drop log messages below `level`. Default is BETL_LOG_INFO. */
void betl_context_set_min_log_level(BetlContext *ctx, BetlLogLevel level);

/* Stream log lines are written to. Default is stderr. Passing NULL
 * is a no-op. The host retains ownership of the stream. */
void betl_context_set_log_stream(BetlContext *ctx, FILE *stream);

/* Tag prefix on each log line (e.g. component instance name). May be
 * NULL to clear. The string is copied. */
void betl_context_set_log_tag(BetlContext *ctx, const char *tag);

void betl_context_request_cancel(BetlContext *ctx);
void betl_context_clear_cancel(BetlContext *ctx);

/* Insert or overwrite a parameter / connection. Returns BETL_OK on
 * success, BETL_ERR_INVALID on NULL inputs, BETL_ERR_INTERNAL on OOM.
 * Strings are copied into context-owned storage. */
int betl_context_set_param(BetlContext *ctx,
                           const char *key, const char *value);
int betl_context_set_connection(BetlContext *ctx,
                                const char *name, const char *json);

/* Iteration variables (mutable, mid-run). The executor pushes one on
 * foreach entry, restores the prior binding on exit. `${vars.NAME}`
 * substitution resolves through this kv-store. unset_var returns
 * BETL_ERR_NOT_FOUND if the name isn't bound. */
int         betl_context_set_var  (BetlContext *ctx,
                                   const char *name, const char *value);
int         betl_context_unset_var(BetlContext *ctx, const char *name);
const char *betl_context_get_var  (BetlContext *ctx, const char *name);

/* Stash a registry pointer so components reachable through this context
 * can find expression engines via betl_get_expr_engine. The runtime sets
 * this once at run start (see betl_run in exec.c). The context does not
 * own the registry. */
struct BetlRegistry;
void  betl_context_set_registry(BetlContext *ctx, struct BetlRegistry *reg);

/* Most recent string written by a component via betl_set_error.
 * Returns "" if none. Pointer valid until the next betl_set_error
 * call on this context. */
const char *betl_context_last_error(const BetlContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_CONTEXT_H */
