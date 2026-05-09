#include "runtime/pg_sql.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================== *
 *  Growable buffer                                                 *
 * ============================================================== */

int betl_buf_reserve(BetlBuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < need) new_cap *= 2;
    char *p = realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap  = new_cap;
    return 0;
}

int betl_buf_append(BetlBuf *b, const char *s, size_t n) {
    if (betl_buf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int betl_buf_appendf(BetlBuf *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof tmp) return betl_buf_append(b, tmp, (size_t)n);

    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = betl_buf_append(b, big, (size_t)n);
    free(big);
    return rc;
}

/* ============================================================== *
 *  Identifier quoting                                              *
 * ============================================================== */

static int append_ident(BetlBuf *b, const char *ident) {
    if (strchr(ident, '"') != NULL) return -1;
    if (betl_buf_append(b, "\"", 1) != 0) return -2;
    if (betl_buf_append(b, ident, strlen(ident)) != 0) return -2;
    if (betl_buf_append(b, "\"", 1) != 0) return -2;
    return 0;
}

static int append_table(BetlBuf *b, const char *table) {
    const char *dot = strchr(table, '.');
    if (!dot) return append_ident(b, table);
    size_t left_len = (size_t)(dot - table);
    char *left = strndup(table, left_len);
    if (!left) return -2;
    int rc = append_ident(b, left);
    free(left);
    if (rc != 0) return rc;
    if (betl_buf_append(b, ".", 1) != 0) return -2;
    return append_ident(b, dot + 1);
}

static int is_key(const char *name, char **keys, size_t n_keys) {
    for (size_t i = 0; i < n_keys; ++i) {
        if (strcmp(name, keys[i]) == 0) return 1;
    }
    return 0;
}

/* ============================================================== *
 *  Public API                                                      *
 * ============================================================== */

int betl_parse_on_conflict(const char *s, BetlOnConflict *out) {
    if (!s || strcmp(s, "update") == 0) {
        *out = BETL_OC_UPDATE; return 0;
    }
    if (strcmp(s, "update_if_changed") == 0) {
        *out = BETL_OC_UPDATE_IF_CHANGED; return 0;
    }
    if (strcmp(s, "ignore") == 0) { *out = BETL_OC_IGNORE; return 0; }
    if (strcmp(s, "error")  == 0) { *out = BETL_OC_ERROR;  return 0; }
    return -1;
}

int betl_build_upsert_sql(BetlBuf *out,
                          const char *table,
                          char **cols, size_t n_cols,
                          char **keys, size_t n_keys,
                          BetlOnConflict mode) {
    /* Verify every key is in `cols`. */
    for (size_t i = 0; i < n_keys; ++i) {
        if (!is_key(keys[i], cols, n_cols)) return -3;
    }

    if (betl_buf_append(out, "INSERT INTO ", 12) != 0) return -2;
    int rc = append_table(out, table);
    if (rc < 0) return rc;
    if (betl_buf_append(out, " (", 2) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        rc = append_ident(out, cols[i]);
        if (rc < 0) return rc;
    }
    if (betl_buf_append(out, ") VALUES (", 10) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (betl_buf_appendf(out, "%s$%zu", i ? ", " : "", i + 1) != 0) return -2;
    }
    if (betl_buf_append(out, ")", 1) != 0) return -2;

    if (mode == BETL_OC_ERROR) return 0;

    if (betl_buf_append(out, " ON CONFLICT (", 14) != 0) return -2;
    for (size_t i = 0; i < n_keys; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        rc = append_ident(out, keys[i]);
        if (rc < 0) return rc;
    }
    if (betl_buf_append(out, ")", 1) != 0) return -2;

    if (mode == BETL_OC_IGNORE) {
        return betl_buf_append(out, " DO NOTHING", 11) == 0 ? 0 : -2;
    }

    /* OC_UPDATE / OC_UPDATE_IF_CHANGED: build SET list of non-key cols. */
    int wrote_set = 0;
    for (size_t i = 0; i < n_cols; ++i) {
        if (is_key(cols[i], keys, n_keys)) continue;
        if (!wrote_set) {
            if (betl_buf_append(out, " DO UPDATE SET ", 15) != 0) return -2;
            wrote_set = 1;
        } else if (betl_buf_append(out, ", ", 2) != 0) {
            return -2;
        }
        rc = append_ident(out, cols[i]);
        if (rc < 0) return rc;
        if (betl_buf_append(out, " = EXCLUDED.", 12) != 0) return -2;
        rc = append_ident(out, cols[i]);
        if (rc < 0) return rc;
    }
    if (!wrote_set) {
        return betl_buf_append(out, " DO NOTHING", 11) == 0 ? 0 : -2;
    }

    if (mode == BETL_OC_UPDATE_IF_CHANGED) {
        if (betl_buf_append(out, " WHERE ", 7) != 0) return -2;
        int wrote_where = 0;
        for (size_t i = 0; i < n_cols; ++i) {
            if (is_key(cols[i], keys, n_keys)) continue;
            if (wrote_where && betl_buf_append(out, " OR ", 4) != 0) return -2;
            wrote_where = 1;
            rc = append_table(out, table);
            if (rc < 0) return rc;
            if (betl_buf_append(out, ".", 1) != 0) return -2;
            rc = append_ident(out, cols[i]);
            if (rc < 0) return rc;
            if (betl_buf_append(out, " IS DISTINCT FROM EXCLUDED.", 27) != 0) return -2;
            rc = append_ident(out, cols[i]);
            if (rc < 0) return rc;
        }
    }
    return 0;
}
