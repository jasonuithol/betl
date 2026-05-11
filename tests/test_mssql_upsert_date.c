/* Roundtrip test for mssql.upsert + DATE / DATETIME2 binding.
 *
 * Mirror of pg_upsert_date, against MSSQL via ODBC. Reads a source
 * table that includes DATE and DATETIME2(6) columns, upserts into a
 * target, then verifies the target contains the same rows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

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
    return "Driver=/opt/projects/betl/deps/lib/odbc/libtdsodbc.so;"
           "Server=host.containers.internal;Port=1433;"
           "UID=sa;PWD=DevP@ssw0rd!42;Database=master;"
           "TDS_Version=7.4;";
}

static int ms_exec(SQLHDBC hdbc, const char *sql) {
    SQLHSTMT h = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &h);
    SQLRETURN r = SQLExecDirect(h, (SQLCHAR *)sql, SQL_NTS);
    int ok = SQL_SUCCEEDED(r) || r == SQL_NO_DATA;
    if (!ok) {
        SQLCHAR state[6], msg[512];
        SQLINTEGER native = 0; SQLSMALLINT ml = 0;
        SQLGetDiagRec(SQL_HANDLE_STMT, h, 1, state, &native, msg, sizeof msg, &ml);
        fprintf(stderr, "SQL fail [%s]: %s\n", sql, msg);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, h);
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
    const char *dsn = getenv("BETL_TEST_MSSQL_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_MSSQL_DSN", dsn, 1);
    }

    SQLHENV henv = SQL_NULL_HENV; SQLHDBC hdbc = SQL_NULL_HDBC;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    SQLCHAR out[1024]; SQLSMALLINT olen = 0;
    SQLRETURN cr = SQLDriverConnect(hdbc, NULL, (SQLCHAR *)dsn, SQL_NTS,
                                    out, sizeof out, &olen, SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(cr)) {
        SQLCHAR state[6], msg[512]; SQLINTEGER native = 0; SQLSMALLINT ml = 0;
        SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, state, &native, msg, sizeof msg, &ml);
        fprintf(stderr, "[skip] connect failed: %s\n", msg);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
        return SKIP_RC;
    }

    char schema[64];
    snprintf(schema, sizeof schema, "betl_ud_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl,
        "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
        "EXEC('DROP SCHEMA [%s]')", schema, schema);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE [%s].[src] ("
        "  id BIGINT PRIMARY KEY,"
        "  ev_date DATE NULL,"
        "  ev_at DATETIME2(6) NULL"
        ")", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE [%s].[dst] ("
        "  id BIGINT PRIMARY KEY,"
        "  ev_date DATE NULL,"
        "  ev_at DATETIME2(6) NULL"
        ")", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;
    snprintf(ddl, sizeof ddl,
        "INSERT INTO [%s].[src] VALUES "
        "  (1, '2026-05-11', '2026-05-11 10:30:00.123456'),"
        "  (2, '2024-02-29', '2024-02-29 23:59:59'),"
        "  (3, NULL, NULL)",
        schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: mssql-upsert-date-it\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: mssql\n"
        "    dsn: ${env.BETL_TEST_MSSQL_DSN}\n"
        "pipeline:\n"
        "  - id: copy\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: mssql.read\n"
        "        connection: warehouse\n"
        "        query: SELECT id, ev_date, ev_at FROM [%s].[src] ORDER BY id\n"
        "      - id: sink\n"
        "        type: mssql.upsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.dst\n"
        "        key: [id]\n",
        schema, schema);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-mssql-upsert-date-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) goto teardown;

    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "pipeline_load: %s\n", err);
        unlink(path);
        ++failures;
        goto teardown;
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

    /* Verify: count rows where src and dst differ. SQL Server doesn't
     * have EXCEPT-friendly behaviour on NULL by default; use NOT EXISTS
     * with explicit NULL-safe comparison. */
    SQLHSTMT h = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &h);
    char sql[768];
    snprintf(sql, sizeof sql,
        "SELECT COUNT(*) FROM [%s].[src] s "
        "WHERE NOT EXISTS ("
        "  SELECT 1 FROM [%s].[dst] d WHERE d.id = s.id "
        "    AND ((d.ev_date = s.ev_date) OR (d.ev_date IS NULL AND s.ev_date IS NULL))"
        "    AND ((d.ev_at   = s.ev_at)   OR (d.ev_at   IS NULL AND s.ev_at   IS NULL))"
        ")", schema, schema);
    if (SQL_SUCCEEDED(SQLExecDirect(h, (SQLCHAR *)sql, SQL_NTS))) {
        if (SQL_SUCCEEDED(SQLFetch(h))) {
            SQLBIGINT v = 0; SQLLEN ind = 0;
            SQLGetData(h, 1, SQL_C_SBIGINT, &v, sizeof v, &ind);
            CHECK(v == 0);
            if (v != 0) {
                fprintf(stderr, "FAIL: %lld rows in src missing from dst\n",
                        (long long)v);
            }
        }
    } else { CHECK(0); }
    SQLFreeHandle(SQL_HANDLE_STMT, h);

    /* And: dst should have exactly 3 rows. */
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &h);
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM [%s].[dst]", schema);
    if (SQL_SUCCEEDED(SQLExecDirect(h, (SQLCHAR *)sql, SQL_NTS))) {
        if (SQL_SUCCEEDED(SQLFetch(h))) {
            SQLBIGINT v = 0; SQLLEN ind = 0;
            SQLGetData(h, 1, SQL_C_SBIGINT, &v, sizeof v, &ind);
            CHECK(v == 3);
        }
    }
    SQLFreeHandle(SQL_HANDLE_STMT, h);

    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);

teardown:
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[src]", schema);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[dst]", schema);
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
    printf("ok: mssql_upsert date/datetime2 roundtrip test passed\n");
    return 0;
}
