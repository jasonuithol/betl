/* Integration test for postgres.upsert.
 *
 * Drives a parsed pipeline (gen_int64 → postgres.upsert) against the
 * sibling Postgres exposed by the db-postgres MCP. The pipeline YAML
 * carries a real `connections:` block with `${env.BETL_TEST_PG_DSN}`
 * substitution, so we exercise both the parser and betl_apply_connections.
 *
 * Creates a disposable schema, runs the pipeline, verifies via libpq,
 * drops the schema. Exits 77 on connection failure so CTest marks it
 * Skipped rather than Failed when no DB is reachable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
#include "runtime/context.h"
#include "runtime/exec.h"

#define SKIP_RC 77

static const char *default_dsn(void) {
    return "postgresql://postgres@host.containers.internal:5432/postgres";
}

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(contents);
    int rc = fwrite(contents, 1, len, f) == len ? 0 : -1;
    fclose(f);
    return rc;
}

static int pg_exec(PGconn *c, const char *sql) {
    PGresult *r = PQexec(c, sql);
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK
          || PQresultStatus(r) == PGRES_TUPLES_OK;
    if (!ok) {
        fprintf(stderr, "SQL fail [%s]: %s", sql, PQerrorMessage(c));
    }
    PQclear(r);
    return ok ? 0 : -1;
}

int main(void) {
    const char *dsn = getenv("BETL_TEST_PG_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        /* The pipeline YAML reads BETL_TEST_PG_DSN via ${env.X}, so we
         * need to make sure it's set even when we fell back to the
         * compiled-in default. */
        setenv("BETL_TEST_PG_DSN", dsn, 1);
    }

    /* --- 1. Set up: connect, create disposable schema + table ------------ */
    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "[skip] connect failed: %s", PQerrorMessage(c));
        PQfinish(c);
        return SKIP_RC;
    }
    char schema[64];
    snprintf(schema, sizeof schema, "betl_test_%d", (int)getpid());
    char ddl[256];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
             "CREATE TABLE %s.upsert_demo (id bigint PRIMARY KEY)", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    /* --- 2. Pipeline: gen_int64 → postgres.upsert ------------------------ */
    char yaml[1024];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-upsert-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: ingest\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: 5\n"
        "        column: id\n"
        "        start: 100\n"
        "      - id: sink\n"
        "        type: postgres.upsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.upsert_demo\n"
        "        key: [id]\n",
        schema);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-upsert-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) {
        fprintf(stderr, "could not write %s\n", path);
        PQfinish(c);
        return 1;
    }

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "pipeline_load: %s\n", err);
        unlink(path);
        PQfinish(c);
        return 1;
    }

    /* --- 3. Context, registry, connection, run --------------------------- */
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);

    int rc = betl_register_builtins(reg);
    CHECK(rc == BETL_OK);

    /* Apply the YAML-declared connections to the context, resolving
     * ${env.BETL_TEST_PG_DSN} as we go. */
    char conn_err[256];
    rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rc != BETL_OK) {
        fprintf(stderr, "apply_connections: %s\n", conn_err);
        ++failures;
    }

    rc = betl_run(ctx, reg, p);
    if (rc != BETL_OK) {
        fprintf(stderr, "betl_run failed (rc=%d): %s\n", rc,
                betl_context_last_error(ctx));
        ++failures;
    }

    /* --- 4. Verify rows landed in the target table ----------------------- */
    char select_sql[128];
    snprintf(select_sql, sizeof select_sql,
             "SELECT count(*), min(id), max(id) FROM %s.upsert_demo",
             schema);
    PGresult *r = PQexec(c, select_sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        fprintf(stderr, "verify SELECT failed: %s\n", PQerrorMessage(c));
        ++failures;
    } else {
        const char *count = PQgetvalue(r, 0, 0);
        const char *min_id = PQgetvalue(r, 0, 1);
        const char *max_id = PQgetvalue(r, 0, 2);
        CHECK(strcmp(count,  "5")   == 0);
        CHECK(strcmp(min_id, "100") == 0);
        CHECK(strcmp(max_id, "104") == 0);
    }
    PQclear(r);

    /* --- 5. Re-run to exercise the upsert (DO NOTHING) path -------------- */
    rc = betl_run(ctx, reg, p);
    if (rc != BETL_OK) {
        fprintf(stderr, "second betl_run failed (rc=%d): %s\n", rc,
                betl_context_last_error(ctx));
        ++failures;
    }
    r = PQexec(c, select_sql);
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "5") == 0);
    } else {
        ++failures;
    }
    PQclear(r);

    /* --- 6. Teardown ----------------------------------------------------- */
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);

    snprintf(ddl, sizeof ddl, "DROP SCHEMA %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures) {
        fprintf(stderr, "FAIL: %d failure(s)\n", failures);
        return 1;
    }
    printf("ok: pg_upsert integration test passed\n");
    return 0;
}
