/* End-to-end tests for `unpivot` and `pivot` transforms.
 *
 * Strategy: drive csv.read → unpivot|pivot → csv.write and inspect the
 * output file. This exercises real Arrow buffer plumbing through the
 * transform without needing to construct ArrowArrays manually.
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
    snprintf(path, sizeof path, "/tmp/betl-pvt-%d.yml", (int)getpid());
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
    /* --- 1. unpivot: 2 rows × 3 value_cols → 6 long rows ----------- */
    {
        char in_path[64], out_path[64], yaml[2048], err[512] = {0};
        snprintf(in_path,  sizeof in_path,  "/tmp/betl-pvt-%d-in.csv",  (int)getpid());
        snprintf(out_path, sizeof out_path, "/tmp/betl-pvt-%d-out.csv", (int)getpid());
        const char input[] =
            "region,year,jan,feb,mar\n"
            "north,2025,10,20,30\n"
            "south,2025,40,50,60\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: unpivot-basic\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: region, type: utf8 }\n"
            "            - { name: year,   type: int64 }\n"
            "            - { name: jan,    type: int64 }\n"
            "            - { name: feb,    type: int64 }\n"
            "            - { name: mar,    type: int64 }\n"
            "      - id: u\n"
            "        type: unpivot\n"
            "        from: source\n"
            "        id_cols: [region, year]\n"
            "        value_cols: [jan, feb, mar]\n"
            "        name_col: month\n"
            "        value_col: amount\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: u\n"
            "        path: %s\n",
            in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "unpivot: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            /* Header + 6 long rows. Check a few representative cells. */
            CHECK(strstr(got, "region,year,month,amount") != NULL);
            CHECK(strstr(got, "north,2025,jan,10") != NULL);
            CHECK(strstr(got, "north,2025,mar,30") != NULL);
            CHECK(strstr(got, "south,2025,feb,50") != NULL);
            free(got);
        }
        unlink(in_path); unlink(out_path);
    }

    /* --- 2. pivot: 6 long rows → 2 wide rows ----------------------- */
    {
        char in_path[64], out_path[64], yaml[2048], err[512] = {0};
        snprintf(in_path,  sizeof in_path,  "/tmp/betl-pvt-%d-pin.csv",  (int)getpid());
        snprintf(out_path, sizeof out_path, "/tmp/betl-pvt-%d-pout.csv", (int)getpid());
        const char input[] =
            "region,year,month,amount\n"
            "north,2025,jan,10\n"
            "north,2025,feb,20\n"
            "north,2025,mar,30\n"
            "south,2025,jan,40\n"
            "south,2025,feb,50\n"
            "south,2025,mar,60\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: pivot-basic\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: region, type: utf8 }\n"
            "            - { name: year,   type: int64 }\n"
            "            - { name: month,  type: utf8 }\n"
            "            - { name: amount, type: int64 }\n"
            "      - id: p\n"
            "        type: pivot\n"
            "        from: source\n"
            "        id_cols: [region, year]\n"
            "        name_col: month\n"
            "        value_col: amount\n"
            "        pivot_keys: [jan, feb, mar]\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: p\n"
            "        path: %s\n",
            in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "pivot: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            CHECK(strstr(got, "region,year,jan,feb,mar") != NULL);
            CHECK(strstr(got, "north,2025,10,20,30") != NULL);
            CHECK(strstr(got, "south,2025,40,50,60") != NULL);
            free(got);
        }
        unlink(in_path); unlink(out_path);
    }

    /* --- 3. pivot with sparse rows (missing months) ----------------- */
    {
        char in_path[64], out_path[64], yaml[2048], err[512] = {0};
        snprintf(in_path,  sizeof in_path,  "/tmp/betl-pvt-%d-sin.csv",  (int)getpid());
        snprintf(out_path, sizeof out_path, "/tmp/betl-pvt-%d-sout.csv", (int)getpid());
        /* north has no jan; south has no mar. Missing pivot slots emit
         * as NULL (empty in csv). */
        const char input[] =
            "region,month,amount\n"
            "north,feb,200\n"
            "north,mar,300\n"
            "south,jan,400\n"
            "south,feb,500\n";
        CHECK(write_file(in_path, input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: pivot-sparse\n"
            "pipeline:\n"
            "  - id: stage\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: region, type: utf8 }\n"
            "            - { name: month,  type: utf8 }\n"
            "            - { name: amount, type: int64 }\n"
            "      - id: p\n"
            "        type: pivot\n"
            "        from: source\n"
            "        id_cols: [region]\n"
            "        name_col: month\n"
            "        value_col: amount\n"
            "        pivot_keys: [jan, feb, mar]\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: p\n"
            "        path: %s\n",
            in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "pivot-sparse: %s\n", err);
        CHECK(rc == BETL_OK);
        char *got = slurp(out_path);
        CHECK(got != NULL);
        if (got) {
            CHECK(strstr(got, "region,jan,feb,mar") != NULL);
            /* north: no jan, has feb=200, mar=300 */
            CHECK(strstr(got, "north,,200,300") != NULL);
            /* south: jan=400, feb=500, no mar */
            CHECK(strstr(got, "south,400,500,") != NULL);
            free(got);
        }
        unlink(in_path); unlink(out_path);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: pivot/unpivot tests passed\n");
    return 0;
}
