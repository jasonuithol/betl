/* End-to-end executor test:
 *   1. Build a registry, register the in-process built-ins.
 *   2. Construct a pipeline file on disk in a temp directory:
 *        gen_int64 (5 rows) -> count_rows (expect: 5)
 *   3. Parse it via the pipeline parser.
 *   4. Run via betl_run.
 *   5. Assert success, and that the captured log contains the count.
 *
 * Also runs negative cases: missing component (no provider for type),
 * count mismatch (configured expect != actual). */

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

/* Run a pipeline string end-to-end. Returns the rc from betl_run.
 * `last_err_out` (if non-NULL) is filled with the final ctx error. */
static int run_yaml(const char *yaml,
                    char *last_err_out, size_t last_err_cap,
                    FILE *log_capture) {
    char path[] = "/tmp/betl-test-exec-XXXXXX.yml";
    /* mkstemps would be ideal but isn't portable; just use the static name. */
    snprintf(path, sizeof path, "/tmp/betl-test-exec-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) {
        if (last_err_out) snprintf(last_err_out, last_err_cap,
                                   "could not write temp pipeline file");
        return BETL_ERR_IO;
    }

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        if (last_err_out) snprintf(last_err_out, last_err_cap, "%s", err);
        unlink(path);
        return BETL_ERR_INVALID;
    }

    BetlContext *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc;
    if (!ctx || !reg) { rc = BETL_ERR_INTERNAL; goto cleanup; }
    if (log_capture) {
        betl_context_set_log_stream(ctx, log_capture);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    }
    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) goto cleanup;

    rc = betl_run(ctx, reg, p);

    if (last_err_out) {
        snprintf(last_err_out, last_err_cap, "%s",
                 betl_context_last_error(ctx));
    }

cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

static const char PIPELINE_HAPPY[] =
    "betl: 1\n"
    "name: gen-and-count\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "        column: id\n"
    "        start: 100\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: source\n"
    "        expect: 5\n";

static const char PIPELINE_MISMATCH[] =
    "betl: 1\n"
    "name: count-mismatch\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: source\n"
    "        expect: 9\n";

static const char PIPELINE_UNKNOWN[] =
    "betl: 1\n"
    "name: unknown-component\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: no.such.component\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: source\n";

static const char PIPELINE_CSV[] =
    "betl: 1\n"
    "name: csv-to-count\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: csv.read\n"
    "        path: %s\n"            /* substituted from BETL_TEST_FIXTURES_DIR */
    "        header: true\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: source\n"
    "        expect: 5\n";

static const char PIPELINE_TWO_STAGES[] =
    "betl: 1\n"
    "name: two-stages-with-after\n"
    "pipeline:\n"
    "  - id: first\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: source\n"
    "        expect: 2\n"
    "  - id: second\n"
    "    type: dataflow\n"
    "    after: [first]\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 7\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: source\n"
    "        expect: 7\n";

int main(void) {
    /* --- happy path -------------------------------------------------- */
    {
        char err[512];
        FILE *log = tmpfile();
        int rc = run_yaml(PIPELINE_HAPPY, err, sizeof err, log);
        if (rc != BETL_OK) fprintf(stderr, "happy err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 5 rows") != NULL);

        /* The log captured INFO lines from the executor and the sink. */
        char *log_text = slurp(log);
        CHECK(log_text != NULL);
        if (log_text) {
            CHECK(strstr(log_text, "stage start: stage_one") != NULL);
            CHECK(strstr(log_text, "count_rows: 5 rows")     != NULL);
            CHECK(strstr(log_text, "stage end: stage_one rc=0") != NULL);
            free(log_text);
        }
        if (log) fclose(log);
    }

    /* --- count mismatch ---------------------------------------------- */
    {
        char err[512];
        int rc = run_yaml(PIPELINE_MISMATCH, err, sizeof err, NULL);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "expected 9 rows but counted 3") != NULL);
    }

    /* --- unknown component type -------------------------------------- */
    {
        char err[512];
        int rc = run_yaml(PIPELINE_UNKNOWN, err, sizeof err, NULL);
        CHECK(rc == BETL_ERR_NOT_FOUND);
        CHECK(strstr(err, "no.such.component") != NULL);
    }

    /* --- csv.read ---------------------------------------------------- */
    {
        const char *fixtures =
#ifdef BETL_TEST_FIXTURES_DIR
            BETL_TEST_FIXTURES_DIR;
#else
            "tests/fixtures";
#endif
        char yaml[2048], csv_path[1024];
        snprintf(csv_path, sizeof csv_path, "%s/sample.csv", fixtures);
        snprintf(yaml, sizeof yaml, PIPELINE_CSV, csv_path);

        char err[512];
        FILE *log = tmpfile();
        int rc = run_yaml(yaml, err, sizeof err, log);
        if (rc != BETL_OK) fprintf(stderr, "csv err: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(strstr(err, "counted 5 rows") != NULL);
        char *log_text = slurp(log);
        if (log_text) {
            CHECK(strstr(log_text, "count_rows: 5 rows") != NULL);
            free(log_text);
        }
        if (log) fclose(log);
    }

    /* --- csv.read on missing file ------------------------------------ */
    {
        char yaml[1024];
        snprintf(yaml, sizeof yaml, PIPELINE_CSV, "/no/such/file.csv");
        char err[512];
        int rc = run_yaml(yaml, err, sizeof err, NULL);
        CHECK(rc == BETL_ERR_IO);
        CHECK(strstr(err, "cannot open") != NULL);
    }

    /* --- two-stage with `after:` ------------------------------------- */
    {
        char err[512];
        FILE *log = tmpfile();
        int rc = run_yaml(PIPELINE_TWO_STAGES, err, sizeof err, log);
        CHECK(rc == BETL_OK);
        char *log_text = slurp(log);
        CHECK(log_text != NULL);
        if (log_text) {
            /* `first` must finish before `second` starts. */
            const char *first_end  = strstr(log_text, "stage end: first rc=0");
            const char *second_beg = strstr(log_text, "stage start: second");
            CHECK(first_end != NULL);
            CHECK(second_beg != NULL);
            if (first_end && second_beg) CHECK(first_end < second_beg);
            free(log_text);
        }
        if (log) fclose(log);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("exec: all checks passed\n");
    return 0;
}
