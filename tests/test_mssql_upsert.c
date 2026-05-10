/* Integration test for mssql.upsert.
 *
 * Drives a parsed pipeline (gen_int64 → mssql.upsert) against the
 * sibling SQL Server exposed by the db-mssql MCP. The pipeline YAML
 * carries a `connections:` block with `${env.BETL_TEST_MSSQL_DSN}`
 * substitution, exercising both the parser and betl_apply_connections.
 *
 * Creates a disposable schema, runs the pipeline, verifies the row
 * count via ODBC, drops the schema. Exits 77 on connection failure
 * so CTest marks it Skipped rather than Failed when no DB is reachable. */

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

/* Connect, run a single statement that returns a single int. */
static int ms_select_int(SQLHDBC hdbc, const char *sql, int *out) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return -1;
    int rc = -1;
    if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR *)sql, SQL_NTS))) {
        if (SQLFetch(hstmt) == SQL_SUCCESS) {
            SQLINTEGER v = 0; SQLLEN ind = 0;
            if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_LONG, &v,
                                         sizeof v, &ind))) {
                *out = (int)v;
                rc = 0;
            }
        }
    } else {
        diag(SQL_HANDLE_STMT, hstmt, sql);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return rc;
}

/* FreeTDS via ODBC returns SQL_NO_DATA (100) for DDL like CREATE/DROP
 * SCHEMA — that's "this statement produced no result set," not an
 * error. Accept it alongside SUCCESS / SUCCESS_WITH_INFO so the test's
 * setup statements don't silently bail to teardown. */
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

int main(void) {
    const char *dsn = getenv("BETL_TEST_MSSQL_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_MSSQL_DSN", dsn, 1);
    }

    /* --- 1. Set up: connect, create disposable schema + table. --- */
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
    snprintf(schema, sizeof schema, "betl_test_%d", (int)getpid());
    char ddl[256];
    snprintf(ddl, sizeof ddl,
             "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
             "EXEC('DROP SCHEMA [%s]')", schema, schema);
    /* Best-effort cleanup if a previous run left state. Tables in the
     * schema would block the drop, but for a fresh PID the schema
     * shouldn't exist anyway. */
    ms_exec(hdbc, ddl);

    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
             "CREATE TABLE [%s].[upsert_demo] (id BIGINT PRIMARY KEY)",
             schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    /* --- 2. Pipeline: gen_int64 → mssql.upsert. --- */
    char yaml[1024];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: mssql-upsert-it\n"
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
        "        row_count: 5\n"
        "        column: id\n"
        "        start: 100\n"
        "      - id: sink\n"
        "        type: mssql.upsert\n"
        "        from: source\n"
        "        connection: warehouse\n"
        "        table: %s.upsert_demo\n"
        "        key: [id]\n",
        schema);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-mssql-upsert-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) {
        fprintf(stderr, "write_file failed\n");
        goto teardown;
    }

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
    int run_rc = BETL_ERR_INTERNAL;
    if (ctx && reg) {
        if (betl_register_builtins(reg) == BETL_OK
            && betl_apply_connections(ctx, p, err, sizeof err) == BETL_OK) {
            run_rc = betl_run(ctx, reg, p);
            if (run_rc != BETL_OK) {
                fprintf(stderr, "run rc=%d: %s\n",
                        run_rc, betl_context_last_error(ctx));
            }
        } else {
            fprintf(stderr, "setup failed: %s\n", err);
        }
    }
    CHECK(run_rc == BETL_OK);
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    /* --- 3. Verify the rows are present. --- */
    char q[256];
    snprintf(q, sizeof q,
             "SELECT COUNT(*) FROM [%s].[upsert_demo]", schema);
    int n = -1;
    CHECK(ms_select_int(hdbc, q, &n) == 0);
    CHECK(n == 5);

    snprintf(q, sizeof q,
             "SELECT MIN(id) FROM [%s].[upsert_demo]", schema);
    int mn = -1;
    CHECK(ms_select_int(hdbc, q, &mn) == 0);
    CHECK(mn == 100);

    snprintf(q, sizeof q,
             "SELECT MAX(id) FROM [%s].[upsert_demo]", schema);
    int mx = -1;
    CHECK(ms_select_int(hdbc, q, &mx) == 0);
    CHECK(mx == 104);

    /* --- 4. Re-run to exercise the UPDATE branch (rows already exist).
     * Result is unchanged: same five rows. --- */
    if (write_file(path, yaml) == 0) {
        p = betl_pipeline_load(path, err, sizeof err);
        unlink(path);
        if (p) {
            ctx = betl_context_create();
            reg = betl_registry_create();
            if (ctx && reg
                && betl_register_builtins(reg) == BETL_OK
                && betl_apply_connections(ctx, p, err, sizeof err) == BETL_OK)
            {
                CHECK(betl_run(ctx, reg, p) == BETL_OK);
            }
            betl_pipeline_free(p);
            betl_registry_destroy(reg);
            betl_context_destroy(ctx);
        }
    }
    n = -1;
    CHECK(ms_select_int(hdbc, q, &mx) == 0);
    snprintf(q, sizeof q,
             "SELECT COUNT(*) FROM [%s].[upsert_demo]", schema);
    CHECK(ms_select_int(hdbc, q, &n) == 0);
    CHECK(n == 5);

teardown:
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[upsert_demo]",
             schema);
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
    printf("ok: mssql_upsert integration test passed\n");
    return 0;
}
