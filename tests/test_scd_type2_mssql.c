/* SCD type-2 recipe (example 06) end-to-end against SQL Server.
 *
 * Mirrors test_scd_type2.c (Postgres). Same two-batch flow:
 *   Batch 1: c1, c2, c3 all NEW           → 3 current, 0 closed
 *   Batch 2: c1 unchanged, c2 address chg → 4 current (c1, c2_new,
 *            c3 still orphaned, c4 new),    1 closed (c2_old)
 *
 * Per-pid schema names so concurrent runs don't trample each other.
 * Generates a one-off pipeline YAML on the fly that mirrors the
 * example 06 shape but with the test's schema names baked in.
 */

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
static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static const char *default_dsn(void) {
    return "Driver=/opt/projects/betl/deps/lib/odbc/libtdsodbc.so;"
           "Server=host.containers.internal;Port=1433;"
           "UID=sa;PWD=DevP@ssw0rd!42;Database=master;"
           "TDS_Version=7.4;";
}

static void diag(SQLSMALLINT type, SQLHANDLE h, const char *step) {
    SQLCHAR state[6] = {0}, msg[400] = {0};
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRec(type, h, 1, state, &native,
                                    msg, sizeof msg, &msg_len))) {
        fprintf(stderr, "[%s] SQLSTATE=%s native=%d msg=%s\n",
                step, state, (int)native, msg);
    }
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

static int ms_count(SQLHDBC hdbc, const char *sql) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return -1;
    int result = -1;
    if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR *)sql, SQL_NTS))) {
        if (SQLFetch(hstmt) == SQL_SUCCESS) {
            SQLINTEGER v = 0; SQLLEN ind = 0;
            if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_LONG, &v, sizeof v, &ind)))
                result = (int)v;
        }
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return result;
}

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(contents);
    int rc = fwrite(contents, 1, len, f) == len ? 0 : -1;
    fclose(f);
    return rc;
}

static int run_pipeline(const char *yaml_path, const char *batch_ts,
                        const char *lua_plugin_path) {
    char err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(yaml_path, err, sizeof err);
    if (!p) { fprintf(stderr, "pipeline_load: %s\n", err); return -1; }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        if (lua_plugin_path
            && betl_registry_load(reg, lua_plugin_path) != BETL_OK) {
            fprintf(stderr, "load lua: %s\n", betl_registry_last_error(reg));
        }
        betl_context_set_param(ctx, "batch_ts", batch_ts);
        char conn_err[256];
        rc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
        if (rc == BETL_OK) rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK)
            fprintf(stderr, "betl_run: %s\n", betl_context_last_error(ctx));
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    return rc;
}

int main(int argc, char **argv) {
    const char *lua_plugin = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
#endif
    const char *dsn = getenv("BETL_TEST_MSSQL_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_MSSQL_DSN", dsn, 1);
    }
    setenv("WAREHOUSE_DSN", dsn, 1);

    SQLHENV henv = SQL_NULL_HENV;
    SQLHDBC hdbc = SQL_NULL_HDBC;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

    SQLCHAR out[1024]; SQLSMALLINT out_len = 0;
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

    char schema_dim[64], schema_stg[64];
    snprintf(schema_dim, sizeof schema_dim, "scd_dim_%d", (int)getpid());
    snprintf(schema_stg, sizeof schema_stg, "scd_stg_%d", (int)getpid());

    char ddl[2048];
    snprintf(ddl, sizeof ddl,
        "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
        "EXEC('DROP SCHEMA [%s]')", schema_dim, schema_dim);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl,
        "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
        "EXEC('DROP SCHEMA [%s]')", schema_stg, schema_stg);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema_dim);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema_stg);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
        "CREATE TABLE [%s].[customer] ("
        " customer_sk BIGINT IDENTITY(1,1) PRIMARY KEY,"
        " customer_id INT NOT NULL,"
        " name NVARCHAR(200), email NVARCHAR(200), address NVARCHAR(400),"
        " valid_from DATETIME2(6) NOT NULL,"
        " valid_to   DATETIME2(6) NULL,"
        " is_current BIT NOT NULL CONSTRAINT DF_%s_is_current DEFAULT (1))",
        schema_dim, schema_dim);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    /* The view is constructed below with the dim schema baked in. Note
     * that CREATE VIEW must be the first statement in its batch — we
     * issue it on a dedicated SQLExecDirect call. */
    snprintf(ddl, sizeof ddl,
        "CREATE VIEW [%s].[customer_current] AS "
        " SELECT customer_sk, customer_id, name, email, address, "
        "        valid_from, valid_to "
        "   FROM [%s].[customer] WHERE is_current = 1",
        schema_dim, schema_dim);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
        "CREATE TABLE [%s].[customer] "
        " (customer_id INT NOT NULL, name NVARCHAR(200), "
        "  email NVARCHAR(200), address NVARCHAR(400))",
        schema_stg);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    char yaml[4096];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: scd-type2-mssql-test\n"
        "parameters:\n"
        "  batch_ts:\n"
        "    type: timestamp\n"
        "    required: true\n"
        "connections:\n"
        "  warehouse:\n"
        "    type: mssql\n"
        "    dsn: ${env.WAREHOUSE_DSN}\n"
        "pipeline:\n"
        "  - id: scd_customer\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: stage\n"
        "        type: mssql.read\n"
        "        connection: warehouse\n"
        "        query: 'SELECT customer_id, name, email, address FROM [%s].[customer]'\n"
        "      - id: current_dim\n"
        "        type: mssql.read\n"
        "        connection: warehouse\n"
        "        query: 'SELECT customer_id AS dim_customer_id, customer_sk AS dim_sk,"
                                " name AS dim_name, email AS dim_email,"
                                " address AS dim_address FROM [%s].[customer_current]'\n"
        "      - id: joined\n"
        "        type: join\n"
        "        kind: left\n"
        "        from: [stage, current_dim]\n"
        "        \"on\": { customer_id: dim_customer_id }\n"
        "      - id: classify\n"
        "        type: map\n"
        "        from: joined\n"
        "        add:\n"
        "          scd_status:\n"
        "            lang: lua\n"
        "            expr: |\n"
        "              row.dim_sk == nil and \"NEW\"\n"
        "                or (row.name ~= row.dim_name "
                              "or row.email ~= row.dim_email "
                              "or row.address ~= row.dim_address) and \"CHANGED\"\n"
        "                or \"UNCHANGED\"\n"
        "      - id: route\n"
        "        type: conditional_split\n"
        "        from: classify\n"
        "        cases:\n"
        "          - { name: new,     where: 'row.scd_status == \"NEW\"' }\n"
        "          - { name: changed, where: 'row.scd_status == \"CHANGED\"' }\n"
        "        default: unchanged\n"
        "      - id: changed_fan\n"
        "        type: multicast\n"
        "        from: route:changed\n"
        "        taps: [for_insert, for_close]\n"
        "      - id: to_insert\n"
        "        type: union\n"
        "        from: [route:new, changed_fan:for_insert]\n"
        "      - id: shape_new\n"
        "        type: map\n"
        "        from: to_insert\n"
        "        select: [customer_id, name, email, address]\n"
        "      - id: insert_new_version\n"
        "        type: mssql.exec\n"
        "        from: shape_new\n"
        "        connection: warehouse\n"
        "        sql: \"INSERT INTO [%s].[customer] (customer_id, name, email, address, valid_from, is_current) VALUES (?, ?, ?, ?, '${params.batch_ts}', 1)\"\n"
        "        parameters: [customer_id, name, email, address]\n"
        "      - id: drain_inserts\n"
        "        type: betl.count_rows\n"
        "        from: insert_new_version\n"
        "      - id: close_old_version\n"
        "        type: mssql.exec\n"
        "        from: changed_fan:for_close\n"
        "        connection: warehouse\n"
        "        sql: \"UPDATE [%s].[customer] SET is_current = 0, valid_to = '${params.batch_ts}' WHERE customer_sk = ?\"\n"
        "        parameters: [dim_sk]\n"
        "      - id: drain_closes\n"
        "        type: betl.count_rows\n"
        "        from: close_old_version\n",
        schema_stg, schema_dim, schema_dim, schema_dim);

    char yaml_path[128];
    snprintf(yaml_path, sizeof yaml_path,
             "/workspace/betl/build/betl-test-scd-mssql-%d.yml",
             (int)getpid());
    if (write_file(yaml_path, yaml) != 0) {
        fprintf(stderr, "write_file failed\n"); goto teardown;
    }

    /* ---- Batch 1: three NEW customers ----------------------- */
    char sql[1024];
    snprintf(sql, sizeof sql,
        "INSERT INTO [%s].[customer] VALUES "
        " (1, N'Alice',   N'a@x', N'Lane 1'),"
        " (2, N'Bob',     N'b@x', N'Lane 2'),"
        " (3, N'Charlie', N'c@x', N'Lane 3')",
        schema_stg);
    if (ms_exec(hdbc, sql) != 0) goto teardown;

    CHECK(run_pipeline(yaml_path, "2026-05-01T00:00:00", lua_plugin) == BETL_OK);
    char qsql[400];
    snprintf(qsql, sizeof qsql,
        "SELECT count(*) FROM [%s].[customer] WHERE is_current = 1", schema_dim);
    CHECK(ms_count(hdbc, qsql) == 3);
    snprintf(qsql, sizeof qsql,
        "SELECT count(*) FROM [%s].[customer] WHERE is_current = 0", schema_dim);
    CHECK(ms_count(hdbc, qsql) == 0);

    /* ---- Batch 2 ----------------------- */
    snprintf(sql, sizeof sql,
        "TRUNCATE TABLE [%s].[customer]; "
        "INSERT INTO [%s].[customer] VALUES "
        " (1, N'Alice', N'a@x', N'Lane 1'),"
        " (2, N'Bob',   N'b@x', N'NEW ADDRESS'),"
        " (4, N'Dora',  N'd@x', N'Lane 4')",
        schema_stg, schema_stg);
    if (ms_exec(hdbc, sql) != 0) goto teardown;

    CHECK(run_pipeline(yaml_path, "2026-05-02T00:00:00", lua_plugin) == BETL_OK);
    snprintf(qsql, sizeof qsql,
        "SELECT count(*) FROM [%s].[customer] WHERE is_current = 1", schema_dim);
    CHECK(ms_count(hdbc, qsql) == 4);
    snprintf(qsql, sizeof qsql,
        "SELECT count(*) FROM [%s].[customer] WHERE is_current = 0", schema_dim);
    CHECK(ms_count(hdbc, qsql) == 1);
    snprintf(qsql, sizeof qsql,
        "SELECT count(*) FROM [%s].[customer] "
        " WHERE customer_id = 2 AND is_current = 1 AND address = N'NEW ADDRESS'",
        schema_dim);
    CHECK(ms_count(hdbc, qsql) == 1);
    snprintf(qsql, sizeof qsql,
        "SELECT count(*) FROM [%s].[customer] "
        " WHERE customer_id = 2 AND is_current = 0 AND address = N'Lane 2'",
        schema_dim);
    CHECK(ms_count(hdbc, qsql) == 1);

    if (failures == 0) unlink(yaml_path);

teardown:
    /* Drop view first (depends on dim table), then tables, then schemas. */
    snprintf(ddl, sizeof ddl,
        "IF OBJECT_ID('[%s].[customer_current]', 'V') IS NOT NULL "
        "DROP VIEW [%s].[customer_current]", schema_dim, schema_dim);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl,
        "IF OBJECT_ID('[%s].[customer]', 'U') IS NOT NULL "
        "DROP TABLE [%s].[customer]", schema_dim, schema_dim);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl,
        "IF OBJECT_ID('[%s].[customer]', 'U') IS NOT NULL "
        "DROP TABLE [%s].[customer]", schema_stg, schema_stg);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl,
        "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
        "EXEC('DROP SCHEMA [%s]')", schema_dim, schema_dim);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl,
        "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
        "EXEC('DROP SCHEMA [%s]')", schema_stg, schema_stg);
    ms_exec(hdbc, ddl);

    SQLDisconnect(hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: scd_type2_mssql integration test passed\n");
    return 0;
}
