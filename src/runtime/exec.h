/* Pipeline executor.
 *
 * Walks a parsed BetlPipeline against a populated registry, runs every
 * stage in topological order:
 *   - TASK stages: lookup the task component, run its lifecycle.
 *   - DATAFLOW stages: topologically sort the inner steps, init each
 *     component, wire input/output Arrow streams between them, then
 *     drive every SINK to completion.
 *
 * v0.1 limitations:
 *   - single-threaded; one stage runs at a time, one batch at a time
 *   - each step has 0 or 1 input (multi-input transforms like join
 *     are accepted by the parser but rejected here)
 *   - the host's BetlContext is shared across all components in the
 *     run; there's no per-component context isolation yet */

#ifndef BETL_RUNTIME_EXEC_H
#define BETL_RUNTIME_EXEC_H

#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Execute the pipeline. Returns BETL_OK on success or the first
 * non-zero status from any component. On failure, ctx's last error
 * carries the diagnostic. The pipeline and registry are not modified. */
int betl_run(BetlContext *ctx, BetlRegistry *reg, const BetlPipeline *p);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_EXEC_H */
