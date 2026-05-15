/* postgres.copy end-to-end test.
 *
 * Drives gen_int64(N) → postgres.copy(table) and verifies N rows
 * landed via libpq. Also exercises the multi-column case with
 * gen_strings (id BIGINT, name TEXT). */

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
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
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

static int run_yaml(const char *yaml, char *err_buf, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-copy-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) return -1;
    BetlPipeline *p = betl_pipeline_load(path, err_buf, err_cap);
    unlink(path);
    if (!p) return -1;
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg
        && betl_register_builtins(reg) == BETL_OK
        && betl_apply_connections(ctx, p, err_buf, err_cap) == BETL_OK)
    {
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK) {
            snprintf(err_buf, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
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
    snprintf(schema, sizeof schema, "betl_copy_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.t1 (id BIGINT NOT NULL)", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.t2 (id BIGINT NOT NULL, name TEXT)", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    char yaml[2048];
    char err[1024];

    /* --- 1. Single-column int64 load. --- */
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-copy-int-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: 100\n"
        "      - id: sink\n"
        "        type: postgres.copy\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.t1\n",
        schema);
    err[0] = 0;
    int rc = run_yaml(yaml, err, sizeof err);
    if (rc != 0) fprintf(stderr, "single-col run failed: %s\n", err);
    CHECK(rc == 0);

    char sql[256];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s.t1", schema);
    PGresult *r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "100") == 0);
    }
    PQclear(r);

    /* --- 2. Two-column int64 + utf8 load. --- */
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-copy-mixed-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_strings\n"
        "        row_count: 50\n"
        "      - id: sink\n"
        "        type: postgres.copy\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.t2\n",
        schema);
    err[0] = 0;
    rc = run_yaml(yaml, err, sizeof err);
    if (rc != 0) fprintf(stderr, "multi-col run failed: %s\n", err);
    CHECK(rc == 0);

    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s.t2", schema);
    r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "50") == 0);
    }
    PQclear(r);

    snprintf(sql, sizeof sql,
        "SELECT count(*) FROM %s.t2 WHERE name = 'row_3'", schema);
    r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "1") == 0);
    }
    PQclear(r);

    /* --- 3. truncate: re-running with truncate=true wipes prior rows. --- */
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-copy-truncate-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: 10\n"
        "      - id: sink\n"
        "        type: postgres.copy\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.t1\n"
        "        truncate: true\n",
        schema);
    err[0] = 0;
    rc = run_yaml(yaml, err, sizeof err);
    if (rc != 0) fprintf(stderr, "truncate run failed: %s\n", err);
    CHECK(rc == 0);

    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s.t1", schema);
    r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        CHECK(strcmp(PQgetvalue(r, 0, 0), "10") == 0);
    }
    PQclear(r);

    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("ok: pg_copy integration test passed\n");
    return 0;
}
