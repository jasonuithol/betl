/* Standard transforms (filter / map) end-to-end test.
 *
 * argv[1] = path to betl-lua.so
 *
 * Drives a series of YAML pipelines that combine gen_int64 / gen_strings
 * → filter / map → count_rows. Verifies that:
 *
 *   - filter with a lua predicate keeps only matching rows
 *   - filter shorthand (where: "...") implies lang=lua
 *   - filter with the literal engine accepts a constant predicate
 *   - filter on utf8 columns works
 *   - map (add) appends columns derived from lua expressions
 *   - map (add) appends a literal column via the literal engine
 *   - filter and map chain into multi-step pipelines
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

static int run_yaml_log(const char *plugin_path,
                        const char *yaml,
                        char *last_err, size_t last_err_cap,
                        FILE *log_capture) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-tx-%d.yml", (int)getpid());
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
    if (log_capture) {
        betl_context_set_log_stream(ctx, log_capture);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    }
    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) goto cleanup;
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

static int run_yaml(const char *plugin_path,
                    const char *yaml,
                    char *last_err, size_t last_err_cap) {
    return run_yaml_log(plugin_path, yaml, last_err, last_err_cap, NULL);
}

static char *slurp_file(FILE *f) {
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

/* --- Pipelines ------------------------------------------------------------ */

/* gen_int64 emits ids 0..N-1 by default. */

static const char PL_FILTER_LUA[] =
    "betl: 1\n"
    "name: tx-filter-lua\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 10\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where:\n"
    "          lang: lua\n"
    "          expr: \"row.id > 5\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 4\n";

static const char PL_FILTER_SHORTHAND[] =
    "betl: 1\n"
    "name: tx-filter-shorthand\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where: \"row.id < 3\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 3\n";

static const char PL_FILTER_LITERAL_TRUE[] =
    "betl: 1\n"
    "name: tx-filter-literal-true\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where:\n"
    "          lang: literal\n"
    "          value: \"true\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 3\n";

static const char PL_FILTER_LITERAL_FALSE[] =
    "betl: 1\n"
    "name: tx-filter-literal-false\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where:\n"
    "          lang: literal\n"
    "          value: \"false\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 0\n";

static const char PL_FILTER_UTF8[] =
    "betl: 1\n"
    "name: tx-filter-utf8\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 4\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where: \"row.id == 1 or row.name == 'row_3'\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 2\n";

static const char PL_MAP_LUA[] =
    "betl: 1\n"
    "name: tx-map-lua\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: extend\n"
    "        type: map\n"
    "        from: source\n"
    "        add:\n"
    "          doubled:\n"
    "            lang: lua\n"
    "            expr: \"row.id * 2\"\n"
    "            type: l\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: extend\n"
    "        expect: 3\n";

static const char PL_MAP_LITERAL[] =
    "betl: 1\n"
    "name: tx-map-literal\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "      - id: extend\n"
    "        type: map\n"
    "        from: source\n"
    "        add:\n"
    "          tag:\n"
    "            lang: literal\n"
    "            value: \"constant\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: extend\n"
    "        expect: 2\n";

/* select: passthrough only — drop the id column. */
static const char PL_SELECT_PASSTHROUGH[] =
    "betl: 1\n"
    "name: tx-select-pass\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 3\n"
    "      - id: project\n"
    "        type: map\n"
    "        from: source\n"
    "        select:\n"
    "          - name\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: project\n"
    "        expect: 3\n";

/* select: rename + computed column. */
static const char PL_SELECT_RENAME_AND_EXPR[] =
    "betl: 1\n"
    "name: tx-select-rename-expr\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 4\n"
    "      - id: project\n"
    "        type: map\n"
    "        from: source\n"
    "        select:\n"
    "          - { name: alias, from: id }\n"
    "          - name: upper\n"
    "            expr: \"string.upper(row.name)\"\n"
    "          - name: tag\n"
    "            lang: literal\n"
    "            value: \"const\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: project\n"
    "        expect: 4\n";

/* Same input column referenced twice — must work via deep copy. */
static const char PL_SELECT_DUP_REF[] =
    "betl: 1\n"
    "name: tx-select-dup\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 2\n"
    "      - id: project\n"
    "        type: map\n"
    "        from: source\n"
    "        select:\n"
    "          - id\n"
    "          - { name: id_alias, from: id }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: project\n"
    "        expect: 2\n";

/* Chained: select renames, downstream map references the new name. */
static const char PL_SELECT_THEN_ADD[] =
    "betl: 1\n"
    "name: tx-select-then-add\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "      - id: project\n"
    "        type: map\n"
    "        from: source\n"
    "        select:\n"
    "          - { name: x, from: id }\n"
    "      - id: extend\n"
    "        type: map\n"
    "        from: project\n"
    "        add:\n"
    "          x_double:\n"
    "            lang: lua\n"
    "            expr: \"row.x * 2\"\n"
    "            type: l\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: extend\n"
    "        expect: 3\n";

/* Both add: and select: → init error. */
static const char PL_SELECT_AND_ADD[] =
    "betl: 1\n"
    "name: tx-select-and-add\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: project\n"
    "        type: map\n"
    "        from: source\n"
    "        select:\n"
    "          - id\n"
    "        add:\n"
    "          extra:\n"
    "            lang: literal\n"
    "            value: \"x\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: project\n";

/* sort: gen_int64(4) → sort by id desc → lua.map (logs each row) →
 * count. The captured log lets us verify the sort actually reordered. */
static const char PL_SORT_DESC[] =
    "betl: 1\n"
    "name: tx-sort-desc\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "      - id: srt\n"
    "        type: sort\n"
    "        from: source\n"
    "        by:\n"
    "          - { col: id, dir: desc }\n"
    "      - id: log\n"
    "        type: lua.map\n"
    "        from: srt\n"
    "        script: |\n"
    "          log.info('seen=' .. row.id)\n"
    "          return row\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: log\n"
    "        expect: 4\n";

/* sort: gen_strings(3) → sort by name asc, then by id asc. */
static const char PL_SORT_UTF8[] =
    "betl: 1\n"
    "name: tx-sort-utf8\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_strings\n"
    "        row_count: 3\n"
    "      - id: srt\n"
    "        type: sort\n"
    "        from: source\n"
    "        by:\n"
    "          - { col: name, dir: asc }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: srt\n"
    "        expect: 3\n";

/* join: two int64 streams. Left has ids 0..4, right has ids 0..2.
 * Inner-join on id should yield 3 rows. */
static const char PL_JOIN_INT64[] =
    "betl: 1\n"
    "name: tx-join-int64\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: left_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "        column: lid\n"
    "      - id: right_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "        column: rid\n"
    "      - id: jn\n"
    "        type: join\n"
    "        from: [left_src, right_src]\n"
    "        on: { lid: rid }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: jn\n"
    "        expect: 3\n";

/* Layout used by LEFT / OUTER cases below:
 *   left:  lid in {0, 1, 2, 3}    (4 rows, gen_int64 row_count=4 start=0)
 *   right: rid in {2, 3, 4}       (3 rows, gen_int64 row_count=3 start=2)
 *
 *   inner matches: lid=2,3 → 2 rows
 *   left  -> 2 matches + 2 left-only (lid=0,1) = 4 rows
 *   outer -> same 4 + 1 right-only (rid=4)     = 5 rows
 */
static const char PL_JOIN_LEFT[] =
    "betl: 1\n"
    "name: tx-join-left\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: left_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "        column: lid\n"
    "      - id: right_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "        start: 2\n"
    "        column: rid\n"
    "      - id: jn\n"
    "        type: join\n"
    "        kind: left\n"
    "        from: [left_src, right_src]\n"
    "        on: { lid: rid }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: jn\n"
    "        expect: 4\n";

static const char PL_JOIN_OUTER[] =
    "betl: 1\n"
    "name: tx-join-outer\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: left_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "        column: lid\n"
    "      - id: right_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "        start: 2\n"
    "        column: rid\n"
    "      - id: jn\n"
    "        type: join\n"
    "        kind: outer\n"
    "        from: [left_src, right_src]\n"
    "        on: { lid: rid }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: jn\n"
    "        expect: 5\n";

/* Bad kind value: must produce a parse-time error. */
static const char PL_JOIN_BAD_KIND[] =
    "betl: 1\n"
    "name: tx-join-bad-kind\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: left_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "        column: lid\n"
    "      - id: right_src\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "        column: rid\n"
    "      - id: jn\n"
    "        type: join\n"
    "        kind: cross\n"
    "        from: [left_src, right_src]\n"
    "        on: { lid: rid }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: jn\n";

/* union: three int64 streams of size 2 → 6 rows total. */
static const char PL_UNION_INT64[] =
    "betl: 1\n"
    "name: tx-union-int64\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: a\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "        start: 0\n"
    "      - id: b\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "        start: 10\n"
    "      - id: c\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "        start: 20\n"
    "      - id: u\n"
    "        type: union\n"
    "        from: [a, b, c]\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: u\n"
    "        expect: 6\n";

/* union with mismatched schemas: int64 stream + utf8 stream → error. */
static const char PL_UNION_BAD_SCHEMA[] =
    "betl: 1\n"
    "name: tx-union-bad-schema\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: a\n"
    "        type: betl.gen_int64\n"
    "        row_count: 2\n"
    "      - id: b\n"
    "        type: betl.gen_strings\n"
    "        row_count: 2\n"
    "      - id: u\n"
    "        type: union\n"
    "        from: [a, b]\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: u\n";

/* distinct: union the same gen twice (5 rows each → 10), then dedupe.
 * Default keys = all columns → expect 5 unique. */
static const char PL_DISTINCT_ALL[] =
    "betl: 1\n"
    "name: tx-distinct-all\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: a\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: b\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: u\n"
    "        type: union\n"
    "        from: [a, b]\n"
    "      - id: dd\n"
    "        type: distinct\n"
    "        from: u\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: dd\n"
    "        expect: 5\n";

/* distinct with explicit keys: gen_strings has int64 id + utf8 name
 * with prefix. Same gen twice via union → 6 rows; dedupe by `name`
 * column → 3 unique names (row_0, row_1, row_2 each once). */
static const char PL_DISTINCT_KEYED[] =
    "betl: 1\n"
    "name: tx-distinct-keyed\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: a\n"
    "        type: betl.gen_strings\n"
    "        row_count: 3\n"
    "      - id: b\n"
    "        type: betl.gen_strings\n"
    "        row_count: 3\n"
    "      - id: u\n"
    "        type: union\n"
    "        from: [a, b]\n"
    "      - id: dd\n"
    "        type: distinct\n"
    "        from: u\n"
    "        keys: [name]\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: dd\n"
    "        expect: 3\n";

/* distinct with a key that doesn't exist on the input. */
static const char PL_DISTINCT_BAD_KEY[] =
    "betl: 1\n"
    "name: tx-distinct-bad-key\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: dd\n"
    "        type: distinct\n"
    "        from: source\n"
    "        keys: [no_such]\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: dd\n";

/* limit: 10 rows in, n=3 → 3 rows out. */
static const char PL_LIMIT_TRIM[] =
    "betl: 1\n"
    "name: tx-limit-trim\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 10\n"
    "      - id: lim\n"
    "        type: limit\n"
    "        from: source\n"
    "        n: 3\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: lim\n"
    "        expect: 3\n";

/* limit n bigger than the stream — pass everything through. */
static const char PL_LIMIT_OVERSHOOT[] =
    "betl: 1\n"
    "name: tx-limit-overshoot\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "      - id: lim\n"
    "        type: limit\n"
    "        from: source\n"
    "        n: 100\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: lim\n"
    "        expect: 4\n";

/* limit with missing n — config error. */
static const char PL_LIMIT_NO_N[] =
    "betl: 1\n"
    "name: tx-limit-no-n\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: lim\n"
    "        type: limit\n"
    "        from: source\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: lim\n";

/* conditional_split: gen_int64(6) → ids 0..5. Cases:
 *   hot:  row.id > 3   → ids 4, 5
 *   cold: row.id < 2   → ids 0, 1
 *   default: rest     → ids 2, 3
 * Each port consumed by its own count_rows sink. */
static const char PL_SPLIT_BASIC[] =
    "betl: 1\n"
    "name: tx-split-basic\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 6\n"
    "      - id: split\n"
    "        type: conditional_split\n"
    "        from: source\n"
    "        cases:\n"
    "          - { name: hot,  where: \"row.id > 3\" }\n"
    "          - { name: cold, where: \"row.id < 2\" }\n"
    "        default: rest\n"
    "      - id: sink_hot\n"
    "        type: betl.count_rows\n"
    "        from: split:hot\n"
    "        expect: 2\n"
    "      - id: sink_cold\n"
    "        type: betl.count_rows\n"
    "        from: split:cold\n"
    "        expect: 2\n"
    "      - id: sink_rest\n"
    "        type: betl.count_rows\n"
    "        from: split:rest\n"
    "        expect: 2\n";

/* No default port: rows that don't match any case are dropped. */
static const char PL_SPLIT_NO_DEFAULT[] =
    "betl: 1\n"
    "name: tx-split-no-default\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: split\n"
    "        type: conditional_split\n"
    "        from: source\n"
    "        cases:\n"
    "          - { name: low,  where: \"row.id < 2\" }\n"
    "          - { name: high, where: \"row.id > 3\" }\n"
    "      - id: sink_low\n"
    "        type: betl.count_rows\n"
    "        from: split:low\n"
    "        expect: 2\n"
    "      - id: sink_high\n"
    "        type: betl.count_rows\n"
    "        from: split:high\n"
    "        expect: 1\n";

/* First-match wins: a row matching both cases goes only to the
 * first one. id in [0..3]; case A is row.id < 3 (matches 0,1,2),
 * case B is row.id < 2 (would match 0,1 but those are claimed by A
 * already → expect 0 rows). */
static const char PL_SPLIT_FIRST_WINS[] =
    "betl: 1\n"
    "name: tx-split-first-wins\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 4\n"
    "      - id: split\n"
    "        type: conditional_split\n"
    "        from: source\n"
    "        cases:\n"
    "          - { name: a, where: \"row.id < 3\" }\n"
    "          - { name: b, where: \"row.id < 2\" }\n"
    "        default: rest\n"
    "      - id: sink_a\n"
    "        type: betl.count_rows\n"
    "        from: split:a\n"
    "        expect: 3\n"
    "      - id: sink_b\n"
    "        type: betl.count_rows\n"
    "        from: split:b\n"
    "        expect: 0\n"
    "      - id: sink_rest\n"
    "        type: betl.count_rows\n"
    "        from: split:rest\n"
    "        expect: 1\n";

/* Reference an unknown port name from a downstream step. */
static const char PL_SPLIT_BAD_PORT[] =
    "betl: 1\n"
    "name: tx-split-bad-port\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: split\n"
    "        type: conditional_split\n"
    "        from: source\n"
    "        cases:\n"
    "          - { name: a, where: \"row.id > 0\" }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: split:no_such_port\n";

/* Empty cases list — must reject at init. */
static const char PL_SPLIT_NO_CASES[] =
    "betl: 1\n"
    "name: tx-split-no-cases\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: split\n"
    "        type: conditional_split\n"
    "        from: source\n"
    "        cases: []\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: split:any\n";

/* limit with n=0 — must reject. */
static const char PL_LIMIT_BAD_N[] =
    "betl: 1\n"
    "name: tx-limit-bad-n\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: lim\n"
    "        type: limit\n"
    "        from: source\n"
    "        n: 0\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: lim\n";

/* csv.read with a `schema:` block: loads a mixed-type file and pipes
 * the utf8 column through a filter predicate. %s is replaced with the
 * absolute path to the fixture at test time. */
static const char PL_CSV_TYPED_FMT[] =
    "betl: 1\n"
    "name: tx-csv-typed\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: csv.read\n"
    "        path: %s\n"
    "        header: true\n"
    "        schema:\n"
    "          columns:\n"
    "            - { name: id,     type: int64 }\n"
    "            - { name: name,   type: utf8  }\n"
    "            - { name: amount, type: int64 }\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where: \"row.name == 'alice' or row.name == 'bob'\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 2\n";

/* aggregate: gen_int64(3, start=10) → group_by [id] → count.
 * Three groups, n=1 each. filter row.n == 1 keeps all 3. */
static const char PL_AGG_COUNT_BY_ID[] =
    "betl: 1\n"
    "name: tx-agg-count\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 3\n"
    "        start: 10\n"
    "      - id: agg\n"
    "        type: aggregate\n"
    "        from: source\n"
    "        group_by: [id]\n"
    "        compute:\n"
    "          n: { agg: count }\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: agg\n"
    "        where: \"row.n == 1\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 3\n";

/* Use map to add a constant bucket so every row is in one group, then
 * sum over id and verify by filter. gen_int64(5, start=0) → sum=10. */
static const char PL_AGG_SUM_ONE_BUCKET[] =
    "betl: 1\n"
    "name: tx-agg-sum-one\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: tag\n"
    "        type: map\n"
    "        from: source\n"
    "        add:\n"
    "          bucket: { lang: lua, expr: \"0\", type: l }\n"
    "      - id: agg\n"
    "        type: aggregate\n"
    "        from: tag\n"
    "        group_by: [bucket]\n"
    "        compute:\n"
    "          n: { agg: count }\n"
    "          s: { agg: sum, over: id }\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: agg\n"
    "        where: \"row.n == 5 and row.s == 10\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 1\n";

/* min/max over a single group. gen_int64(5, start=10) → min=10, max=14. */
static const char PL_AGG_MIN_MAX[] =
    "betl: 1\n"
    "name: tx-agg-min-max\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "        start: 10\n"
    "      - id: tag\n"
    "        type: map\n"
    "        from: source\n"
    "        add:\n"
    "          b: { lang: lua, expr: \"0\", type: l }\n"
    "      - id: agg\n"
    "        type: aggregate\n"
    "        from: tag\n"
    "        group_by: [b]\n"
    "        compute:\n"
    "          mn: { agg: min, over: id }\n"
    "          mx: { agg: max, over: id }\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: agg\n"
    "        where: \"row.mn == 10 and row.mx == 14\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 1\n";

/* Multi-group via lua expression bucket = id % 2.
 * 6 rows id=0..5 → group 0: ids 0,2,4 (n=3, s=6); group 1: ids 1,3,5 (n=3, s=9).
 * Filter row.n == 3 keeps both groups. */
static const char PL_AGG_TWO_GROUPS[] =
    "betl: 1\n"
    "name: tx-agg-two-groups\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 6\n"
    "      - id: tag\n"
    "        type: map\n"
    "        from: source\n"
    "        add:\n"
    "          bucket: { lang: lua, expr: \"row.id % 2\", type: l }\n"
    "      - id: agg\n"
    "        type: aggregate\n"
    "        from: tag\n"
    "        group_by: [bucket]\n"
    "        compute:\n"
    "          n: { agg: count }\n"
    "          s: { agg: sum, over: id }\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: agg\n"
    "        where: \"row.n == 3 and (row.s == 6 or row.s == 9)\"\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: keep\n"
    "        expect: 2\n";

/* group_by names a column that doesn't exist on the upstream schema. */
static const char PL_AGG_BAD_GROUP_BY[] =
    "betl: 1\n"
    "name: tx-agg-bad-gb\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: agg\n"
    "        type: aggregate\n"
    "        from: source\n"
    "        group_by: [no_such]\n"
    "        compute:\n"
    "          n: { agg: count }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: agg\n";

/* Unknown agg name (e.g., 'stddev' which we don't support). */
static const char PL_AGG_BAD_AGG[] =
    "betl: 1\n"
    "name: tx-agg-bad-agg\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: agg\n"
    "        type: aggregate\n"
    "        from: source\n"
    "        group_by: [id]\n"
    "        compute:\n"
    "          x: { agg: stddev, over: id }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: agg\n";

/* select references a column that doesn't exist on the input. */
static const char PL_SELECT_BAD_FROM[] =
    "betl: 1\n"
    "name: tx-select-bad-from\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 1\n"
    "      - id: project\n"
    "        type: map\n"
    "        from: source\n"
    "        select:\n"
    "          - { name: foo, from: not_a_real_col }\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: project\n";

static const char PL_FILTER_THEN_MAP[] =
    "betl: 1\n"
    "name: tx-filter-then-map\n"
    "pipeline:\n"
    "  - id: stage_one\n"
    "    type: dataflow\n"
    "    steps:\n"
    "      - id: source\n"
    "        type: betl.gen_int64\n"
    "        row_count: 5\n"
    "      - id: keep\n"
    "        type: filter\n"
    "        from: source\n"
    "        where: \"row.id > 2\"\n"
    "      - id: extend\n"
    "        type: map\n"
    "        from: keep\n"
    "        add:\n"
    "          doubled:\n"
    "            lang: lua\n"
    "            expr: \"row.id * 2\"\n"
    "            type: l\n"
    "      - id: sink\n"
    "        type: betl.count_rows\n"
    "        from: extend\n"
    "        expect: 2\n";

/* --- main ----------------------------------------------------------------- */

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

    /* Cases that should succeed. */
    struct { const char *name; const char *yaml; } ok_cases[] = {
        { "filter-lua",            PL_FILTER_LUA            },
        { "filter-shorthand",      PL_FILTER_SHORTHAND      },
        { "filter-literal-true",   PL_FILTER_LITERAL_TRUE   },
        { "filter-literal-false",  PL_FILTER_LITERAL_FALSE  },
        { "filter-utf8",           PL_FILTER_UTF8           },
        { "map-lua",               PL_MAP_LUA               },
        { "map-literal",           PL_MAP_LITERAL           },
        { "select-passthrough",    PL_SELECT_PASSTHROUGH    },
        { "select-rename-expr",    PL_SELECT_RENAME_AND_EXPR},
        { "select-dup-ref",        PL_SELECT_DUP_REF        },
        { "select-then-add",       PL_SELECT_THEN_ADD       },
        { "filter-then-map",       PL_FILTER_THEN_MAP       },
        { "agg-count-by-id",       PL_AGG_COUNT_BY_ID       },
        { "agg-sum-one-bucket",    PL_AGG_SUM_ONE_BUCKET    },
        { "agg-min-max",           PL_AGG_MIN_MAX           },
        { "agg-two-groups",        PL_AGG_TWO_GROUPS        },
        { "sort-utf8",             PL_SORT_UTF8             },
        { "join-int64",            PL_JOIN_INT64            },
        { "join-left",             PL_JOIN_LEFT             },
        { "join-outer",            PL_JOIN_OUTER            },
        { "union-int64",           PL_UNION_INT64           },
        { "distinct-all",          PL_DISTINCT_ALL          },
        { "distinct-keyed",        PL_DISTINCT_KEYED        },
        { "limit-trim",            PL_LIMIT_TRIM            },
        { "limit-overshoot",       PL_LIMIT_OVERSHOOT       },
        { "split-basic",           PL_SPLIT_BASIC           },
        { "split-no-default",      PL_SPLIT_NO_DEFAULT      },
        { "split-first-wins",      PL_SPLIT_FIRST_WINS      },
    };

    /* csv.read typed: substitute the fixture path into the template. */
    char csv_yaml[2048];
    {
        const char *fixtures =
#ifdef BETL_TEST_FIXTURES_DIR
            BETL_TEST_FIXTURES_DIR;
#else
            "tests/fixtures";
#endif
        char csv_path[1024];
        snprintf(csv_path, sizeof csv_path, "%s/sample_typed.csv", fixtures);
        snprintf(csv_yaml, sizeof csv_yaml, PL_CSV_TYPED_FMT, csv_path);
    }
    for (size_t i = 0; i < sizeof ok_cases / sizeof ok_cases[0]; ++i) {
        char err[512] = {0};
        int rc = run_yaml(plugin_path, ok_cases[i].yaml, err, sizeof err);
        if (rc != BETL_OK) {
            fprintf(stderr, "case '%s' failed: %s\n", ok_cases[i].name, err);
        }
        CHECK(rc == BETL_OK);
    }
    {
        char err[512] = {0};
        int rc = run_yaml(plugin_path, csv_yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "csv-typed failed: %s\n", err);
        CHECK(rc == BETL_OK);
    }

    /* sort + log: capture log to verify the order is 3,2,1,0 (desc by id). */
    {
        char err[512] = {0};
        FILE *log = tmpfile();
        int rc = run_yaml_log(plugin_path, PL_SORT_DESC, err, sizeof err, log);
        if (rc != BETL_OK) fprintf(stderr, "sort-desc failed: %s\n", err);
        CHECK(rc == BETL_OK);
        char *txt = slurp_file(log);
        if (txt) {
            const char *s3 = strstr(txt, "seen=3");
            const char *s2 = strstr(txt, "seen=2");
            const char *s1 = strstr(txt, "seen=1");
            const char *s0 = strstr(txt, "seen=0");
            CHECK(s3 && s2 && s1 && s0);
            if (s3 && s2 && s1 && s0) {
                CHECK(s3 < s2);
                CHECK(s2 < s1);
                CHECK(s1 < s0);
            }
            free(txt);
        }
        fclose(log);
    }

    /* outer join + lua.map log: verify the four expected lid/rid pairs
     * appear, including the unmatched ones (which lua sees as nil). */
    {
        char err[512] = {0};
        FILE *log = tmpfile();
        static const char PL_OUTER_LOG[] =
            "betl: 1\n"
            "name: tx-join-outer-log\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: left_src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 4\n"
            "        column: lid\n"
            "      - id: right_src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 3\n"
            "        start: 2\n"
            "        column: rid\n"
            "      - id: jn\n"
            "        type: join\n"
            "        kind: outer\n"
            "        from: [left_src, right_src]\n"
            "        on: { lid: rid }\n"
            "      - id: probe\n"
            "        type: lua.map\n"
            "        from: jn\n"
            "        script: |\n"
            "          log.info('row lid=' .. tostring(row.lid) "
            ".. ' rid=' .. tostring(row.rid))\n"
            "          return row\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: probe\n"
            "        expect: 5\n";
        int rc = run_yaml_log(plugin_path, PL_OUTER_LOG, err, sizeof err, log);
        if (rc != BETL_OK) fprintf(stderr, "outer-log failed: %s\n", err);
        CHECK(rc == BETL_OK);
        char *txt = slurp_file(log);
        if (txt) {
            /* Matches: lid=2 rid=2, lid=3 rid=3 */
            CHECK(strstr(txt, "row lid=2 rid=2") != NULL);
            CHECK(strstr(txt, "row lid=3 rid=3") != NULL);
            /* Left-only (rid is null): lid=0,1 */
            CHECK(strstr(txt, "row lid=0 rid=nil") != NULL);
            CHECK(strstr(txt, "row lid=1 rid=nil") != NULL);
            /* Right-only (lid is null): rid=4 */
            CHECK(strstr(txt, "row lid=nil rid=4") != NULL);
            free(txt);
        }
        fclose(log);
    }

    /* Cases that should fail with a specific error keyword. */
    struct {
        const char *name;
        const char *yaml;
        const char *err_needle;
    } fail_cases[] = {
        { "select-and-add", PL_SELECT_AND_ADD,
          "only one of `add:` or `select:`" },
        { "select-bad-from", PL_SELECT_BAD_FROM,
          "unknown input column" },
        { "agg-bad-group-by", PL_AGG_BAD_GROUP_BY,
          "group_by column 'no_such' not found" },
        { "agg-bad-agg", PL_AGG_BAD_AGG,
          "unsupported agg" },
        { "join-bad-kind", PL_JOIN_BAD_KIND,
          "kind must be inner|left|outer" },
        { "union-bad-schema", PL_UNION_BAD_SCHEMA,
          "schema differs from input 0" },
        { "distinct-bad-key", PL_DISTINCT_BAD_KEY,
          "key column 'no_such' not found" },
        { "limit-no-n", PL_LIMIT_NO_N,
          "required `n:`" },
        { "limit-bad-n", PL_LIMIT_BAD_N,
          "must be a positive integer" },
        { "split-bad-port", PL_SPLIT_BAD_PORT,
          "no output port 'no_such_port'" },
        { "split-no-cases", PL_SPLIT_NO_CASES,
          "`cases:` list is empty" },
    };
    for (size_t i = 0; i < sizeof fail_cases / sizeof fail_cases[0]; ++i) {
        char err[512] = {0};
        int rc = run_yaml(plugin_path, fail_cases[i].yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        if (rc == BETL_OK) {
            fprintf(stderr, "case '%s' should have failed but didn't\n",
                    fail_cases[i].name);
        } else if (!strstr(err, fail_cases[i].err_needle)) {
            fprintf(stderr, "case '%s' wrong error: '%s' (expected '%s')\n",
                    fail_cases[i].name, err, fail_cases[i].err_needle);
            CHECK(0);
        }
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("transforms: all checks passed\n");
    return 0;
}
