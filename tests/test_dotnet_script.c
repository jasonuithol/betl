/* dotnet.script end-to-end test. v0.2 phase 4 of the SSIS
 * async-Script-Component replacement.
 *
 * argv[1] = path to betl-dotnet.so.
 *
 * Strategy: drive gen_int64 → dotnet.script (typed InputRow/OutputRow
 * accessing row.id) → count_rows with `expect:` knob doing the
 * verification. SKIPs (rc=77) when the .NET SDK / linker toolchain
 * isn't reachable.
 *
 * Patterns covered:
 *   1. 1:1 mapping — emit one output row per input row, double the id.
 *   2. Fan-out — emit N output rows per input row.
 *   3. Fan-in — accumulate, emit summary at on_eof.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/exec.h"

#define SKIP_RC 77
static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static int sdk_available(void) {
    const char *paths[] = {
        "deps/dotnet/dotnet",
        "/workspace/betl/deps/dotnet/dotnet",
        "/opt/projects/betl/deps/dotnet/dotnet",
        NULL
    };
    for (int i = 0; paths[i]; ++i)
        if (access(paths[i], X_OK) == 0) return 1;
    return 0;
}
static int prog_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":")) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", tok, name);
        if (access(p, X_OK) == 0) { found = 1; break; }
    }
    free(copy);
    return found;
}
static int has_aot_link_toolchain(void) {
    if (!prog_on_path("clang") && !prog_on_path("gcc")) return 0;
    const char *libz[] = {
        "/usr/lib/x86_64-linux-gnu/libz.so",
        "/usr/lib/x86_64-linux-gnu/libz.a",
        "/usr/lib/libz.so",
        NULL
    };
    for (int i = 0; libz[i]; ++i)
        if (access(libz[i], R_OK) == 0) return 1;
    return 0;
}

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(contents);
    int rc = (fwrite(contents, 1, n, f) == n) ? 0 : -1;
    fclose(f);
    return rc;
}

static int run_yaml(const char *plugin_path, const char *yaml,
                    char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-dotnet-script-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) {
        if (err) snprintf(err, err_cap, "write temp yaml failed");
        return BETL_ERR_IO;
    }
    char load_err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, load_err, sizeof load_err);
    if (!p) {
        if (err) snprintf(err, err_cap, "%s", load_err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (!ctx || !reg) goto cleanup;
    betl_context_set_min_log_level(ctx, BETL_LOG_WARN);
    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) {
        if (err) snprintf(err, err_cap, "%s", betl_registry_last_error(reg));
        goto cleanup;
    }
    rc = betl_registry_load(reg, plugin_path);
    if (rc != BETL_OK) {
        if (err) snprintf(err, err_cap, "%s", betl_registry_last_error(reg));
        goto cleanup;
    }
    rc = betl_run(ctx, reg, p);
    if (err) snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

/* gen_int64 emits a single int64 column named "id" (0..N-1). The
 * script's UserScript reads InputRow.id, doubles it, emits an
 * OutputRow with id (echo) + doubled. count_rows asserts the expected
 * row count. */

/* --- 1:1 — 4 in, 4 out, doubled --------------------------------- */
static const char PL_ONE_TO_ONE[] =
    "betl: 1\n"
    "name: dotnet-script-1to1\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "      - id: t\n"
    "        type: dotnet.script\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "          - { name: doubled, type: l }\n"
    "        source: |\n"
    "          public class UserScript : Betl.BetlScript {\n"
    "            public override void OnRow(Betl.InputRow row) {\n"
    "              Emit(new Betl.OutputRow {\n"
    "                id = row.id, doubled = (row.id ?? 0) * 2\n"
    "              });\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 4\n";

/* --- fan-out — 3 input rows × 2 emitted each = 6 -----------------*/
static const char PL_FAN_OUT[] =
    "betl: 1\n"
    "name: dotnet-script-fanout\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: t\n"
    "        type: dotnet.script\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: out_id, type: l }\n"
    "        source: |\n"
    "          public class UserScript : Betl.BetlScript {\n"
    "            public override void OnRow(Betl.InputRow row) {\n"
    "              Emit(new Betl.OutputRow { out_id = row.id });\n"
    "              Emit(new Betl.OutputRow { out_id = (row.id ?? 0) * 10 });\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 6\n";

/* --- fan-in via on_eof — 5 rows in, 1 row out ------------------- */
static const char PL_FAN_IN[] =
    "betl: 1\n"
    "name: dotnet-script-fanin\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: t\n"
    "        type: dotnet.script\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: total, type: l }\n"
    "        source: |\n"
    "          public class UserScript : Betl.BetlScript {\n"
    "            long sum = 0;\n"
    "            public override void OnRow(Betl.InputRow row) {\n"
    "              sum += row.id ?? 0;\n"
    "            }\n"
    "            public override void OnEof() {\n"
    "              Emit(new Betl.OutputRow { total = sum });\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 1\n";

int main(int argc, char **argv) {
    if (!sdk_available()) {
        fprintf(stderr, "[skip] .NET SDK not installed\n"); return SKIP_RC;
    }
    if (!has_aot_link_toolchain()) {
        fprintf(stderr, "[skip] NativeAOT toolchain unavailable\n"); return SKIP_RC;
    }
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

    char err[2048];
    int  rc;

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_ONE_TO_ONE, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "1:1: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_FAN_OUT, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "fan-out: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_FAN_IN, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "fan-in: %s\n", err);
    CHECK(rc == BETL_OK);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: dotnet.script test passed\n");
    return 0;
}
