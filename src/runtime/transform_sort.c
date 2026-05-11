/* `sort` TRANSFORM (SPEC §4.3).
 *
 * v0.1: materialize all rows, qsort by index using a multi-key
 * comparator (asc/desc per key, stable tie-break on original row),
 * emit one batch in sorted order. int64 + utf8 cells supported.
 */

#include <ctype.h>
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
    char *col_name;
    int   asc;          /* 1 = ascending, 0 = descending */
    int   col_idx;      /* resolved at first batch */
    char  fmt;          /* 'l' or 'u' — same as input col */
} SortKey;

typedef struct {
    BetlContext *ctx;

    SortKey *keys;
    size_t   n_keys;

    struct ArrowArrayStream input;
    int                     have_input;

    int    schema_cached;
    size_t n_input_cols;
    char **col_names;
    char  *col_fmts;

    int64_t **i64_vals;
    char  ***u8_strs;
    size_t **u8_lens;
    size_t   n_rows;
    size_t   row_cap;

    int materialized;
    int emitted;

    char last_err[256];
} SortState;

static void sset_err(SortState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

typedef struct { SortState *s; int err; } SortCtx;

static int sort_by_visit(const char *value, size_t value_len, void *user) {
    SortCtx *c = user;
    SortState *s = c->s;
    SortKey *grow = realloc(s->keys, (s->n_keys + 1) * sizeof *grow);
    if (!grow) { c->err = 1; return -1; }
    s->keys = grow;
    SortKey *k = &s->keys[s->n_keys++];
    memset(k, 0, sizeof *k);
    k->asc = 1;
    k->col_idx = -1;

    if (value_len == 0) { c->err = 1; return -1; }
    if (value[0] == '"') {
        if (betl_tx_json_decode_str(value, &k->col_name) != 0 || !k->col_name) {
            c->err = 1; return -1;
        }
        return 0;
    }
    if (value[0] != '{') {
        sset_err(s, "sort: by entry must be a string or {col, dir} object");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *col = NULL, *dir = NULL;
    betl_tx_json_string_at(vbuf, "col", &col);
    betl_tx_json_string_at(vbuf, "dir", &dir);
    free(vbuf);
    if (!col) {
        free(dir);
        sset_err(s, "sort: by entry needs a `col` field");
        c->err = 1; return -1;
    }
    k->col_name = col;
    if (dir) {
        if (strcmp(dir, "asc") == 0)        k->asc = 1;
        else if (strcmp(dir, "desc") == 0)  k->asc = 0;
        else {
            sset_err(s, "sort: dir must be 'asc' or 'desc' (got '%s')", dir);
            free(dir); c->err = 1; return -1;
        }
        free(dir);
    }
    return 0;
}

static int sort_init(BetlContext *ctx, const char *cfg, void **state) {
    SortState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";
    const char *by = betl_tx_json_value_after(cfg, "by");
    if (!by || *by != '[') {
        sset_err(s, "sort: requires a `by:` list");
        free(s); return BETL_ERR_INVALID;
    }
    SortCtx c = { .s = s, .err = 0 };
    if (betl_tx_json_walk_array(by, sort_by_visit, &c) != 0 || c.err
        || s->n_keys == 0) {
        if (s->last_err[0] == '\0') sset_err(s, "sort: `by:` is empty");
        for (size_t i = 0; i < s->n_keys; ++i) free(s->keys[i].col_name);
        free(s->keys); free(s);
        return BETL_ERR_INVALID;
    }
    *state = s;
    return BETL_OK;
}

static int sort_attach_input(void *state, int port,
                             struct ArrowArrayStream *in) {
    (void)port;
    SortState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void sort_destroy(void *state) {
    if (!state) return;
    SortState *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    for (size_t i = 0; i < s->n_keys; ++i) free(s->keys[i].col_name);
    free(s->keys);
    if (s->col_names) {
        for (size_t i = 0; i < s->n_input_cols; ++i) free(s->col_names[i]);
        free(s->col_names);
    }
    free(s->col_fmts);
    if (s->i64_vals) {
        for (size_t c = 0; c < s->n_input_cols; ++c) free(s->i64_vals[c]);
        free(s->i64_vals);
    }
    if (s->u8_strs) {
        for (size_t c = 0; c < s->n_input_cols; ++c) {
            if (s->u8_strs[c]) {
                for (size_t r = 0; r < s->n_rows; ++r) free(s->u8_strs[c][r]);
                free(s->u8_strs[c]);
            }
        }
        free(s->u8_strs);
    }
    if (s->u8_lens) {
        for (size_t c = 0; c < s->n_input_cols; ++c) free(s->u8_lens[c]);
        free(s->u8_lens);
    }
    free(s);
}

static int sort_resolve(SortState *s) {
    if (s->schema_cached) return 0;
    if (!s->have_input || !s->input.get_schema) {
        sset_err(s, "sort: input has no get_schema"); return -1;
    }
    struct ArrowSchema sch = {0};
    if (s->input.get_schema(&s->input, &sch) != 0) {
        sset_err(s, "sort: upstream get_schema failed"); return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        sset_err(s, "sort: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) { free(names); free(fmts);
        sset_err(s, "sort: out of memory"); goto done;
    }
    int ok = 1;
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        int is_fixed = fmt && betl_tx_fixed_width_for_fmt(fmt[0]) != 0
                       && (fmt[0] == 'c' || fmt[0] == 'C' ||
                           fmt[0] == 's' || fmt[0] == 'S' ||
                           fmt[0] == 'i' || fmt[0] == 'I' ||
                           fmt[0] == 'l' || fmt[0] == 'L' ||
                           fmt[0] == 'f' || fmt[0] == 'g');
        int is_utf8  = fmt && strcmp(fmt, "u") == 0;
        if (!is_fixed && !is_utf8) {
            sset_err(s, "sort: input column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            ok = 0; break;
        }
        fmts[i]  = fmt[0];
        names[i] = strdup((c && c->name) ? c->name : "");
        if (!names[i]) { sset_err(s, "sort: out of memory"); ok = 0; break; }
    }
    if (!ok) {
        for (size_t i = 0; i < n; ++i) free(names[i]);
        free(names); free(fmts);
        goto done;
    }
    s->n_input_cols = n;
    s->col_names    = names;
    s->col_fmts     = fmts;

    s->i64_vals = calloc(n, sizeof *s->i64_vals);
    s->u8_strs  = calloc(n, sizeof *s->u8_strs);
    s->u8_lens  = calloc(n, sizeof *s->u8_lens);
    if (!s->i64_vals || !s->u8_strs || !s->u8_lens) {
        sset_err(s, "sort: out of memory"); goto done;
    }

    for (size_t i = 0; i < s->n_keys; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], s->keys[i].col_name) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            sset_err(s, "sort: by column '%s' not found in input",
                     s->keys[i].col_name);
            goto done;
        }
        s->keys[i].col_idx = idx;
        s->keys[i].fmt     = fmts[idx];
    }

    s->schema_cached = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

static int sort_grow_rows(SortState *s) {
    size_t nc = s->row_cap ? s->row_cap * 2 : 64;
    for (size_t c = 0; c < s->n_input_cols; ++c) {
        if (s->col_fmts[c] == 'u') {
            char **sp = realloc(s->u8_strs[c], nc * sizeof *sp);
            if (!sp) return -1;
            s->u8_strs[c] = sp;
            size_t *lp = realloc(s->u8_lens[c], nc * sizeof *lp);
            if (!lp) return -1;
            s->u8_lens[c] = lp;
        } else {
            /* All fixed-width types stash through the int64 slot: ints widen
             * (signed extension for c/s/i/l, zero for C/S/I/L), floats are
             * stored bit-for-bit via memcpy and reinterpreted at compare /
             * emit time. */
            int64_t *p = realloc(s->i64_vals[c], nc * sizeof *p);
            if (!p) return -1;
            s->i64_vals[c] = p;
        }
    }
    s->row_cap = nc;
    return 0;
}

static int sort_stash_cell(SortState *s, size_t c,
                           const struct ArrowArray *src,
                           size_t row_idx, size_t r) {
    size_t row = row_idx + (size_t)src->offset;
    char fmt = s->col_fmts[c];
    switch (fmt) {
        case 'l':
            s->i64_vals[c][r] = ((const int64_t *)src->buffers[1])[row]; return 0;
        case 'L':
            s->i64_vals[c][r] = (int64_t)((const uint64_t *)src->buffers[1])[row]; return 0;
        case 'c':
            s->i64_vals[c][r] = ((const int8_t   *)src->buffers[1])[row]; return 0;
        case 'C':
            s->i64_vals[c][r] = ((const uint8_t  *)src->buffers[1])[row]; return 0;
        case 's':
            s->i64_vals[c][r] = ((const int16_t  *)src->buffers[1])[row]; return 0;
        case 'S':
            s->i64_vals[c][r] = ((const uint16_t *)src->buffers[1])[row]; return 0;
        case 'i':
            s->i64_vals[c][r] = ((const int32_t  *)src->buffers[1])[row]; return 0;
        case 'I':
            s->i64_vals[c][r] = (int64_t)((const uint32_t *)src->buffers[1])[row]; return 0;
        case 'g': {
            double d = ((const double *)src->buffers[1])[row];
            memcpy(&s->i64_vals[c][r], &d, sizeof d);
            return 0;
        }
        case 'f': {
            float f = ((const float *)src->buffers[1])[row];
            double d = (double)f;
            memcpy(&s->i64_vals[c][r], &d, sizeof d);
            return 0;
        }
        default: break;
    }
    const int32_t *off = src->buffers[1];
    const char    *dat = src->buffers[2];
    size_t len = (size_t)(off[row + 1] - off[row]);
    char *dup = malloc(len + 1);
    if (!dup) return -1;
    if (len) memcpy(dup, dat + off[row], len);
    dup[len] = '\0';
    s->u8_strs[c][r] = dup;
    s->u8_lens[c][r] = len;
    return 0;
}

static int sort_materialize(SortState *s) {
    if (s->materialized) return 0;
    if (sort_resolve(s) != 0) return -1;
    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            sset_err(s, "sort: cancelled"); return -1;
        }
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            const char *e = s->input.get_last_error
                                ? s->input.get_last_error(&s->input) : NULL;
            sset_err(s, "sort: upstream get_next failed: %s",
                     e ? e : "(no detail)");
            return -1;
        }
        if (!batch.release) break;
        if (batch.n_children != (int64_t)s->n_input_cols) {
            int64_t got = batch.n_children;
            size_t expected = s->n_input_cols;
            batch.release(&batch);
            sset_err(s, "sort: batch has %lld cols, expected %zu",
                     (long long)got, expected);
            return -1;
        }
        size_t length = (size_t)batch.length;
        for (size_t r = 0; r < length; ++r) {
            if (s->n_rows == s->row_cap && sort_grow_rows(s) != 0) {
                batch.release(&batch);
                sset_err(s, "sort: out of memory"); return -1;
            }
            for (size_t c = 0; c < s->n_input_cols; ++c) {
                if (sort_stash_cell(s, c, batch.children[c], r, s->n_rows) != 0) {
                    batch.release(&batch);
                    sset_err(s, "sort: out of memory"); return -1;
                }
            }
            ++s->n_rows;
        }
        batch.release(&batch);
    }
    s->materialized = 1;
    return 0;
}

/* qsort comparator. Uses a thread-local pointer to the active state;
 * v0.1 betl runs single-threaded so this is safe. */
static _Thread_local SortState *cmp_state;

static int sort_cmp_rows(const void *a, const void *b) {
    SortState *s = cmp_state;
    size_t ra = *(const size_t *)a;
    size_t rb = *(const size_t *)b;
    for (size_t k = 0; k < s->n_keys; ++k) {
        SortKey *key = &s->keys[k];
        int dir = key->asc ? 1 : -1;
        char fmt = key->fmt;
        if (fmt == 'f' || fmt == 'g') {
            double va, vb;
            memcpy(&va, &s->i64_vals[key->col_idx][ra], sizeof va);
            memcpy(&vb, &s->i64_vals[key->col_idx][rb], sizeof vb);
            if (va < vb) return -1 * dir;
            if (va > vb) return  1 * dir;
        } else if (fmt == 'L') {
            uint64_t va = (uint64_t)s->i64_vals[key->col_idx][ra];
            uint64_t vb = (uint64_t)s->i64_vals[key->col_idx][rb];
            if (va < vb) return -1 * dir;
            if (va > vb) return  1 * dir;
        } else if (fmt == 'u') {
            const char *sa = s->u8_strs[key->col_idx][ra];
            const char *sb = s->u8_strs[key->col_idx][rb];
            size_t la = s->u8_lens[key->col_idx][ra];
            size_t lb = s->u8_lens[key->col_idx][rb];
            size_t mn = la < lb ? la : lb;
            int c = memcmp(sa, sb, mn);
            if (c != 0) return c * dir;
            if (la != lb) return (la < lb ? -1 : 1) * dir;
        } else {
            int64_t va = s->i64_vals[key->col_idx][ra];
            int64_t vb = s->i64_vals[key->col_idx][rb];
            if (va < vb) return -1 * dir;
            if (va > vb) return  1 * dir;
        }
    }
    if (ra < rb) return -1;
    if (ra > rb) return  1;
    return 0;
}

/* Emit a fixed-width leaf in the original format. The int64 slot already
 * holds the value (widened for narrow ints, memcpy'd bits for floats);
 * we just narrow it back. */
static int sort_build_fixed(struct ArrowArray *out, char fmt,
                            const int64_t *src, const size_t *order, size_t n) {
    size_t w = betl_tx_fixed_width_for_fmt(fmt);
    if (w == 0) return -1;
    uint8_t *vals = malloc((n ? n : 1) * w);
    if (!vals) return -1;
    for (size_t i = 0; i < n; ++i) {
        int64_t v = src[order[i]];
        switch (fmt) {
            case 'l': case 'L': {
                memcpy(vals + i * 8, &v, 8); break;
            }
            case 'c': case 'C': {
                uint8_t b = (uint8_t)v;
                vals[i] = b; break;
            }
            case 's': case 'S': {
                uint16_t b = (uint16_t)v;
                memcpy(vals + i * 2, &b, 2); break;
            }
            case 'i': case 'I': {
                uint32_t b = (uint32_t)v;
                memcpy(vals + i * 4, &b, 4); break;
            }
            case 'g': {
                memcpy(vals + i * 8, &v, 8); break;
            }
            case 'f': {
                double d;
                memcpy(&d, &v, sizeof d);
                float  f = (float)d;
                memcpy(vals + i * 4, &f, 4); break;
            }
            default: free(vals); return -1;
        }
    }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); return -1; }
    bufs[0] = NULL; bufs[1] = vals;
    out->length = (int64_t)n;
    out->null_count = 0;
    out->n_buffers = 2;
    out->buffers = bufs;
    out->release = betl_tx_release_int64_leaf;  /* free bufs[0,1] same shape */
    return 0;
}

static int sort_build_utf8(struct ArrowArray *out,
                           char *const *strs, const size_t *lens,
                           const size_t *order, size_t n) {
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0;
    offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        total += lens[order[i]];
        if (total > (size_t)INT32_MAX) { free(offs); return -1; }
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t k = order[i];
        if (lens[k]) memcpy(data + pos, strs[k], lens[k]);
        pos += lens[k];
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); return -1; }
    bufs[0] = NULL; bufs[1] = offs; bufs[2] = data;
    out->length = (int64_t)n;
    out->null_count = 0;
    out->n_buffers = 3;
    out->buffers = bufs;
    out->release = betl_tx_release_utf8_leaf;
    return 0;
}

static int sort_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    SortState *s = st->private_data;
    if (!s) return EINVAL;
    if (sort_resolve(s) != 0) return EIO;
    /* Sort doesn't change the schema — forward upstream's. */
    return s->input.get_schema(&s->input, out);
}

static int sort_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    SortState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (sort_materialize(s) != 0) return EIO;
    if (s->emitted) return 0;

    size_t n = s->n_rows;
    size_t *order = malloc((n ? n : 1) * sizeof *order);
    if (!order) { sset_err(s, "sort: out of memory"); return EIO; }
    for (size_t i = 0; i < n; ++i) order[i] = i;
    cmp_state = s;
    qsort(order, n, sizeof *order, sort_cmp_rows);
    cmp_state = NULL;

    size_t n_total = s->n_input_cols;
    struct ArrowArray **kids = calloc(n_total ? n_total : 1, sizeof *kids);
    if (!kids) { free(order); sset_err(s, "sort: out of memory"); return EIO; }
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        int rc;
        if (s->col_fmts[c] == 'u') {
            rc = sort_build_utf8(kids[c], s->u8_strs[c], s->u8_lens[c], order, n);
        } else {
            rc = sort_build_fixed(kids[c], s->col_fmts[c],
                                  s->i64_vals[c], order, n);
        }
        if (!kids[c] || rc != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); free(order);
            sset_err(s, "sort: failed to build output column");
            return EIO;
        }
    }
    free(order);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        sset_err(s, "sort: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;

    s->emitted = 1;
    return 0;
}

static const char *sort_get_last_error(struct ArrowArrayStream *st) {
    SortState *s = st->private_data;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void sort_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int sort_attach_output(void *state, int port,
                              struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = sort_get_schema;
    out->get_next       = sort_get_next;
    out->get_last_error = sort_get_last_error;
    out->release        = sort_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef sort_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to sort" },
};
static const BetlPortDef sort_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "sorted rows" },
};

static const BetlComponentDef sort_components[] = {
    { .name               = "sort",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = sort_inputs,
      .input_count        = 1,
      .outputs            = sort_outputs,
      .output_count       = 1,
      .init               = sort_init,
      .destroy            = sort_destroy,
      .attach_input       = sort_attach_input,
      .attach_output      = sort_attach_output },
};

static const BetlProvider sort_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-sort",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = sort_components,
    .component_count = sizeof sort_components / sizeof sort_components[0],
};

int betl_tx_register_sort(BetlRegistry *r) {
    return betl_registry_register(r, &sort_provider, "<builtin:sort>");
}
