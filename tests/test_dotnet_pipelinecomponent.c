/* dotnet.pipelinecomponent end-to-end test. Phase 1a — sync
 * transform, single column space = output_schema, types l/g/b/u.
 *
 * argv[1] = path to betl-dotnet.so.
 *
 * Strategy: drive gen_int64 → dotnet.pipelinecomponent → count_rows
 * with `expect:` knob doing the verification. SKIPs (rc=77) when
 * the .NET SDK / linker toolchain isn't reachable.
 *
 * Patterns covered:
 *   1. Hardcoded column index — double the id in place.
 *   2. SSIS-idiomatic PreExecute + BufferManager.FindColumnByLineageID
 *      lookup — proves the metadata + buffer-manager wiring works. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
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
    snprintf(path, sizeof path, "/tmp/betl-test-dotnet-pc-%d.yml", (int)getpid());
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
    char conn_err[1024] = {0};
    rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rc != BETL_OK) {
        if (err) snprintf(err, err_cap, "apply_connections: %s", conn_err);
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

/* gen_int64 emits a single int64 column named "id" (0..N-1).
 * UserComponent's output_schema has [id, doubled]; "id" pairs with
 * the same-named input and starts populated; "doubled" is added
 * (initial NULL) and the user fills it in ProcessInput. */

/* --- hardcoded column indices ----------------------------------- */
static const char PL_HARDCODED[] =
    "betl: 1\n"
    "name: dotnet-pc-hardcoded\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "          - { name: doubled, type: l }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                buffer.SetInt64(1, buffer.GetInt64(0) * 2);\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 4\n";

/* --- Phase 1b: narrow ints + float32 ---------------------------- */
static const char PL_NARROW[] =
    "betl: 1\n"
    "name: dotnet-pc-narrow\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: id,    type: l }\n"
    "          - { name: as_i8, type: c }\n"
    "          - { name: as_i4, type: i }\n"
    "          - { name: as_u4, type: I }\n"
    "          - { name: as_r4, type: f }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                long id = buffer.GetInt64(0);\n"
    "                buffer.SetInt32(1, (int)id);\n"     /* widens through SetInt64 */
    "                buffer.SetInt32(2, (int)id);\n"
    "                buffer.SetInt32(3, (int)id);\n"
    "                buffer.SetSingle(4, (float)id);\n"  /* widens through SetDouble */
    "              }\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 3\n";

/* --- SSIS-idiomatic: PreExecute + FindColumnByLineageID lookup -- */
static const char PL_LINEAGE_LOOKUP[] =
    "betl: 1\n"
    "name: dotnet-pc-lineage\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "          - { name: doubled, type: l }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          using Microsoft.SqlServer.Dts.Pipeline.Wrapper;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            int idIdx = -1, doubledIdx = -1;\n"
    "            public override void PreExecute() {\n"
    "              base.PreExecute();\n"
    "              var input = ComponentMetaData.InputCollection[(object)0];\n"
    "              foreach (IDTSInputColumn100 c in input.InputColumnCollection) {\n"
    "                if (c.Name == \"id\")\n"
    "                  idIdx = BufferManager.FindColumnByLineageID(input.Buffer, c.LineageID);\n"
    "              }\n"
    "              var output = ComponentMetaData.OutputCollection[(object)0];\n"
    "              foreach (IDTSOutputColumn100 c in output.OutputColumnCollection) {\n"
    "                if (c.Name == \"doubled\")\n"
    "                  doubledIdx = BufferManager.FindColumnByLineageID(input.Buffer, c.LineageID);\n"
    "              }\n"
    "            }\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                buffer.SetInt64(doubledIdx, buffer.GetInt64(idIdx) * 2);\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 5\n";

/* --- Phase 1c: async filter (N input rows → M output rows) ----- */
static const char PL_ASYNC_FILTER[] =
    "betl: 1\n"
    "name: dotnet-pc-async-filter\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 6\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        async: true\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            PipelineBuffer? outBuf;\n"
    "            public override void PrimeOutput(int outputs, int[] outputIDs, PipelineBuffer[] buffers) {\n"
    "              outBuf = buffers[0];\n"
    "            }\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                long id = buffer.GetInt64(0);\n"
    "                if ((id & 1) == 0) {\n"
    "                  outBuf!.AddRow();\n"
    "                  outBuf.SetInt64(0, id);\n"
    "                }\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 3\n";

/* --- Phase 1c: async aggregator (N rows → 1 summary at PostExecute) -- */
static const char PL_ASYNC_AGGREGATE[] =
    "betl: 1\n"
    "name: dotnet-pc-async-aggregate\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        async: true\n"
    "        output_schema:\n"
    "          - { name: total, type: l }\n"
    "          - { name: count, type: l }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            PipelineBuffer? outBuf;\n"
    "            long sum = 0; long count = 0;\n"
    "            public override void PrimeOutput(int outputs, int[] outputIDs, PipelineBuffer[] buffers) {\n"
    "              outBuf = buffers[0];\n"
    "            }\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                sum += buffer.GetInt64(0);\n"
    "                count++;\n"
    "              }\n"
    "            }\n"
    "            public override void PostExecute() {\n"
    "              outBuf!.AddRow();\n"
    "              outBuf.SetInt64(0, sum);\n"
    "              outBuf.SetInt64(1, count);\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 1\n";

/* --- Phase 2: Connection Manager lookup via RuntimeConnectionCollection */
static const char PL_CONN_MGR[] =
    "betl: 1\n"
    "name: dotnet-pc-conn-mgr\n"
    "connections:\n"
    "  warehouse:\n"
    "    type: postgres\n"
    "    dsn: \"host=localhost dbname=test_betl_warehouse\"\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            public override void PreExecute() {\n"
    "              var cm = ComponentMetaData.RuntimeConnectionCollection[(object)\"warehouse\"];\n"
    "              var json = (string)cm.AcquireConnection(null!);\n"
    "              if (!json.Contains(\"dbname=test_betl_warehouse\"))\n"
    "                throw new System.Exception(\"unexpected CN JSON: \" + json);\n"
    "            }\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) { /* passthrough */ }\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 2\n";

/* --- Phase 2: error-row routing via DirectErrorRow + error_out port */
static const char PL_ERROR_ROUTE[] =
    "betl: 1\n"
    "name: dotnet-pc-error-route\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 6\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        error_output: true\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                long id = buffer.GetInt64(0);\n"
    "                if ((id & 1) == 1) buffer.DirectErrorRow(0, 42, 0);\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "      - id: main_sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 3\n"
    "      - id: err_sink\n"
    "        type: betl.count_rows\n"
    "        from: t:error_out\n"
    "        expect: 3\n";

/* --- Phase 1b.2: temporal + binary types (date32 / timestamp_us /
 *                 time_us / binary). Source emits int64; the
 *                 component writes derived date/timestamp/time/binary
 *                 cells using SSIS-style accessors. -------------- */
static const char PL_TEMPORAL_BINARY[] =
    "betl: 1\n"
    "name: dotnet-pc-temporal-binary\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: t\n"
    "        type: dotnet.pipelinecomponent\n"
    "        from: source\n"
    "        lang: csharp\n"
    "        output_schema:\n"
    "          - { name: id,       type: l }\n"
    "          - { name: day,      type: D }\n"
    "          - { name: when_us,  type: T }\n"
    "          - { name: time_us,  type: M }\n"
    "          - { name: payload,  type: z }\n"
    "        source: |\n"
    "          using Microsoft.SqlServer.Dts.Pipeline;\n"
    "          namespace Betl;\n"
    "          public class UserComponent : PipelineComponent {\n"
    "            public override void ProcessInput(int inputID, PipelineBuffer buffer) {\n"
    "              while (buffer.NextRow()) {\n"
    "                long id = buffer.GetInt64(0);\n"
    "                /* day = id-th day after epoch via SetDate sugar */\n"
    "                buffer.SetDate(1, System.DateTime.UnixEpoch.AddDays(id));\n"
    "                /* timestamp_us: id microseconds after epoch */\n"
    "                buffer.SetInt64(2, id);\n"
    "                /* time_us: id microseconds since midnight */\n"
    "                buffer.SetInt64(3, id);\n"
    "                /* payload: 4 bytes encoding id */\n"
    "                buffer.SetBytes(4, System.BitConverter.GetBytes((int)id));\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 3\n";

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

    char err[4096];
    int  rc;

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_HARDCODED, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "hardcoded: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_LINEAGE_LOOKUP, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "lineage-lookup: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_NARROW, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "narrow: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_ASYNC_FILTER, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "async-filter: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_ASYNC_AGGREGATE, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "async-aggregate: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_CONN_MGR, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "conn-mgr: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_ERROR_ROUTE, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "error-route: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_TEMPORAL_BINARY, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "temporal-binary: %s\n", err);
    CHECK(rc == BETL_OK);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: dotnet.pipelinecomponent test passed\n");
    return 0;
}
