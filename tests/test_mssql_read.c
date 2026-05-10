/* Integration test for the mssql.read SOURCE.
 *
 * Setup creates a disposable schema with a 5-row table that includes
 * one NULL utf8 value. The pipeline runs `SELECT id, color FROM ...`
 * via mssql.read, pipes through lua.map (which logs each row), and
 * counts. We verify:
 *   - row count is 5
 *   - the lua log shows the four non-null colors
 *   - the lua log shows `nil` for the row whose color is SQL NULL
 *   - small batch_size (=2) → still works (covers > 1 batch)
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

static int ms_exec(SQLHDBC hdbc, const char *sql) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return -1;
    int rc = 0;
    if (!SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR *)sql, SQL_NTS))) {
        diag(SQL_HANDLE_STMT, hstmt, sql);
        rc = -1;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return rc;
}

int main(int argc, char **argv) {
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
    snprintf(schema, sizeof schema, "betl_rd_%d", (int)getpid());
    char ddl[400];
    snprintf(ddl, sizeof ddl,
             "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
             "EXEC('DROP SCHEMA [%s]')", schema, schema);
    ms_exec(hdbc, ddl);

    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    snprintf(ddl, sizeof ddl,
             "CREATE TABLE [%s].[colors] (id BIGINT PRIMARY KEY, "
             "color VARCHAR(50) NULL)", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    /* Five rows; row 4 has color = NULL. */
    snprintf(ddl, sizeof ddl,
             "INSERT INTO [%s].[colors] VALUES "
             "(1,'red'),(2,'green'),(3,'blue'),(4,NULL),(5,'cyan')", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    /* Pipeline: SELECT via mssql.read with batch_size=2 (forces three
     * batches over five rows), log each row, count. */
    char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: mssql-read-it\n"
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
        "        query: SELECT id, color FROM [%s].[colors] ORDER BY id\n"
        "        batch_size: 2\n"
        "      - id: log\n"
        "        type: lua.map\n"
        "        from: source\n"
        "        script: |\n"
        "          log.info('id=' .. row.id .. ' color=' .. tostring(row.color))\n"
        "          return row\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: log\n"
        "        expect: 5\n",
        schema);

    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-mssql-read-%d.yml", (int)getpid());
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

    int rrc = betl_register_builtins(reg);
    CHECK(rrc == BETL_OK);
    rrc = betl_registry_load(reg, plugin_path);
    if (rrc != BETL_OK) {
        fprintf(stderr, "load lua plugin: %s\n", betl_registry_last_error(reg));
        ++failures;
    }
    char conn_err[256];
    rrc = betl_apply_connections(ctx, p, conn_err, sizeof conn_err);
    if (rrc != BETL_OK) {
        fprintf(stderr, "apply_connections: %s\n", conn_err);
        ++failures;
    }

    FILE *log = tmpfile();
    if (log) {
        betl_context_set_log_stream(ctx, log);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    }
    rrc = betl_run(ctx, reg, p);
    if (rrc != BETL_OK) {
        fprintf(stderr, "betl_run rc=%d: %s\n", rrc,
                betl_context_last_error(ctx));
        ++failures;
    }

    if (log) {
        char *txt = slurp(log);
        if (txt) {
            CHECK(strstr(txt, "id=1 color=red")   != NULL);
            CHECK(strstr(txt, "id=2 color=green") != NULL);
            CHECK(strstr(txt, "id=3 color=blue")  != NULL);
            CHECK(strstr(txt, "id=4 color=nil")   != NULL); /* SQL NULL */
            CHECK(strstr(txt, "id=5 color=cyan")  != NULL);
            free(txt);
        }
        fclose(log);
    }

    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

teardown:
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[colors]", schema);
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
    printf("ok: mssql_read integration test passed\n");
    return 0;
}
