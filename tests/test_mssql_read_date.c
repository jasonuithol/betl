/* Integration test for mssql.read + DATE / DATETIME2 columns.
 *
 * Mirrors the pg_read_date test, but against MSSQL via ODBC.
 * Creates a table with DATE and DATETIME2(6) columns, drives
 * mssql.read directly, and checks the Arrow schema + buffer
 * contents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/date_util.h"

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
        SQLINTEGER native = 0;
        SQLSMALLINT ml = 0;
        SQLGetDiagRec(SQL_HANDLE_STMT, h, 1, state, &native, msg, sizeof msg, &ml);
        fprintf(stderr, "SQL fail [%s]: %s\n", sql, msg);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, h);
    return ok ? 0 : -1;
}

static int bit_clear(const uint8_t *bm, size_t i) {
    return bm == NULL ? 0 : !((bm[i / 8] >> (i % 8)) & 1u);
}

int main(void) {
    const char *dsn = getenv("BETL_TEST_MSSQL_DSN");
    if (!dsn || !*dsn) {
        dsn = default_dsn();
        setenv("BETL_TEST_MSSQL_DSN", dsn, 1);
    }
    SQLHENV henv = SQL_NULL_HENV;
    SQLHDBC hdbc = SQL_NULL_HDBC;
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
    snprintf(schema, sizeof schema, "betl_dt_%d", (int)getpid());
    char ddl[512];
    snprintf(ddl, sizeof ddl,
        "IF EXISTS (SELECT 1 FROM sys.schemas WHERE name='%s') "
        "EXEC('DROP SCHEMA [%s]')", schema, schema);
    ms_exec(hdbc, ddl);
    snprintf(ddl, sizeof ddl, "CREATE SCHEMA [%s]", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;
    snprintf(ddl, sizeof ddl,
        "CREATE TABLE [%s].[events] ("
        "  id BIGINT PRIMARY KEY,"
        "  ev_date DATE NULL,"
        "  ev_at DATETIME2(6) NULL"
        ")", schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;
    snprintf(ddl, sizeof ddl,
        "INSERT INTO [%s].[events] VALUES "
        "  (1, '2026-05-11', '2026-05-11 10:30:00.123456'),"
        "  (2, '2024-02-29', '2024-02-29 23:59:59'),"
        "  (3, NULL, NULL)",
        schema);
    if (ms_exec(hdbc, ddl) != 0) goto teardown;

    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    int rc = betl_register_builtins(reg);
    CHECK(rc == BETL_OK);

    char conn_json[1280];
    snprintf(conn_json, sizeof conn_json, "{\"dsn\":\"%s\"}", dsn);
    betl_context_set_connection(ctx, "warehouse", conn_json);

    const BetlComponentDef *src = betl_registry_find(reg, "mssql.read");
    CHECK(src != NULL);
    if (!src) goto done;

    char cfg[400];
    snprintf(cfg, sizeof cfg,
        "{\"connection\":\"warehouse\","
        " \"query\":\"SELECT id, ev_date, ev_at FROM [%s].[events] ORDER BY id\","
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
        CHECK(strcmp(sch.children[0]->format, "l")    == 0);
        CHECK(strcmp(sch.children[1]->format, "tdD")  == 0);
        CHECK(strcmp(sch.children[2]->format, "tsu:") == 0);
    }

    struct ArrowArray batch = {0};
    rc = stream.get_next(&stream, &batch);
    CHECK(rc == 0);
    CHECK(batch.length == 3);
    if (batch.length == 3 && batch.n_children == 3) {
        const int64_t *ids = batch.children[0]->buffers[1];
        CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3);

        const struct ArrowArray *dcol = batch.children[1];
        const uint8_t *dvalid = dcol->null_count > 0 ? dcol->buffers[0] : NULL;
        const int32_t *days = dcol->buffers[1];
        CHECK(days[0] == betl_days_from_civil(2026, 5, 11));
        CHECK(days[1] == betl_days_from_civil(2024, 2, 29));
        CHECK(bit_clear(dvalid, 2));
        CHECK(dcol->null_count == 1);

        const struct ArrowArray *tcol = batch.children[2];
        const uint8_t *tvalid = tcol->null_count > 0 ? tcol->buffers[0] : NULL;
        const int64_t *us = tcol->buffers[1];
        int64_t expect0 = (int64_t)betl_days_from_civil(2026, 5, 11) * 86400000000LL
                        + (int64_t)10 * 3600000000LL
                        + (int64_t)30 *   60000000LL
                        + 123456LL;
        int64_t expect1 = (int64_t)betl_days_from_civil(2024, 2, 29) * 86400000000LL
                        + (int64_t)23 * 3600000000LL
                        + (int64_t)59 *   60000000LL
                        + 59 * 1000000LL;
        CHECK(us[0] == expect0);
        CHECK(us[1] == expect1);
        CHECK(bit_clear(tvalid, 2));
        CHECK(tcol->null_count == 1);
    }
    if (batch.release)  batch.release(&batch);
    if (sch.release)    sch.release(&sch);
    if (stream.release) stream.release(&stream);
    src->destroy(st);

done:
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

teardown:
    snprintf(ddl, sizeof ddl, "DROP TABLE IF EXISTS [%s].[events]", schema);
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
    printf("ok: mssql_read date/timestamp integration test passed\n");
    return 0;
}
