/* Apply declared parameters to a runtime context.
 *
 * For each `parameters:` entry in the parsed pipeline:
 *   1. If the user passed `--param NAME=VALUE` on the CLI, use that value.
 *   2. Otherwise, if a default is declared, use that. Sentinel defaults
 *      (`today`, `now`) are resolved against the wall clock at apply time.
 *   3. Otherwise, if `required: true`, surface an error.
 *   4. Otherwise leave the parameter unset (referencing it via
 *      `${params.X}` will fail at substitution time).
 *
 * `cli_overrides` is a NULL-terminated-style array of "NAME=VALUE"
 * strings (n_overrides counts how many to read). Entries naming
 * parameters that aren't declared are treated as a hard error to catch
 * typos; this matches the spec's "fail loud" principle. */

#ifndef BETL_RUNTIME_PARAMETERS_H
#define BETL_RUNTIME_PARAMETERS_H

#include "betl/provider.h"
#include "pipeline/pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_apply_parameters(BetlContext *ctx, const BetlPipeline *p,
                          char **cli_overrides, size_t n_overrides,
                          char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_PARAMETERS_H */
