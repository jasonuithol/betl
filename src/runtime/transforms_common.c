/* Shared helpers for the standard transforms. See transforms_internal.h
 * for the prototypes. These were originally inline static functions in
 * transforms.c — extracted here so each transform component can live in
 * its own translation unit without duplicating them.
 */

#include "runtime/transforms_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ============================================================== *
 *  JSON helpers                                                    *
 * ============================================================== */

const char *betl_tx_json_value_after(const char *json, const char *key) {
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

int betl_tx_json_decode_str(const char *p, char **out) {
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

int betl_tx_json_string_at(const char *json, const char *key, char **out) {
    return betl_tx_json_decode_str(betl_tx_json_value_after(json, key), out);
}

int betl_tx_json_value_to_string(const char *json, const char *key, char **out) {
    const char *p = betl_tx_json_value_after(json, key);
    if (!p) return -1;
    if (*p == '"') return betl_tx_json_decode_str(p, out);
    const char *end = p;
    while (*end && *end != ',' && *end != '}' && *end != ']'
           && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t') {
        ++end;
    }
    size_t len = (size_t)(end - p);
    if (len == 0) return -1;
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, p, len);
    s[len] = '\0';
    *out = s;
    return 0;
}

int betl_tx_json_walk_object(const char *p,
                             betl_tx_kv_visit_fn cb, void *user) {
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

int betl_tx_json_walk_array(const char *p,
                            betl_tx_item_visit_fn cb, void *user) {
    if (!p || *p != '[') return -1;
    ++p;
    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ']' || *p == '\0') return 0;
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
        while (vlen > 0 && (val_start[vlen - 1] == ' '
                         || val_start[vlen - 1] == '\n'
                         || val_start[vlen - 1] == '\t'
                         || val_start[vlen - 1] == '\r')) {
            --vlen;
        }
        if (cb(val_start, vlen, user) != 0) return -1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == ']' || *p == '\0') return 0;
        return -1;
    }
}


/* ============================================================== *
 *  Arrow leaf / schema releases                                    *
 * ============================================================== */

void betl_tx_release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

void betl_tx_release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

void betl_tx_release_struct(struct ArrowArray *arr) {
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

void betl_tx_release_schema_named_leaf(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->release = NULL;
}

void betl_tx_release_schema_struct_owned(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i] && sch->children[i]->release) {
            sch->children[i]->release(sch->children[i]);
        }
        free(sch->children[i]);
    }
    free(sch->children);
    sch->release = NULL;
}

struct ArrowSchema *betl_tx_new_leaf_schema(const char *name, const char *format) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    char *nm = strdup(name);
    if (!c || !nm) { free(c); free(nm); return NULL; }
    c->format  = format;
    c->name    = nm;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = betl_tx_release_schema_named_leaf;
    return c;
}


/* ============================================================== *
 *  Row-mask leaf builders                                          *
 *                                                                  *
 *  Used by every transform that selects a subset of input rows by  *
 *  a boolean keep[] mask: filter, distinct, limit, split. Builds   *
 *  a fresh int64 / utf8 leaf containing the kept rows in order,    *
 *  preserving NULL-ness from the input via the validity bitmap.    *
 * ============================================================== */

int betl_tx_build_int64_filtered(struct ArrowArray *out,
                                 const struct ArrowArray *src,
                                 const uint8_t *keep, size_t n_rows,
                                 size_t n_kept) {
    int64_t *vals = malloc((n_kept ? n_kept : 1) * sizeof *vals);
    if (!vals) return -1;
    size_t bytes = (n_kept + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!vmap) { free(vals); return -1; }
    memset(vmap, 0xFF, bytes ? bytes : 1);

    int64_t null_count = 0;
    const uint8_t *src_valid = (src->null_count > 0) ? src->buffers[0] : NULL;
    const int64_t *src_vals  = src->buffers[1];
    size_t off = (size_t)src->offset;
    size_t w = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!keep[i]) continue;
        size_t row = i + off;
        int is_null = src_valid && !betl_tx_bit_at(src_valid, row);
        if (is_null) {
            vmap[w / 8] &= (uint8_t)~(1u << (w % 8));
            ++null_count;
        } else {
            vals[w] = src_vals[row];
        }
        ++w;
    }
    if (null_count == 0) { free(vmap); vmap = NULL; }

    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = vals;
    out->length     = (int64_t)n_kept;
    out->null_count = null_count;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = betl_tx_release_int64_leaf;
    return 0;
}

int betl_tx_build_utf8_filtered(struct ArrowArray *out,
                                const struct ArrowArray *src,
                                const uint8_t *keep, size_t n_rows,
                                size_t n_kept) {
    const uint8_t *src_valid = (src->null_count > 0) ? src->buffers[0] : NULL;
    const int32_t *src_off   = src->buffers[1];
    const char    *src_data  = src->buffers[2];
    size_t off = (size_t)src->offset;

    /* First pass: total bytes for kept non-null strings. */
    size_t total = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!keep[i]) continue;
        size_t row = i + off;
        if (src_valid && !betl_tx_bit_at(src_valid, row)) continue;
        total += (size_t)(src_off[row + 1] - src_off[row]);
    }

    int32_t *offs = malloc((n_kept + 1) * sizeof *offs);
    char    *data = malloc(total ? total : 1);
    size_t bytes  = (n_kept + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!offs || !data || !vmap) {
        free(offs); free(data); free(vmap); return -1;
    }
    memset(vmap, 0xFF, bytes ? bytes : 1);
    offs[0] = 0;

    int64_t null_count = 0;
    size_t  pos = 0;
    size_t  w   = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!keep[i]) continue;
        size_t row = i + off;
        if (src_valid && !betl_tx_bit_at(src_valid, row)) {
            vmap[w / 8] &= (uint8_t)~(1u << (w % 8));
            ++null_count;
        } else {
            size_t slen = (size_t)(src_off[row + 1] - src_off[row]);
            if (slen) memcpy(data + pos, src_data + src_off[row], slen);
            pos += slen;
        }
        offs[w + 1] = (int32_t)pos;
        ++w;
    }
    if (null_count == 0) { free(vmap); vmap = NULL; }

    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = offs;
    bufs[2] = data;
    out->length     = (int64_t)n_kept;
    out->null_count = null_count;
    out->offset     = 0;
    out->n_buffers  = 3;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = betl_tx_release_utf8_leaf;
    return 0;
}
