/* Glue between the pipeline parser and the runtime context.
 *
 * `betl_apply_connections` walks the connections declared on a parsed
 * BetlPipeline, resolves any `${env.NAME}` references against the
 * process environment, and stores the resulting JSON on the context
 * via betl_context_set_connection. Components later read the same
 * JSON by calling `betl_get_connection(ctx, "<name>")`.
 *
 * Layered separately from pipeline.c so the parser stays free of any
 * dependency on the runtime / context API. */

#ifndef BETL_RUNTIME_CONNECTIONS_H
#define BETL_RUNTIME_CONNECTIONS_H

#include "betl/provider.h"
#include "pipeline/pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apply every connection declared in `p` to `ctx`. Returns BETL_OK on
 * success or BETL_ERR_* on the first failure, with a description in
 * `err_buf` (which must have capacity >= 2). Failures we surface:
 *   - `${env.X}` referencing an unset env variable
 *   - betl_context_set_connection failing (OOM)
 * Calls betl_context_set_connection for each successfully-resolved
 * connection in source order. */
int betl_apply_connections(BetlContext *ctx, const BetlPipeline *p,
                           char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_CONNECTIONS_H */
