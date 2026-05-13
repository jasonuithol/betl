/* dotnet.pipelinecomponent C-side alloc/init/destroy paths.
 *
 * Exercises the parts of the provider that run BEFORE the first
 * Arrow batch is pulled — config JSON parsing, schema setup, error
 * stream allocation, and the matching destroy path. The dotnet
 * compile + dlopen only fires on the first call to get_next, so
 * this test stays C-only and runs in any environment with the
 * plugin binary, regardless of whether the .NET SDK is present.
 *
 * Value: catches malloc/free mismatches in the new fields added
 * across Phase 1a–Phase 1b.4 (DsCol.tz / DsCol.scale / err_staging
 * / err_pending_set / per-port handles) without needing valgrind
 * to step through .NET code.
 *
 * Run under valgrind via `analyze(tool="valgrind", scope="dotnet_pc_alloc")`. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/context.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

/* Drive init + destroy for a single component config. Returns
 * BETL_OK or the error code from init. The destroy is always run
 * for any non-NULL state. */
static int init_destroy(const BetlComponentDef *def, BetlContext *ctx,
                        const char *cfg) {
    void *state = NULL;
    int rc = def->init(ctx, cfg, &state);
    if (state) def->destroy(state);
    return rc;
}

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1] :
#ifdef BETL_TEST_PLUGIN_PATH
        BETL_TEST_PLUGIN_PATH;
#else
        NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-dotnet.so>\n", argv[0]);
        return 2;
    }
#endif

    /* dlopen + find the dotnet.pipelinecomponent component def. We
     * use the registry to load (same path as betl_run uses) so the
     * provider's ABI handshake runs through its proper checks. */
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);

    int rc = betl_registry_load(reg, plugin_path);
    if (rc != BETL_OK) {
        fprintf(stderr, "registry_load: %s\n", betl_registry_last_error(reg));
        betl_registry_destroy(reg);
        betl_context_destroy(ctx);
        return 1;
    }
    const BetlComponentDef *def =
        betl_registry_find(reg, "dotnet.pipelinecomponent");
    CHECK(def != NULL);
    if (!def) {
        betl_registry_destroy(reg);
        betl_context_destroy(ctx);
        return 1;
    }

    /* Basic int64 output: smallest viable config. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\","
        "\"output_schema\":[{\"name\":\"id\",\"type\":\"l\"}]}");
    CHECK(rc == BETL_OK);

    /* Decimal column with scale: exercises DsCol.scale parsing. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\","
        "\"output_schema\":["
          "{\"name\":\"id\",\"type\":\"l\"},"
          "{\"name\":\"amount\",\"type\":\"E\",\"scale\":4}]}");
    CHECK(rc == BETL_OK);

    /* Timestamp with timezone: exercises DsCol.tz strdup + free pair. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\","
        "\"output_schema\":["
          "{\"name\":\"id\",\"type\":\"l\"},"
          "{\"name\":\"when\",\"type\":\"T\",\"tz\":\"UTC\"}]}");
    CHECK(rc == BETL_OK);

    /* error_output: true — exercises error_output_enabled flag.
     * (err_staging itself is allocated lazily in compile_and_load
     * so init/destroy alone don't exercise it; this still validates
     * the config parse path.) */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\","
        "\"error_output\":true,"
        "\"output_schema\":[{\"name\":\"id\",\"type\":\"l\"}]}");
    CHECK(rc == BETL_OK);

    /* async: true — exercises the async flag. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\",\"async\":true,"
        "\"output_schema\":[{\"name\":\"id\",\"type\":\"l\"}]}");
    CHECK(rc == BETL_OK);

    /* Many columns of all stored types: hits every alloc branch in
     * parse_output_schema's strdup-name path. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\","
        "\"output_schema\":["
          "{\"name\":\"a\",\"type\":\"l\"},"
          "{\"name\":\"b\",\"type\":\"g\"},"
          "{\"name\":\"c\",\"type\":\"b\"},"
          "{\"name\":\"d\",\"type\":\"u\"},"
          "{\"name\":\"e\",\"type\":\"i\"},"
          "{\"name\":\"f\",\"type\":\"s\"},"
          "{\"name\":\"g\",\"type\":\"c\"},"
          "{\"name\":\"h\",\"type\":\"C\"},"
          "{\"name\":\"i\",\"type\":\"S\"},"
          "{\"name\":\"j\",\"type\":\"I\"},"
          "{\"name\":\"k\",\"type\":\"L\"},"
          "{\"name\":\"m\",\"type\":\"D\"},"
          "{\"name\":\"n\",\"type\":\"M\"},"
          "{\"name\":\"o\",\"type\":\"z\"},"
          "{\"name\":\"p\",\"type\":\"G\"}]}");
    CHECK(rc == BETL_OK);

    /* Error path: missing 'source' field — init must fail and not
     * leak partial state. */
    rc = init_destroy(def, ctx,
        "{\"output_schema\":[{\"name\":\"id\",\"type\":\"l\"}]}");
    CHECK(rc != BETL_OK);

    /* Error path: malformed type — fails after partial output_schema
     * parse. Verifies cleanup of partially-populated out_cols. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"csharp\","
        "\"output_schema\":["
          "{\"name\":\"good\",\"type\":\"l\"},"
          "{\"name\":\"bad\",\"type\":\"???\"}]}");
    CHECK(rc != BETL_OK);

    /* Error path: unsupported lang. */
    rc = init_destroy(def, ctx,
        "{\"source\":\"x\",\"lang\":\"fortran\","
        "\"output_schema\":[{\"name\":\"id\",\"type\":\"l\"}]}");
    CHECK(rc != BETL_OK);

    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: dotnet.pipelinecomponent alloc test passed\n");
    return 0;
}
