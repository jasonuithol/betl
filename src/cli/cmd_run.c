/* `betl run <pipeline.yml> [--param NAME=VALUE]...`
 *
 * 1. Parse the pipeline (and its includes).
 * 2. Spin up a registry, register the in-process built-ins. Future:
 *    discover and dlopen external providers from --provider-dir.
 * 3. Apply parameters (CLI overrides + declared defaults), then
 *    apply connections (with ${env.X}/${params.X} resolved).
 * 4. Drive betl_run with a default context (stderr logs at INFO).
 * 5. Echo any final error string captured on the context.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "cli/commands.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
#include "runtime/context.h"
#include "runtime/exec.h"
#include "runtime/parameters.h"

int cmd_run(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: betl run <pipeline.yml> [--param NAME=VALUE]...\n");
        return 2;
    }

    const char *path = NULL;
    /* Collect --param overrides as we go; cap at 64 — generous for v0.1. */
    char  *overrides[64];
    size_t n_overrides = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--param") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--param requires NAME=VALUE\n");
                return 2;
            }
            if (n_overrides >= sizeof overrides / sizeof overrides[0]) {
                fprintf(stderr, "--param: too many overrides\n");
                return 2;
            }
            overrides[n_overrides++] = argv[++i];
        } else if (strncmp(a, "--param=", 8) == 0) {
            if (n_overrides >= sizeof overrides / sizeof overrides[0]) {
                fprintf(stderr, "--param: too many overrides\n");
                return 2;
            }
            overrides[n_overrides++] = (char *)(a + 8);
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        } else if (!path) {
            path = a;
        } else {
            fprintf(stderr, "unexpected positional argument: %s\n", a);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr,
            "usage: betl run <pipeline.yml> [--param NAME=VALUE]...\n");
        return 2;
    }

    char err[1024];
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_OK;
    if (!ctx || !reg) {
        fprintf(stderr, "out of memory\n");
        rc = 1;
        goto cleanup;
    }
    betl_context_set_log_stream(ctx, stderr);
    betl_context_set_min_log_level(ctx, BETL_LOG_INFO);

    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) {
        fprintf(stderr, "register builtins failed: %s\n",
                betl_registry_last_error(reg));
        rc = 1;
        goto cleanup;
    }

    char param_err[512];
    rc = betl_apply_parameters(ctx, p, overrides, n_overrides,
                               param_err, sizeof param_err);
    if (rc != BETL_OK) {
        fprintf(stderr, "%s\n", param_err);
        rc = 1;
        goto cleanup;
    }

    char conn_err[512];
    rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rc != BETL_OK) {
        fprintf(stderr, "%s\n", conn_err);
        rc = 1;
        goto cleanup;
    }

    rc = betl_run(ctx, reg, p);
    if (rc != BETL_OK) {
        const char *last = betl_context_last_error(ctx);
        fprintf(stderr, "run failed: rc=%d %s\n",
                rc, (last && *last) ? last : "");
        rc = 1;
    } else {
        fprintf(stderr, "run ok\n");
        rc = 0;
    }

cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    return rc;
}
