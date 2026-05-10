/* mssql `mssql.lookup` TRANSFORM — sibling of postgres.lookup, built on
 * unixODBC. Same v0.1 strategy: SELECT once at first-batch time, cache
 * rows in memory, probe linearly per upstream row. */

#include "runtime/mssql_lookup.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "betl/provider.h"
#include "loader/registry.h"

/* ============================================================== *
 *  JSON helpers (same shape as postgres_lookup.c)                  *
 * ============================================================== */

static const char *msl_json_value_after(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof needle) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

static int msl_json_decode_str(const char *p, char **out) {
    *out = NULL;
    if (!p || *p != '"') return -1;
    ++p;
    size_t cap = strlen(p) + 1;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p != '\\') { buf[i++] = *p++; continue; }
        ++p;
        if (!*p) { free(buf); return -1; }
        switch (*p) {
            case '"':  buf[i++] = '"';  ++p; break;
            case '\\': buf[i++] = '\\'; ++p; break;
            case '/':  buf[i++] = '/';  ++p; break;
            case 'n':  buf[i++] = '\n'; ++p; break;
            case 't':  buf[i++] = '\t'; ++p; break;
            case 'r':  buf[i++] = '\r'; ++p; break;
            case 'b':  buf[i++] = '\b'; ++p; break;
            case 'f':  buf[i++] = '\f'; ++p; break;
            default: free(buf); return -1;
        }
    }
    if (*p != '"') { free(buf); return -1; }
    buf[i] = '\0';
    *out = buf;
    return 0;
}

static int msl_json_string(const char *json, const char *key, char **out) {
    return msl_json_decode_str(msl_json_value_after(json, key), out);
}

typedef int (*msl_kv_fn)(const char *key, const char *value, size_t value_len, void *user);

static int msl_walk_object(const char *p, msl_kv_fn cb, void *user) {
    if (!p || *p != '{') return -1;
    ++p;
    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == '}' || *p == '\0') return 0;
        if (*p != '"') return -1;
        const char *key_start = p + 1;
        const char *key_end = strchr(key_start, '"');
        if (!key_end) return -1;
        size_t klen = (size_t)(key_end - key_start);
        char key[128];
        if (klen >= sizeof key) return -1;
        memcpy(key, key_start, klen);
        key[klen] = '\0';
        p = key_end + 1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p != ':') return -1;
        ++p;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        const char *val_start = p;
        int depth = 0, in_str = 0;
        while (*p) {
            if (in_str) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '"') in_str = 0;
                ++p; continue;
            }
            if (*p == '"') { in_str = 1; ++p; continue; }
            if (*p == '{' || *p == '[') { ++depth; ++p; continue; }
            if (*p == '}' || *p == ']') {
                if (depth == 0) break;
                --depth; ++p; continue;
            }
            if (*p == ',' && depth == 0) break;
            ++p;
        }
        size_t vlen = (size_t)(p - val_start);
        if (cb(key, val_start, vlen, user) != 0) return -1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == '}' || *p == '\0') return 0;
        return -1;
    }
}

/* ============================================================== *
 *  Arrow leaf release helpers                                      *
 * ============================================================== */

static void msl_release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msl_release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void msl_release_struct(struct ArrowArray *arr) {
    for (int64_t i = 0; i < arr->n_children; ++i) {
        if (arr->children[i] && arr->children[i]->release) {
            arr->children[i]->release(arr->children[i]);
        }
        free(arr->children[i]);
    }
    free(arr->children);
    free(arr->buffers);
    arr->release = NULL;
}

static void msl_release_schema_named(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->release = NULL;
}

static void msl_release_schema_struct(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i] && sch->children[i]->release) {
            sch->children[i]->release(sch->children[i]);
        }
        free(sch->children[i]);
    }
    free(sch->children);
    sch->release = NULL;
}

/* ============================================================== *
 *  State                                                           *
 * ============================================================== */

typedef enum { OM_ERROR = 1, OM_NULL = 2, OM_DROP = 3 } OnMiss;

typedef struct {
    char *input_col;
    char *lookup_col;
    int   input_idx;
    char  fmt;
} LkMatch;

typedef struct {
    char *output_col;
    char *lookup_col;
    char  fmt;
} LkSelect;

typedef struct {
    char *connection;
    char *table;
    OnMiss on_miss;

    LkMatch  *matches;
    size_t    n_matches;
    LkSelect *selects;
    size_t    n_selects;

    BetlContext *ctx;
    struct ArrowArrayStream input;
    int                     have_input;

    int    schema_cached;
    size_t n_input_cols;
    char **input_col_names;
    char  *input_col_fmts;

    SQLHENV  henv;
    SQLHDBC  hdbc;
    int      cache_built;
    size_t   cache_rows;
    int64_t **cache_i64;
    char  ***cache_u8s;
    size_t **cache_u8l;

    char last_err[256];
} LookupState;

static void lset_err(LookupState *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->last_err, sizeof l->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(l->ctx, "%s", l->last_err);
}

static void copy_diag(SQLSMALLINT type, SQLHANDLE h, char *out, size_t cap) {
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

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { LookupState *l; int err; } MapCtx;

static int match_visit(const char *key, const char *value, size_t value_len, void *user) {
    MapCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        lset_err(c->l, "mssql.lookup: match['%s'] must be a column-name string", key);
        c->err = 1; return -1;
    }
    char *rcol = NULL;
    if (msl_json_decode_str(value, &rcol) != 0 || !rcol) { c->err = 1; return -1; }
    LkMatch *grow = realloc(c->l->matches, (c->l->n_matches + 1) * sizeof *grow);
    if (!grow) { free(rcol); c->err = 1; return -1; }
    c->l->matches = grow;
    LkMatch *m = &c->l->matches[c->l->n_matches++];
    memset(m, 0, sizeof *m);
    m->input_col  = strdup(key);
    m->lookup_col = rcol;
    m->input_idx  = -1;
    if (!m->input_col) { c->err = 1; return -1; }
    return 0;
}

static int select_visit(const char *key, const char *value, size_t value_len, void *user) {
    MapCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        lset_err(c->l, "mssql.lookup: select['%s'] must be a column-name string", key);
        c->err = 1; return -1;
    }
    char *rcol = NULL;
    if (msl_json_decode_str(value, &rcol) != 0 || !rcol) { c->err = 1; return -1; }
    LkSelect *grow = realloc(c->l->selects, (c->l->n_selects + 1) * sizeof *grow);
    if (!grow) { free(rcol); c->err = 1; return -1; }
    c->l->selects = grow;
    LkSelect *s = &c->l->selects[c->l->n_selects++];
    memset(s, 0, sizeof *s);
    s->output_col = strdup(key);
    s->lookup_col = rcol;
    if (!s->output_col) { c->err = 1; return -1; }
    return 0;
}

static int lookup_init(BetlContext *ctx, const char *cfg, void **state) {
    LookupState *l = calloc(1, sizeof *l);
    if (!l) return BETL_ERR_INTERNAL;
    l->ctx = ctx;
    l->on_miss = OM_ERROR;
    l->henv = SQL_NULL_HENV;
    l->hdbc = SQL_NULL_HDBC;
    cfg = cfg ? cfg : "{}";

    if (msl_json_string(cfg, "connection", &l->connection) != 0
        || !l->connection) {
        lset_err(l, "mssql.lookup: 'connection:' is required");
        free(l); return BETL_ERR_INVALID;
    }
    if (msl_json_string(cfg, "table", &l->table) != 0 || !l->table) {
        lset_err(l, "mssql.lookup: 'table:' is required");
        free(l->connection); free(l); return BETL_ERR_INVALID;
    }
    char *om = NULL;
    msl_json_string(cfg, "on_miss", &om);
    if (om) {
        if      (strcmp(om, "error") == 0) l->on_miss = OM_ERROR;
        else if (strcmp(om, "null")  == 0) l->on_miss = OM_NULL;
        else if (strcmp(om, "drop")  == 0) l->on_miss = OM_DROP;
        else {
            lset_err(l, "mssql.lookup: on_miss must be error|null|drop (got '%s')", om);
            free(om); free(l->connection); free(l->table); free(l);
            return BETL_ERR_INVALID;
        }
        free(om);
    }

    const char *m = msl_json_value_after(cfg, "match");
    if (!m || *m != '{') {
        lset_err(l, "mssql.lookup: `match:` map is required");
        free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    MapCtx mc = { .l = l, .err = 0 };
    if (msl_walk_object(m, match_visit, &mc) != 0 || mc.err
        || l->n_matches == 0) {
        if (l->last_err[0] == '\0') lset_err(l, "mssql.lookup: `match:` is empty");
        for (size_t i = 0; i < l->n_matches; ++i) {
            free(l->matches[i].input_col); free(l->matches[i].lookup_col);
        }
        free(l->matches); free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    const char *s = msl_json_value_after(cfg, "select");
    if (!s || *s != '{') {
        lset_err(l, "mssql.lookup: `select:` map is required");
        for (size_t i = 0; i < l->n_matches; ++i) {
            free(l->matches[i].input_col); free(l->matches[i].lookup_col);
        }
        free(l->matches); free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    MapCtx sc = { .l = l, .err = 0 };
    if (msl_walk_object(s, select_visit, &sc) != 0 || sc.err
        || l->n_selects == 0) {
        if (l->last_err[0] == '\0') lset_err(l, "mssql.lookup: `select:` is empty");
        for (size_t i = 0; i < l->n_matches; ++i) {
            free(l->matches[i].input_col); free(l->matches[i].lookup_col);
        }
        for (size_t i = 0; i < l->n_selects; ++i) {
            free(l->selects[i].output_col); free(l->selects[i].lookup_col);
        }
        free(l->matches); free(l->selects);
        free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    *state = l;
    return BETL_OK;
}

static int lookup_attach_input(void *state, int port,
                               struct ArrowArrayStream *in) {
    (void)port;
    LookupState *l = state;
    l->input      = *in;
    l->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void lookup_destroy(void *state) {
    if (!state) return;
    LookupState *l = state;
    if (l->have_input && l->input.release) l->input.release(&l->input);
    if (l->hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(l->hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, l->hdbc);
    }
    if (l->henv != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, l->henv);
    free(l->connection);
    free(l->table);
    for (size_t i = 0; i < l->n_matches; ++i) {
        free(l->matches[i].input_col); free(l->matches[i].lookup_col);
    }
    free(l->matches);
    for (size_t i = 0; i < l->n_selects; ++i) {
        free(l->selects[i].output_col); free(l->selects[i].lookup_col);
    }
    free(l->selects);
    if (l->input_col_names) {
        for (size_t i = 0; i < l->n_input_cols; ++i) free(l->input_col_names[i]);
        free(l->input_col_names);
    }
    free(l->input_col_fmts);
    size_t n_total = l->n_matches + l->n_selects;
    if (l->cache_i64) {
        for (size_t c = 0; c < n_total; ++c) free(l->cache_i64[c]);
        free(l->cache_i64);
    }
    if (l->cache_u8s) {
        for (size_t c = 0; c < n_total; ++c) {
            if (l->cache_u8s[c]) {
                for (size_t r = 0; r < l->cache_rows; ++r) free(l->cache_u8s[c][r]);
                free(l->cache_u8s[c]);
            }
        }
        free(l->cache_u8s);
    }
    if (l->cache_u8l) {
        for (size_t c = 0; c < n_total; ++c) free(l->cache_u8l[c]);
        free(l->cache_u8l);
    }
    free(l);
}

/* ============================================================== *
 *  ODBC type mapping + connection                                  *
 * ============================================================== */

static int sql_type_to_fmt(SQLSMALLINT t, char *out) {
    switch (t) {
    case SQL_BIGINT:
    case SQL_INTEGER:
    case SQL_SMALLINT:
    case SQL_TINYINT:
        *out = 'l'; return 0;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
        *out = 'u'; return 0;
    default:
        return -1;
    }
}

static int lookup_open_conn(LookupState *l) {
    const char *cjson = betl_get_connection(l->ctx, l->connection);
    if (!cjson) {
        lset_err(l, "mssql.lookup: connection '%s' not found", l->connection);
        return -1;
    }
    char *dsn = NULL;
    if (msl_json_string(cjson, "dsn", &dsn) != 0 || !dsn) {
        lset_err(l, "mssql.lookup: connection '%s' missing 'dsn'", l->connection);
        return -1;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &l->henv))) {
        lset_err(l, "mssql.lookup: SQLAllocHandle(ENV) failed");
        free(dsn); return -1;
    }
    SQLSetEnvAttr(l->henv, SQL_ATTR_ODBC_VERSION,
                  (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, l->henv, &l->hdbc))) {
        lset_err(l, "mssql.lookup: SQLAllocHandle(DBC) failed");
        free(dsn); return -1;
    }
    SQLCHAR out[1024];
    SQLSMALLINT out_len = 0;
    SQLRETURN rc = SQLDriverConnect(l->hdbc, NULL,
                                    (SQLCHAR *)dsn, SQL_NTS,
                                    out, sizeof out, &out_len,
                                    SQL_DRIVER_NOPROMPT);
    free(dsn);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_DBC, l->hdbc, msg, sizeof msg);
        lset_err(l, "mssql.lookup: connect failed: %s", msg);
        return -1;
    }
    return 0;
}

/* Bracket-quote a SQL Server identifier; reject embedded `]` (same
 * conservative rule as mssql_sql.c). */
static int append_bracket_ident(char *buf, size_t cap, size_t *pos, const char *id) {
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '[';
    for (const char *p = id; *p; ++p) {
        if (*p == ']') return -1;
        if (*pos + 1 >= cap) return -1;
        buf[(*pos)++] = *p;
    }
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = ']';
    buf[*pos] = '\0';
    return 0;
}

static int append_table_ident(char *buf, size_t cap, size_t *pos, const char *table) {
    const char *dot = strchr(table, '.');
    if (!dot) return append_bracket_ident(buf, cap, pos, table);
    char schema[128];
    size_t sl = (size_t)(dot - table);
    if (sl >= sizeof schema) return -1;
    memcpy(schema, table, sl); schema[sl] = '\0';
    if (append_bracket_ident(buf, cap, pos, schema) != 0) return -1;
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '.';
    return append_bracket_ident(buf, cap, pos, dot + 1);
}

static int build_select_sql(const LookupState *l, char *buf, size_t cap) {
    size_t pos = 0;
    static const char SEL[] = "SELECT ";
    if (cap < sizeof SEL) return -1;
    memcpy(buf + pos, SEL, sizeof SEL - 1); pos += sizeof SEL - 1;
    int first = 1;
    for (size_t i = 0; i < l->n_matches; ++i) {
        if (!first) {
            if (pos + 2 >= cap) return -1;
            buf[pos++] = ','; buf[pos++] = ' ';
        }
        if (append_bracket_ident(buf, cap, &pos, l->matches[i].lookup_col) != 0) return -1;
        first = 0;
    }
    for (size_t i = 0; i < l->n_selects; ++i) {
        if (!first) {
            if (pos + 2 >= cap) return -1;
            buf[pos++] = ','; buf[pos++] = ' ';
        }
        if (append_bracket_ident(buf, cap, &pos, l->selects[i].lookup_col) != 0) return -1;
        first = 0;
    }
    static const char FRM[] = " FROM ";
    if (pos + sizeof FRM >= cap) return -1;
    memcpy(buf + pos, FRM, sizeof FRM - 1); pos += sizeof FRM - 1;
    if (append_table_ident(buf, cap, &pos, l->table) != 0) return -1;
    if (pos + 1 >= cap) return -1;
    buf[pos] = '\0';
    return 0;
}

/* Discover types via SQLDescribeCol, fetch all rows via SQLFetch +
 * SQLGetData, stash into the parallel cache arrays. */
static int lookup_build_cache(LookupState *l) {
    if (l->cache_built) return 0;
    if (lookup_open_conn(l) != 0) return -1;

    char sql[2048];
    if (build_select_sql(l, sql, sizeof sql) != 0) {
        lset_err(l, "mssql.lookup: failed to build SQL (table or column too long?)");
        return -1;
    }

    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, l->hdbc, &hstmt))) {
        lset_err(l, "mssql.lookup: SQLAllocHandle(STMT) failed");
        return -1;
    }
    SQLRETURN rc = SQLExecDirect(hstmt, (SQLCHAR *)sql, SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
        char msg[512] = {0};
        copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
        lset_err(l, "mssql.lookup: SELECT failed: %s", msg);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return -1;
    }

    SQLSMALLINT n_cols_db = 0;
    SQLNumResultCols(hstmt, &n_cols_db);
    int n_total = (int)(l->n_matches + l->n_selects);
    if (n_total <= 0) {
        /* Unreachable: lookup_init rejects empty match/select. The
         * explicit check pacifies clang-analyzer's interprocedural
         * scan, which can't see that invariant from this entry. */
        lset_err(l, "mssql.lookup: no match/select columns");
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return -1;
    }
    if ((int)n_cols_db != n_total) {
        lset_err(l, "mssql.lookup: SELECT returned %d cols, expected %d",
                 (int)n_cols_db, n_total);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return -1;
    }

    char *fmts = malloc((size_t)n_total);
    if (!fmts) {
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        lset_err(l, "mssql.lookup: out of memory"); return -1;
    }
    for (int c = 0; c < n_total; ++c) {
        SQLCHAR colname[128];
        SQLSMALLINT name_len = 0, sql_type = 0, decimal_digits = 0, nullable = 0;
        SQLULEN col_size = 0;
        if (!SQL_SUCCEEDED(SQLDescribeCol(hstmt, (SQLUSMALLINT)(c + 1),
                                          colname, sizeof colname, &name_len,
                                          &sql_type, &col_size,
                                          &decimal_digits, &nullable))) {
            lset_err(l, "mssql.lookup: SQLDescribeCol(%d) failed", c + 1);
            free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
        }
        char fmt;
        if (sql_type_to_fmt(sql_type, &fmt) != 0) {
            lset_err(l, "mssql.lookup: column %d has unsupported SQL type %d "
                        "(v0.1: int family, char/varchar family)",
                     c + 1, (int)sql_type);
            free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
        }
        fmts[c] = fmt;
        if (c >= (int)l->n_matches) {
            l->selects[c - (int)l->n_matches].fmt = fmt;
        } else {
            l->matches[c].fmt = fmt;
        }
    }

    /* Allocate per-column cache arrays (we don't yet know n_rows so
     * grow dynamically). */
    l->cache_i64 = calloc((size_t)n_total, sizeof *l->cache_i64);
    l->cache_u8s = calloc((size_t)n_total, sizeof *l->cache_u8s);
    l->cache_u8l = calloc((size_t)n_total, sizeof *l->cache_u8l);
    if (!l->cache_i64 || !l->cache_u8s || !l->cache_u8l) {
        free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        lset_err(l, "mssql.lookup: out of memory"); return -1;
    }
    size_t cap = 0;
    size_t n_rows = 0;
    while (1) {
        SQLRETURN fr = SQLFetch(hstmt);
        if (fr == SQL_NO_DATA) break;
        if (!SQL_SUCCEEDED(fr)) {
            char msg[512] = {0};
            copy_diag(SQL_HANDLE_STMT, hstmt, msg, sizeof msg);
            lset_err(l, "mssql.lookup: SQLFetch failed: %s", msg);
            free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
        }
        if (n_rows >= cap) {
            size_t nc = cap ? cap * 2 : 16;
            for (int c = 0; c < n_total; ++c) {
                if (fmts[c] == 'l') {
                    int64_t *p = realloc(l->cache_i64[c], nc * sizeof *p);
                    if (!p) {
                        lset_err(l, "mssql.lookup: out of memory");
                        free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                    }
                    l->cache_i64[c] = p;
                } else {
                    char **sp = realloc(l->cache_u8s[c], nc * sizeof *sp);
                    if (!sp) {
                        lset_err(l, "mssql.lookup: out of memory");
                        free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                    }
                    l->cache_u8s[c] = sp;
                    size_t *lp = realloc(l->cache_u8l[c], nc * sizeof *lp);
                    if (!lp) {
                        lset_err(l, "mssql.lookup: out of memory");
                        free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                    }
                    l->cache_u8l[c] = lp;
                }
            }
            cap = nc;
        }
        for (int c = 0; c < n_total; ++c) {
            if (fmts[c] == 'l') {
                SQLBIGINT v = 0;
                SQLLEN ind = 0;
                if (!SQL_SUCCEEDED(SQLGetData(hstmt, (SQLUSMALLINT)(c + 1),
                                              SQL_C_SBIGINT, &v, sizeof v, &ind))) {
                    lset_err(l, "mssql.lookup: SQLGetData(int) col %d failed", c + 1);
                    free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                }
                if (ind == SQL_NULL_DATA) {
                    lset_err(l, "mssql.lookup: NULL in cached row %zu col %d not "
                                "supported in v0.1", n_rows, c + 1);
                    free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                }
                l->cache_i64[c][n_rows] = (int64_t)v;
            } else {
                /* utf8: use SQLGetData with a small buffer + grow. The
                 * driver returns SQL_SUCCESS_WITH_INFO and updates `ind`
                 * to the total length when the buffer was too small,
                 * but on subsequent calls it returns the *remaining*
                 * data, not the full string. Simpler approach: stage
                 * with a generous fixed buffer; if it overflows, redo
                 * with a sized buffer. */
                SQLCHAR  small[1024];
                SQLLEN   ind = 0;
                SQLRETURN gr = SQLGetData(hstmt, (SQLUSMALLINT)(c + 1),
                                          SQL_C_CHAR, small, sizeof small, &ind);
                if (!SQL_SUCCEEDED(gr)) {
                    lset_err(l, "mssql.lookup: SQLGetData(str) col %d failed", c + 1);
                    free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                }
                if (ind == SQL_NULL_DATA) {
                    lset_err(l, "mssql.lookup: NULL in cached row %zu col %d not "
                                "supported in v0.1", n_rows, c + 1);
                    free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                }
                size_t total_len = (size_t)ind;
                char *dup = NULL;
                if (gr == SQL_SUCCESS) {
                    dup = malloc(total_len + 1);
                    if (!dup) {
                        lset_err(l, "mssql.lookup: out of memory");
                        free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                    }
                    memcpy(dup, small, total_len);
                    dup[total_len] = '\0';
                } else {
                    /* SUCCESS_WITH_INFO + truncation. The first call
                     * already copied (sizeof small - 1) bytes (driver
                     * NUL-terminated). Pull the rest with another call
                     * sized to fit. */
                    dup = malloc(total_len + 1);
                    if (!dup) {
                        lset_err(l, "mssql.lookup: out of memory");
                        free(fmts); SQLFreeHandle(SQL_HANDLE_STMT, hstmt); return -1;
                    }
                    size_t got = sizeof small - 1;
                    memcpy(dup, small, got);
                    SQLLEN ind2 = 0;
                    SQLRETURN gr2 = SQLGetData(hstmt, (SQLUSMALLINT)(c + 1),
                                               SQL_C_CHAR, dup + got,
                                               (SQLLEN)(total_len - got + 1),
                                               &ind2);
                    if (!SQL_SUCCEEDED(gr2)) {
                        free(dup); free(fmts);
                        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
                        lset_err(l, "mssql.lookup: SQLGetData(str cont) col %d failed", c + 1);
                        return -1;
                    }
                    dup[total_len] = '\0';
                }
                l->cache_u8s[c][n_rows] = dup;
                l->cache_u8l[c][n_rows] = total_len;
            }
        }
        ++n_rows;
    }
    l->cache_rows  = n_rows;
    l->cache_built = 1;
    free(fmts);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return 0;
}

/* ============================================================== *
 *  Schema resolution + emit                                        *
 * ============================================================== */

static int lookup_resolve_schema(LookupState *l) {
    if (l->schema_cached) return 0;
    if (!l->have_input || !l->input.get_schema) {
        lset_err(l, "mssql.lookup: input has no get_schema");
        return -1;
    }
    if (lookup_build_cache(l) != 0) return -1;

    struct ArrowSchema sch = {0};
    if (l->input.get_schema(&l->input, &sch) != 0) {
        lset_err(l, "mssql.lookup: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        lset_err(l, "mssql.lookup: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) { free(names); free(fmts);
        lset_err(l, "mssql.lookup: out of memory"); goto done; }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            lset_err(l, "mssql.lookup: input column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
        fmts[i] = fmt[0];
        names[i] = strdup((c && c->name) ? c->name : "");
        if (!names[i]) {
            lset_err(l, "mssql.lookup: out of memory");
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
    }
    for (size_t i = 0; i < l->n_matches; ++i) {
        LkMatch *m = &l->matches[i];
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], m->input_col) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            lset_err(l, "mssql.lookup: match input column '%s' not found", m->input_col);
            for (size_t k = 0; k < n; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
        if (fmts[idx] != m->fmt) {
            lset_err(l, "mssql.lookup: match '%s'/'%s': input fmt '%c' but lookup fmt '%c'",
                     m->input_col, m->lookup_col, fmts[idx], m->fmt);
            for (size_t k = 0; k < n; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
        m->input_idx = idx;
    }
    l->n_input_cols    = n;
    l->input_col_names = names;
    l->input_col_fmts  = fmts;
    l->schema_cached   = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

static struct ArrowSchema *new_leaf(const char *name, const char *fmt) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    char *nm = strdup(name);
    if (!c || !nm) { free(c); free(nm); return NULL; }
    c->format  = fmt;
    c->name    = nm;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = msl_release_schema_named;
    return c;
}

static int lookup_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    LookupState *l = st->private_data;
    memset(out, 0, sizeof *out);
    if (!l) return EINVAL;
    if (lookup_resolve_schema(l) != 0) return EIO;

    size_t n_total = l->n_input_cols + l->n_selects;
    struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < l->n_input_cols; ++i) {
        const char *fmt = (l->input_col_fmts[i] == 'l') ? "l" : "u";
        kids[i] = new_leaf(l->input_col_names[i], fmt);
        if (!kids[i]) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    for (size_t i = 0; i < l->n_selects; ++i) {
        const char *fmt = (l->selects[i].fmt == 'l') ? "l" : "u";
        kids[l->n_input_cols + i] = new_leaf(l->selects[i].output_col, fmt);
        if (!kids[l->n_input_cols + i]) {
            for (size_t k = 0; k < l->n_input_cols + i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = msl_release_schema_struct;
    return 0;
}

static int lookup_probe(const LookupState *l,
                        const int64_t *l_i64_at_match,
                        char *const *l_u8s_at_match,
                        const size_t *l_u8l_at_match) {
    int found = -1;
    for (size_t r = 0; r < l->cache_rows; ++r) {
        int eq = 1;
        for (size_t k = 0; k < l->n_matches; ++k) {
            if (l->matches[k].fmt == 'l') {
                if (l->cache_i64[k][r] != l_i64_at_match[k]) { eq = 0; break; }
            } else {
                size_t la = l->cache_u8l[k][r];
                size_t lb = l_u8l_at_match[k];
                if (la != lb) { eq = 0; break; }
                if (la && memcmp(l->cache_u8s[k][r], l_u8s_at_match[k], la) != 0) {
                    eq = 0; break;
                }
            }
        }
        if (!eq) continue;
        if (found != -1) return -2;
        found = (int)r;
    }
    return found;
}

static void read_int64_cell_at(const struct ArrowArray *col, size_t r, int64_t *out) {
    size_t row = r + (size_t)col->offset;
    *out = ((const int64_t *)col->buffers[1])[row];
}

static void read_utf8_cell_at(const struct ArrowArray *col, size_t r,
                              const char **out_data, size_t *out_len) {
    size_t row = r + (size_t)col->offset;
    const int32_t *off = col->buffers[1];
    const char    *dat = col->buffers[2];
    *out_data = dat + off[row];
    *out_len  = (size_t)(off[row + 1] - off[row]);
}

static int build_int64_leaf(struct ArrowArray *out,
                            const int64_t *vals, const uint8_t *nulls,
                            size_t n) {
    int64_t *vbuf = malloc((n ? n : 1) * sizeof *vbuf);
    if (!vbuf) return -1;
    if (n) memcpy(vbuf, vals, n * sizeof *vbuf);

    int64_t  null_count = 0;
    uint8_t *vmap = NULL;
    if (nulls) {
        for (size_t i = 0; i < n; ++i) if (nulls[i]) ++null_count;
        if (null_count > 0) {
            size_t bytes = (n + 7) / 8;
            vmap = malloc(bytes ? bytes : 1);
            if (!vmap) { free(vbuf); return -1; }
            memset(vmap, 0xFF, bytes ? bytes : 1);
            for (size_t i = 0; i < n; ++i) {
                if (nulls[i]) vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
            }
        }
    }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vbuf); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = msl_release_int64_leaf;
    return 0;
}

static int build_utf8_leaf(struct ArrowArray *out,
                           char *const *strs, const size_t *lens,
                           const uint8_t *nulls, size_t n) {
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0;
    offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t le = (nulls && nulls[i]) ? 0 : lens[i];
        total += le;
        if (total > (size_t)INT32_MAX) { free(offs); return -1; }
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    int64_t null_count = 0;
    uint8_t *vmap = NULL;
    if (nulls) {
        for (size_t i = 0; i < n; ++i) if (nulls[i]) ++null_count;
        if (null_count > 0) {
            size_t bytes = (n + 7) / 8;
            vmap = malloc(bytes ? bytes : 1);
            if (!vmap) { free(offs); free(data); return -1; }
            memset(vmap, 0xFF, bytes ? bytes : 1);
            for (size_t i = 0; i < n; ++i) {
                if (nulls[i]) vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (nulls && nulls[i]) continue;
        if (lens[i]) memcpy(data + pos, strs[i], lens[i]);
        pos += lens[i];
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = offs;
    bufs[2] = data;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 3;
    out->buffers    = bufs;
    out->release    = msl_release_utf8_leaf;
    return 0;
}

static int lookup_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    LookupState *l = st->private_data;
    memset(out, 0, sizeof *out);
    if (!l) return EINVAL;
    if (lookup_resolve_schema(l) != 0) return EIO;

    struct ArrowArray batch = {0};
    if (l->input.get_next(&l->input, &batch) != 0) {
        const char *e = l->input.get_last_error
                            ? l->input.get_last_error(&l->input) : NULL;
        lset_err(l, "mssql.lookup: upstream get_next failed: %s", e ? e : "(no detail)");
        return EIO;
    }
    if (!batch.release) return 0;
    if (batch.n_children != (int64_t)l->n_input_cols) {
        int64_t got = batch.n_children;
        size_t expected = l->n_input_cols;
        batch.release(&batch);
        lset_err(l, "mssql.lookup: batch has %lld cols, expected %zu",
                 (long long)got, expected);
        return EIO;
    }
    size_t length = (size_t)batch.length;

    int *match_row = malloc((length ? length : 1) * sizeof *match_row);
    uint8_t *keep  = malloc((length ? length : 1) * sizeof *keep);
    if (!match_row || !keep) {
        free(match_row); free(keep); batch.release(&batch);
        lset_err(l, "mssql.lookup: out of memory"); return EIO;
    }
    int64_t      *l_i64 = calloc(l->n_matches ? l->n_matches : 1, sizeof *l_i64);
    const char  **l_str = calloc(l->n_matches ? l->n_matches : 1, sizeof *l_str);
    size_t       *l_len = calloc(l->n_matches ? l->n_matches : 1, sizeof *l_len);
    if (!l_i64 || !l_str || !l_len) {
        free(l_i64); free(l_str); free(l_len);
        free(match_row); free(keep); batch.release(&batch);
        lset_err(l, "mssql.lookup: out of memory"); return EIO;
    }

    size_t n_kept = 0;
    for (size_t r = 0; r < length; ++r) {
        for (size_t k = 0; k < l->n_matches; ++k) {
            const struct ArrowArray *col = batch.children[l->matches[k].input_idx];
            if (l->matches[k].fmt == 'l') {
                read_int64_cell_at(col, r, &l_i64[k]);
            } else {
                read_utf8_cell_at(col, r, &l_str[k], &l_len[k]);
            }
        }
        char *u8s_view[16] = {0};
        for (size_t k = 0; k < l->n_matches && k < 16; ++k) {
            u8s_view[k] = (char *)l_str[k];
        }
        int mr = lookup_probe(l, l_i64, u8s_view, l_len);
        if (mr == -2) {
            free(l_i64); free(l_str); free(l_len);
            free(match_row); free(keep); batch.release(&batch);
            lset_err(l, "mssql.lookup: row %zu has multiple matches in '%s'", r, l->table);
            return EIO;
        }
        match_row[r] = mr;
        if (mr < 0 && l->on_miss == OM_ERROR) {
            free(l_i64); free(l_str); free(l_len);
            free(match_row); free(keep); batch.release(&batch);
            lset_err(l, "mssql.lookup: row %zu missed in '%s' (on_miss=error)", r, l->table);
            return EIO;
        }
        if (mr < 0 && l->on_miss == OM_DROP) {
            keep[r] = 0;
        } else {
            keep[r] = 1; ++n_kept;
        }
    }
    free(l_i64); free(l_str); free(l_len);

    size_t n_total = l->n_input_cols + l->n_selects;
    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        free(match_row); free(keep); batch.release(&batch);
        lset_err(l, "mssql.lookup: out of memory"); return EIO;
    }

    for (size_t c = 0; c < l->n_input_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) goto fail;
        const struct ArrowArray *src = batch.children[c];
        if (l->input_col_fmts[c] == 'l') {
            int64_t *v = malloc((n_kept ? n_kept : 1) * sizeof *v);
            if (!v) goto fail;
            size_t w = 0;
            for (size_t r = 0; r < length; ++r) {
                if (!keep[r]) continue;
                int64_t cell;
                read_int64_cell_at(src, r, &cell);
                v[w++] = cell;
            }
            if (build_int64_leaf(kids[c], v, NULL, n_kept) != 0) {
                free(v); goto fail;
            }
            free(v);
        } else {
            char  **strs = calloc(n_kept ? n_kept : 1, sizeof *strs);
            size_t *lens = calloc(n_kept ? n_kept : 1, sizeof *lens);
            if (!strs || !lens) { free(strs); free(lens); goto fail; }
            size_t w = 0;
            for (size_t r = 0; r < length; ++r) {
                if (!keep[r]) continue;
                const char *p; size_t n;
                read_utf8_cell_at(src, r, &p, &n);
                strs[w] = (char *)p;
                lens[w] = n;
                ++w;
            }
            int rc = build_utf8_leaf(kids[c], strs, lens, NULL, n_kept);
            free(strs); free(lens);
            if (rc != 0) goto fail;
        }
    }

    for (size_t s = 0; s < l->n_selects; ++s) {
        size_t out_idx = l->n_input_cols + s;
        kids[out_idx] = calloc(1, sizeof **kids);
        if (!kids[out_idx]) goto fail;
        size_t cache_col = l->n_matches + s;
        if (l->selects[s].fmt == 'l') {
            int64_t *v = malloc((n_kept ? n_kept : 1) * sizeof *v);
            uint8_t *nulls = calloc(n_kept ? n_kept : 1, 1);
            if (!v || !nulls) { free(v); free(nulls); goto fail; }
            size_t w = 0;
            for (size_t r = 0; r < length; ++r) {
                if (!keep[r]) continue;
                if (match_row[r] >= 0) {
                    v[w] = l->cache_i64[cache_col][match_row[r]];
                } else {
                    v[w]    = 0;
                    nulls[w] = 1;
                }
                ++w;
            }
            int rc = build_int64_leaf(kids[out_idx], v, nulls, n_kept);
            free(v); free(nulls);
            if (rc != 0) goto fail;
        } else {
            char  **strs = calloc(n_kept ? n_kept : 1, sizeof *strs);
            size_t *lens = calloc(n_kept ? n_kept : 1, sizeof *lens);
            uint8_t *nulls = calloc(n_kept ? n_kept : 1, 1);
            if (!strs || !lens || !nulls) {
                free(strs); free(lens); free(nulls); goto fail;
            }
            size_t w = 0;
            for (size_t r = 0; r < length; ++r) {
                if (!keep[r]) continue;
                if (match_row[r] >= 0) {
                    strs[w] = l->cache_u8s[cache_col][match_row[r]];
                    lens[w] = l->cache_u8l[cache_col][match_row[r]];
                } else {
                    strs[w] = ""; lens[w] = 0; nulls[w] = 1;
                }
                ++w;
            }
            int rc = build_utf8_leaf(kids[out_idx], strs, lens, nulls, n_kept);
            free(strs); free(lens); free(nulls);
            if (rc != 0) goto fail;
        }
    }

    free(match_row); free(keep);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c] && kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids); batch.release(&batch);
        lset_err(l, "mssql.lookup: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n_kept;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = msl_release_struct;

    batch.release(&batch);
    return 0;

fail:
    for (size_t c = 0; c < n_total; ++c) {
        if (kids[c] && kids[c]->release) kids[c]->release(kids[c]);
        free(kids[c]);
    }
    free(kids); free(match_row); free(keep);
    batch.release(&batch);
    lset_err(l, "mssql.lookup: failed to build output column");
    return EIO;
}

static const char *lookup_get_last_error(struct ArrowArrayStream *st) {
    LookupState *l = st->private_data;
    return (l && l->last_err[0]) ? l->last_err : NULL;
}

static void lookup_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int lookup_attach_output(void *state, int port,
                                struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = lookup_get_schema;
    out->get_next       = lookup_get_next;
    out->get_last_error = lookup_get_last_error;
    out->release        = lookup_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef lookup_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to enrich" },
};
static const BetlPortDef lookup_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "in + select cols" },
};

static const BetlComponentDef lookup_components[] = {
    { .name               = "mssql.lookup",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = lookup_inputs,
      .input_count        = 1,
      .outputs            = lookup_outputs,
      .output_count       = 1,
      .init               = lookup_init,
      .destroy            = lookup_destroy,
      .attach_input       = lookup_attach_input,
      .attach_output      = lookup_attach_output },
};

static const BetlProvider lookup_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-mssql-lookup",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = lookup_components,
    .component_count = sizeof lookup_components / sizeof lookup_components[0],
};

int betl_register_mssql_lookup(BetlRegistry *r) {
    return betl_registry_register(r, &lookup_provider, "<builtin:mssql-lookup>");
}
