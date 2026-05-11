/* `betl run <pipeline.yml> [--param NAME=VALUE]... [--provider <path>]...`
 *
 * 1. Parse the pipeline (and its includes).
 * 2. Spin up a registry, register the in-process built-ins, then
 *    auto-load external provider plugins from (in order):
 *      a) <exe_dir>/providers/PLUGIN/betl-PLUGIN.so   — dev build tree
 *      b) each colon-separated entry of $BETL_PROVIDER_DIR
 *      c) each `--provider <path>` on the CLI (explicit override)
 *    Each load is logged; a failure is a warning, not fatal, so a
 *    pipeline that doesn't reference a broken plugin still runs.
 * 3. Apply parameters (CLI overrides + declared defaults), then
 *    apply connections (with ${env.X}/${params.X} resolved).
 * 4. Drive betl_run with a default context (stderr logs at INFO).
 * 5. Echo any final error string captured on the context.
 */

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "cli/commands.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
#include "runtime/context.h"
#include "runtime/exec.h"
#include "runtime/parameters.h"

/* Resolve the directory the betl binary itself lives in, via
 * /proc/self/exe. Returns 0 on success and writes the dir into `buf`
 * (NUL-terminated, no trailing slash); -1 if we can't determine it. */
static int get_exe_dir(char *buf, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return -1;
    *slash = '\0';
    return 0;
}

/* Glob `pattern` and try to load each match into the registry. A load
 * failure for any one file is reported on stderr but does not abort
 * — the user's pipeline may not reference that provider at all. */
static void load_glob(BetlRegistry *reg, const char *pattern) {
    glob_t g;
    if (glob(pattern, GLOB_NOSORT, NULL, &g) != 0) {
        globfree(&g);
        return;
    }
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        if (betl_registry_load(reg, g.gl_pathv[i]) != BETL_OK) {
            fprintf(stderr, "warning: provider %s failed to load: %s\n",
                    g.gl_pathv[i], betl_registry_last_error(reg));
        }
    }
    globfree(&g);
}

/* Auto-discover plugins from the dev-tree convention path plus
 * $BETL_PROVIDER_DIR. Explicit `--provider <path>` overrides are
 * handled separately in cmd_run after this returns. */
static void autoload_providers(BetlRegistry *reg) {
    char exe_dir[4096];
    if (get_exe_dir(exe_dir, sizeof exe_dir) == 0) {
        char pat[4096 + 64];
        int n = snprintf(pat, sizeof pat, "%s/providers/*/betl-*.so", exe_dir);
        if (n > 0 && (size_t)n < sizeof pat) load_glob(reg, pat);
    }
    const char *env = getenv("BETL_PROVIDER_DIR");
    if (env && *env) {
        char buf[4096];
        size_t bl = strlen(env);
        if (bl < sizeof buf) {
            memcpy(buf, env, bl + 1);
            char *tok = buf;
            while (tok && *tok) {
                char *colon = strchr(tok, ':');
                if (colon) *colon = '\0';
                if (*tok) {
                    char pat[4096 + 64];
                    int n = snprintf(pat, sizeof pat, "%s/betl-*.so", tok);
                    if (n > 0 && (size_t)n < sizeof pat) load_glob(reg, pat);
                }
                tok = colon ? colon + 1 : NULL;
            }
        }
    }
}

int cmd_run(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: betl run <pipeline.yml> [--param NAME=VALUE]... "
            "[--provider <path>]...\n");
        return 2;
    }

    const char *path = NULL;
    /* Collect --param overrides as we go; cap at 64 — generous for v0.1. */
    char  *overrides[64];
    size_t n_overrides = 0;
    /* Explicit `--provider <path>` plugin loads, applied after the
     * auto-discovered ones. */
    const char *explicit_providers[32];
    size_t n_explicit = 0;

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
        } else if (strcmp(a, "--provider") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--provider requires a path\n");
                return 2;
            }
            if (n_explicit >= sizeof explicit_providers / sizeof explicit_providers[0]) {
                fprintf(stderr, "--provider: too many entries (max %zu)\n",
                        sizeof explicit_providers / sizeof explicit_providers[0]);
                return 2;
            }
            explicit_providers[n_explicit++] = argv[++i];
        } else if (strncmp(a, "--provider=", 11) == 0) {
            if (n_explicit >= sizeof explicit_providers / sizeof explicit_providers[0]) {
                fprintf(stderr, "--provider: too many entries (max %zu)\n",
                        sizeof explicit_providers / sizeof explicit_providers[0]);
                return 2;
            }
            explicit_providers[n_explicit++] = a + 11;
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
            "usage: betl run <pipeline.yml> [--param NAME=VALUE]... "
            "[--provider <path>]...\n");
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

    /* Pull in external provider .so files: dev tree, then env path,
     * then explicit --provider flags. */
    autoload_providers(reg);
    for (size_t i = 0; i < n_explicit; ++i) {
        if (betl_registry_load(reg, explicit_providers[i]) != BETL_OK) {
            fprintf(stderr, "warning: provider %s failed to load: %s\n",
                    explicit_providers[i], betl_registry_last_error(reg));
        }
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
