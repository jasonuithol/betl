/* Pipeline parser tests:
 *
 *   1. Loading the three example pipelines and asserting their shape:
 *      stage count, kinds, step counts, names, key edges.
 *   2. Negative cases: every fixture under tests/fixtures/bad_*.yml
 *      must fail to parse, and the error string must mention the path
 *      and a relevant diagnostic.
 *
 * The fixture and example paths are passed in as argv. CMake fills
 * them in via add_test args; for direct invocation they fall back to
 * baked-in defaults that point at the build/source layout. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipeline/pipeline.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

#define CHECK_STREQ(a, b) do {                                  \
    const char *_a = (a), *_b = (b);                            \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                    \
        fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n",       \
                __FILE__, __LINE__,                             \
                _a ? _a : "(null)", _b ? _b : "(null)");        \
        failures++;                                             \
    }                                                           \
} while (0)

static void test_example_01(const char *path) {
    char err[1024];
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "load %s: %s\n", path, err);
        failures++;
        return;
    }

    CHECK_STREQ(betl_pipeline_name(p), "orders-daily-ingest");
    CHECK(betl_pipeline_stage_count(p) == 2);
    CHECK(betl_pipeline_total_steps(p) == 5);

    const BetlStage *ingest = betl_pipeline_find_stage(p, "ingest_orders");
    CHECK(ingest != NULL);
    if (ingest) {
        CHECK(ingest->kind == BETL_STAGE_DATAFLOW);
        CHECK(ingest->step_count == 5);
        CHECK(ingest->after_count == 0);
        /* Step order matches source order. */
        CHECK_STREQ(ingest->steps[0].id,   "read");
        CHECK_STREQ(ingest->steps[0].type, "csv.read");
        CHECK(ingest->steps[0].input_count == 0);
        CHECK_STREQ(ingest->steps[1].id, "clean");
        CHECK(ingest->steps[1].input_count == 1);
        CHECK_STREQ(ingest->steps[1].inputs[0], "read");
        CHECK_STREQ(ingest->steps[4].id, "write");
        CHECK_STREQ(ingest->steps[4].type, "postgres.upsert");
        CHECK_STREQ(ingest->steps[4].inputs[0], "tag_load");
    }

    const BetlStage *check = betl_pipeline_find_stage(p, "row_count_check");
    CHECK(check != NULL);
    if (check) {
        CHECK(check->kind == BETL_STAGE_TASK);
        CHECK_STREQ(check->task_type, "sql.execute");
        CHECK(check->step_count == 0);
        CHECK(check->after_count == 1);
        CHECK_STREQ(check->after[0], "ingest_orders");
    }

    /* Lookup of an unknown id returns NULL. */
    CHECK(betl_pipeline_find_stage(p, "no_such_stage") == NULL);

    /* Connections block: example 01 declares one (`warehouse: postgres`)
     * with a ${env.WAREHOUSE_DSN} placeholder. */
    CHECK(betl_pipeline_connection_count(p) == 1);
    const BetlConnectionDecl *wh = betl_pipeline_connection(p, 0);
    CHECK(wh != NULL);
    if (wh) {
        CHECK_STREQ(wh->name, "warehouse");
        CHECK(wh->config_json != NULL);
        if (wh->config_json) {
            CHECK(strstr(wh->config_json, "\"type\":\"postgres\"") != NULL);
            /* Substitution placeholders survive parsing as plain strings;
             * resolution happens in the apply step, not the parser. */
            CHECK(strstr(wh->config_json, "${env.WAREHOUSE_DSN}") != NULL);
        }
    }

    /* Parameters: src_dir (required string) and load_date (date with
     * `default: today` sentinel). */
    CHECK(betl_pipeline_parameter_count(p) == 2);
    const BetlParameterDecl *src_dir = NULL, *load_date = NULL;
    for (size_t i = 0; i < betl_pipeline_parameter_count(p); ++i) {
        const BetlParameterDecl *pa = betl_pipeline_parameter(p, i);
        if (strcmp(pa->name, "src_dir")   == 0) src_dir   = pa;
        if (strcmp(pa->name, "load_date") == 0) load_date = pa;
    }
    CHECK(src_dir != NULL);
    if (src_dir) {
        CHECK_STREQ(src_dir->type, "string");
        CHECK(src_dir->required == 1);
        CHECK(src_dir->has_default == 0);
        CHECK(src_dir->is_sentinel == 0);
        CHECK(src_dir->doc != NULL);
    }
    CHECK(load_date != NULL);
    if (load_date) {
        CHECK_STREQ(load_date->type, "date");
        CHECK(load_date->required == 0);
        CHECK(load_date->has_default == 1);
        CHECK(load_date->is_sentinel == 1);
        CHECK_STREQ(load_date->default_value, "today");
    }

    /* config_json sanity check on a known step. The first step in
     * `ingest_orders` is `read` (csv.read). The serialized config must
     * include the basic keys and unquoted booleans/strings. */
    if (ingest && ingest->step_count > 0) {
        const char *cfg = ingest->steps[0].config_json;
        CHECK(cfg != NULL);
        if (cfg) {
            CHECK(strstr(cfg, "\"id\":\"read\"")        != NULL);
            CHECK(strstr(cfg, "\"type\":\"csv.read\"")  != NULL);
            CHECK(strstr(cfg, "\"header\":true")        != NULL);  /* plain `true` */
            CHECK(strstr(cfg, "\"delimiter\":\",\"")    != NULL);  /* quoted "," */
        }
    }
    /* Task stages also carry config_json. */
    if (check) {
        CHECK(check->task_config_json != NULL);
        if (check->task_config_json) {
            CHECK(strstr(check->task_config_json, "\"sql.execute\"") != NULL);
            CHECK(strstr(check->task_config_json, "\"connection\":\"warehouse\"") != NULL);
        }
    }

    betl_pipeline_free(p);
}

static void test_example_02(const char *path) {
    char err[1024];
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "load %s: %s\n", path, err);
        failures++;
        return;
    }

    CHECK_STREQ(betl_pipeline_name(p), "build-sales-star");
    CHECK(betl_pipeline_stage_count(p) == 4);

    const BetlStage *fact = betl_pipeline_find_stage(p, "load_fact_sales");
    CHECK(fact != NULL);
    if (fact) {
        CHECK(fact->kind == BETL_STAGE_DATAFLOW);
        CHECK(fact->step_count == 5);
        CHECK(fact->after_count == 2);
        /* `after:` order matters for diagnostics; preserve it. */
        CHECK_STREQ(fact->after[0], "load_dim_customer");
        CHECK_STREQ(fact->after[1], "load_dim_product");
    }

    const BetlStage *refresh = betl_pipeline_find_stage(p, "refresh_marts");
    CHECK(refresh != NULL);
    if (refresh) {
        CHECK(refresh->kind == BETL_STAGE_TASK);
        CHECK_STREQ(refresh->task_type, "sql.execute");
        CHECK(refresh->after_count == 1);
        CHECK_STREQ(refresh->after[0], "load_fact_sales");
    }

    betl_pipeline_free(p);
}

static void test_example_03(const char *path) {
    char err[1024];
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "load %s: %s\n", path, err);
        failures++;
        return;
    }

    CHECK_STREQ(betl_pipeline_name(p), "crm-migration");
    CHECK(betl_pipeline_stage_count(p) == 3);

    const BetlStage *cust = betl_pipeline_find_stage(p, "migrate_customers");
    CHECK(cust != NULL);
    if (cust) {
        CHECK(cust->kind == BETL_STAGE_DATAFLOW);
        CHECK(cust->step_count == 4);
        CHECK(cust->after_count == 0);
    }

    const BetlStage *orders = betl_pipeline_find_stage(p, "migrate_orders");
    CHECK(orders != NULL);
    if (orders) {
        CHECK(orders->after_count == 1);
        CHECK_STREQ(orders->after[0], "migrate_customers");
    }

    const BetlStage *addrs = betl_pipeline_find_stage(p, "migrate_addresses");
    CHECK(addrs != NULL);
    if (addrs) {
        CHECK(addrs->after_count == 1);
        CHECK_STREQ(addrs->after[0], "migrate_customers");
    }

    /* Example 03 declares no inline connections; both come from the
     * `betl_connections:` bundle pulled in via `include:`. After load
     * we should see both `legacy_crm` and `new_crm` on the pipeline. */
    CHECK(betl_pipeline_connection_count(p) == 2);
    int saw_legacy = 0, saw_new = 0;
    for (size_t i = 0; i < betl_pipeline_connection_count(p); ++i) {
        const BetlConnectionDecl *c = betl_pipeline_connection(p, i);
        if (strcmp(c->name, "legacy_crm") == 0) saw_legacy = 1;
        if (strcmp(c->name, "new_crm")    == 0) saw_new    = 1;
    }
    CHECK(saw_legacy && saw_new);

    betl_pipeline_free(p);
}

/* Each negative case: load must fail and the error string must contain
 * the given diagnostic substring AND a file path (so we know it's
 * properly attributed). For include-cascade errors the error may be
 * attributed to a different file than the one passed to load — pass
 * `attrib_basename` to the helper to specify which path basename the
 * error must mention; pass NULL to fall back to the loaded path. */
static void expect_bad_at(const char *path,
                          const char *attrib_basename,
                          const char *needle) {
    char err[1024];
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (p) {
        fprintf(stderr,
            "FAIL: expected %s to fail (looking for '%s'), but it loaded\n",
            path, needle);
        betl_pipeline_free(p);
        failures++;
        return;
    }
    if (err[0] == '\0') {
        fprintf(stderr, "FAIL: %s failed but error message is empty\n", path);
        failures++;
        return;
    }
    const char *attrib = attrib_basename ? attrib_basename : path;
    if (!strstr(err, attrib)) {
        fprintf(stderr,
            "FAIL: %s error doesn't mention attribution path '%s': %s\n",
            path, attrib, err);
        failures++;
    }
    if (!strstr(err, needle)) {
        fprintf(stderr,
            "FAIL: %s error missing expected substring '%s': %s\n",
            path, needle, err);
        failures++;
    }
}

int main(int argc, char **argv) {
    /* argv layout (matched by tests/CMakeLists.txt):
     *   1 = examples dir
     *   2 = fixtures dir */
    const char *examples = (argc > 1) ? argv[1]
#ifdef BETL_TEST_EXAMPLES_DIR
        : BETL_TEST_EXAMPLES_DIR;
#else
        : "examples";
#endif
    const char *fixtures = (argc > 2) ? argv[2]
#ifdef BETL_TEST_FIXTURES_DIR
        : BETL_TEST_FIXTURES_DIR;
#else
        : "tests/fixtures";
#endif

    char path[1024];

    /* --- positive cases --------------------------------------------- */
    snprintf(path, sizeof path, "%s/01-csv-to-postgres/pipeline.betl.yml", examples);
    test_example_01(path);
    snprintf(path, sizeof path, "%s/02-build-sales-star/pipeline.betl.yml", examples);
    test_example_02(path);
    snprintf(path, sizeof path, "%s/03-crm-migration/pipeline.betl.yml",   examples);
    test_example_03(path);

    /* --- negative cases --------------------------------------------- */
    /* `attrib` is the basename the error attribution must mention. NULL
     * means "the file we tried to load" — used for everything except
     * include-cascade errors, which surface from a different file. */
    struct {
        const char *name;
        const char *attrib;
        const char *needle;
    } cases[] = {
        { "bad_no_pipeline.yml",     NULL,
                                     "missing required `pipeline:`"        },
        { "bad_dup_stage_id.yml",    NULL,
                                     "duplicate stage id 'do_thing'"       },
        { "bad_dup_step_id.yml",     NULL,
                                     "duplicate step id 'read'"            },
        { "bad_after_unknown.yml",   NULL,
                                     "undefined stage 'no_such_stage'"     },
        { "bad_from_unknown.yml",    NULL,
                                     "undefined sibling 'no_such_step'"    },
        { "bad_stage_cycle.yml",     NULL, "cycle"                         },
        { "bad_step_cycle.yml",      NULL, "cycle"                         },
        { "bad_step_no_type.yml",    NULL,
                                     "missing required scalar `type`"      },
        { "bad_dataflow_empty.yml",  NULL,
                                     "empty `steps:` sequence"             },
        { "bad_self_after.yml",      NULL, "depends on itself"             },
        { "bad_conn_no_dsn.yml",     NULL,
                                     "missing required scalar `dsn`"       },
        { "bad_conn_inline_password.yml", NULL,
                                     "inline literal password"             },
        /* Cycle: error gets reported from inside the recursion, points
         * at one of the two files in the loop. */
        { "bad_include_cycle_a.yml", "bad_include_cycle_",
                                     "include cycle"                       },
        { "bad_include_missing.yml", NULL,
                                     "this_file_does_not_exist_42.yml"     },
        { "bad_bundle_root.yml",     "bad_bundle_with_pipeline.yml",
                                     "may only contribute connections"     },
        { "bad_param_no_type.yml",   NULL,
                                     "missing required scalar `type`"      },
        { "bad_param_required_with_default.yml", NULL,
                                     "incompatible with `default`"         },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", fixtures, cases[i].name);
        expect_bad_at(path, cases[i].attrib, cases[i].needle);
    }

    /* --- file-level errors ------------------------------------------ */
    {
        char err[256];
        BetlPipeline *p = betl_pipeline_load("/no/such/file.yml", err, sizeof err);
        CHECK(p == NULL);
        CHECK(strstr(err, "cannot open") != NULL);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("pipeline: all checks passed\n");
    return 0;
}
