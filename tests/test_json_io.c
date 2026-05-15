/* json.read / json.write end-to-end round-trip.
 *
 * Three pipelines, each writing then re-reading:
 *   1. NDJSON: gen_int64 → json.write (ndjson) → json.read → count_rows
 *   2. Array : gen_int64 → json.write (array)  → json.read → count_rows
 *   3. Types : a small literal JSON file with strings/numbers/bool/null
 *              is read and counted; verifies json.read NULL-handling.
 *
 * No DB, no network. count_rows fails the pipeline if the row count
 * doesn't match the declared expect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/exec.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(contents);
    int rc = fwrite(contents, 1, len, f) == len ? 0 : -1;
    fclose(f);
    return rc;
}

static int file_exists(const char *path) {
    struct stat st; return stat(path, &st) == 0;
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int run_yaml(const char *yaml, char *err_out, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-json-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) return BETL_ERR_IO;
    char perr[512] = {0};
    BetlPipeline *p = betl_pipeline_load(path, perr, sizeof perr);
    if (!p) {
        if (err_out) snprintf(err_out, err_cap, "pipeline_load: %s", perr);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK && err_out) {
            snprintf(err_out, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

int main(void) {
    char ndjson_path[64], array_path[64], typed_path[64];
    snprintf(ndjson_path, sizeof ndjson_path,
             "/tmp/betl-json-nd-%d.json", (int)getpid());
    snprintf(array_path, sizeof array_path,
             "/tmp/betl-json-arr-%d.json", (int)getpid());
    snprintf(typed_path, sizeof typed_path,
             "/tmp/betl-json-typed-%d.json", (int)getpid());

    /* --- 1: NDJSON round-trip ---------------------------------- */
    {
        unlink(ndjson_path);
        char yaml[2048];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: nd-write\n"
            "pipeline:\n"
            "  - id: write_stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: betl.gen_int64\n"
            "        row_count: 25\n"
            "      - id: sink\n"
            "        type: json.write\n"
            "        from: source\n"
            "        path: \"%s\"\n"
            "        format: ndjson\n",
            ndjson_path);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 1 write: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(file_exists(ndjson_path));
        long sz = file_size(ndjson_path);
        CHECK(sz > 0);

        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: nd-read\n"
            "pipeline:\n"
            "  - id: read_stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: json.read\n"
            "        path: \"%s\"\n"
            "        format: ndjson\n"
            "        columns: [id]\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: source\n"
            "        expect: 25\n",
            ndjson_path);
        memset(err, 0, sizeof err);
        rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 1 read: %s\n", err);
        CHECK(rc == BETL_OK);
        unlink(ndjson_path);
    }

    /* --- 2: Array round-trip ----------------------------------- */
    {
        unlink(array_path);
        char yaml[2048];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: arr-write\n"
            "pipeline:\n"
            "  - id: write_stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: betl.gen_int64\n"
            "        row_count: 7\n"
            "      - id: sink\n"
            "        type: json.write\n"
            "        from: source\n"
            "        path: \"%s\"\n"
            "        format: array\n",
            array_path);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 2 write: %s\n", err);
        CHECK(rc == BETL_OK);

        /* The written file must be a single JSON array (starts with [
         * and ends with ]). Quick visual check. */
        FILE *f = fopen(array_path, "rb");
        CHECK(f != NULL);
        if (f) {
            char first = (char)fgetc(f);
            fseek(f, -1, SEEK_END);
            char last = (char)fgetc(f);
            CHECK(first == '[');
            CHECK(last  == ']');
            fclose(f);
        }

        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: arr-read\n"
            "pipeline:\n"
            "  - id: read_stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: json.read\n"
            "        path: \"%s\"\n"
            "        format: array\n"
            "        columns: [id]\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: source\n"
            "        expect: 7\n",
            array_path);
        memset(err, 0, sizeof err);
        rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 2 read: %s\n", err);
        CHECK(rc == BETL_OK);
        unlink(array_path);
    }

    /* --- 3: Mixed-type literal NDJSON. Verifies that NULLs and
     *        unusual cells are handled without barfing. */
    {
        const char *body =
            "{\"id\":1,\"name\":\"alice\",\"score\":3.14,\"active\":true}\n"
            "{\"id\":2,\"name\":null,\"score\":42,\"active\":false}\n"
            "{\"id\":3,\"name\":\"bob\",\"score\":null,\"active\":null}\n";
        CHECK(write_file(typed_path, body) == 0);

        char yaml[2048];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: typed-read\n"
            "pipeline:\n"
            "  - id: read_stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: json.read\n"
            "        path: \"%s\"\n"
            "        format: ndjson\n"
            "        columns: [id, name, score, active, missing_key]\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: source\n"
            "        expect: 3\n",
            typed_path);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 3: %s\n", err);
        CHECK(rc == BETL_OK);
        unlink(typed_path);
    }

    /* --- 4: missing path → INVALID at init. -------------------- */
    {
        char yaml[512] =
            "betl: 1\n"
            "name: bad\n"
            "pipeline:\n"
            "  - id: read_stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: json.read\n"
            "        format: ndjson\n"
            "        columns: [id]\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: source\n"
            "        expect: 0\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: json_io integration test passed\n");
    return 0;
}
