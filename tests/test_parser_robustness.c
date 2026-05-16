/* Parser-robustness corpus: feed malformed YAML / CSV input through
 * the loaders and verify they error cleanly — no segfaults, no infinite
 * loops, no leaks. This is "fuzz coverage by hand" — a curated corpus
 * that exercises the parser entry points with bytes that real users
 * (typo'd recipes, corrupted files, hostile input) would produce.
 *
 * Each case loads one synthetic input and asserts the loader returns
 * non-OK and stays alive. Valgrind clean is implied — run this binary
 * under `analyze` periodically to confirm no leak path. */

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

static int try_load_yaml(const char *yaml) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-pr-%d.yml", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(yaml, 1, strlen(yaml), f);
    fclose(f);
    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    int rc = p ? 0 : -1;
    if (p) betl_pipeline_free(p);
    unlink(path);
    return rc;
}

static int try_run_csv(const char *csv_bytes, size_t csv_len,
                       const char *yaml_template) {
    char csv_path[64], yml_path[64], yaml[1024];
    snprintf(csv_path, sizeof csv_path, "/tmp/betl-pr-%d.csv", (int)getpid());
    snprintf(yml_path, sizeof yml_path, "/tmp/betl-pr-%d.yml", (int)getpid());
    FILE *f = fopen(csv_path, "wb");
    if (!f) return -1;
    fwrite(csv_bytes, 1, csv_len, f);
    fclose(f);
    snprintf(yaml, sizeof yaml, yaml_template, csv_path);
    f = fopen(yml_path, "wb");
    if (!f) { unlink(csv_path); return -1; }
    fwrite(yaml, 1, strlen(yaml), f);
    fclose(f);
    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(yml_path, err, sizeof err);
    int rc = BETL_ERR_INTERNAL;
    if (p) {
        BetlContext  *ctx = betl_context_create();
        BetlRegistry *reg = betl_registry_create();
        if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
            rc = betl_run(ctx, reg, p);
        }
        if (reg) betl_registry_destroy(reg);
        if (ctx) betl_context_destroy(ctx);
        betl_pipeline_free(p);
    }
    unlink(csv_path);
    unlink(yml_path);
    return rc;
}

int main(void) {
    /* ====================================================================
     * YAML loader: malformed inputs must be rejected without crashing.
     * ================================================================== */

    /* Empty file. */
    CHECK(try_load_yaml("") != 0);

    /* Random binary garbage. */
    CHECK(try_load_yaml("\x00\x01\x02\x03\xff\xfe\x00") != 0);

    /* Valid YAML but wrong schema (no betl: marker, no pipeline). */
    CHECK(try_load_yaml("name: nope\n") != 0);

    /* Missing pipeline key. */
    CHECK(try_load_yaml("betl: 1\nname: only-name\n") != 0);

    /* Pipeline is a scalar, not a list. */
    CHECK(try_load_yaml("betl: 1\nname: p\npipeline: 42\n") != 0);

    /* Stage missing required `type:`. */
    CHECK(try_load_yaml(
        "betl: 1\n"
        "name: p\n"
        "pipeline:\n"
        "  - id: orphan\n") != 0);

    /* Indentation broken — tab/space mix yaml-libyaml usually catches. */
    CHECK(try_load_yaml(
        "betl: 1\n"
        "\tname: tabbed\n"
        "pipeline: []\n") != 0);

    /* Unterminated string. */
    CHECK(try_load_yaml(
        "betl: 1\n"
        "name: \"unterminated\n"
        "pipeline: []\n") != 0);

    /* Recursive anchor (YAML billion-laughs guard). libyaml has a
     * default depth limit. The expansion shouldn't OOM or hang. */
    CHECK(try_load_yaml(
        "betl: 1\n"
        "name: anchor-loop\n"
        "x: &a\n"
        "  k: *a\n"
        "pipeline: []\n") != 0);

    /* Deep nesting — guard against stack overflow. 50 levels at most
     * 100 chars per line = 10 KB. The parser should error gracefully
     * (the YAML is structurally broken since each `- id` ought to be
     * at the same indent), not crash. */
    {
        const size_t buf_cap = 64 * 1024;
        char *buf = malloc(buf_cap);
        CHECK(buf != NULL);
        if (buf) {
            size_t off = 0;
            off += (size_t)snprintf(buf + off, buf_cap - off,
                                    "betl: 1\nname: deep\npipeline:\n");
            for (int i = 0; i < 50; ++i) {
                off += (size_t)snprintf(buf + off, buf_cap - off,
                                        "%*s- id: s%d\n", i, "", i);
                off += (size_t)snprintf(buf + off, buf_cap - off,
                                        "%*s  type: noop\n", i, "");
                if (off + 256 > buf_cap) break;   /* safety belt */
            }
            (void)try_load_yaml(buf);   /* just must not crash */
            free(buf);
        }
    }

    /* ====================================================================
     * CSV reader: malformed inputs must error cleanly.
     * ================================================================== */

    /* Header declares N columns, every data row has fewer. csv.read
     * should either error or pad with NULLs — but not crash. */
    {
        const char *bad = "id,name,extra\n1,only-two\n";
        const char *yml =
            "betl: 1\n"
            "name: bad\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: src\n"
            "        expect: 1\n";
        int rc = try_run_csv(bad, strlen(bad), yml);
        (void)rc;   /* either OK with pad or error, just no crash */
    }

    /* Quoted field never closes — must terminate at EOF. */
    {
        const char *bad = "id,note\n1,\"unterminated\n";
        const char *yml =
            "betl: 1\n"
            "name: bad\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: src\n"
            "        expect: 1\n";
        (void)try_run_csv(bad, strlen(bad), yml);   /* must not hang */
    }

    /* int64 column with non-numeric data — must error not crash. */
    {
        const char *bad = "id\nnot-a-number\n";
        const char *yml =
            "betl: 1\n"
            "name: bad-int\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id, type: int64 }\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: src\n"
            "        expect: 1\n";
        int rc = try_run_csv(bad, strlen(bad), yml);
        CHECK(rc != BETL_OK);
    }

    /* Embedded NULs in CSV data — should pass through utf8 cells but
     * not derail parsing. */
    {
        const char bad[] = { 'i','d',',','n','\n', '1', ',', 'a', 0x00, 'b', '\n' };
        const char *yml =
            "betl: 1\n"
            "name: nul-csv\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id, type: int64 }\n"
            "            - { name: n,  type: utf8  }\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: src\n"
            "        expect: 1\n";
        (void)try_run_csv(bad, sizeof bad, yml);   /* no crash */
    }

    /* Huge over-long line — guard against unbounded record growth.
     * 64 KB of one cell. Should either read it as one big string or
     * error, but not loop. */
    {
        size_t big_sz = 64 * 1024;
        char *bad = malloc(big_sz + 32);
        CHECK(bad != NULL);
        if (bad) {
            strcpy(bad, "id,n\n1,");
            size_t hdr = strlen(bad);
            memset(bad + hdr, 'A', big_sz);
            bad[hdr + big_sz] = '\n';
            bad[hdr + big_sz + 1] = '\0';
            const char *yml =
                "betl: 1\n"
                "name: long\n"
                "pipeline:\n"
                "  - id: s\n"
                "    type: dataflow\n"
                "    steps:\n"
                "      - id: src\n"
                "        type: csv.read\n"
                "        path: %s\n"
                "        schema:\n"
                "          columns:\n"
                "            - { name: id, type: int64 }\n"
                "            - { name: n,  type: utf8  }\n"
                "      - id: sink\n"
                "        type: betl.count_rows\n"
                "        from: src\n"
                "        expect: 1\n";
            (void)try_run_csv(bad, hdr + big_sz + 1, yml);
            free(bad);
        }
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: parser robustness corpus passed\n");
    return 0;
}
