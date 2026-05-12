/* lua.script (async transform) end-to-end test.
 *
 * argv[1] = path to betl-lua.so.
 *
 * Covers the four shapes the component is meant to enable:
 *   1. 1:1 — emit one row per input row (same as lua.map for comparison).
 *   2. fan-out — emit N output rows per input row.
 *   3. fan-in (windowed) — accumulate state, emit only every K rows.
 *   4. on_eof flush — accumulator drained at end-of-stream.
 *
 * Each pipeline pipes gen_int64 → lua.script → count_rows; the
 * `expect:` knob on count_rows fails the stage if the row count is
 * wrong, which is how every case below verifies its arithmetic.
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

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(contents);
    int rc = (fwrite(contents, 1, n, f) == n) ? 0 : -1;
    fclose(f);
    return rc;
}

static int run_yaml(const char *plugin_path,
                    const char *yaml,
                    char *last_err, size_t last_err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-lua-script-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) {
        if (last_err) snprintf(last_err, last_err_cap, "could not write temp pipeline");
        return BETL_ERR_IO;
    }
    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        if (last_err) snprintf(last_err, last_err_cap, "%s", err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (!ctx || !reg) goto cleanup;
    if (ctx) betl_context_set_min_log_level(ctx, BETL_LOG_WARN);
    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) {
        if (last_err) snprintf(last_err, last_err_cap, "%s", betl_registry_last_error(reg));
        goto cleanup;
    }
    rc = betl_registry_load(reg, plugin_path);
    if (rc != BETL_OK) {
        if (last_err) snprintf(last_err, last_err_cap, "%s", betl_registry_last_error(reg));
        goto cleanup;
    }
    rc = betl_run(ctx, reg, p);
    if (last_err) snprintf(last_err, last_err_cap, "%s", betl_context_last_error(ctx));
cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

/* --- 1:1 emit. 4 rows in, 4 rows out, doubled id. ---------------- */
static const char PL_PASS_THROUGH[] =
    "betl: 1\n"
    "name: lua-script-1to1\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "      - id: t\n"
    "        type: lua.script\n"
    "        from: source\n"
    "        output_schema:\n"
    "          - { name: doubled, type: l }\n"
    "        script: |\n"
    "          function on_row(row)\n"
    "            emit({ doubled = row.id * 2 })\n"
    "          end\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 4\n";

/* --- fan-out: each input row emits 3 output rows. 5 in → 15 out. - */
static const char PL_FAN_OUT[] =
    "betl: 1\n"
    "name: lua-script-fanout\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: t\n"
    "        type: lua.script\n"
    "        from: source\n"
    "        output_schema:\n"
    "          - { name: id,   type: l }\n"
    "          - { name: copy, type: l }\n"
    "        script: |\n"
    "          function on_row(row)\n"
    "            for k = 1, 3 do\n"
    "              emit({ id = row.id, copy = k })\n"
    "            end\n"
    "          end\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 15\n";

/* --- fan-in: emit every 4th row only. 10 in → 2 out. ------------- */
static const char PL_FAN_IN[] =
    "betl: 1\n"
    "name: lua-script-fanin\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 10\n"
    "        batch_size: 3\n"
    "      - id: t\n"
    "        type: lua.script\n"
    "        from: source\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "        script: |\n"
    "          local seen = 0\n"
    "          function on_row(row)\n"
    "            seen = seen + 1\n"
    "            if seen % 4 == 0 then\n"
    "              emit({ id = row.id })\n"
    "            end\n"
    "          end\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 2\n";

/* --- on_eof flush: buffer all input, emit summary on EOF. -------- */
static const char PL_EOF_FLUSH[] =
    "betl: 1\n"
    "name: lua-script-eof-flush\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: t\n"
    "        type: lua.script\n"
    "        from: source\n"
    "        output_schema:\n"
    "          - { name: count, type: l }\n"
    "          - { name: sum,   type: l }\n"
    "        script: |\n"
    "          local n, s = 0, 0\n"
    "          function on_row(row)\n"
    "            n = n + 1\n"
    "            s = s + row.id\n"
    "          end\n"
    "          function on_eof()\n"
    "            emit({ count = n, sum = s })\n"
    "          end\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n"
    "        expect: 1\n";

/* --- output_schema missing: compile-time error. ------------------ */
static const char PL_NO_SCHEMA[] =
    "betl: 1\n"
    "name: lua-script-no-schema\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: t\n"
    "        type: lua.script\n"
    "        from: source\n"
    "        script: |\n"
    "          function on_row(row) end\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n";

/* --- missing on_row: runtime error on init. ---------------------- */
static const char PL_NO_ON_ROW[] =
    "betl: 1\n"
    "name: lua-script-no-on-row\n"
    "pipeline:\n"
    "  - id: stage\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: t\n"
    "        type: lua.script\n"
    "        from: source\n"
    "        output_schema:\n"
    "          - { name: id, type: l }\n"
    "        script: |\n"
    "          -- no on_row defined; should fail at init.\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: t\n";

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1] :
#ifdef BETL_TEST_PLUGIN_PATH
        BETL_TEST_PLUGIN_PATH;
#else
        NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-lua.so>\n", argv[0]);
        return 2;
    }
#endif

    char err[512];
    int rc;

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_PASS_THROUGH, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "pass-through: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_FAN_OUT, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "fan-out: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_FAN_IN, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "fan-in: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_EOF_FLUSH, err, sizeof err);
    if (rc != BETL_OK) fprintf(stderr, "eof-flush: %s\n", err);
    CHECK(rc == BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_NO_SCHEMA, err, sizeof err);
    CHECK(rc != BETL_OK);

    err[0] = 0;
    rc = run_yaml(plugin_path, PL_NO_ON_ROW, err, sizeof err);
    CHECK(rc != BETL_OK);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: lua.script test passed\n");
    return 0;
}
