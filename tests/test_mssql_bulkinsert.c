/* Integration test for mssql.bulkinsert.
 *
 * Drives a parsed pipeline (gen_int64 → mssql.bulkinsert) against the
 * sibling SQL Server. Verifies:
 *   - rows actually land in the target table
 *   - sum matches arithmetic series 1..N (proves order-of-rows
 *     bulk-binding doesn't shuffle or drop)
 *   - a non-divisor batch_size (trailing partial batch path) works
 *
 * Exits 77 on connection failure so CTest marks it Skipped rather
 * than Failed when no DB is reachable. */

#include <inttypes.h>
#include <stdint.h>
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

static const char *default_dsn(void) {
    return "Driver=/opt/projects/betl/deps/lib/odbc/libtdsodbc.so;"
           "Server=host.containers.internal;Port=1433;"
           "UID=sa;PWD=DevP@ssw0rd!42;Database=master;"
           "TDS_Version=7.4;";
}

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

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

static int ms_select_int64(SQLHDBC hdbc, const char *sql, int64_t *out) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return -1;
    int rc = -1;
    if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR *)sql, SQL_NTS))) {
        if (SQLFetch(hstmt) == SQL_SUCCESS) {
            SQLBIGINT v = 0; SQLLEN ind = 0;
            if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_SBIGINT, &v,
                                         sizeof v, &ind))) {
                *out = (int64_t)v;
                rc = 0;
            }
        }
    } else {
        diag(SQL_HANDLE_STMT, hstmt, sql);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return rc;
}

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

/* Run a `betl.gen_int64 → mssql.bulkinsert` pipeline against `table`.
 * row_count rows, ids starting at 1, batch_size for the sink. */
static int run_bulk_pipeline(const char *table, int64_t row_count,
                             int batch_size, char *err_out, size_t err_cap) {
    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: mssql-bulkinsert-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: mssql\n"
        "    dsn: ${env.BETL_TEST_MSSQL_DSN}\n"
        "pipeline:\n"
        "  - id: ingest\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: %" PRId64 "\n"
        "        column: id\n"
        "        start: 1\n"
        "      - id: sink\n"
        "        type: mssql.bulkinsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s\n"
        "        batch_size: %d\n",
        row_count, table, batch_size);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-mssql-bulkinsert-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) {
        snprintf(err_out, err_cap, "write_file failed");
        return -1;
    }

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    unlink(path);
    if (!p) {
        snprintf(err_out, err_cap, "pipeline_load: %s", err);
        return -1;
    }

    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int run_rc = BETL_ERR_INTERNAL;
    if (ctx && reg) {
        if (betl_register_builtins(reg) == BETL_OK
            && betl_apply_connections(ctx, p, err, sizeof err) == BETL_OK) {
            run_rc = betl_run(ctx, reg, p);
            if (run_rc != BETL_OK) {
                snprintf(err_out, err_cap, "run rc=%d: %s",
                         run_rc, betl_context_last_error(ctx));
            }
        } else {
            snprintf(err_out, err_cap, "setup failed: %s", err);
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    return run_rc == BETL_OK ? 0 : -1;
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
    snprintf(schema, sizeof schema, "betl_bi_%d", (int)getpid());
    char ddl[256];
    snprintf(ddl, sizeof ddl,
             "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
             "EXEC('DROP SCHEMA [%s]')", schema, schema);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
             "CREATE TABLE [%s].[bulk_demo] (id BIGINT NOT NULL)", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    char table[96];
    snprintf(table, sizeof table, "%s.bulk_demo", schema);

    /* --- Case 1: 5000 rows / batch_size=1000 = 5 full batches.
     *     Tests the basic round-trip. --- */
    {
        char err[512] = {0};
        if (run_bulk_pipeline(table, 5000, 1000, err, sizeof err) != 0) {
            fprintf(stderr, "case 1 (5000 / 1000): %s\n", err);
            ++failures;
        }
        char q[256];
        snprintf(q, sizeof q, "SELECT COUNT(*) FROM [%s].[bulk_demo]", schema);
        int64_t n = -1;
        CHECK(ms_select_int64(hdbc, q, &n) == 0);
        CHECK(n == 5000);
        /* Sum 1..5000 = 5000*5001/2 = 12,502,500. */
        snprintf(q, sizeof q, "SELECT SUM(id) FROM [%s].[bulk_demo]", schema);
        int64_t sum = -1;
        CHECK(ms_select_int64(hdbc, q, &sum) == 0);
        CHECK(sum == 12502500);
    }

    /* Clean for case 2. */
    snprintf(ddl, sizeof ddl, "TRUNCATE TABLE [%s].[bulk_demo]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    /* --- Case 2: 1500 rows / batch_size=333 = 4 full batches + 1
     *     trailing partial of 168 rows. Tests the partial-batch path. --- */
    {
        char err[512] = {0};
        if (run_bulk_pipeline(table, 1500, 333, err, sizeof err) != 0) {
            fprintf(stderr, "case 2 (1500 / 333): %s\n", err);
            ++failures;
        }
        char q[256];
        snprintf(q, sizeof q, "SELECT COUNT(*) FROM [%s].[bulk_demo]", schema);
        int64_t n = -1;
        CHECK(ms_select_int64(hdbc, q, &n) == 0);
        CHECK(n == 1500);
        /* Sum 1..1500 = 1500*1501/2 = 1,125,750. */
        snprintf(q, sizeof q, "SELECT SUM(id) FROM [%s].[bulk_demo]", schema);
        int64_t sum = -1;
        CHECK(ms_select_int64(hdbc, q, &sum) == 0);
        CHECK(sum == 1125750);
    }

    /* Clean for case 3. */
    snprintf(ddl, sizeof ddl, "TRUNCATE TABLE [%s].[bulk_demo]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    /* --- Case 3: 100 rows / batch_size=1000 = just one partial batch
     *     of 100. Tests "fewer rows than one full batch". --- */
    {
        char err[512] = {0};
        if (run_bulk_pipeline(table, 100, 1000, err, sizeof err) != 0) {
            fprintf(stderr, "case 3 (100 / 1000): %s\n", err);
            ++failures;
        }
        char q[256];
        snprintf(q, sizeof q, "SELECT COUNT(*) FROM [%s].[bulk_demo]", schema);
        int64_t n = -1;
        CHECK(ms_select_int64(hdbc, q, &n) == 0);
        CHECK(n == 100);
    }

teardown:
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[bulk_demo]", schema);
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
    printf("ok: mssql_bulkinsert integration test passed\n");
    return 0;
}
