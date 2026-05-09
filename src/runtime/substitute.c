#include "runtime/substitute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================== *
 *  Helpers                                                         *
 * ============================================================== */

static int name_char(char c, int first) {
    if (first) return (c >= 'A' && c <= 'Z')
                  || (c >= 'a' && c <= 'z')
                  ||  c == '_';
    return (c >= '0' && c <= '9')
        || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        ||  c == '_';
}

/* Buffer that grows as we append. Sized aggressively because we copy
 * the source as we go so the resulting string is at least as long. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static int reserve(Buf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < need) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap  = cap;
    return 0;
}

static int put_char(Buf *b, char c) {
    if (reserve(b, 1) != 0) return -1;
    b->data[b->len++] = c;
    return 0;
}

static int put_lit(Buf *b, const char *s, size_t n) {
    if (reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 0;
}

/* If src starts with `${prefix.NAME}`, decode it. On match returns the
 * length consumed from src (so caller can advance) and sets *name_out
 * (NUL-terminated, points into a caller-owned buffer of `name_cap`).
 * Returns 0 if no match. Returns -1 on syntax error (truncated etc),
 * with `err` filled in. */
static int try_match(const char *src, const char *prefix,
                     char *name_out, size_t name_cap,
                     char *err, size_t err_cap) {
    size_t plen = strlen(prefix);
    /* Need at least `${PREFIX.X}` */
    if (src[0] != '$' || src[1] != '{') return 0;
    if (strncmp(src + 2, prefix, plen) != 0) return 0;
    if (src[2 + plen] != '.') return 0;

    const char *q = src + 2 + plen + 1;
    const char *name_start = q;
    if (!name_char(*q, 1)) {
        snprintf(err, err_cap,
            "${%s.X}: name must start with a letter or underscore", prefix);
        return -1;
    }
    ++q;
    while (name_char(*q, 0)) ++q;
    if (*q != '}') {
        snprintf(err, err_cap,
            "${%s.X}: missing closing `}` after '%.*s'",
            prefix, (int)(q - name_start), name_start);
        return -1;
    }
    size_t name_len = (size_t)(q - name_start);
    if (name_len + 1 > name_cap) {
        snprintf(err, err_cap, "${%s.X}: variable name too long", prefix);
        return -1;
    }
    memcpy(name_out, name_start, name_len);
    name_out[name_len] = '\0';
    return (int)((q + 1) - src);   /* total consumed */
}

/* ============================================================== *
 *  Public API                                                      *
 * ============================================================== */

char *betl_substitute_refs(const char *src,
                           BetlContext *ctx,
                           char *err_buf, size_t err_cap) {
    if (err_buf && err_cap > 0) err_buf[0] = '\0';
    if (!src) {
        if (err_buf) snprintf(err_buf, err_cap, "null source");
        return NULL;
    }

    Buf b = {0};
    if (reserve(&b, strlen(src) + 1) != 0) goto oom;

    for (const char *p = src; *p; ) {
        if (p[0] != '$' || p[1] != '{') {
            if (put_char(&b, *p++) != 0) goto oom;
            continue;
        }

        char name[256];
        char inner_err[256];
        int consumed;

        /* env.X */
        consumed = try_match(p, "env", name, sizeof name,
                             inner_err, sizeof inner_err);
        if (consumed < 0) {
            if (err_buf) snprintf(err_buf, err_cap, "%s", inner_err);
            free(b.data);
            return NULL;
        }
        if (consumed > 0) {
            const char *val = getenv(name);
            if (!val) {
                if (err_buf) snprintf(err_buf, err_cap,
                    "${env.%s}: environment variable is not set", name);
                free(b.data);
                return NULL;
            }
            if (put_lit(&b, val, strlen(val)) != 0) goto oom;
            p += consumed;
            continue;
        }

        /* params.X */
        consumed = try_match(p, "params", name, sizeof name,
                             inner_err, sizeof inner_err);
        if (consumed < 0) {
            if (err_buf) snprintf(err_buf, err_cap, "%s", inner_err);
            free(b.data);
            return NULL;
        }
        if (consumed > 0) {
            if (!ctx) {
                if (err_buf) snprintf(err_buf, err_cap,
                    "${params.%s}: no context available", name);
                free(b.data);
                return NULL;
            }
            const char *val = betl_get_param(ctx, name);
            if (!val) {
                if (err_buf) snprintf(err_buf, err_cap,
                    "${params.%s}: parameter is not set", name);
                free(b.data);
                return NULL;
            }
            if (put_lit(&b, val, strlen(val)) != 0) goto oom;
            p += consumed;
            continue;
        }

        /* Unknown ${...} prefix: pass through unchanged. */
        if (put_char(&b, *p++) != 0) goto oom;
    }

    if (reserve(&b, 0) != 0) goto oom;
    b.data[b.len] = '\0';
    return b.data;

oom:
    if (err_buf) snprintf(err_buf, err_cap, "out of memory");
    free(b.data);
    return NULL;
}
