/* postgres `lookup` TRANSFORM — SPEC §4.3.
 *
 * v0.1 strategy: at first-batch time, run `SELECT <m1>, ..., <s1>, ...
 * FROM <table>` once over the configured connection and cache the rows
 * in memory. Probe the cache linearly per input row. Match types are
 * inferred from the libpq column type OIDs (int8 -> int64, text/varchar
 * -> utf8); other types are rejected with a useful error.
 *
 * SPEC differences from join:
 *   - one-sided: the right-hand "table" is a static DB table, not a stream
 *   - single-row result: a multi-match in the lookup is an error
 *   - on_miss policy is configurable: error | null | drop
 */

#include "runtime/postgres_lookup.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "betl/provider.h"
#include "loader/registry.h"


/* ============================================================== *
 *  JSON helpers (same shape as the rest of the runtime)            *
 * ============================================================== */

static const char *pgl_json_value_after(const char *json, const char *key) {
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

static int pgl_json_decode_str(const char *p, char **out) {
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

static int pgl_json_string(const char *json, const char *key, char **out) {
    return pgl_json_decode_str(pgl_json_value_after(json, key), out);
}

/* JSON object walker. */
typedef int (*pgl_kv_fn)(const char *key, const char *value, size_t value_len, void *user);

static int pgl_walk_object(const char *p, pgl_kv_fn cb, void *user) {
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

static void pgl_release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgl_release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void pgl_release_struct(struct ArrowArray *arr) {
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

static void pgl_release_schema_named(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->release = NULL;
}

static void pgl_release_schema_struct(struct ArrowSchema *sch) {
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
    char *input_col;     /* upstream column name */
    char *lookup_col;    /* table column it must equal */
    int   input_idx;     /* resolved at first batch */
    char  fmt;           /* must be int64 or utf8 in v0.1 */
} LkMatch;

typedef struct {
    char *output_col;    /* name in this transform's output */
    char *lookup_col;    /* table column to read */
    char  fmt;           /* discovered from PG OID */
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

    /* Cached upstream schema. Each input column's type is mirrored on
     * the output (passthrough); selects are appended after. */
    int    schema_cached;
    size_t n_input_cols;
    char **input_col_names;
    char  *input_col_fmts;

    /* Cached lookup rows. Per row: match values + select values, all
     * heap-typed. Layout is (n_matches + n_selects) values per row.
     * int64 cols use a parallel int64 array; utf8 cols use parallel
     * `char *` and `size_t` arrays. col-position 0..n_matches-1 are
     * match cols, n_matches..end are select cols. */
    PGconn  *conn;
    int      cache_built;
    size_t   cache_rows;
    /* Per cache column (n_matches + n_selects entries). */
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


/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { LookupState *l; int err; } MapCtx;

static int match_visit(const char *key, const char *value, size_t value_len, void *user) {
    MapCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        lset_err(c->l, "lookup: match['%s'] must be a column-name string", key);
        c->err = 1; return -1;
    }
    char *rcol = NULL;
    if (pgl_json_decode_str(value, &rcol) != 0 || !rcol) { c->err = 1; return -1; }
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
        lset_err(c->l, "lookup: select['%s'] must be a column-name string", key);
        c->err = 1; return -1;
    }
    char *rcol = NULL;
    if (pgl_json_decode_str(value, &rcol) != 0 || !rcol) { c->err = 1; return -1; }
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
    cfg = cfg ? cfg : "{}";

    if (pgl_json_string(cfg, "connection", &l->connection) != 0
        || !l->connection) {
        lset_err(l, "lookup: 'connection:' is required");
        free(l); return BETL_ERR_INVALID;
    }
    if (pgl_json_string(cfg, "table", &l->table) != 0 || !l->table) {
        lset_err(l, "lookup: 'table:' is required");
        free(l->connection); free(l); return BETL_ERR_INVALID;
    }
    char *om = NULL;
    pgl_json_string(cfg, "on_miss", &om);
    if (om) {
        if      (strcmp(om, "error") == 0) l->on_miss = OM_ERROR;
        else if (strcmp(om, "null")  == 0) l->on_miss = OM_NULL;
        else if (strcmp(om, "drop")  == 0) l->on_miss = OM_DROP;
        else {
            lset_err(l, "lookup: on_miss must be error|null|drop (got '%s')", om);
            free(om); free(l->connection); free(l->table); free(l);
            return BETL_ERR_INVALID;
        }
        free(om);
    }

    const char *m = pgl_json_value_after(cfg, "match");
    if (!m || *m != '{') {
        lset_err(l, "lookup: `match:` map is required");
        free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    MapCtx mc = { .l = l, .err = 0 };
    if (pgl_walk_object(m, match_visit, &mc) != 0 || mc.err
        || l->n_matches == 0) {
        if (l->last_err[0] == '\0') lset_err(l, "lookup: `match:` is empty");
        for (size_t i = 0; i < l->n_matches; ++i) {
            free(l->matches[i].input_col); free(l->matches[i].lookup_col);
        }
        free(l->matches); free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    const char *s = pgl_json_value_after(cfg, "select");
    if (!s || *s != '{') {
        lset_err(l, "lookup: `select:` map is required");
        for (size_t i = 0; i < l->n_matches; ++i) {
            free(l->matches[i].input_col); free(l->matches[i].lookup_col);
        }
        free(l->matches); free(l->connection); free(l->table); free(l);
        return BETL_ERR_INVALID;
    }
    MapCtx sc = { .l = l, .err = 0 };
    if (pgl_walk_object(s, select_visit, &sc) != 0 || sc.err
        || l->n_selects == 0) {
        if (l->last_err[0] == '\0') lset_err(l, "lookup: `select:` is empty");
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
    if (l->conn) PQfinish(l->conn);
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
 *  Schema + cache build                                            *
 * ============================================================== */

/* PG type OIDs we recognise. Hardcoded to avoid pg_type include. */
#define PG_OID_INT2     21
#define PG_OID_INT4     23
#define PG_OID_INT8     20
#define PG_OID_TEXT     25
#define PG_OID_VARCHAR  1043
#define PG_OID_BPCHAR   1042

static int pg_oid_to_fmt(Oid t, char *out) {
    if (t == PG_OID_INT8) { *out = 'l'; return 0; }
    if (t == PG_OID_INT4) { *out = 'i'; return 0; }
    if (t == PG_OID_INT2) { *out = 's'; return 0; }
    if (t == PG_OID_TEXT || t == PG_OID_VARCHAR || t == PG_OID_BPCHAR) { *out = 'u'; return 0; }
    return -1;
}

static int pg_fmt_is_int(char fmt) {
    return fmt == 'l' || fmt == 'i' || fmt == 's';
}

/* Look up the dsn from the connection registry, open libpq. */
static int lookup_open_conn(LookupState *l) {
    const char *cjson = betl_get_connection(l->ctx, l->connection);
    if (!cjson) {
        lset_err(l, "lookup: connection '%s' not found", l->connection);
        return -1;
    }
    char *dsn = NULL;
    if (pgl_json_string(cjson, "dsn", &dsn) != 0 || !dsn) {
        lset_err(l, "lookup: connection '%s' missing 'dsn'", l->connection);
        return -1;
    }
    l->conn = PQconnectdb(dsn);
    free(dsn);
    if (!l->conn || PQstatus(l->conn) != CONNECTION_OK) {
        const char *m = l->conn ? PQerrorMessage(l->conn) : "(no PGconn)";
        lset_err(l, "lookup: PQconnectdb failed: %s", m ? m : "(no detail)");
        return -1;
    }
    return 0;
}

/* Quote an identifier for the SELECT statement. Rejects embedded `"`
 * to avoid SQL injection vectors; v0.1 limitation. */
static int append_quoted_ident(char *buf, size_t cap, size_t *pos, const char *id) {
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '"';
    for (const char *p = id; *p; ++p) {
        if (*p == '"') return -1;
        if (*pos + 1 >= cap) return -1;
        buf[(*pos)++] = *p;
    }
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '"';
    buf[*pos] = '\0';
    return 0;
}

/* Append a comma-separated list of (possibly schema-qualified) ident. */
static int append_table_ident(char *buf, size_t cap, size_t *pos, const char *table) {
    /* Allow optional schema.table form. Split on the first dot. */
    const char *dot = strchr(table, '.');
    if (!dot) return append_quoted_ident(buf, cap, pos, table);
    /* schema."table" style */
    char schema[128];
    size_t sl = (size_t)(dot - table);
    if (sl >= sizeof schema) return -1;
    memcpy(schema, table, sl); schema[sl] = '\0';
    if (append_quoted_ident(buf, cap, pos, schema) != 0) return -1;
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = '.';
    return append_quoted_ident(buf, cap, pos, dot + 1);
}

/* Build SELECT m1, m2, ..., s1, ..., FROM <table>. */
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
        if (append_quoted_ident(buf, cap, &pos, l->matches[i].lookup_col) != 0) return -1;
        first = 0;
    }
    for (size_t i = 0; i < l->n_selects; ++i) {
        if (!first) {
            if (pos + 2 >= cap) return -1;
            buf[pos++] = ','; buf[pos++] = ' ';
        }
        if (append_quoted_ident(buf, cap, &pos, l->selects[i].lookup_col) != 0) return -1;
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

/* Run the SELECT and stash results into the cache. Match types must
 * match the upstream input column types; select types are recorded for
 * later schema construction. */
static int lookup_build_cache(LookupState *l) {
    if (l->cache_built) return 0;
    if (lookup_open_conn(l) != 0) return -1;

    char sql[2048];
    if (build_select_sql(l, sql, sizeof sql) != 0) {
        lset_err(l, "lookup: failed to build SQL (table or column too long?)");
        return -1;
    }
    PGresult *res = PQexec(l->conn, sql);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *m = res ? PQresultErrorMessage(res) : "(no PGresult)";
        lset_err(l, "lookup: SELECT failed: %s",
                 m && m[0] ? m : (l->conn ? PQerrorMessage(l->conn) : "(no detail)"));
        if (res) PQclear(res);
        return -1;
    }
    int n_cols_pg = PQnfields(res);
    int n_total   = (int)(l->n_matches + l->n_selects);
    if (n_total == 0) {
        /* Unreachable: init validates both maps are non-empty. */
        lset_err(l, "lookup: no match/select columns"); PQclear(res); return -1;
    }
    if (n_cols_pg != n_total) {
        lset_err(l, "lookup: SELECT returned %d cols, expected %d", n_cols_pg, n_total);
        PQclear(res); return -1;
    }
    /* Discover types: matches by hardcoded fmt (must match input later);
     * selects by PG OID. */
    char *fmts = malloc((size_t)n_total);
    if (!fmts) { PQclear(res); lset_err(l, "lookup: out of memory"); return -1; }
    for (int c = 0; c < n_total; ++c) {
        Oid t = PQftype(res, c);
        char fmt;
        if (pg_oid_to_fmt(t, &fmt) != 0) {
            lset_err(l, "lookup: PG col %d has unsupported OID %u (v0.1: int*, text/varchar)",
                     c, (unsigned)t);
            free(fmts); PQclear(res); return -1;
        }
        fmts[c] = fmt;
        if (c >= (int)l->n_matches) {
            l->selects[c - (int)l->n_matches].fmt = fmt;
        } else {
            l->matches[c].fmt = fmt;
        }
    }

    int n_rows = PQntuples(res);
    l->cache_rows = (size_t)n_rows;
    l->cache_i64 = calloc((size_t)n_total, sizeof *l->cache_i64);
    l->cache_u8s = calloc((size_t)n_total, sizeof *l->cache_u8s);
    l->cache_u8l = calloc((size_t)n_total, sizeof *l->cache_u8l);
    if (!l->cache_i64 || !l->cache_u8s || !l->cache_u8l) {
        free(fmts); PQclear(res); lset_err(l, "lookup: out of memory"); return -1;
    }
    for (int c = 0; c < n_total; ++c) {
        if (pg_fmt_is_int(fmts[c])) {
            l->cache_i64[c] = malloc((size_t)((n_rows ? n_rows : 1)) * sizeof(int64_t));
            if (!l->cache_i64[c]) {
                free(fmts); PQclear(res); lset_err(l, "lookup: out of memory"); return -1;
            }
        } else {
            l->cache_u8s[c] = malloc((size_t)((n_rows ? n_rows : 1)) * sizeof(char *));
            l->cache_u8l[c] = malloc((size_t)((n_rows ? n_rows : 1)) * sizeof(size_t));
            if (!l->cache_u8s[c] || !l->cache_u8l[c]) {
                free(fmts); PQclear(res); lset_err(l, "lookup: out of memory"); return -1;
            }
        }
    }
    for (int r = 0; r < n_rows; ++r) {
        for (int c = 0; c < n_total; ++c) {
            if (PQgetisnull(res, r, c)) {
                lset_err(l, "lookup: NULL in cached row %d col %d not supported in v0.1", r, c);
                free(fmts); PQclear(res); return -1;
            }
            const char *v = PQgetvalue(res, r, c);
            if (pg_fmt_is_int(fmts[c])) {
                char *end = NULL;
                long long iv = strtoll(v, &end, 10);
                if (end == v || *end != '\0') {
                    lset_err(l, "lookup: row %d col %d not a valid int ('%s')", r, c, v);
                    free(fmts); PQclear(res); return -1;
                }
                l->cache_i64[c][r] = (int64_t)iv;
            } else {
                size_t len = (size_t)PQgetlength(res, r, c);
                char *dup = malloc(len + 1);
                if (!dup) {
                    free(fmts); PQclear(res); lset_err(l, "lookup: OOM"); return -1;
                }
                if (len) memcpy(dup, v, len);
                dup[len] = '\0';
                l->cache_u8s[c][r] = dup;
                l->cache_u8l[c][r] = len;
            }
        }
    }
    free(fmts);
    PQclear(res);
    l->cache_built = 1;
    return 0;
}

/* Cache upstream input schema; resolve match input column indices and
 * verify their formats agree with the cached match column formats. */
static int lookup_resolve_schema(LookupState *l) {
    if (l->schema_cached) return 0;
    if (!l->have_input || !l->input.get_schema) {
        lset_err(l, "lookup: input has no get_schema");
        return -1;
    }
    if (lookup_build_cache(l) != 0) return -1;

    struct ArrowSchema sch = {0};
    if (l->input.get_schema(&l->input, &sch) != 0) {
        lset_err(l, "lookup: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        lset_err(l, "lookup: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) { free(names); free(fmts);
        lset_err(l, "lookup: out of memory"); goto done; }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        int is_int = fmt && (fmt[0] == 'l' || fmt[0] == 'i' || fmt[0] == 's')
                     && fmt[1] == '\0';
        int is_utf = fmt && strcmp(fmt, "u") == 0;
        if (!is_int && !is_utf) {
            lset_err(l, "lookup: input column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
        fmts[i] = fmt[0];
        names[i] = strdup((c && c->name) ? c->name : "");
        if (!names[i]) {
            lset_err(l, "lookup: out of memory");
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
    }
    /* Resolve match input cols. */
    for (size_t i = 0; i < l->n_matches; ++i) {
        LkMatch *m = &l->matches[i];
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], m->input_col) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            lset_err(l, "lookup: match input column '%s' not found", m->input_col);
            for (size_t k = 0; k < n; ++k) free(names[k]);
            free(names); free(fmts);
            goto done;
        }
        if (fmts[idx] != m->fmt) {
            lset_err(l, "lookup: match '%s'/'%s': input fmt '%c' but lookup fmt '%c'",
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


/* ============================================================== *
 *  get_schema + get_next                                           *
 * ============================================================== */

/* Build a fresh leaf schema for a column with the given name + format. */
static struct ArrowSchema *new_leaf(const char *name, const char *fmt) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    char *nm = strdup(name);
    if (!c || !nm) { free(c); free(nm); return NULL; }
    c->format  = fmt;
    c->name    = nm;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = pgl_release_schema_named;
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
        const char *fmt = "u";
        switch (l->input_col_fmts[i]) {
            case 'l': fmt = "l"; break;
            case 'i': fmt = "i"; break;
            case 's': fmt = "s"; break;
            case 'u': fmt = "u"; break;
            default:  fmt = "u"; break;
        }
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
        const char *fmt = "u";
        switch (l->selects[i].fmt) {
            case 'l': fmt = "l"; break;
            case 'i': fmt = "i"; break;
            case 's': fmt = "s"; break;
            case 'u': fmt = "u"; break;
            default:  fmt = "u"; break;
        }
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
    out->release    = pgl_release_schema_struct;
    return 0;
}

/* Find a unique cached row whose match cols equal the given values.
 * Returns:
 *   >= 0 — row index
 *   -1   — no match
 *   -2   — multiple matches (lookup spec violation) */
static int lookup_probe(const LookupState *l,
                        const int64_t *l_i64_at_match,
                        char *const *l_u8s_at_match,
                        const size_t *l_u8l_at_match) {
    int found = -1;
    for (size_t r = 0; r < l->cache_rows; ++r) {
        int eq = 1;
        for (size_t k = 0; k < l->n_matches; ++k) {
            if (pg_fmt_is_int(l->matches[k].fmt)) {
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

/* Read a typed int cell out of an Arrow leaf at row r (offsets handled).
 * Widens narrow int leaves (s/i/l) into int64. */
static void read_int_cell_at(const struct ArrowArray *col, char fmt,
                             size_t r, int64_t *out) {
    size_t row = r + (size_t)col->offset;
    switch (fmt) {
        case 'l': *out = ((const int64_t *)col->buffers[1])[row]; return;
        case 'i': *out = ((const int32_t *)col->buffers[1])[row]; return;
        case 's': *out = ((const int16_t *)col->buffers[1])[row]; return;
        default:  *out = 0; return;
    }
}

static void read_utf8_cell_at(const struct ArrowArray *col, size_t r,
                              const char **out_data, size_t *out_len) {
    size_t row = r + (size_t)col->offset;
    const int32_t *off = col->buffers[1];
    const char    *dat = col->buffers[2];
    *out_data = dat + off[row];
    *out_len  = (size_t)(off[row + 1] - off[row]);
}

/* Build an int64 leaf with optional null tracking. nulls[i] non-zero
 * → null at row i. Pass NULL nulls if no nulls. */
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
    out->release    = pgl_release_int64_leaf;
    return 0;
}

/* Narrow-int leaf: vals[] is the int64 stash, elem_size selects 2/4/8. */
static int build_narrow_int_leaf(struct ArrowArray *out, size_t elem_size,
                                 const int64_t *vals, const uint8_t *nulls,
                                 size_t n) {
    if (elem_size == 8) return build_int64_leaf(out, vals, nulls, n);
    if (elem_size != 2 && elem_size != 4) return -1;
    uint8_t *vbuf = malloc((n ? n : 1) * elem_size);
    if (!vbuf) return -1;
    for (size_t i = 0; i < n; ++i) {
        int64_t v = (nulls && nulls[i]) ? 0 : vals[i];
        if (elem_size == 2) { int16_t b = (int16_t)v; memcpy(vbuf + i * 2, &b, 2); }
        else                { int32_t b = (int32_t)v; memcpy(vbuf + i * 4, &b, 4); }
    }
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
    bufs[0] = vmap; bufs[1] = vbuf;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = pgl_release_int64_leaf;  /* same shape */
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
    out->release    = pgl_release_utf8_leaf;
    return 0;
}

static int lookup_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    LookupState *l = st->private_data;
    memset(out, 0, sizeof *out);
    if (!l) return EINVAL;
    if (lookup_resolve_schema(l) != 0) return EIO;

    /* Pull one upstream batch. */
    struct ArrowArray batch = {0};
    if (l->input.get_next(&l->input, &batch) != 0) {
        const char *e = l->input.get_last_error
                            ? l->input.get_last_error(&l->input) : NULL;
        lset_err(l, "lookup: upstream get_next failed: %s", e ? e : "(no detail)");
        return EIO;
    }
    if (!batch.release) return 0;
    if (batch.n_children != (int64_t)l->n_input_cols) {
        int64_t got = batch.n_children;
        size_t expected = l->n_input_cols;
        batch.release(&batch);
        lset_err(l, "lookup: batch has %lld cols, expected %zu",
                 (long long)got, expected);
        return EIO;
    }
    size_t length = (size_t)batch.length;

    /* For each input row, probe the cache and decide whether to keep. */
    int *match_row = malloc((length ? length : 1) * sizeof *match_row);
    uint8_t *keep  = malloc((length ? length : 1) * sizeof *keep);
    if (!match_row || !keep) {
        free(match_row); free(keep);
        batch.release(&batch);
        lset_err(l, "lookup: out of memory"); return EIO;
    }
    /* Pre-extract per-row match values into reusable scratch buffers. */
    int64_t      *l_i64 = calloc(l->n_matches ? l->n_matches : 1, sizeof *l_i64);
    const char  **l_str = calloc(l->n_matches ? l->n_matches : 1, sizeof *l_str);
    size_t       *l_len = calloc(l->n_matches ? l->n_matches : 1, sizeof *l_len);
    if (!l_i64 || !l_str || !l_len) {
        free(l_i64); free(l_str); free(l_len);
        free(match_row); free(keep); batch.release(&batch);
        lset_err(l, "lookup: out of memory"); return EIO;
    }

    size_t n_kept = 0;
    for (size_t r = 0; r < length; ++r) {
        for (size_t k = 0; k < l->n_matches; ++k) {
            const struct ArrowArray *col = batch.children[l->matches[k].input_idx];
            if (pg_fmt_is_int(l->matches[k].fmt)) {
                read_int_cell_at(col, l->matches[k].fmt, r, &l_i64[k]);
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
            lset_err(l, "lookup: row %zu has multiple matches in '%s'", r, l->table);
            return EIO;
        }
        match_row[r] = mr;
        if (mr < 0 && l->on_miss == OM_ERROR) {
            free(l_i64); free(l_str); free(l_len);
            free(match_row); free(keep); batch.release(&batch);
            lset_err(l, "lookup: row %zu missed in '%s' (on_miss=error)", r, l->table);
            return EIO;
        }
        if (mr < 0 && l->on_miss == OM_DROP) {
            keep[r] = 0;
        } else {
            keep[r] = 1; ++n_kept;
        }
    }
    free(l_i64); free(l_str); free(l_len);

    /* Build output rows: keep input row r if keep[r]; for each select
     * col, take cached value if match_row[r] >= 0, else null (for
     * on_miss=null). */
    size_t n_total = l->n_input_cols + l->n_selects;
    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        free(match_row); free(keep); batch.release(&batch);
        lset_err(l, "lookup: out of memory"); return EIO;
    }

    /* Input cols: deep-copy with selection. */
    for (size_t c = 0; c < l->n_input_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) goto fail;
        const struct ArrowArray *src = batch.children[c];
        char fmt = l->input_col_fmts[c];
        if (pg_fmt_is_int(fmt)) {
            int64_t *v = malloc((n_kept ? n_kept : 1) * sizeof *v);
            if (!v) goto fail;
            size_t w = 0;
            for (size_t r = 0; r < length; ++r) {
                if (!keep[r]) continue;
                int64_t cell;
                read_int_cell_at(src, fmt, r, &cell);
                v[w++] = cell;
            }
            size_t elem = (fmt == 's') ? 2 : (fmt == 'i') ? 4 : 8;
            if (build_narrow_int_leaf(kids[c], elem, v, NULL, n_kept) != 0) {
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

    /* Select cols: pulled from cache (or null for misses). */
    for (size_t s = 0; s < l->n_selects; ++s) {
        size_t out_idx = l->n_input_cols + s;
        kids[out_idx] = calloc(1, sizeof **kids);
        if (!kids[out_idx]) goto fail;
        size_t cache_col = l->n_matches + s;
        if (pg_fmt_is_int(l->selects[s].fmt)) {
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
            size_t elem = (l->selects[s].fmt == 's') ? 2
                        : (l->selects[s].fmt == 'i') ? 4 : 8;
            int rc = build_narrow_int_leaf(kids[out_idx], elem, v, nulls, n_kept);
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
        lset_err(l, "lookup: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n_kept;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = pgl_release_struct;

    batch.release(&batch);
    return 0;

fail:
    for (size_t c = 0; c < n_total; ++c) {
        if (kids[c] && kids[c]->release) kids[c]->release(kids[c]);
        free(kids[c]);
    }
    free(kids); free(match_row); free(keep);
    batch.release(&batch);
    lset_err(l, "lookup: failed to build output column");
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
    { .name               = "postgres.lookup",
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
    .name            = "betl-builtins-postgres-lookup",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = lookup_components,
    .component_count = sizeof lookup_components / sizeof lookup_components[0],
};

int betl_register_postgres_lookup(BetlRegistry *r) {
    return betl_registry_register(r, &lookup_provider, "<builtin:postgres-lookup>");
}
