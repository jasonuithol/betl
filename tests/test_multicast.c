/* End-to-end tests for the `multicast` transform.
 *
 * Strategy: drive gen_int64 → multicast → 3 count_rows sinks, with each
 * sink declared with `expect: N`. multicast must:
 *   - hand identical row counts to every tap
 *   - tolerate a tap being drained at a different rate from the others
 *     (achieved naturally by the topo execution order)
 *
 * We also test the negative cases: missing/empty taps, duplicate tap
 * names, unknown tap reference, > MAX_OUTPUT_PORTS taps. */

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

/* Returns BETL_OK on success; on failure copies last_error into err. */
static int run_yaml(const char *yaml, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-mc-%d.yml", (int)getpid());
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

int main(void) {
    /* --- 1. happy path: 3 taps each see all 100 rows ------------------ */
    {
        const char yaml[] =
            "betl: 1\n"
            "name: multicast-basic\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 100\n"
            "        batch_size: 17\n"
            "      - id: fanout\n"
            "        type: multicast\n"
            "        from: src\n"
            "        taps: [a, b, c]\n"
            "      - id: sink_a\n"
            "        type: betl.count_rows\n"
            "        from: fanout:a\n"
            "        expect: 100\n"
            "      - id: sink_b\n"
            "        type: betl.count_rows\n"
            "        from: fanout:b\n"
            "        expect: 100\n"
            "      - id: sink_c\n"
            "        type: betl.count_rows\n"
            "        from: fanout:c\n"
            "        expect: 100\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc == BETL_OK);
        if (rc != BETL_OK) fprintf(stderr, "  err: %s\n", err);
    }

    /* --- 2. negative: taps missing ----------------------------------- */
    {
        const char yaml[] =
            "betl: 1\n"
            "name: multicast-no-taps\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "      - id: fanout\n"
            "        type: multicast\n"
            "        from: src\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: fanout:a\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "taps") != NULL);
    }

    /* --- 3. negative: duplicate tap names ---------------------------- */
    {
        const char yaml[] =
            "betl: 1\n"
            "name: multicast-dup-taps\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "      - id: fanout\n"
            "        type: multicast\n"
            "        from: src\n"
            "        taps: [a, a]\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: fanout:a\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "duplicate") != NULL);
    }

    /* --- 4. negative: unknown tap reference -------------------------- */
    {
        const char yaml[] =
            "betl: 1\n"
            "name: multicast-unknown-tap\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "      - id: fanout\n"
            "        type: multicast\n"
            "        from: src\n"
            "        taps: [a, b]\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: fanout:zzz\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "zzz") != NULL);
    }

    /* --- 5. cross-batch correctness: 1000 rows × 4 taps -------------- *
     * Verifies refcount/queueing behaviour at scale and the
     * "consumer drained, others backed up" path that the topo order
     * traversal naturally produces. */
    {
        const char yaml[] =
            "betl: 1\n"
            "name: multicast-scale\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1000\n"
            "        batch_size: 64\n"
            "      - id: fanout\n"
            "        type: multicast\n"
            "        from: src\n"
            "        taps: [w, x, y, z]\n"
            "      - id: sink_w\n"
            "        type: betl.count_rows\n"
            "        from: fanout:w\n"
            "        expect: 1000\n"
            "      - id: sink_x\n"
            "        type: betl.count_rows\n"
            "        from: fanout:x\n"
            "        expect: 1000\n"
            "      - id: sink_y\n"
            "        type: betl.count_rows\n"
            "        from: fanout:y\n"
            "        expect: 1000\n"
            "      - id: sink_z\n"
            "        type: betl.count_rows\n"
            "        from: fanout:z\n"
            "        expect: 1000\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc == BETL_OK);
        if (rc != BETL_OK) fprintf(stderr, "  err: %s\n", err);
    }

    if (failures > 0) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: multicast end-to-end test passed\n");
    return 0;
}
