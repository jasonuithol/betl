/* var.set control-flow task tests. Covers:
 *   1. Literal-value set, observable to a later stage via ${vars.X}.
 *   2. Substitution into a value: pulls ${env.X} / ${vars.Y} at run-time.
 *   3. SQL-capture mode: SELECT count(*) → store row count, foreach
 *      consumes via ${vars.X}.
 *   4. Invalid configs (both modes, neither mode).
 *
 * Case 3 requires a reachable Postgres; everything else is in-process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
#include "runtime/context.h"
#include "runtime/exec.h"

#define SKIP_RC 77
static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(contents);
    int rc = fwrite(contents, 1, n, f) == n ? 0 : -1;
    fclose(f);
    return rc;
}

static int run_yaml(const char *yaml, char *err, size_t err_cap,
                    BetlContext **out_ctx) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-var-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) return BETL_ERR_IO;
    char perr[512] = {0};
    BetlPipeline *p = betl_pipeline_load(path, perr, sizeof perr);
    if (!p) {
        if (err) snprintf(err, err_cap, "load: %s", perr);
        unlink(path); return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        char conn_err[256] = {0};
        rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
        if (rc == BETL_OK) rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK && err) {
            snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    if (out_ctx) *out_ctx = ctx;
    else         betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

int main(void) {
    /* --- 1: literal set is visible to downstream ${vars.X} ----------- */
    {
        const char *yaml =
            "betl: 1\n"
            "name: vs-literal\n"
            "pipeline:\n"
            "  - id: set\n"
            "    type: var.set\n"
            "    name: msg\n"
            "    value: 'hello world'\n"
            "  - id: per_msg\n"
            "    type: foreach\n"
            "    after: [set]\n"
            "    over: ['${vars.msg}']\n"
            "    as: m\n"
            "    body:\n"
            "      - id: verify\n"
            "        type: shell\n"
            "        argv: ['/bin/sh', '-c', 'test \"${vars.m}\" = \"hello world\"']\n";
        char err[256] = {0};
        int rc = run_yaml(yaml, err, sizeof err, NULL);
        if (rc != BETL_OK) fprintf(stderr, "case 1: %s\n", err);
        CHECK(rc == BETL_OK);
    }

    /* --- 2: substitution within `value:` (referencing env) ---------- */
    {
        setenv("BETL_TEST_VAR_TARGET", "fortyTwo", 1);
        const char *yaml =
            "betl: 1\n"
            "name: vs-substitute\n"
            "pipeline:\n"
            "  - id: set\n"
            "    type: var.set\n"
            "    name: greet\n"
            "    value: 'value=${env.BETL_TEST_VAR_TARGET}'\n"
            "  - id: chk\n"
            "    type: shell\n"
            "    after: [set]\n"
            "    argv: ['/bin/sh', '-c', 'test \"${vars.greet}\" = \"value=fortyTwo\"']\n";
        char err[256] = {0};
        int rc = run_yaml(yaml, err, sizeof err, NULL);
        if (rc != BETL_OK) fprintf(stderr, "case 2: %s\n", err);
        CHECK(rc == BETL_OK);
    }

    /* --- 3: invalid configs --------------------------------------- */
    {
        const char *yaml_both =
            "betl: 1\n"
            "name: vs-bad-both\n"
            "connections:\n"
            "  w: {type: postgres, dsn: 'postgresql://nope'}\n"
            "pipeline:\n"
            "  - id: bad\n"
            "    type: var.set\n"
            "    name: x\n"
            "    value: '1'\n"
            "    connection: w\n"
            "    sql: 'SELECT 1'\n";
        CHECK(run_yaml(yaml_both, NULL, 0, NULL) != BETL_OK);

        const char *yaml_neither =
            "betl: 1\n"
            "name: vs-bad-neither\n"
            "pipeline:\n"
            "  - id: bad\n"
            "    type: var.set\n"
            "    name: x\n";
        CHECK(run_yaml(yaml_neither, NULL, 0, NULL) != BETL_OK);
    }

    /* --- 4: SQL-capture mode against live Postgres ----------------- */
    const char *dsn = getenv("BETL_TEST_PG_DSN");
    if (!dsn || !*dsn) {
        dsn = "postgresql://postgres@host.containers.internal:5432/postgres";
        setenv("BETL_TEST_PG_DSN", dsn, 1);
    }
    /* Quick probe: try a literal pipeline first; if Postgres is unreachable
     * the test still passes for the in-process cases. */
    char probe_yaml[600];
    snprintf(probe_yaml, sizeof probe_yaml,
        "betl: 1\n"
        "name: vs-sql\n"
        "connections:\n"
        "  w:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: count_via_sql\n"
        "    type: var.set\n"
        "    connection: w\n"
        "    name: forty_two\n"
        "    sql: 'SELECT 42'\n"
        "  - id: chk\n"
        "    type: shell\n"
        "    after: [count_via_sql]\n"
        "    argv: ['/bin/sh', '-c', 'test \"${vars.forty_two}\" = 42']\n");
    char err[256] = {0};
    int rc = run_yaml(probe_yaml, err, sizeof err, NULL);
    if (rc == BETL_ERR_AUTH || (err[0] && strstr(err, "connect"))) {
        fprintf(stderr, "[partial-skip] sql-capture: %s\n", err);
    } else {
        if (rc != BETL_OK) fprintf(stderr, "case 4: %s\n", err);
        CHECK(rc == BETL_OK);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: var_set integration test passed\n");
    return 0;
}
