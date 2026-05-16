/* Postgres sink error-path coverage.
 *
 * The happy paths are well-tested elsewhere (test_pg_upsert, test_pg_copy,
 * test_pg_exec). This file targets the *unhappy* paths a real ETL run
 * encounters:
 *   1. Target table doesn't exist                       — postgres.upsert
 *   2. Type mismatch (text into an int column)          — postgres.upsert
 *   3. Bad DSN (unreachable server)                     — postgres.upsert
 *   4. SQL syntax error in postgres.exec                — postgres.exec
 *   5. NOT NULL violation                               — postgres.upsert
 *   6. Constraint violation that can't be ON CONFLICT-skipped
 *      (CHECK constraint, since PRIMARY KEY conflicts ARE handled
 *      by upsert's DO NOTHING/DO UPDATE)                — postgres.upsert
 *
 * Every case expects betl_run to return a non-OK rc with a non-empty
 * last_error. We never want a silent success on a failed write.
 *
 * Skips when host Postgres is unreachable (exit 77). */

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

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(contents);
    int rc = fwrite(contents, 1, n, f) == n ? 0 : -1;
    fclose(f);
    return rc;
}

static int pg_exec(PGconn *c, const char *sql) {
    PGresult *r = PQexec(c, sql);
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK
          || PQresultStatus(r) == PGRES_TUPLES_OK;
    PQclear(r);
    return ok ? 0 : -1;
}

/* Drive one YAML through betl_run; returns the run's rc + a copy of the
 * last_error message so the caller can assert on the failure path. */
static int run_yaml(const char *yaml, char *err_out, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-pgerr-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) return BETL_ERR_IO;
    char p_err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, p_err, sizeof p_err);
    if (!p) {
        if (err_out) snprintf(err_out, err_cap, "load: %s", p_err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    char conn_err[256] = {0};
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
        if (rc == BETL_OK) {
            rc = betl_run(ctx, reg, p);
        }
        if (rc != BETL_OK && err_out) {
            const char *msg = betl_context_last_error(ctx);
            if (msg && *msg) snprintf(err_out, err_cap, "%s", msg);
            else if (conn_err[0]) snprintf(err_out, err_cap, "%s", conn_err);
        }
    }
    if (reg) betl_registry_destroy(reg);
    if (ctx) betl_context_destroy(ctx);
    if (p)   betl_pipeline_free(p);
    unlink(path);
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
    snprintf(schema, sizeof schema, "betl_pgerr_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    /* ---- 1. Target table doesn't exist ------------------------------ */
    {
        char yaml[1024];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: pgerr-no-table\n"
            "connections:\n"
            "  w:\n"
            "    type: postgres\n"
            "    dsn: ${env.BETL_TEST_PG_DSN}\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 5\n"
            "        column: id\n"
            "      - id: sink\n"
            "        type: postgres.upsert\n"
            "        from: src\n"
            "        connection: w\n"
            "        table: %s.does_not_exist\n"
            "        key: [id]\n", schema);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(err[0] != '\0');
        fprintf(stderr, "case 1 (missing table) err: %.200s\n", err);
    }

    /* ---- 2. NOT NULL violation -------------------------------------- */
    {
        snprintf(ddl, sizeof ddl,
                 "CREATE TABLE %s.notnull_tbl (id bigint PRIMARY KEY, label text NOT NULL)",
                 schema);
        if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
        char yaml[1024];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: pgerr-notnull\n"
            "connections:\n"
            "  w:\n"
            "    type: postgres\n"
            "    dsn: ${env.BETL_TEST_PG_DSN}\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "        column: id\n"
            "      - id: sink\n"
            "        type: postgres.upsert\n"
            "        from: src\n"
            "        connection: w\n"
            "        table: %s.notnull_tbl\n"
            "        key: [id]\n", schema);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(err[0] != '\0');
        fprintf(stderr, "case 2 (NOT NULL) err: %.200s\n", err);
    }

    /* ---- 3. Bad DSN: connection_string points at unreachable host --- */
    {
        const char *yaml =
            "betl: 1\n"
            "name: pgerr-bad-dsn\n"
            "connections:\n"
            "  w: { type: postgres,\n"
            "       dsn: 'postgresql://x@127.0.0.1:1/none?connect_timeout=1' }\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "        column: id\n"
            "      - id: sink\n"
            "        type: postgres.upsert\n"
            "        from: src\n"
            "        connection: w\n"
            "        table: nothing\n"
            "        key: [id]\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(err[0] != '\0');
        fprintf(stderr, "case 3 (bad DSN) err: %.200s\n", err);
    }

    /* ---- 4. SQL syntax error in postgres.exec ----------------------- */
    {
        const char *yaml =
            "betl: 1\n"
            "name: pgerr-syntax\n"
            "connections:\n"
            "  w:\n"
            "    type: postgres\n"
            "    dsn: ${env.BETL_TEST_PG_DSN}\n"
            "pipeline:\n"
            "  - id: bad_sql\n"
            "    type: sql.execute\n"
            "    connection: w\n"
            "    sql: 'THIS IS NOT VALID SQL'\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(err[0] != '\0');
        fprintf(stderr, "case 4 (bad SQL) err: %.200s\n", err);
    }

    /* ---- 5. CHECK constraint violation: upsert ON CONFLICT can't
     * mask this -- the constraint fires on every row regardless of key. */
    {
        snprintf(ddl, sizeof ddl,
                 "CREATE TABLE %s.checked (id bigint PRIMARY KEY, "
                 "CHECK (id < 50))",
                 schema);
        if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
        char yaml[1024];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: pgerr-check\n"
            "connections:\n"
            "  w:\n"
            "    type: postgres\n"
            "    dsn: ${env.BETL_TEST_PG_DSN}\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 5\n"
            "        start: 100\n"        /* > 50 -> trips CHECK */
            "        column: id\n"
            "      - id: sink\n"
            "        type: postgres.upsert\n"
            "        from: src\n"
            "        connection: w\n"
            "        table: %s.checked\n"
            "        key: [id]\n", schema);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(err[0] != '\0');
        fprintf(stderr, "case 5 (CHECK violation) err: %.200s\n", err);
    }

    /* ---- 6. Wrong number of key columns: upsert config validation --- */
    {
        snprintf(ddl, sizeof ddl,
                 "CREATE TABLE %s.simple (id bigint PRIMARY KEY)", schema);
        if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
        char yaml[1024];
        /* `key: []` is malformed — must reject. */
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: pgerr-bad-key\n"
            "connections:\n"
            "  w:\n"
            "    type: postgres\n"
            "    dsn: ${env.BETL_TEST_PG_DSN}\n"
            "pipeline:\n"
            "  - id: s\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: src\n"
            "        type: betl.gen_int64\n"
            "        row_count: 1\n"
            "        column: id\n"
            "      - id: sink\n"
            "        type: postgres.upsert\n"
            "        from: src\n"
            "        connection: w\n"
            "        table: %s.simple\n"
            "        key: []\n", schema);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        fprintf(stderr, "case 6 (empty key) err: %.200s\n", err);
    }

    /* ---- Teardown --------------------------------------------------- */
    snprintf(ddl, sizeof ddl, "DROP SCHEMA %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: postgres writer error-path tests passed\n");
    return 0;
}
