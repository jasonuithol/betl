/* Connectivity smoke test for unixODBC + FreeTDS from the c-build container.
 *
 * Confirms:
 *   1. unixODBC is linked correctly (the static libodbc.a from deps/lib),
 *   2. unixODBC can dlopen the FreeTDS driver at deps/lib/odbc/libtdsodbc.so,
 *   3. SQLDriverConnect succeeds against the sibling MSSQL,
 *   4. a trivial SELECT round-trips through the wire.
 *
 * DSN comes from BETL_TEST_MSSQL_DSN; falls back to the connection-info
 * advertised by the db-mssql MCP service. The driver path is hard-coded
 * to the tdsodbc.so we install via install_dep — production users would
 * register a driver name in odbcinst.ini instead. We exit 77 on connect
 * failure so CTest (with SKIP_RETURN_CODE=77) marks the test Skipped
 * rather than Failed when the sibling DB isn't reachable. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>

#define SKIP_RC 77

static const char *default_dsn(void) {
    /* Driver= points at the FreeTDS ODBC driver in deps/lib/odbc.
     * TDS_Version=7.4 = SQL Server 2012+ (the c-build container ships
     * an MSSQL 2022 sibling). */
    return "Driver=/opt/projects/betl/deps/lib/odbc/libtdsodbc.so;"
           "Server=host.containers.internal;Port=1433;"
           "UID=sa;PWD=DevP@ssw0rd!42;Database=master;"
           "TDS_Version=7.4;";
}

static void diag(SQLSMALLINT type, SQLHANDLE h, const char *step) {
    SQLCHAR state[6], msg[512];
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    SQLRETURN rc = SQLGetDiagRec(type, h, 1, state, &native,
                                 msg, sizeof msg, &msg_len);
    if (SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "[%s] SQLSTATE=%s native=%d msg=%s\n",
                step, state, (int)native, msg);
    } else {
        fprintf(stderr, "[%s] (no diag rec available)\n", step);
    }
}

int main(void) {
    const char *dsn = getenv("BETL_TEST_MSSQL_DSN");
    if (!dsn || !*dsn) dsn = default_dsn();

    SQLHENV  henv = SQL_NULL_HENV;
    SQLHDBC  hdbc = SQL_NULL_HDBC;
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    int rc_main = 1;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv))) {
        fprintf(stderr, "SQLAllocHandle(ENV) failed\n");
        return 1;
    }
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc))) {
        fprintf(stderr, "SQLAllocHandle(DBC) failed\n");
        goto cleanup;
    }

    SQLCHAR out_dsn[1024];
    SQLSMALLINT out_dsn_len = 0;
    SQLRETURN rc = SQLDriverConnect(hdbc, NULL,
                                    (SQLCHAR *)dsn, SQL_NTS,
                                    out_dsn, sizeof out_dsn, &out_dsn_len,
                                    SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "[skip] SQLDriverConnect failed:\n");
        diag(SQL_HANDLE_DBC, hdbc, "connect");
        rc_main = SKIP_RC;
        goto cleanup;
    }

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) {
        fprintf(stderr, "SQLAllocHandle(STMT) failed\n");
        goto cleanup;
    }
    rc = SQLExecDirect(hstmt, (SQLCHAR *)"SELECT 1, @@VERSION", SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
        diag(SQL_HANDLE_STMT, hstmt, "exec");
        goto cleanup;
    }
    rc = SQLFetch(hstmt);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        diag(SQL_HANDLE_STMT, hstmt, "fetch");
        goto cleanup;
    }

    SQLINTEGER one = 0;
    SQLLEN     one_ind = 0;
    SQLCHAR    ver[256] = {0};
    SQLLEN     ver_ind = 0;
    SQLGetData(hstmt, 1, SQL_C_LONG, &one, sizeof one, &one_ind);
    SQLGetData(hstmt, 2, SQL_C_CHAR, ver, sizeof ver, &ver_ind);

    if (one != 1) {
        fprintf(stderr, "expected 1, got %ld\n", (long)one);
        goto cleanup;
    }
    printf("ok: mssql server says %s\n", ver);
    rc_main = 0;

cleanup:
    if (hstmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    if (hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    }
    if (henv != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, henv);
    return rc_main;
}
