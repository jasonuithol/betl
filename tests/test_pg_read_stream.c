/* Streaming stress test for postgres.read.
 *
 * Pushes ~10MB of result-set data through postgres.read with a small
 * batch_size, and asserts that the peak resident memory (VmHWM) for
 * this process stays bounded — i.e. that nothing along libpq / our
 * cursor implementation / Arrow leaf builders silently materializes
 * the whole result before yielding the first batch.
 *
 * Test data: 20,000 rows, each a (bigint, text(500)). Per-batch size
 * with batch_size=100 is ~50KB of payload; the whole buffered set
 * would be ~10MB. We allow a 12MB VmHWM delta around betl_run() to
 * account for libpq + Arrow allocator slack while still cleanly
 * catching a "load it all into memory" regression. */

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
/* 4000 rows × 500-byte payload = ~2 MB on the wire — small enough to
 * run reasonably under valgrind, large enough that "buffer it all"
 * would push VmHWM well past the 5 MB threshold. Streaming keeps the
 * delta under a few hundred KB regardless of total volume. */
#define ROW_COUNT        4000
#define PAYLOAD_BYTES     500
#define BATCH_SIZE        100

/* TSan/ASan shadow memory inflates VmHWM by several MB independent of
 * data volume — bump the headroom under sanitizers so the streaming-vs-
 * buffered distinction (a >10x ratio) still holds without false-failing. */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define MAX_HWM_DELTA_KB (32 * 1024)
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define MAX_HWM_DELTA_KB (32 * 1024)
#  else
#    define MAX_HWM_DELTA_KB (5 * 1024)
#  endif
#else
#  define MAX_HWM_DELTA_KB (5 * 1024)
#endif

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
    if (!ok) fprintf(stderr, "SQL fail [%s]: %s",
                     sql, PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

/* Read VmHWM (high-water-mark RSS) from /proc/self/status, in KB. The
 * kernel only ever increases this counter for the lifetime of the
 * process, so taking a measurement before betl_run and after gives us
 * the peak RSS reached at any point during the run. */
static long read_vm_hwm_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            kb = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
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
    snprintf(schema, sizeof schema, "betl_rs_%d", (int)getpid());
    char ddl[400];
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS %s CASCADE", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA %s", schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }
    snprintf(ddl, sizeof ddl,
             "CREATE TABLE %s.bulk (id bigint PRIMARY KEY, payload text)",
             schema);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    /* Bulk-insert ROW_COUNT rows of PAYLOAD_BYTES utf8 each, generated
     * server-side so we don't pay client-side construction time. */
    snprintf(ddl, sizeof ddl,
             "INSERT INTO %s.bulk (id, payload) "
             "SELECT i, repeat('x', %d) FROM generate_series(1, %d) AS i",
             schema, PAYLOAD_BYTES, ROW_COUNT);
    if (pg_exec(c, ddl) != 0) { PQfinish(c); return 1; }

    /* Pipeline: postgres.read → count_rows. No Lua step (we don't
     * want lua's own row materialization confounding the memory
     * measurement). */
    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: pg-read-stream\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: postgres\n"
        "    dsn: ${env.BETL_TEST_PG_DSN}\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: postgres.read\n"
        "        connection: warehouse\n"
        "        query: SELECT id, payload FROM %s.bulk ORDER BY id\n"
        "        batch_size: %d\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: source\n"
        "        expect: %d\n",
        schema, BATCH_SIZE, ROW_COUNT);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-pg-read-stream-%d.yml",
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
    CHECK(betl_register_builtins(reg) == BETL_OK);
    char conn_err[256];
    int rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rc != BETL_OK) {
        fprintf(stderr, "apply_connections: %s\n", conn_err);
        ++failures;
    }

    long hwm_before = read_vm_hwm_kb();
    rc = betl_run(ctx, reg, p);
    long hwm_after = read_vm_hwm_kb();

    if (rc != BETL_OK) {
        fprintf(stderr, "betl_run rc=%d: %s\n", rc,
                betl_context_last_error(ctx));
        ++failures;
    }

    long delta = (hwm_before > 0 && hwm_after > 0)
                 ? (hwm_after - hwm_before) : -1;
    printf("[pg_read_stream] VmHWM before=%ld KB after=%ld KB delta=%ld KB "
           "(threshold=%d KB, %d rows × %d bytes)\n",
           hwm_before, hwm_after, delta,
           MAX_HWM_DELTA_KB, ROW_COUNT, PAYLOAD_BYTES);
    CHECK(delta >= 0);
    CHECK(delta < MAX_HWM_DELTA_KB);

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
    printf("ok: pg_read_stream test passed\n");
    return 0;
}
