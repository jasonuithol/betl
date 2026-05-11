/* Roundtrip test for postgres.upsert + DATE / TIMESTAMP binding.
 *
 * Creates a source table populated with date / timestamp data, then
 * runs a pg.read → pg.upsert pipeline that copies it into a target
 * table. We verify by reading the target back and comparing values.
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
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
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
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_PG_DSN", dsn, 1);
    }
    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "[skip] connect failed: %s", PQerrorMessage(c));
        PQfinish(c);
        return SKIP_RC;
    }

    char schema[64];
    snprintf(schema, sizeof schema, "betl_ud_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.src ("
        "  id bigint PRIMARY KEY,"
        "  ev_date date,"
        "  ev_at  timestamp"
        ")", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.dst ("
        "  id bigint PRIMARY KEY,"
        "  ev_date date,"
        "  ev_at  timestamp"
        ")", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "INSERT INTO %s.src VALUES "
        "  (1, DATE '2026-05-11', TIMESTAMP '2026-05-11 10:30:00.123456'),"
        "  (2, DATE '2024-02-29', TIMESTAMP '2024-02-29 23:59:59'),"
        "  (3, NULL,              NULL)",
        schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-upsert-date-it\n"
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
        "        query: SELECT id, ev_date, ev_at FROM %s.src ORDER BY id\n"
        "      - id: sink\n"
        "        type: postgres.upsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.dst\n"
        "        key: [id]\n",
        schema, schema);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-upsert-date-%d.yml",
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
    if (rc != BETL_OK) {
        fprintf(stderr, "apply_connections: %s\n", conn_err); ++failures;
    }
    rc = betl_run(ctx, reg, p);
    if (rc != BETL_OK) {
        fprintf(stderr, "betl_run rc=%d: %s\n", rc, betl_context_last_error(ctx));
        ++failures;
    }

    /* Verify by comparing src and dst row-for-row. */
    char sql[256];
    snprintf(sql, sizeof sql,
        "SELECT count(*) FROM ("
        "  SELECT id, ev_date, ev_at FROM %s.src EXCEPT "
        "  SELECT id, ev_date, ev_at FROM %s.dst"
        ") d", schema, schema);
    PGresult *r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        const char *cnt = PQgetvalue(r, 0, 0);
        CHECK(strcmp(cnt, "0") == 0);
        if (strcmp(cnt, "0") != 0) {
            fprintf(stderr, "FAIL: %s rows in src not present in dst\n", cnt);
        }
    }
    PQclear(r);

    /* And also: dst should have exactly 3 rows. */
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s.dst", schema);
    r = PQexec(c, sql);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "3") == 0);
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
    printf("ok: pg_upsert date/timestamp roundtrip test passed\n");
    return 0;
}
