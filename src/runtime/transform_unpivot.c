/* `unpivot` TRANSFORM. Turns a row with N value columns into N rows,
 * one per value column, repeating the id columns.
 *
 * YAML:
 *   - id: u
 *     type: unpivot
 *     from: source
 *     id_cols:    [region, year]    # passed through verbatim
 *     value_cols: [jan, feb, mar]   # become rows; share one format
 *     name_col:   month             # new utf8 column with the col name
 *     value_col:  amount            # new column, format = value_cols[*]
 *
 * Streaming-natural: per input batch of N rows we emit one output batch
 * of N * len(value_cols) rows. NULL value cells pass through as NULL.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/transforms_internal.h"

typedef struct {
    BetlContext              *ctx;
    struct ArrowArrayStream   input;
    int                       have_input;

    /* config */
    char **id_col_names;     size_t n_id;
    char **value_col_names;  size_t n_value;
    char  *name_col;
    char  *value_col;

    /* cached schema resolution */
    int    schema_resolved;
    size_t n_input_cols;
    char **input_col_names;
    char  *input_col_fmts;
    int   *id_idx;                   /* indices in input batch */
    int   *value_idx;                /* indices in input batch */
    char   value_fmt;                /* shared format of value_cols */
    char *value_fmt_str;             /* full Arrow format string for value_col */

    int    emitted_eof;
    char   last_err[256];
} UnpivotState;

static void upset_err(UnpivotState *u, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->last_err, sizeof u->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(u->ctx, "%s", u->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { char ***arr; size_t *n; int err; } StrlistCtx;
static int strlist_visit(const char *value, size_t value_len, void *user) {
    StrlistCtx *c = user;
    if (value_len == 0 || value[0] != '"') { c->err = 1; return -1; }
    char *s = NULL;
    if (betl_tx_json_decode_str(value, &s) != 0 || !s) { c->err = 1; return -1; }
    char **grow = realloc(*c->arr, (*c->n + 1) * sizeof *grow);
    if (!grow) { free(s); c->err = 1; return -1; }
    *c->arr = grow;
    (*c->arr)[(*c->n)++] = s;
    return 0;
}

static int unpivot_init(BetlContext *ctx, const char *cfg, void **state) {
    UnpivotState *u = calloc(1, sizeof *u);
    if (!u) return BETL_ERR_INTERNAL;
    u->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    const char *p;
    if ((p = betl_tx_json_value_after(cfg, "id_cols")) && *p == '[') {
        StrlistCtx sc = { .arr = &u->id_col_names, .n = &u->n_id };
        if (betl_tx_json_walk_array(p, strlist_visit, &sc) != 0 || sc.err) {
            betl_set_error(ctx, "unpivot: `id_cols:` must be a list of strings");
            goto fail;
        }
    }
    if ((p = betl_tx_json_value_after(cfg, "value_cols")) && *p == '[') {
        StrlistCtx sc = { .arr = &u->value_col_names, .n = &u->n_value };
        if (betl_tx_json_walk_array(p, strlist_visit, &sc) != 0 || sc.err) {
            betl_set_error(ctx, "unpivot: `value_cols:` must be a list of strings");
            goto fail;
        }
    }
    if (u->n_value == 0) {
        betl_set_error(ctx, "unpivot: `value_cols:` is required and must be non-empty");
        goto fail;
    }
    if (betl_tx_json_string_at(cfg, "name_col", &u->name_col) != 0 || !u->name_col) {
        betl_set_error(ctx, "unpivot: `name_col:` is required");
        goto fail;
    }
    if (betl_tx_json_string_at(cfg, "value_col", &u->value_col) != 0 || !u->value_col) {
        betl_set_error(ctx, "unpivot: `value_col:` is required");
        goto fail;
    }
    *state = u;
    return BETL_OK;
fail:
    for (size_t i = 0; i < u->n_id; ++i) free(u->id_col_names[i]);
    free(u->id_col_names);
    for (size_t i = 0; i < u->n_value; ++i) free(u->value_col_names[i]);
    free(u->value_col_names);
    free(u->name_col); free(u->value_col);
    free(u);
    return BETL_ERR_INVALID;
}

static void unpivot_destroy(void *state) {
    if (!state) return;
    UnpivotState *u = state;
    if (u->have_input && u->input.release) u->input.release(&u->input);
    for (size_t i = 0; i < u->n_id; ++i) free(u->id_col_names[i]);
    free(u->id_col_names);
    for (size_t i = 0; i < u->n_value; ++i) free(u->value_col_names[i]);
    free(u->value_col_names);
    if (u->input_col_names) {
        for (size_t i = 0; i < u->n_input_cols; ++i) free(u->input_col_names[i]);
        free(u->input_col_names);
    }
    free(u->input_col_fmts);
    free(u->id_idx);
    free(u->value_idx);
    free(u->value_fmt_str);
    free(u->name_col);
    free(u->value_col);
    free(u);
}

static int unpivot_attach_input(void *state, int port,
                                struct ArrowArrayStream *in) {
    (void)port;
    UnpivotState *u = state;
    u->input      = *in;
    u->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  Schema resolution                                               *
 * ============================================================== */

static int unpivot_resolve_schema(UnpivotState *u) {
    if (u->schema_resolved) return 0;
    if (!u->have_input || !u->input.get_schema) {
        upset_err(u, "unpivot: no input attached");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (u->input.get_schema(&u->input, &sch) != 0) {
        upset_err(u, "unpivot: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        upset_err(u, "unpivot: input must be a struct with >=1 child");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    u->input_col_names = calloc(n, sizeof *u->input_col_names);
    u->input_col_fmts  = calloc(n, 1);
    if (!u->input_col_names || !u->input_col_fmts) {
        upset_err(u, "unpivot: out of memory"); goto done;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt) { upset_err(u, "unpivot: column %zu has no format", i); goto done; }
        u->input_col_fmts[i] = fmt[0];
        u->input_col_names[i] = strdup(c->name ? c->name : "");
        if (!u->input_col_names[i]) { upset_err(u, "unpivot: out of memory"); goto done; }
    }
    u->n_input_cols = n;

    u->id_idx    = calloc(u->n_id ? u->n_id : 1, sizeof *u->id_idx);
    u->value_idx = calloc(u->n_value, sizeof *u->value_idx);
    if ((u->n_id && !u->id_idx) || !u->value_idx) {
        upset_err(u, "unpivot: out of memory"); goto done;
    }
    for (size_t i = 0; i < u->n_id; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(u->input_col_names[k], u->id_col_names[i]) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            upset_err(u, "unpivot: id_col '%s' not found in input", u->id_col_names[i]);
            goto done;
        }
        u->id_idx[i] = idx;
    }
    /* value_cols must share a single format. Capture the full format
     * string for the first one for schema emission. */
    for (size_t i = 0; i < u->n_value; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(u->input_col_names[k], u->value_col_names[i]) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            upset_err(u, "unpivot: value_col '%s' not found in input", u->value_col_names[i]);
            goto done;
        }
        u->value_idx[i] = idx;
        if (i == 0) {
            u->value_fmt = u->input_col_fmts[idx];
            const char *fs = sch.children[idx]->format;
            u->value_fmt_str = strdup(fs ? fs : "");
            if (!u->value_fmt_str) { upset_err(u, "unpivot: out of memory"); goto done; }
        } else if (u->input_col_fmts[idx] != u->value_fmt) {
            upset_err(u,
                "unpivot: value_cols must share one format; '%s' is '%c' but '%s' is '%c'",
                u->value_col_names[0], u->value_fmt,
                u->value_col_names[i], u->input_col_fmts[idx]);
            goto done;
        }
    }
    /* The value format must be one we know how to copy. */
    if (betl_tx_fixed_width_for_fmt(u->value_fmt) == 0 && u->value_fmt != 'u') {
        upset_err(u, "unpivot: value_col format '%c' not supported", u->value_fmt);
        goto done;
    }

    u->schema_resolved = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

/* ============================================================== *
 *  Schema + batch emission                                         *
 * ============================================================== */

static void release_schema_named_leaf(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->name = NULL;
    sch->release = NULL;
}

static void release_schema_struct_owned(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i]) {
            if (sch->children[i]->release) sch->children[i]->release(sch->children[i]);
            free(sch->children[i]);
        }
    }
    free(sch->children);
    sch->release = NULL;
}

static struct ArrowSchema *new_leaf(const char *name, const char *fmt) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->name    = strdup(name);
    if (!c->name) { free(c); return NULL; }
    c->format  = fmt;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = release_schema_named_leaf;
    return c;
}

static int unpivot_get_schema(struct ArrowArrayStream *st,
                              struct ArrowSchema *out) {
    UnpivotState *u = st->private_data;
    memset(out, 0, sizeof *out);
    if (!u) return EINVAL;
    if (unpivot_resolve_schema(u) != 0) return EIO;

    struct ArrowSchema up = {0};
    if (u->input.get_schema(&u->input, &up) != 0) return EIO;

    size_t n_total = u->n_id + 2;     /* + name_col + value_col */
    struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
    if (!kids) { if (up.release) up.release(&up); return ENOMEM; }
    for (size_t i = 0; i < u->n_id; ++i) {
        struct ArrowSchema *src = up.children[u->id_idx[i]];
        struct ArrowSchema *leaf = new_leaf(u->id_col_names[i], src->format);
        if (!leaf) goto oom;
        kids[i] = leaf;
    }
    kids[u->n_id]     = new_leaf(u->name_col,  "u");
    kids[u->n_id + 1] = new_leaf(u->value_col, u->value_fmt_str);
    if (!kids[u->n_id] || !kids[u->n_id + 1]) goto oom;

    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = release_schema_struct_owned;
    if (up.release) up.release(&up);
    return 0;
oom:
    for (size_t i = 0; i < n_total; ++i) {
        if (kids[i]) {
            if (kids[i]->release) kids[i]->release(kids[i]);
            free(kids[i]);
        }
    }
    free(kids);
    if (up.release) up.release(&up);
    return ENOMEM;
}

/* Copy a single cell from `src` row `sr` into `dst` row `dr`. Fixed-width
 * branches write directly into a pre-allocated narrow buffer; utf8 appends
 * via the running offsets pair. */
static int copy_fixed_cell(uint8_t *dst_vals, size_t dr, size_t elem_size,
                           uint8_t *dst_validity,
                           const struct ArrowArray *src, size_t sr) {
    size_t srow = sr + (size_t)src->offset;
    int is_null = 0;
    if (src->null_count > 0 && src->buffers[0]) {
        const uint8_t *v = src->buffers[0];
        is_null = !((v[srow / 8] >> (srow % 8)) & 1u);
    }
    if (is_null) {
        memset(dst_vals + dr * elem_size, 0, elem_size);
        dst_validity[dr / 8] &= (uint8_t)~(1u << (dr % 8));
        return 1;
    }
    const uint8_t *sv = src->buffers[1];
    memcpy(dst_vals + dr * elem_size, sv + srow * elem_size, elem_size);
    return 0;
}

/* utf8 cell append. Grows `*data` as needed. Returns 0 ok / 1 if-null. */
static int append_utf8_cell(char **data, size_t *data_len, size_t *data_cap,
                            int32_t *offsets, size_t dr,
                            uint8_t *dst_validity,
                            const struct ArrowArray *src, size_t sr) {
    size_t srow = sr + (size_t)src->offset;
    int is_null = 0;
    if (src->null_count > 0 && src->buffers[0]) {
        const uint8_t *v = src->buffers[0];
        is_null = !((v[srow / 8] >> (srow % 8)) & 1u);
    }
    if (is_null) {
        offsets[dr + 1] = (int32_t)*data_len;
        dst_validity[dr / 8] &= (uint8_t)~(1u << (dr % 8));
        return 1;
    }
    const int32_t *soff = src->buffers[1];
    const char    *sdat = src->buffers[2];
    size_t slen = (size_t)(soff[srow + 1] - soff[srow]);
    if (*data_len + slen > *data_cap) {
        size_t nc = *data_cap ? *data_cap : 64;
        while (nc < *data_len + slen) nc *= 2;
        char *nd = realloc(*data, nc);
        if (!nd) return -1;
        *data = nd; *data_cap = nc;
    }
    if (slen) memcpy(*data + *data_len, sdat + soff[srow], slen);
    *data_len += slen;
    offsets[dr + 1] = (int32_t)*data_len;
    return 0;
}

/* Append a fixed literal utf8 string (the name_col values). */
static int append_utf8_literal(char **data, size_t *data_len, size_t *data_cap,
                               int32_t *offsets, size_t dr,
                               const char *s, size_t slen) {
    if (*data_len + slen > *data_cap) {
        size_t nc = *data_cap ? *data_cap : 64;
        while (nc < *data_len + slen) nc *= 2;
        char *nd = realloc(*data, nc);
        if (!nd) return -1;
        *data = nd; *data_cap = nc;
    }
    if (slen) memcpy(*data + *data_len, s, slen);
    *data_len += slen;
    offsets[dr + 1] = (int32_t)*data_len;
    return 0;
}

static void release_fixed_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

/* Build an Arrow utf8 array from collected offsets + data + validity.
 * Takes ownership of the inputs only on success. On failure, leaves
 * everything intact so the caller's fail path can clean up. */
static int finalize_utf8(struct ArrowArray *out,
                         int32_t *offsets, char *data, size_t data_len,
                         uint8_t *validity, size_t n, int64_t null_count) {
    (void)data_len;
    char *data_to_store = data ? data : malloc(1);
    if (!data_to_store) return -1;
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) {
        if (data_to_store != data) free(data_to_store);
        return -1;
    }
    bufs[0] = (null_count == 0) ? NULL : validity;
    bufs[1] = offsets;
    bufs[2] = data_to_store;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 3;
    out->buffers    = bufs;
    out->release    = release_utf8_leaf;
    /* Success: now ownership has transferred. Free the validity buffer
     * the caller passed in if we dropped it. */
    if (null_count == 0) free(validity);
    return 0;
}

static int finalize_fixed(struct ArrowArray *out,
                          uint8_t *vals, uint8_t *validity,
                          size_t n, int64_t null_count) {
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) return -1;
    bufs[0] = (null_count == 0) ? NULL : validity;
    bufs[1] = vals;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = release_fixed_leaf;
    if (null_count == 0) free(validity);
    return 0;
}

static int unpivot_get_next(struct ArrowArrayStream *st,
                            struct ArrowArray *out) {
    UnpivotState *u = st->private_data;
    memset(out, 0, sizeof *out);
    if (!u) return EINVAL;
    if (unpivot_resolve_schema(u) != 0) return EIO;
    if (u->emitted_eof) return 0;

    struct ArrowArray in_arr = {0};
    if (u->input.get_next(&u->input, &in_arr) != 0) {
        upset_err(u, "unpivot: upstream get_next failed");
        return EIO;
    }
    if (!in_arr.release) {
        u->emitted_eof = 1;
        return 0;
    }
    size_t n_in  = (size_t)in_arr.length;
    size_t n_out = n_in * u->n_value;
    size_t n_total = u->n_id + 2;     /* always > 0 */

    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) { in_arr.release(&in_arr); upset_err(u, "unpivot: OOM"); return EIO; }

    /* For each id_col: build a fixed-width or utf8 leaf with each input
     * row's value repeated n_value times. */
    size_t bytes = (n_out + 7) / 8;
    for (size_t c = 0; c < u->n_id; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) goto fail;
        char fmt = u->input_col_fmts[u->id_idx[c]];
        size_t w = betl_tx_fixed_width_for_fmt(fmt);
        const struct ArrowArray *src = in_arr.children[u->id_idx[c]];
        uint8_t *validity = malloc(bytes ? bytes : 1);
        if (!validity) goto fail;
        memset(validity, 0xFF, bytes ? bytes : 1);
        int64_t null_count = 0;
        if (w != 0) {
            size_t vsz = n_out * w;
            if (vsz == 0) vsz = w;
            uint8_t *vals = malloc(vsz);
            if (!vals) { free(validity); goto fail; }
            for (size_t r = 0; r < n_in; ++r) {
                for (size_t k = 0; k < u->n_value; ++k) {
                    size_t dr = r * u->n_value + k;
                    int is_null = copy_fixed_cell(vals, dr, w, validity, src, r);
                    if (is_null) ++null_count;
                }
            }
            if (finalize_fixed(kids[c], vals, validity, n_out, null_count) != 0) {
                free(vals); free(validity); goto fail;
            }
        } else if (fmt == 'u') {
            int32_t *offsets = malloc((n_out + 1) * sizeof *offsets);
            char    *data    = NULL;
            size_t   data_len = 0, data_cap = 0;
            if (!offsets) { free(validity); goto fail; }
            offsets[0] = 0;
            for (size_t r = 0; r < n_in; ++r) {
                for (size_t k = 0; k < u->n_value; ++k) {
                    size_t dr = r * u->n_value + k;
                    int rc = append_utf8_cell(&data, &data_len, &data_cap,
                                              offsets, dr, validity, src, r);
                    if (rc < 0) { free(offsets); free(data); free(validity); goto fail; }
                    if (rc == 1) ++null_count;
                }
            }
            if (finalize_utf8(kids[c], offsets, data, data_len,
                              validity, n_out, null_count) != 0) {
                free(offsets); free(data); free(validity); goto fail;
            }
        } else {
            free(validity);
            upset_err(u, "unpivot: id_col format '%c' not supported", fmt);
            goto fail;
        }
    }

    /* name_col: utf8 cycling through value_col_names. */
    {
        kids[u->n_id] = calloc(1, sizeof **kids);
        if (!kids[u->n_id]) goto fail;
        int32_t *offsets = malloc((n_out + 1) * sizeof *offsets);
        char    *data    = NULL;
        size_t   data_len = 0, data_cap = 0;
        uint8_t *validity = malloc(bytes ? bytes : 1);
        if (!offsets || !validity) { free(offsets); free(validity); goto fail; }
        memset(validity, 0xFF, bytes ? bytes : 1);
        offsets[0] = 0;
        for (size_t r = 0; r < n_in; ++r) {
            for (size_t k = 0; k < u->n_value; ++k) {
                size_t dr = r * u->n_value + k;
                const char *s = u->value_col_names[k];
                if (append_utf8_literal(&data, &data_len, &data_cap,
                                        offsets, dr, s, strlen(s)) != 0) {
                    free(offsets); free(data); free(validity); goto fail;
                }
            }
        }
        if (finalize_utf8(kids[u->n_id], offsets, data, data_len,
                          validity, n_out, 0) != 0) {
            free(offsets); free(data); free(validity); goto fail;
        }
    }

    /* value_col: same format as input value_cols. */
    {
        kids[u->n_id + 1] = calloc(1, sizeof **kids);
        if (!kids[u->n_id + 1]) goto fail;
        size_t w = betl_tx_fixed_width_for_fmt(u->value_fmt);
        uint8_t *validity = malloc(bytes ? bytes : 1);
        if (!validity) goto fail;
        memset(validity, 0xFF, bytes ? bytes : 1);
        int64_t null_count = 0;
        if (w != 0) {
            size_t vsz = n_out * w;
            if (vsz == 0) vsz = w;
            uint8_t *vals = malloc(vsz);
            if (!vals) { free(validity); goto fail; }
            for (size_t r = 0; r < n_in; ++r) {
                for (size_t k = 0; k < u->n_value; ++k) {
                    size_t dr = r * u->n_value + k;
                    const struct ArrowArray *src = in_arr.children[u->value_idx[k]];
                    int is_null = copy_fixed_cell(vals, dr, w, validity, src, r);
                    if (is_null) ++null_count;
                }
            }
            if (finalize_fixed(kids[u->n_id + 1], vals, validity,
                               n_out, null_count) != 0) {
                free(vals); free(validity); goto fail;
            }
        } else {
            int32_t *offsets = malloc((n_out + 1) * sizeof *offsets);
            char    *data    = NULL;
            size_t   data_len = 0, data_cap = 0;
            if (!offsets) { free(validity); goto fail; }
            offsets[0] = 0;
            for (size_t r = 0; r < n_in; ++r) {
                for (size_t k = 0; k < u->n_value; ++k) {
                    size_t dr = r * u->n_value + k;
                    const struct ArrowArray *src = in_arr.children[u->value_idx[k]];
                    int rc = append_utf8_cell(&data, &data_len, &data_cap,
                                              offsets, dr, validity, src, r);
                    if (rc < 0) { free(offsets); free(data); free(validity); goto fail; }
                    if (rc == 1) ++null_count;
                }
            }
            if (finalize_utf8(kids[u->n_id + 1], offsets, data, data_len,
                              validity, n_out, null_count) != 0) {
                free(offsets); free(data); free(validity); goto fail;
            }
        }
    }

    {
        const void **outer = malloc(1 * sizeof *outer);
        if (!outer) goto fail;
        outer[0] = NULL;
        out->length     = (int64_t)n_out;
        out->null_count = 0;
        out->n_buffers  = 1;
        out->n_children = (int64_t)n_total;
        out->buffers    = outer;
        out->children   = kids;
        out->release    = betl_tx_release_struct;
    }
    in_arr.release(&in_arr);
    return 0;

fail:
    for (size_t c = 0; c < n_total; ++c) {
        if (kids[c]) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
    }
    free(kids);
    if (in_arr.release) in_arr.release(&in_arr);
    upset_err(u, "unpivot: build failed");
    return EIO;
}

static const char *unpivot_get_last_error(struct ArrowArrayStream *st) {
    UnpivotState *u = st->private_data;
    return (u && u->last_err[0]) ? u->last_err : NULL;
}
static void unpivot_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int unpivot_attach_output(void *state, int port,
                                 struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = unpivot_get_schema;
    out->get_next       = unpivot_get_next;
    out->get_last_error = unpivot_get_last_error;
    out->release        = unpivot_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef unpivot_inputs[] = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "wide rows" },
};
static const BetlPortDef unpivot_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "long rows" },
};

static const BetlComponentDef unpivot_components[] = {
    { .name               = "unpivot",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = unpivot_inputs,
      .input_count        = 1,
      .outputs            = unpivot_outputs,
      .output_count       = 1,
      .init               = unpivot_init,
      .destroy            = unpivot_destroy,
      .attach_input       = unpivot_attach_input,
      .attach_output      = unpivot_attach_output },
};

static const BetlProvider unpivot_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-unpivot",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = unpivot_components,
    .component_count = sizeof unpivot_components / sizeof unpivot_components[0],
};

int betl_tx_register_unpivot(BetlRegistry *r) {
    return betl_registry_register(r, &unpivot_provider, "<builtin:unpivot>");
}
