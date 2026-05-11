/* Roundtrip test for postgres.upsert + NUMERIC columns.
 *
 * Mirrors test_pg_upsert_date — populates a source table with NUMERIC
 * data, runs pg.read → pg.upsert into a target, verifies via SQL
 * EXCEPT that every row landed identically (including NULLs).
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
static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static const char *default_dsn(void) {
    return "postgresql://postgres@host.containers.internal:5432/postgres";
}

static int pg_exec(PGconn *c, const char *sql) {
    PGresult *r = PQexec(c, sql);
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK
          || PQresultStatus(r) == PGRES_TUPLES_OK;
    if (!ok) fprintf(stderr, "SQL fail [%s]: %s", sql, PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(contents);
    int rc = fwrite(contents, 1, len, f) == len ? 0 : -1;
    fclose(f);
    return rc;
}

int main(void) {
    const char *dsn = getenv("BETL_TEST_PG_DSN");
    if (!dsn || !*dsn) { dsn = default_dsn(); setenv("BETL_TEST_PG_DSN", dsn, 1); }
    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "[skip] connect failed: %s", PQerrorMessage(c));
        PQfinish(c);
        return SKIP_RC;
    }

    char schema[64];
    snprintf(schema, sizeof schema, "betl_dec_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.src ("
        "  id     BIGINT PRIMARY KEY,"
        "  amount NUMERIC(12, 2),"          /* currency */
        "  ratio  NUMERIC(20, 9)"           /* high-precision */
        ")", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.dst ("
        "  id     BIGINT PRIMARY KEY,"
        "  amount NUMERIC(12, 2),"
        "  ratio  NUMERIC(20, 9)"
        ")", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "INSERT INTO %s.src VALUES "
        "  (1, 123.45,        3.141592653),"
        "  (2, -99999999.99,  0.000000001),"   /* near max + smallest */
        "  (3, 0.00,          NULL),"
        "  (4, NULL,          NULL)",
        schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-upsert-decimal-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: copy\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: postgres.read\n"
        "        connection: warehouse\n"
        "        query: SELECT id, amount, ratio FROM %s.src ORDER BY id\n"
        "      - id: sink\n"
        "        type: postgres.upsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.dst\n"
        "        key: [id]\n",
        schema, schema);
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-upsert-decimal-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) { PQfinish(c); return 1; }

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "pipeline_load: %s\n", err);
        unlink(path); PQfinish(c); return 1;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    int rc = betl_register_builtins(reg);
    CHECK(rc == BETL_OK);
    char conn_err[256];
    rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rc != BETL_OK) { fprintf(stderr, "apply_connections: %s\n", conn_err); ++failures; }
    rc = betl_run(ctx, reg, p);
    if (rc != BETL_OK) {
        fprintf(stderr, "betl_run rc=%d: %s\n", rc, betl_context_last_error(ctx));
        ++failures;
    }

    /* Diff src and dst. */
    char sql[384];
    snprintf(sql, sizeof sql,
        "SELECT count(*) FROM ("
        "  SELECT id, amount, ratio FROM %s.src EXCEPT "
        "  SELECT id, amount, ratio FROM %s.dst"
        ") d", schema, schema);
    PGresult *r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        const char *cnt = PQgetvalue(r, 0, 0);
        CHECK(strcmp(cnt, "0") == 0);
        if (strcmp(cnt, "0") != 0) {
            fprintf(stderr, "FAIL: %s rows in src missing from dst\n", cnt);
        }
    }
    PQclear(r);

    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s.dst", schema);
    r = PQexec(c, sql);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "4") == 0);
    }
    PQclear(r);

    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);

    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: pg_upsert decimal roundtrip test passed\n");
    return 0;
}
