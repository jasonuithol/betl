/* Streaming stress test for mssql.read.
 *
 * Same shape as test_pg_read_stream but against the sibling MSSQL via
 * unixODBC + FreeTDS. Pushes ~10MB of result-set data through
 * mssql.read with a small batch_size and asserts that VmHWM delta
 * around betl_run stays bounded — i.e. that FreeTDS / unixODBC don't
 * silently buffer the whole result before the first SQLFetch returns. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sql.h>
#include <sqlext.h>

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
    return "Driver=/opt/projects/betl/deps/lib/odbc/libtdsodbc.so;"
           "Server=host.containers.internal;Port=1433;"
           "UID=sa;PWD=DevP@ssw0rd!42;Database=master;"
           "TDS_Version=7.4;";
}

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(contents);
    int rc = fwrite(contents, 1, len, f) == len ? 0 : -1;
    fclose(f);
    return rc;
}

static void diag(SQLSMALLINT type, SQLHANDLE h, const char *step) {
    SQLCHAR state[6], msg[400];
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRec(type, h, 1, state, &native,
                                    msg, sizeof msg, &msg_len))) {
        fprintf(stderr, "[%s] SQLSTATE=%s native=%d msg=%s\n",
                step, state, (int)native, msg);
    }
}

/* Run a one-shot statement. FreeTDS via ODBC returns SQL_NO_DATA (100)
 * for DDL like CREATE/DROP SCHEMA — that isn't an error, just "this
 * statement produced no result set." Accept it alongside SUCCESS /
 * SUCCESS_WITH_INFO so DDL setup doesn't silently goto teardown. */
static int ms_exec(SQLHDBC hdbc, const char *sql) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return -1;
    int rc = 0;
    SQLRETURN er = SQLExecDirect(hstmt, (SQLCHAR *)sql, SQL_NTS);
    if (!SQL_SUCCEEDED(er) && er != SQL_NO_DATA) {
        diag(SQL_HANDLE_STMT, hstmt, sql);
        rc = -1;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return rc;
}

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
    const char *dsn = getenv("BETL_TEST_MSSQL_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_MSSQL_DSN", dsn, 1);
    }
    SQLHENV  henv = SQL_NULL_HENV;
    SQLHDBC  hdbc = SQL_NULL_HDBC;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

    SQLCHAR out[1024];
    SQLSMALLINT out_len = 0;
    SQLRETURN rc = SQLDriverConnect(hdbc, NULL, (SQLCHAR *)dsn, SQL_NTS,
                                    out, sizeof out, &out_len,
                                    SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "[skip] connect failed:\n");
        diag(SQL_HANDLE_DBC, hdbc, "connect");
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
        return SKIP_RC;
    }

    char schema[64];
    snprintf(schema, sizeof schema, "betl_rs_%d", (int)getpid());
    char ddl[1024];
    snprintf(ddl, sizeof ddl,
             "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
             "EXEC('DROP SCHEMA [%s]')", schema, schema);
    ms_exec(hdbc, ddl);

    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema);
    if (ms_exec(hdbc, ddl) != 0) {
        fprintf(stderr, "FAIL: CREATE SCHEMA\n"); ++failures; goto teardown;
    }

    snprintf(ddl, sizeof ddl,
             "CREATE TABLE [%s].[bulk] (id BIGINT PRIMARY KEY, "
             "payload VARCHAR(%d))", schema, PAYLOAD_BYTES + 16);
    if (ms_exec(hdbc, ddl) != 0) {
        fprintf(stderr, "FAIL: CREATE TABLE\n"); ++failures; goto teardown;
    }

    /* Server-side row generation via cross-join over sys.all_objects.
     * MSSQL has no generate_series; this gives plenty of rows quickly. */
    snprintf(ddl, sizeof ddl,
             "INSERT INTO [%s].[bulk] (id, payload) "
             "SELECT TOP %d "
             "ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS id, "
             "REPLICATE('x', %d) AS payload "
             "FROM sys.all_objects a CROSS JOIN sys.all_objects b",
             schema, ROW_COUNT, PAYLOAD_BYTES);
    if (ms_exec(hdbc, ddl) != 0) {
        fprintf(stderr, "FAIL: bulk INSERT\n"); ++failures; goto teardown;
    }

    /* Verify the bulk insert actually populated the table. */
    {
        char q[160];
        snprintf(q, sizeof q, "SELECT COUNT(*) FROM [%s].[bulk]", schema);
        SQLHSTMT hs = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hs);
        int got_rows = -1;
        if (SQL_SUCCEEDED(SQLExecDirect(hs, (SQLCHAR *)q, SQL_NTS))
            && SQLFetch(hs) == SQL_SUCCESS) {
            SQLINTEGER v = 0; SQLLEN ind = 0;
            if (SQL_SUCCEEDED(SQLGetData(hs, 1, SQL_C_LONG, &v, sizeof v, &ind))) {
                got_rows = (int)v;
            }
        }
        SQLFreeHandle(SQL_HANDLE_STMT, hs);
        printf("[mssql_read_stream] inserted rows=%d (want %d)\n",
               got_rows, ROW_COUNT);
        if (got_rows != ROW_COUNT) {
            fprintf(stderr, "FAIL: insert count mismatch\n");
            ++failures; goto teardown;
        }
    }

    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: mssql-read-stream\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: mssql\n"
        "    dsn: ${env.BETL_TEST_MSSQL_DSN}\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: mssql.read\n"
        "        connection: warehouse\n"
        "        query: SELECT id, payload FROM [%s].[bulk] ORDER BY id\n"
        "        batch_size: %d\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: source\n"
        "        expect: %d\n",
        schema, BATCH_SIZE, ROW_COUNT);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-mssql-read-stream-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) goto teardown;

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    unlink(path);
    if (!p) {
        fprintf(stderr, "pipeline_load: %s\n", err);
        ++failures;
        goto teardown;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    CHECK(betl_register_builtins(reg) == BETL_OK);
    char conn_err[256];
    int rrc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rrc != BETL_OK) {
        fprintf(stderr, "apply_connections: %s\n", conn_err);
        ++failures;
    }

    long hwm_before = read_vm_hwm_kb();
    rrc = betl_run(ctx, reg, p);
    long hwm_after = read_vm_hwm_kb();

    if (rrc != BETL_OK) {
        fprintf(stderr, "betl_run rc=%d: %s\n", rrc,
                betl_context_last_error(ctx));
        ++failures;
    }

    long delta = (hwm_before > 0 && hwm_after > 0)
                 ? (hwm_after - hwm_before) : -1;
    printf("[mssql_read_stream] VmHWM before=%ld KB after=%ld KB delta=%ld KB "
           "(threshold=%d KB, %d rows × %d bytes)\n",
           hwm_before, hwm_after, delta,
           MAX_HWM_DELTA_KB, ROW_COUNT, PAYLOAD_BYTES);
    CHECK(delta >= 0);
    CHECK(delta < MAX_HWM_DELTA_KB);

    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

teardown:
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[bulk]", schema);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl, "DROP SCHEMA IF EXISTS [%s]", schema);
    ms_exec(hdbc, ddl);

    SQLDisconnect(hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: mssql_read_stream test passed\n");
    return 0;
}
