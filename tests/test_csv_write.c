/* csv.write end-to-end test.
 *
 * Drives a series of YAML pipelines that pipe the int64 / utf8 generators
 * into csv.write, then slurps the resulting file and verifies the exact
 * bytes. No Lua dependency — the pipelines stay in built-ins-only land.
 *
 * Coverage:
 *   - int64 round-trip with the default delimiter and header
 *   - utf8 + int64, default delimiter
 *   - header=false suppresses the header line
 *   - delimiter override (single character)
 *   - RFC 4180 quoting kicks in when a utf8 cell contains the delimiter
 *   - missing `path` is an init error
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

static char *slurp_file(const char *path) {
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

/* Run a YAML pipeline. last_err receives the host's last error message
 * on failure. Returns BETL_OK on success. */
static int run_yaml(const char *yaml, char *last_err, size_t last_err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-csvw-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) {
        if (last_err) snprintf(last_err, last_err_cap, "write failed");
        return BETL_ERR_IO;
    }
    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        if (last_err) snprintf(last_err, last_err_cap, "%s", err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (!ctx || !reg) goto cleanup;
    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) goto cleanup;
    rc = betl_run(ctx, reg, p);
    if (last_err) snprintf(last_err, last_err_cap, "%s",
                           betl_context_last_error(ctx));
cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

/* Build the pipeline YAML for `gen_int64(row_count) → csv.write(out_path)`
 * with the supplied options. delim_yaml may be empty to take the default. */
static void mk_int64_pipeline(char *buf, size_t cap,
                              int row_count, const char *out_path,
                              const char *header_yaml,
                              const char *delim_yaml) {
    snprintf(buf, cap,
        "betl: 1\n"
        "name: csvw-int64\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: %d\n"
        "      - id: sink\n"
        "        type: csv.write\n"
        "        from: source\n"
        "        path: %s\n"
        "%s%s",
        row_count, out_path,
        header_yaml ? header_yaml : "",
        delim_yaml  ? delim_yaml  : "");
}

static void mk_strings_pipeline(char *buf, size_t cap,
                                int row_count, const char *prefix,
                                const char *out_path,
                                const char *delim_yaml) {
    snprintf(buf, cap,
        "betl: 1\n"
        "name: csvw-strings\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_strings\n"
        "        row_count: %d\n"
        "        prefix: \"%s\"\n"
        "      - id: sink\n"
        "        type: csv.write\n"
        "        from: source\n"
        "        path: %s\n"
        "%s",
        row_count, prefix, out_path,
        delim_yaml ? delim_yaml : "");
}

static void unique_path(char *buf, size_t cap, const char *tag) {
    snprintf(buf, cap, "/tmp/betl-csvw-%d-%s.csv", (int)getpid(), tag);
}

int main(void) {
    /* --- 1. int64 default: header line + one row per id. --------------- */
    {
        char out[64], yaml[1024], err[512] = {0};
        unique_path(out, sizeof out, "int64");
        mk_int64_pipeline(yaml, sizeof yaml, 3, out, NULL, NULL);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "int64 case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp_file(out);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got, "id\n0\n1\n2\n") == 0);
            free(got);
        }
        unlink(out);
    }

    /* --- 2. utf8 + int64: gen_strings produces id,name pairs. ---------- */
    {
        char out[64], yaml[1024], err[512] = {0};
        unique_path(out, sizeof out, "utf8");
        mk_strings_pipeline(yaml, sizeof yaml, 2, "row_", out, NULL);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "utf8 case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp_file(out);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got, "id,name\n0,row_0\n1,row_1\n") == 0);
            free(got);
        }
        unlink(out);
    }

    /* --- 3. header=false: no header line. ------------------------------ */
    {
        char out[64], yaml[1024], err[512] = {0};
        unique_path(out, sizeof out, "noheader");
        mk_int64_pipeline(yaml, sizeof yaml, 2, out,
                          "        header: false\n", NULL);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "noheader case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp_file(out);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got, "0\n1\n") == 0);
            free(got);
        }
        unlink(out);
    }

    /* --- 4. delimiter='|': pipe-separated. ----------------------------- */
    {
        char out[64], yaml[1024], err[512] = {0};
        unique_path(out, sizeof out, "pipe");
        mk_strings_pipeline(yaml, sizeof yaml, 2, "row_", out,
                            "        delimiter: \"|\"\n");
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "delim case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp_file(out);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got, "id|name\n0|row_0\n1|row_1\n") == 0);
            free(got);
        }
        unlink(out);
    }

    /* --- 5. RFC 4180 quoting: utf8 value contains the delimiter. ------- */
    /* prefix "a,b" + default delim ',' produces names "a,b0", "a,b1".
     * csv.write must wrap each in double-quotes. Internal commas survive. */
    {
        char out[64], yaml[1024], err[512] = {0};
        unique_path(out, sizeof out, "quoting");
        mk_strings_pipeline(yaml, sizeof yaml, 2, "a,b", out, NULL);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "quoting case: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp_file(out);
        CHECK(got != NULL);
        if (got) {
            CHECK(strcmp(got, "id,name\n0,\"a,b0\"\n1,\"a,b1\"\n") == 0);
            free(got);
        }
        unlink(out);
    }

    /* --- 6. Missing path is an init error. ----------------------------- */
    {
        const char yaml[] =
            "betl: 1\n"
            "name: csvw-no-path\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "missing required `path`") != NULL);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: csv.write end-to-end test passed\n");
    return 0;
}
