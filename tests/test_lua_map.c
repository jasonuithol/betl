/* lua.map end-to-end test.
 *
 * argv[1] = path to betl-lua.so (also baked in via BETL_TEST_PLUGIN_PATH).
 *
 * Runs four small pipelines that drive gen_int64 → lua.map → count_rows
 * through the full parser+executor. Verifies:
 *   1. Happy path: per-row mutation runs, count_rows reports the right
 *      total, the captured log shows that the script saw each row.
 *   2. Script runtime error: pipeline fails, error message names the row.
 *   3. Script returns a non-table: pipeline fails with a type error.
 *   4. Setting a column to nil produces a null cell (does not crash).
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

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(contents);
    int rc = (fwrite(contents, 1, n, f) == n) ? 0 : -1;
    fclose(f);
    return rc;
}

static char *slurp(FILE *f) {
    fflush(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) return NULL;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) return NULL;
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    return buf;
}

/* Run pipeline YAML to completion. last_err filled with the final ctx
 * error message. log_capture (if non-NULL) receives all betl_log output. */
static int run_yaml(const char *plugin_path,
                    const char *yaml,
                    char *last_err, size_t last_err_cap,
                    FILE *log_capture) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-lua-map-%d.yml", (int)getpid());
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
    if (log_capture) {
        betl_context_set_log_stream(ctx, log_capture);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    }
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

/* --- Pipelines ------------------------------------------------------------- */

static const char PL_HAPPY[] =
    "betl: 1\n"
    "name: lua-map-happy\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "        column: id\n"
    "        start: 10\n"
    "      - id: bump\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          log.info('saw id=' .. row.id)\n"
    "          row.id = row.id + 1000\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: bump\n"
    "        expect: 4\n";

/* Script forgets to return → trailing wrap kicks in, mutated row used. */
static const char PL_IMPLICIT_RETURN[] =
    "betl: 1\n"
    "name: lua-map-implicit-return\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "      - id: bump\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          row.id = (row.id or 0) * 2\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: bump\n"
    "        expect: 2\n";

static const char PL_RUNTIME_ERROR[] =
    "betl: 1\n"
    "name: lua-map-runtime-error\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: explode\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          if row.id == 1 then error('boom on row 1') end\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: explode\n";

static const char PL_NON_TABLE_RETURN[] =
    "betl: 1\n"
    "name: lua-map-non-table\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: bad\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          return 42\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: bad\n";

/* nil column → null cell. count_rows doesn't validate nulls; this just
 * verifies the path doesn't crash and the count is correct. */
static const char PL_NIL_COLUMN[] =
    "betl: 1\n"
    "name: lua-map-nil-column\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: nullify\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          row.id = nil\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: nullify\n"
    "        expect: 3\n";

/* utf8 happy path: gen_strings → uppercase the names → count. The
 * script reads both id (int64) and name (utf8), logs them to confirm
 * they survived the marshal, and writes a transformed string back. */
static const char PL_UTF8_HAPPY[] =
    "betl: 1\n"
    "name: lua-map-utf8\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 3\n"
    "        prefix: ROW_\n"
    "      - id: upper\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          log.info('id=' .. row.id .. ' name=' .. row.name)\n"
    "          row.name = row.name .. '!'\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: upper\n"
    "        expect: 3\n";

/* utf8 type-mismatch: script writes a number to a utf8 column. */
static const char PL_UTF8_TYPE_MISMATCH[] =
    "betl: 1\n"
    "name: lua-map-utf8-mismatch\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 1\n"
    "      - id: bad\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          row.name = 42\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: bad\n";

/* utf8 nil cell mid-stream: row 1 nulls out the name, rows 0 and 2
 * keep it. Verifies that null offsets stay coherent. */
static const char PL_UTF8_NULL_CELL[] =
    "betl: 1\n"
    "name: lua-map-utf8-null\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 3\n"
    "      - id: nuller\n"
    "        type: lua.map\n"
    "        from: source\n"
    "        script: |\n"
    "          if row.id == 1 then row.name = nil end\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: nuller\n"
    "        expect: 3\n";

/* --- main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-lua.so>\n", argv[0]);
        return 2;
    }
#endif

    /* --- 1: happy path ------------------------------------------------- */
    {
        char err[512];
        FILE *log = tmpfile();
        int rc = run_yaml(plugin_path, PL_HAPPY, err, sizeof err, log);
        if (rc != BETL_OK) fprintf(stderr, "happy err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 4 rows") != NULL);
        char *log_text = slurp(log);
        CHECK(log_text != NULL);
        if (log_text) {
            CHECK(strstr(log_text, "saw id=10") != NULL);
            CHECK(strstr(log_text, "saw id=11") != NULL);
            CHECK(strstr(log_text, "saw id=12") != NULL);
            CHECK(strstr(log_text, "saw id=13") != NULL);
            CHECK(strstr(log_text, "count_rows: 4 rows") != NULL);
            free(log_text);
        }
        fclose(log);
    }

    /* --- 2: implicit return (no explicit `return`) -------------------- */
    {
        char err[512];
        int rc = run_yaml(plugin_path, PL_IMPLICIT_RETURN, err, sizeof err, NULL);
        if (rc != BETL_OK) fprintf(stderr, "implicit err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 2 rows") != NULL);
    }

    /* --- 3: runtime error in script ----------------------------------- */
    {
        char err[512];
        int rc = run_yaml(plugin_path, PL_RUNTIME_ERROR, err, sizeof err, NULL);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "boom on row 1") != NULL);
    }

    /* --- 4: script returns a non-table -------------------------------- */
    {
        char err[512];
        int rc = run_yaml(plugin_path, PL_NON_TABLE_RETURN, err, sizeof err, NULL);
        CHECK(rc != BETL_OK);
        /* Error mentions either "table" or the wrong type ("number"). */
        CHECK(strstr(err, "table") != NULL || strstr(err, "number") != NULL);
    }

    /* --- 5: row.id = nil → null cell, count is preserved -------------- */
    {
        char err[512];
        int rc = run_yaml(plugin_path, PL_NIL_COLUMN, err, sizeof err, NULL);
        if (rc != BETL_OK) fprintf(stderr, "nil-col err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 3 rows") != NULL);
    }

    /* --- 6: utf8 round-trip, log shows id+name marshaled in ----------- */
    {
        char err[512];
        FILE *log = tmpfile();
        int rc = run_yaml(plugin_path, PL_UTF8_HAPPY, err, sizeof err, log);
        if (rc != BETL_OK) fprintf(stderr, "utf8 happy err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 3 rows") != NULL);
        char *log_text = slurp(log);
        if (log_text) {
            CHECK(strstr(log_text, "id=0 name=ROW_0") != NULL);
            CHECK(strstr(log_text, "id=1 name=ROW_1") != NULL);
            CHECK(strstr(log_text, "id=2 name=ROW_2") != NULL);
            free(log_text);
        }
        fclose(log);
    }

    /* --- 7: writing a number to a utf8 column is a type error --------- */
    {
        char err[512];
        int rc = run_yaml(plugin_path, PL_UTF8_TYPE_MISMATCH, err, sizeof err, NULL);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "string") != NULL);
    }

    /* --- 8: utf8 null cell in the middle of the batch ---------------- */
    {
        char err[512];
        int rc = run_yaml(plugin_path, PL_UTF8_NULL_CELL, err, sizeof err, NULL);
        if (rc != BETL_OK) fprintf(stderr, "utf8 null err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 3 rows") != NULL);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("lua_map: all checks passed\n");
    return 0;
}
