/* var.set — TASK that writes one entry into the runtime's
 * `${vars.NAME}` store. Two modes (mutually exclusive):
 *
 *   value:                 literal text. `${...}` substitution happens
 *                          at run-time (so you can reference params,
 *                          env, or other vars).
 *   connection: + sql:     run the statement, fetch the first column
 *                          of the first row, stringify it. NULL cell
 *                          unsets the var.
 *
 * Config:
 *   name        required, the variable name (no `vars.` prefix)
 *   value       literal mode
 *   connection  SQL mode
 *   sql         SQL mode
 *
 * Dispatch by connection type follows sql.execute (postgres / mssql). */

#include "runtime/var_set.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "runtime/context.h"
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
    char        *name;
    char        *value;        /* literal mode */
    char        *connection;   /* SQL mode */
    char        *sql;          /* SQL mode */
} VsState;

static int vs_init(BetlContext *ctx, const char *cfg, void **state) {
    VsState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "name", &s->name) != 0 || !s->name) {
        betl_set_error(ctx, "var.set: missing required `name`");
        free(s); return BETL_ERR_INVALID;
    }
    (void)betl_tx_json_string_at(cfg, "value",      &s->value);
    (void)betl_tx_json_string_at(cfg, "connection", &s->connection);
    (void)betl_tx_json_string_at(cfg, "sql",        &s->sql);

    /* Exactly one of (value) or (connection+sql) must be present. */
    int literal = (s->value != NULL);
    int sqlish  = (s->connection != NULL || s->sql != NULL);
    if (literal && sqlish) {
        betl_set_error(ctx, "var.set: `value:` and `connection:`+`sql:` "
                            "are mutually exclusive");
        goto bail;
    }
    if (!literal && !sqlish) {
        betl_set_error(ctx, "var.set: one of `value:` or `connection:`+`sql:` "
                            "is required");
        goto bail;
    }
    if (sqlish && (!s->connection || !s->sql)) {
        betl_set_error(ctx, "var.set: `connection:` and `sql:` are both "
                            "required for SQL-capture mode");
        goto bail;
    }
    *state = s;
    return BETL_OK;
bail:
    free(s->name); free(s->value); free(s->connection); free(s->sql);
    free(s);
    return BETL_ERR_INVALID;
}

static void vs_destroy(void *state) {
    if (!state) return;
    VsState *s = state;
    free(s->name);
    free(s->value);
    free(s->connection);
    free(s->sql);
    free(s);
}

/* Mirror sql_execute.c — extract { "type", "dsn" } out of the resolved
 * connection JSON so we can dispatch. */
static int extract_str(const char *json, const char *key, char **out) {
    return betl_tx_json_string_at(json, key, out);
}

#ifdef BETL_HAVE_LIBPQ
static int capture_pg(VsState *s, const char *dsn) {
    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        betl_set_error(s->ctx, "var.set: postgres connect failed: %s",
                       PQerrorMessage(c));
        PQfinish(c); return BETL_ERR_AUTH;
    }
    PGresult *r = PQexec(c, s->sql);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        betl_set_error(s->ctx, "var.set: postgres exec failed: %s",
                       PQresultErrorMessage(r));
        PQclear(r); PQfinish(c); return BETL_ERR_IO;
    }
    int ret = BETL_OK;
    if (PQntuples(r) > 0 && PQnfields(r) > 0) {
        if (PQgetisnull(r, 0, 0)) {
            betl_context_unset_var(s->ctx, s->name);
        } else {
            const char *v = PQgetvalue(r, 0, 0);
            if (betl_context_set_var(s->ctx, s->name, v) != BETL_OK) {
                betl_set_error(s->ctx, "var.set: out of memory");
                ret = BETL_ERR_INTERNAL;
            }
        }
    } else {
        /* Empty result → leave the var untouched. */
    }
    PQclear(r); PQfinish(c);
    return ret;
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

static int capture_mssql(VsState *s, const char *dsn) {
    SQLHENV  henv  = 0;
    SQLHDBC  hdbc  = 0;
    SQLHSTMT hstmt = 0;
    int ret = BETL_OK;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv))) {
        betl_set_error(s->ctx, "var.set: SQLAllocHandle(ENV) failed");
        return BETL_ERR_INTERNAL;
    }
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc))) {
        ret = BETL_ERR_INTERNAL; goto out;
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
        betl_set_error(s->ctx, "var.set: mssql connect failed: %s", msg);
        ret = BETL_ERR_AUTH; goto out;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) {
        ret = BETL_ERR_INTERNAL; goto out;
    }
    rc = SQLExecDirect(hstmt, (SQLCHAR *)s->sql, SQL_NTS);
    if (rc != SQL_NO_DATA && !SQL_SUCCEEDED(rc)) {
        char msg[400] = {0};
        odbc_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        betl_set_error(s->ctx, "var.set: mssql exec failed: %s", msg);
        ret = BETL_ERR_IO; goto out;
    }
    /* Fetch the first row's first column as text via SQL_C_CHAR. */
    if (SQLFetch(hstmt) == SQL_SUCCESS) {
        char   buf[1024] = {0};
        SQLLEN ind = 0;
        if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_CHAR,
                                     buf, sizeof buf, &ind))) {
            if (ind == SQL_NULL_DATA) {
                betl_context_unset_var(s->ctx, s->name);
            } else {
                if (betl_context_set_var(s->ctx, s->name, buf) != BETL_OK) {
                    betl_set_error(s->ctx, "var.set: out of memory");
                    ret = BETL_ERR_INTERNAL;
                }
            }
        }
    }
out:
    if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    if (hdbc)  { SQLDisconnect(hdbc); SQLFreeHandle(SQL_HANDLE_DBC, hdbc); }
    if (henv)  SQLFreeHandle(SQL_HANDLE_ENV, henv);
    return ret;
}
#endif

static int vs_run(void *state) {
    VsState *s = state;
    if (s->value) {
        /* The cfg JSON's value: was already passed through betl_substitute_refs
         * by the stage executor (see run_task_stage in exec.c). So s->value
         * is the final string — write it. */
        if (betl_context_set_var(s->ctx, s->name, s->value) != BETL_OK) {
            betl_set_error(s->ctx, "var.set: out of memory");
            return BETL_ERR_INTERNAL;
        }
        return BETL_OK;
    }

    const char *conn_json = betl_get_connection(s->ctx, s->connection);
    if (!conn_json) {
        betl_set_error(s->ctx, "var.set: connection '%s' not declared",
                       s->connection);
        return BETL_ERR_NOT_FOUND;
    }
    char *type = NULL, *dsn = NULL;
    if (extract_str(conn_json, "type", &type) != 0 || !type) {
        betl_set_error(s->ctx, "var.set: connection '%s' missing `type`",
                       s->connection);
        return BETL_ERR_INVALID;
    }
    if (extract_str(conn_json, "dsn", &dsn) != 0 || !dsn) {
        betl_set_error(s->ctx, "var.set: connection '%s' missing `dsn`",
                       s->connection);
        free(type); return BETL_ERR_INVALID;
    }
    int ret;
    if (strcmp(type, "postgres") == 0) {
#ifdef BETL_HAVE_LIBPQ
        ret = capture_pg(s, dsn);
#else
        betl_set_error(s->ctx, "var.set: postgres connection requires libpq build");
        ret = BETL_ERR_INVALID;
#endif
    } else if (strcmp(type, "mssql") == 0) {
#ifdef BETL_HAVE_ODBC
        ret = capture_mssql(s, dsn);
#else
        betl_set_error(s->ctx, "var.set: mssql connection requires ODBC build");
        ret = BETL_ERR_INVALID;
#endif
    } else {
        betl_set_error(s->ctx, "var.set: unsupported connection type '%s'", type);
        ret = BETL_ERR_INVALID;
    }
    free(type); free(dsn);
    return ret;
}

static const BetlComponentDef vs_components[] = {
    { .name               = "var.set",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = vs_init,
      .destroy            = vs_destroy,
      .task_run           = vs_run },
};

static const BetlProvider vs_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-var-set",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = vs_components,
    .component_count = sizeof vs_components / sizeof vs_components[0],
};

int betl_register_var_set(BetlRegistry *r) {
    return betl_registry_register(r, &vs_provider, "<builtin:var-set>");
}
