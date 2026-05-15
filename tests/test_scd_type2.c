/* SCD type-2 recipe end-to-end. Exercises the example 05 pattern
 * (lookup → classify → split → union → insert + close-old) against
 * a real Postgres. Two batches:
 *
 *   Batch 1: staging has c1, c2, c3 (all NEW)
 *            → dim has 3 current rows, 0 closed
 *
 *   Batch 2: staging has c1 (unchanged), c2 (address changed), c4 (NEW)
 *            → dim has 4 current rows (c1, c2_new, c3 still there since
 *               the recipe doesn't handle deletes, c4), 1 closed (c2_old)
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

static int run_pipeline(const char *yaml_path, const char *batch_ts) {
    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(yaml_path, err, sizeof err);
    if (!p) { fprintf(stderr, "pipeline_load: %s\n", err); return -1; }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        betl_context_set_param(ctx, "batch_ts", batch_ts);
        char conn_err[256];
        rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
        if (rc == BETL_OK) rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK) {
            fprintf(stderr, "betl_run: %s\n", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    return rc;
}

/* Count rows matching a WHERE clause. Returns -1 on SQL error. */
static int count_where(PGconn *c, const char *schema_dim,
                       const char *where) {
    char sql[512];
    snprintf(sql, sizeof sql,
        "SELECT count(*) FROM %s.customer WHERE %s", schema_dim, where);
    PGresult *r = PQexec(c, sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -1; }
    int n = atoi(PQgetvalue(r, 0, 0));
    PQclear(r);
    return n;
}

int main(void) {
    const char *dsn = getenv("BETL_TEST_PG_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_PG_DSN", dsn, 1);
    }
    setenv("WAREHOUSE_DSN", dsn, 1);

    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "[skip] connect: %s", PQerrorMessage(c));
        PQfinish(c);
        return SKIP_RC;
    }

    /* Per-pid schemas so concurrent tests / re-runs don't trample. */
    char schema_dim[64], schema_stg[64];
    snprintf(schema_dim, sizeof schema_dim, "scd_dim_%d", (int)getpid());
    snprintf(schema_stg, sizeof schema_stg, "scd_stg_%d", (int)getpid());

    char ddl[2048];
    snprintf(ddl, sizeof ddl,
        "DROP SCHEMA IF EXISTS %s CASCADE; DROP SCHEMA IF EXISTS %s CASCADE; "
        "CREATE SCHEMA %s; CREATE SCHEMA %s;",
        schema_dim, schema_stg, schema_dim, schema_stg);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.customer ("
        "  customer_sk BIGSERIAL PRIMARY KEY, "
        "  customer_id INTEGER NOT NULL, "
        "  name TEXT, email TEXT, address TEXT, "
        "  valid_from TIMESTAMPTZ NOT NULL, "
        "  valid_to   TIMESTAMPTZ, "
        "  is_current BOOLEAN NOT NULL DEFAULT TRUE);",
        schema_dim);
    if (pg_exec(c, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
        "CREATE VIEW %s.customer_current AS "
        "  SELECT customer_sk, customer_id, name, email, address, "
        "         valid_from, valid_to "
        "    FROM %s.customer WHERE is_current;",
        schema_dim, schema_dim);
    if (pg_exec(c, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.customer "
        "  (customer_id INTEGER NOT NULL, name TEXT, email TEXT, address TEXT);",
        schema_stg);
    if (pg_exec(c, ddl) != 0) goto teardown;

    /* Build a per-test copy of the pipeline YAML — same shape as
     * examples/05-scd-type2/pipeline.betl.yml but with our per-pid
     * schema names substituted in. */
    char yaml[4096];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: scd-type2-test\n"
        "parameters:\n"
        "  batch_ts:\n"
        "    type: timestamp\n"
        "    required: true\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.WAREHOUSE_DSN}\n"
        "pipeline:\n"
        "  - id: scd_customer\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: stage\n"
        "        type: postgres.read\n"
        "        connection: warehouse\n"
        "        query: 'SELECT customer_id, name, email, address FROM %s.customer'\n"
        "      - id: current_dim\n"
        "        type: postgres.read\n"
        "        connection: warehouse\n"
        "        query: |\n"
        "          SELECT customer_id  AS dim_customer_id,\n"
        "                 customer_sk  AS dim_sk,\n"
        "                 name         AS dim_name,\n"
        "                 email        AS dim_email,\n"
        "                 address      AS dim_address\n"
        "            FROM %s.customer_current\n"
        "      - id: joined\n"
        "        type: join\n"
        "        kind: left\n"
        "        from: [stage, current_dim]\n"
        "        on: { customer_id: dim_customer_id }\n"
        "      - id: classify\n"
        "        type: map\n"
        "        from: joined\n"
        "        add:\n"
        "          scd_status:\n"
        "            lang: lua\n"
        "            expr: |\n"
        "              if row.dim_sk == nil then\n"
        "                return \"NEW\"\n"
        "              elseif row.name ~= row.dim_name "
                          "or row.email ~= row.dim_email "
                          "or row.address ~= row.dim_address then\n"
        "                return \"CHANGED\"\n"
        "              else\n"
        "                return \"UNCHANGED\"\n"
        "              end\n"
        "      - id: route\n"
        "        type: conditional_split\n"
        "        from: classify\n"
        "        cases:\n"
        "          - { name: new,     where: 'row.scd_status == \"NEW\"' }\n"
        "          - { name: changed, where: 'row.scd_status == \"CHANGED\"' }\n"
        "        default: unchanged\n"
        "      - id: to_insert\n"
        "        type: union\n"
        "        from: [route:new, route:changed]\n"
        "      - id: shape_new\n"
        "        type: map\n"
        "        from: to_insert\n"
        "        select: [customer_id, name, email, address]\n"
        "      - id: insert_new_version\n"
        "        type: postgres.exec\n"
        "        from: shape_new\n"
        "        connection: warehouse\n"
        "        sql: \"INSERT INTO %s.customer (customer_id, name, email, address, valid_from, is_current) VALUES ($1, $2, $3, $4, '${params.batch_ts}', TRUE)\"\n"
        "        parameters: [customer_id, name, email, address]\n"
        "      - id: close_old_version\n"
        "        type: postgres.exec\n"
        "        from: route:changed\n"
        "        connection: warehouse\n"
        "        sql: \"UPDATE %s.customer SET is_current = FALSE, valid_to = '${params.batch_ts}' WHERE customer_sk = $1\"\n"
        "        parameters: [dim_sk]\n",
        schema_stg, schema_dim, schema_dim, schema_dim);

    char yaml_path[128];
    snprintf(yaml_path, sizeof yaml_path,
             "/workspace/betl/build/betl-test-scd-%d.yml", (int)getpid());
    if (write_file(yaml_path, yaml) != 0) {
        fprintf(stderr, "write_file failed\n");
        goto teardown;
    }

    /* ---- Batch 1: three NEW customers ----------------------- */
    char seed[1024];
    snprintf(seed, sizeof seed,
        "INSERT INTO %s.customer VALUES "
        "  (1, 'Alice',   'a@x', 'Lane 1'), "
        "  (2, 'Bob',     'b@x', 'Lane 2'), "
        "  (3, 'Charlie', 'c@x', 'Lane 3');",
        schema_stg);
    if (pg_exec(c, seed) != 0) goto teardown;

    CHECK(run_pipeline(yaml_path, "2026-05-01T00:00:00+00") == BETL_OK);
    CHECK(count_where(c, schema_dim, "is_current = true")  == 3);
    CHECK(count_where(c, schema_dim, "is_current = false") == 0);
    CHECK(count_where(c, schema_dim,
        "customer_id = 1 AND is_current AND address = 'Lane 1'") == 1);

    /* ---- Batch 2: c1 unchanged, c2 changed address, c3 gone, c4 new
     *      The recipe doesn't model deletes — c3 stays current. */
    snprintf(seed, sizeof seed,
        "TRUNCATE %s.customer; "
        "INSERT INTO %s.customer VALUES "
        "  (1, 'Alice', 'a@x', 'Lane 1'),"
        "  (2, 'Bob',   'b@x', 'NEW ADDRESS'),"
        "  (4, 'Dora',  'd@x', 'Lane 4');",
        schema_stg, schema_stg);
    if (pg_exec(c, seed) != 0) goto teardown;

    CHECK(run_pipeline(yaml_path, "2026-05-02T00:00:00+00") == BETL_OK);

    /* dim now has:
     *   sk_1: customer 1, current   (no change)
     *   sk_2: customer 2, CLOSED at 2026-05-02
     *   sk_3: customer 3, current   (orphan — recipe doesn't model deletes)
     *   sk_4: customer 2 NEW version, current, address = NEW ADDRESS
     *   sk_5: customer 4, current
     * → 4 current, 1 closed */
    CHECK(count_where(c, schema_dim, "is_current = true")  == 4);
    CHECK(count_where(c, schema_dim, "is_current = false") == 1);
    CHECK(count_where(c, schema_dim,
        "customer_id = 2 AND is_current AND address = 'NEW ADDRESS'") == 1);
    CHECK(count_where(c, schema_dim,
        "customer_id = 2 AND is_current = false AND address = 'Lane 2'") == 1);
    CHECK(count_where(c, schema_dim,
        "customer_id = 4 AND is_current AND name = 'Dora'") == 1);
    /* c3 untouched */
    CHECK(count_where(c, schema_dim,
        "customer_id = 3 AND is_current AND address = 'Lane 3'") == 1);

    /* unlink(yaml_path);  -- keep on failure for inspection */
    if (failures == 0) unlink(yaml_path);

teardown:
    snprintf(ddl, sizeof ddl,
        "DROP SCHEMA IF EXISTS %s CASCADE; DROP SCHEMA IF EXISTS %s CASCADE;",
        schema_dim, schema_stg);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: scd_type2 integration test passed\n");
    return 0;
}
