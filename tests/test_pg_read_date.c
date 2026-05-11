/* Integration test for postgres.read + DATE / TIMESTAMP columns.
 *
 * Creates a small table with a date column, a timestamp column, and an
 * id, then drives postgres.read directly via the component API. We
 * verify:
 *   - the emitted Arrow schema has "tdD" for the date column and
 *     "tsu:" for the timestamp column
 *   - the int32 days-since-epoch values are correct
 *   - the int64 micros-since-epoch values are correct (including a
 *     row with fractional seconds)
 *   - NULL cells land in the validity bitmap
 *   - TIMESTAMPTZ is rejected with a clear error
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <libpq-fe.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
#include "runtime/context.h"
#include "runtime/date_util.h"
#include "pipeline/pipeline.h"

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

static int bit_clear(const uint8_t *bm, size_t i) {
    return bm == NULL ? 0 : !((bm[i / 8] >> (i % 8)) & 1u);
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
    snprintf(schema, sizeof schema, "betl_dt_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.events ("
        "  id bigint PRIMARY KEY,"
        "  ev_date date,"
        "  ev_at  timestamp"
        ")", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
        "INSERT INTO %s.events VALUES "
        "  (1, DATE '2026-05-11', TIMESTAMP '2026-05-11 10:30:00.123456'),"
        "  (2, DATE '2026-05-12', TIMESTAMP '2026-05-12 00:00:00'),"
        "  (3, NULL,              NULL)",
        schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    /* Build the runtime objects and the postgres.read state. */
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    int rc = betl_register_builtins(reg);
    CHECK(rc == BETL_OK);

    /* Apply a manual connection: skip YAML and call set_connection. */
    char conn_json[512];
    snprintf(conn_json, sizeof conn_json, "{\"dsn\":\"%s\"}", dsn);
    betl_context_set_connection(ctx, "warehouse", conn_json);

    const BetlComponentDef *src = betl_registry_find(reg, "postgres.read");
    CHECK(src != NULL);
    if (!src) goto done;

    char cfg[400];
    snprintf(cfg, sizeof cfg,
        "{\"connection\":\"warehouse\","
        " \"query\":\"SELECT id, ev_date, ev_at FROM %s.events ORDER BY id\","
        " \"batch_size\":16}", schema);

    void *st = NULL;
    rc = src->init(ctx, cfg, &st);
    CHECK(rc == BETL_OK);
    if (rc != BETL_OK) {
        fprintf(stderr, "init: %s\n", betl_context_last_error(ctx));
        goto done;
    }

    struct ArrowArrayStream stream = {0};
    rc = src->attach_output(st, 0, &stream);
    CHECK(rc == BETL_OK);

    struct ArrowSchema sch = {0};
    rc = stream.get_schema(&stream, &sch);
    CHECK(rc == 0);
    CHECK(sch.n_children == 3);
    if (sch.n_children == 3) {
        CHECK(strcmp(sch.children[0]->format, "l")    == 0);  /* id */
        CHECK(strcmp(sch.children[1]->format, "tdD")  == 0);  /* ev_date */
        CHECK(strcmp(sch.children[2]->format, "tsu:") == 0);  /* ev_at */
    }

    struct ArrowArray batch = {0};
    rc = stream.get_next(&stream, &batch);
    CHECK(rc == 0);
    CHECK(batch.length == 3);
    if (batch.length == 3 && batch.n_children == 3) {
        /* id column — three rows of int64 */
        const int64_t *ids = batch.children[0]->buffers[1];
        CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3);

        /* ev_date — int32 days since epoch */
        const struct ArrowArray *dcol = batch.children[1];
        const uint8_t *dvalid = dcol->null_count > 0 ? dcol->buffers[0] : NULL;
        const int32_t *days = dcol->buffers[1];
        CHECK(days[0] == betl_days_from_civil(2026, 5, 11));
        CHECK(days[1] == betl_days_from_civil(2026, 5, 12));
        CHECK(bit_clear(dvalid, 2));  /* row 3 is null */
        CHECK(dcol->null_count == 1);

        /* ev_at — int64 micros since epoch */
        const struct ArrowArray *tcol = batch.children[2];
        const uint8_t *tvalid = tcol->null_count > 0 ? tcol->buffers[0] : NULL;
        const int64_t *us = tcol->buffers[1];
        int64_t expect0 = (int64_t)betl_days_from_civil(2026, 5, 11) * 86400000000LL
                        + (int64_t)10 * 3600000000LL
                        + (int64_t)30 *   60000000LL
                        + 123456LL;
        int64_t expect1 = (int64_t)betl_days_from_civil(2026, 5, 12) * 86400000000LL;
        CHECK(us[0] == expect0);
        CHECK(us[1] == expect1);
        CHECK(bit_clear(tvalid, 2));
        CHECK(tcol->null_count == 1);
    }
    if (batch.release)  batch.release(&batch);
    if (sch.release)    sch.release(&sch);
    if (stream.release) stream.release(&stream);
    src->destroy(st);

    /* And — a TIMESTAMPTZ column should be rejected with a clear error. */
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE %s.events_tz (id bigint, ev timestamptz)", schema);
    if (pg_exec(c, ddl) == 0) {
        char cfg2[256];
        snprintf(cfg2, sizeof cfg2,
            "{\"connection\":\"warehouse\","
            " \"query\":\"SELECT id, ev FROM %s.events_tz\"}", schema);
        void *st2 = NULL;
        rc = src->init(ctx, cfg2, &st2);
        if (rc == BETL_OK && st2) {
            /* init may succeed lazily; schema resolution happens on first
             * get_schema / get_next. */
            struct ArrowArrayStream s2 = {0};
            int ar = src->attach_output(st2, 0, &s2);
            if (ar == BETL_OK) {
                struct ArrowSchema sch2 = {0};
                int gs = s2.get_schema(&s2, &sch2);
                CHECK(gs != 0);
                CHECK(strstr(betl_context_last_error(ctx), "TIMESTAMP WITH TIME ZONE") != NULL);
                if (sch2.release) sch2.release(&sch2);
                if (s2.release)   s2.release(&s2);
            }
            src->destroy(st2);
        }
    }

done:
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    pg_exec(c, ddl);
    PQfinish(c);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: pg_read date/timestamp integration test passed\n");
    return 0;
}
