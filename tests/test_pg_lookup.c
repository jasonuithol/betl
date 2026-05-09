/* Integration test for the postgres `lookup` TRANSFORM (SPEC §4.3).
 *
 * Drives gen_int64 → lookup → lua.map → count_rows against the sibling
 * Postgres. The lookup table has three (id, color) rows; the pipeline
 * generates ids 1..3, joins to the cached colors, logs each row, and
 * the test then verifies the captured log contains the expected color
 * for each id. Skips on no DB.
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
    if (!ok) fprintf(stderr, "SQL fail [%s]: %s", sql, PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

static char *slurp(FILE *f) {
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

int main(int argc, char **argv) {
    /* argv[1] is the path to betl-lua.so so we can register the lua
     * engine and use lua.map for log-based verification. */
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
    snprintf(schema, sizeof schema, "betl_lk_%d", (int)getpid());
    char ddl[256];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
             "CREATE TABLE %s.dim_color (id bigint PRIMARY KEY, color text)", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
             "INSERT INTO %s.dim_color VALUES (1,'red'),(2,'green'),(3,'blue')", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    /* Pipeline: gen 3 ids 1..3, lookup color, log "id=N color=X", count. */
    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-lookup-it\n"
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
        "        row_count: 3\n"
        "        column: id\n"
        "        start: 1\n"
        "      - id: lk\n"
        "        type: lookup\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.dim_color\n"
        "        match:  { id: id }\n"
        "        select: { color: color }\n"
        "        on_miss: error\n"
        "      - id: log\n"
        "        type: lua.map\n"
        "        from: lk\n"
        "        script: |\n"
        "          log.info('id=' .. row.id .. ' color=' .. row.color)\n"
        "          return row\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: log\n"
        "        expect: 3\n",
        schema);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-lookup-%d.yml", (int)getpid());
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
    rc = betl_registry_load(reg, plugin_path);
    if (rc != BETL_OK) {
        fprintf(stderr, "load lua plugin: %s\n", betl_registry_last_error(reg));
        ++failures;
    }

    char conn_err[256];
    rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rc != BETL_OK) {
        fprintf(stderr, "apply_connections: %s\n", conn_err);
        ++failures;
    }

    FILE *log = tmpfile();
    if (log) {
        betl_context_set_log_stream(ctx, log);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    }
    rc = betl_run(ctx, reg, p);
    if (rc != BETL_OK) {
        fprintf(stderr, "betl_run rc=%d: %s\n", rc,
                betl_context_last_error(ctx));
        ++failures;
    }

    if (log) {
        char *txt = slurp(log);
        if (txt) {
            CHECK(strstr(txt, "id=1 color=red")   != NULL);
            CHECK(strstr(txt, "id=2 color=green") != NULL);
            CHECK(strstr(txt, "id=3 color=blue")  != NULL);
            free(txt);
        }
        fclose(log);
    }

    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);

    /* Cleanup PG side. */
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: pg_lookup integration test passed\n");
    return 0;
}
