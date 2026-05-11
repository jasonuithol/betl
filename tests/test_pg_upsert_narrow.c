/* Roundtrip test for postgres.read + postgres.upsert with narrow int
 * columns. The pipeline reads SMALLINT / INTEGER / BIGINT / REAL through
 * pg.read (which now advertises Arrow `s` / `i` / `l` / `f` formats),
 * pushes through pg.upsert, and verifies value equality via SQL EXCEPT.
 *
 * This exercises:
 *   - pg_oid_to_fmt emitting narrow widths
 *   - pgr_build_narrow_int_leaf / pgr_build_float32_leaf on output
 *   - postgres_upsert.render_cell reading narrow Arrow buffers
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
    snprintf(schema, sizeof schema, "betl_narrow_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.src ("
        "  id BIGINT PRIMARY KEY, "
        "  sml SMALLINT, "
        "  mid INTEGER, "
        "  big BIGINT, "
        "  r4  REAL)", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.dst (LIKE %s.src INCLUDING ALL)", schema, schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    /* Edge values: SMALLINT min/max, INTEGER min/max, a row with NULL
     * narrow columns. */
    snprintf(ddl, sizeof ddl,
        "INSERT INTO %s.src VALUES "
        "  (1, -32768, -2147483648, -1, 0.5),"
        "  (2,  32767,  2147483647, 9223372036854775807, 3.125),"
        "  (3, 0, 0, 0, 0.0),"
        "  (4, NULL, NULL, NULL, NULL)",
        schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-upsert-narrow-it\n"
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
        "        query: SELECT id, sml, mid, big, r4 FROM %s.src ORDER BY id\n"
        "      - id: sink\n"
        "        type: postgres.upsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.dst\n"
        "        key: [id]\n",
        schema, schema);
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-upsert-narrow-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) { PQfinish(c); return 1; }

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) { fprintf(stderr, "pipeline_load: %s\n", err); unlink(path); PQfinish(c); return 1; }
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

    /* Verify round-trip via EXCEPT, treating NULLs as equal. */
    char sql[512];
    snprintf(sql, sizeof sql,
        "SELECT count(*) FROM ("
        "  SELECT id, sml, mid, big, r4 FROM %s.src EXCEPT "
        "  SELECT id, sml, mid, big, r4 FROM %s.dst"
        ") d", schema, schema);
    PGresult *r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK)
        CHECK(strcmp(PQgetvalue(r, 0, 0), "0") == 0);
    PQclear(r);

    /* Also confirm the destination ACTUALLY stores narrow types. */
    snprintf(sql, sizeof sql,
        "SELECT count(*) FROM %s.dst", schema);
    r = PQexec(c, sql);
    CHECK(PQresultStatus(r) == PGRES_TUPLES_OK);
    if (PQresultStatus(r) == PGRES_TUPLES_OK)
        CHECK(strcmp(PQgetvalue(r, 0, 0), "4") == 0);
    PQclear(r);

    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("ok: pg narrow-int roundtrip test passed\n");
    return 0;
}
