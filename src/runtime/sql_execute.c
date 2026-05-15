/* sql.execute — TASK component: run a single SQL statement against a
 * declared connection (no row trigger, no rowset out). Dispatches by
 * the connection's `type:` field. Mirrors SSIS Execute SQL Task at
 * the control-flow layer; betl-dtsx2yaml emits this shape for every
 * <DTS:Executable> that wraps a SqlTaskData. */

#include "runtime/sql_execute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "runtime/transforms_internal.h"

#ifdef BETL_HAVE_LIBPQ
#include <libpq-fe.h>
#endif

#ifdef BETL_HAVE_ODBC
#include <sql.h>
#include <sqlext.h>
#endif

typedef struct {
    BetlContext *ctx;
    char        *connection_name;
    char        *sql;
} SeState;

/* Tiny wrapper for consistency: betl_tx_json_string_at decodes JSON
 * string escapes (\n, \", \\, \uXXXX) — important for multi-line SQL
 * arriving as a YAML block scalar that the host serializes with
 * embedded \n. */
static int extract_json_string(const char *json, const char *key, char **out) {
    return betl_tx_json_string_at(json, key, out);
}

#ifdef BETL_HAVE_LIBPQ
static int run_pg(SeState *s, const char *dsn) {
    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        betl_set_error(s->ctx, "sql.execute: postgres connect failed: %s",
                       PQerrorMessage(c));
        PQfinish(c);
        return BETL_ERR_AUTH;
    }
    PGresult *r = PQexec(c, s->sql);
    ExecStatusType st = PQresultStatus(r);
    /* PGRES_COMMAND_OK for DDL/DML without rowset; PGRES_TUPLES_OK if
     * the user pasted a SELECT — we discard rows but don't error. */
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        betl_set_error(s->ctx, "sql.execute: postgres exec failed: %s",
                       PQresultErrorMessage(r));
        PQclear(r);
        PQfinish(c);
        return BETL_ERR_IO;
    }
    PQclear(r);
    PQfinish(c);
    return BETL_OK;
}
#endif

#ifdef BETL_HAVE_ODBC
static void odbc_diag(SQLSMALLINT type, SQLHANDLE h, char *out, size_t cap) {
    out[0] = '\0';
    SQLCHAR state[6] = {0}, msg[400] = {0};
    SQLINTEGER native = 0;
    SQLSMALLINT msg_len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRec(type, h, 1, state, &native,
                                    msg, sizeof msg, &msg_len))) {
        snprintf(out, cap, "SQLSTATE=%s native=%d %s",
                 state, (int)native, msg);
    }
}

static int run_mssql(SeState *s, const char *dsn) {
    SQLHENV  henv  = 0;
    SQLHDBC  hdbc  = 0;
    SQLHSTMT hstmt = 0;
    int ret = BETL_OK;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv))) {
        betl_set_error(s->ctx, "sql.execute: SQLAllocHandle(ENV) failed");
        return BETL_ERR_INTERNAL;
    }
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc))) {
        betl_set_error(s->ctx, "sql.execute: SQLAllocHandle(DBC) failed");
        ret = BETL_ERR_INTERNAL;
        goto out;
    }
    SQLCHAR     odsn[1024] = {0};
    SQLSMALLINT ol = 0;
    SQLRETURN rc = SQLDriverConnect(hdbc, NULL,
                                    (SQLCHAR *)dsn, SQL_NTS,
                                    odsn, sizeof odsn, &ol,
                                    SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[400] = {0};
        odbc_diag(SQL_HANDLE_DBC, hdbc, msg, sizeof msg);
        betl_set_error(s->ctx, "sql.execute: mssql connect failed: %s", msg);
        ret = BETL_ERR_AUTH;
        goto out;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) {
        betl_set_error(s->ctx, "sql.execute: SQLAllocHandle(STMT) failed");
        ret = BETL_ERR_INTERNAL;
        goto out;
    }
    rc = SQLExecDirect(hstmt, (SQLCHAR *)s->sql, SQL_NTS);
    /* FreeTDS returns SQL_NO_DATA (100) for many DDL/sproc shapes;
     * treat alongside SUCCESS to avoid false failures. */
    if (rc != SQL_NO_DATA && !SQL_SUCCEEDED(rc)) {
        char msg[400] = {0};
        odbc_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(s->ctx, "sql.execute: mssql exec failed: %s", msg);
        ret = BETL_ERR_IO;
        goto out;
    }
out:
    if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    if (hdbc)  { SQLDisconnect(hdbc); SQLFreeHandle(SQL_HANDLE_DBC, hdbc); }
    if (henv)  SQLFreeHandle(SQL_HANDLE_ENV, henv);
    return ret;
}
#endif

static int se_init(BetlContext *ctx, const char *cfg, void **state) {
    SeState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    if (extract_json_string(cfg, "connection", &s->connection_name) != 0
        || !s->connection_name) {
        betl_set_error(ctx, "sql.execute: missing required `connection`");
        free(s);
        return BETL_ERR_INVALID;
    }
    if (extract_json_string(cfg, "sql", &s->sql) != 0 || !s->sql) {
        betl_set_error(ctx, "sql.execute: missing required `sql`");
        free(s->connection_name);
        free(s);
        return BETL_ERR_INVALID;
    }
    *state = s;
    return BETL_OK;
}

static void se_destroy(void *state) {
    if (!state) return;
    SeState *s = state;
    free(s->connection_name);
    free(s->sql);
    free(s);
}

static int se_run(void *state) {
    SeState *s = state;
    const char *conn_json = betl_get_connection(s->ctx, s->connection_name);
    if (!conn_json) {
        betl_set_error(s->ctx,
                       "sql.execute: connection '%s' not declared",
                       s->connection_name);
        return BETL_ERR_NOT_FOUND;
    }
    char *type = NULL, *dsn = NULL;
    if (extract_json_string(conn_json, "type", &type) != 0 || !type) {
        betl_set_error(s->ctx,
                       "sql.execute: connection '%s' missing `type` field",
                       s->connection_name);
        return BETL_ERR_INVALID;
    }
    if (extract_json_string(conn_json, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(s->ctx,
                       "sql.execute: connection '%s' missing `dsn` field",
                       s->connection_name);
        free(type);
        return BETL_ERR_INVALID;
    }

    int ret;
    if (strcmp(type, "postgres") == 0) {
#ifdef BETL_HAVE_LIBPQ
        ret = run_pg(s, dsn);
#else
        betl_set_error(s->ctx,
                       "sql.execute: connection '%s' is type='postgres' but "
                       "betl was built without libpq",
                       s->connection_name);
        ret = BETL_ERR_INVALID;
#endif
    } else if (strcmp(type, "mssql") == 0) {
#ifdef BETL_HAVE_ODBC
        ret = run_mssql(s, dsn);
#else
        betl_set_error(s->ctx,
                       "sql.execute: connection '%s' is type='mssql' but "
                       "betl was built without ODBC",
                       s->connection_name);
        ret = BETL_ERR_INVALID;
#endif
    } else {
        betl_set_error(s->ctx,
                       "sql.execute: connection '%s' has unsupported "
                       "type='%s' (supported: postgres, mssql)",
                       s->connection_name, type);
        ret = BETL_ERR_INVALID;
    }
    free(type);
    free(dsn);
    return ret;
}

static const BetlComponentDef se_components[] = {
    { .name               = "sql.execute",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = se_init,
      .destroy            = se_destroy,
      .task_run           = se_run },
};

static const BetlProvider se_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-sql-execute",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = se_components,
    .component_count = sizeof se_components / sizeof se_components[0],
};

int betl_register_sql_execute(BetlRegistry *r) {
    return betl_registry_register(r, &se_provider, "<builtin:sql-execute>");
}
