/* csv.read v2 end-to-end test: streaming + RFC 4180 quoted fields.
 *
 * Each case writes a CSV file to a temp path, runs a pipeline that
 * pipes csv.read into either betl.count_rows (for counting) or
 * csv.write (for round-trip verification), and checks the result.
 *
 * Coverage:
 *   - Streaming: 4096-row file with batch_size=1024 produces 4 batches
 *     totalling 4096 rows.
 *   - Quoted utf8 cell containing the delimiter is preserved.
 *   - Quoted utf8 cell containing `""` decodes to a single `"`.
 *   - Quoted utf8 cell spanning a newline reassembles correctly.
 *   - Header row may itself contain quoted column names.
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

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int run_yaml(const char *yaml, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-csvr-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) return BETL_ERR_IO;
    char p_err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, p_err, sizeof p_err);
    if (!p) {
        if (err) snprintf(err, err_cap, "%s", p_err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK && err) {
            snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

static void unique_path(char *buf, size_t cap, const char *tag) {
    snprintf(buf, cap, "/tmp/betl-csvr-%d-%s.csv", (int)getpid(), tag);
}

int main(void) {
    /* --- 1. Streaming: 4096 rows + small batch_size triggers multiple
     * get_next batches. The count_rows sink confirms the pipeline saw
     * every row across batches. ---------------------------------------- */
    {
        char in_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path, sizeof in_path, "stream");

        FILE *f = fopen(in_path, "wb");
        CHECK(f != NULL);
        if (f) {
            fputs("id\n", f);
            for (int i = 0; i < 4096; ++i) fprintf(f, "%d\n", i);
            fclose(f);
        }
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvr-stream\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        batch_size: 1024\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: source\n"
            "        expect: 4096\n", in_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "stream case: %s\n", err);
        CHECK(rc == BETL_OK);
        unlink(in_path);
    }

    /* --- 2. Quoted field containing the delimiter survives a round
     * trip through csv.read → csv.write. ------------------------------- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "qcomma-in");
        unique_path(out_path, sizeof out_path, "qcomma-out");
        const char input[] =
            "id,note\n"
            "1,\"hello, world\"\n"
            "2,plain\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvr-quote-comma\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: note, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "qcomma case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            /* csv.write quotes any utf8 with embedded delim, so the
             * `hello, world` cell comes back wrapped. */
            CHECK(strcmp(got,
                "id,note\n"
                "1,\"hello, world\"\n"
                "2,plain\n") == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 3. Embedded `""` decodes to a literal `"` and round-trips
     * through csv.write (which quotes the cell because of the `"`). --- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "qquote-in");
        unique_path(out_path, sizeof out_path, "qquote-out");
        const char input[] =
            "id,note\n"
            "1,\"he said \"\"hi\"\"\"\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvr-quote-quote\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: note, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "qquote case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got,
                "id,note\n"
                "1,\"he said \"\"hi\"\"\"\n") == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 4. Quoted multi-line field: the value contains a newline.
     * After read+write the cell text is preserved (csv.write wraps
     * because of the embedded \n). --------------------------------- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "qmline-in");
        unique_path(out_path, sizeof out_path, "qmline-out");
        const char input[] =
            "id,note\n"
            "1,\"line1\nline2\"\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvr-multiline\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: note, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "qmline case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got,
                "id,note\n"
                "1,\"line1\nline2\"\n") == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 5. Quoted column NAMES in the header are unquoted properly
     * and the resulting schema names survive a round-trip. ------------ */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "qhead-in");
        unique_path(out_path, sizeof out_path, "qhead-out");
        const char input[] =
            "\"id, with comma\",plain\n"
            "1,2\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvr-qhead\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "qhead case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            /* csv.write quotes the unquoted column name `id, with comma`
             * because the comma forces RFC 4180 quoting. */
            CHECK(strcmp(got,
                "\"id, with comma\",plain\n"
                "1,2\n") == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: csv.read v2 end-to-end test passed\n");
    return 0;
}
