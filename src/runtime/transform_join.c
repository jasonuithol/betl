/* `join` TRANSFORM (SPEC §4.3) — two-stream inner join.
 *
 * v0.1: inner only, multi-key match via `on: { left_col: right_col }`,
 * int64 + utf8 columns, both sides materialized into per-column tables
 * (JTab) and probed with a nested loop. Output schema = left columns
 * followed by right columns; collisions on names are not renamed (use
 * `map: select:` upstream to disambiguate).
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


/* Per-side materialized table. */
typedef struct {
    size_t   n_cols;
    char   **names;
    char    *fmts;
    int64_t **i64;
    char  ***u8s;
    size_t **u8l;
    size_t   n_rows;
    size_t   row_cap;
} JTab;

static void jtab_free(JTab *t) {
    if (t->names) {
        for (size_t i = 0; i < t->n_cols; ++i) free(t->names[i]);
        free(t->names);
    }
    free(t->fmts);
    if (t->i64) {
        for (size_t c = 0; c < t->n_cols; ++c) free(t->i64[c]);
        free(t->i64);
    }
    if (t->u8s) {
        for (size_t c = 0; c < t->n_cols; ++c) {
            if (t->u8s[c]) {
                for (size_t r = 0; r < t->n_rows; ++r) free(t->u8s[c][r]);
                free(t->u8s[c]);
            }
        }
        free(t->u8s);
    }
    if (t->u8l) {
        for (size_t c = 0; c < t->n_cols; ++c) free(t->u8l[c]);
        free(t->u8l);
    }
    memset(t, 0, sizeof *t);
}

static int jtab_set_schema(JTab *t, const struct ArrowSchema *sch,
                           char *err, size_t err_cap) {
    if (!sch->format || strcmp(sch->format, "+s") != 0 || sch->n_children <= 0) {
        snprintf(err, err_cap, "input must be a struct array");
        return -1;
    }
    size_t n = (size_t)sch->n_children;
    t->names = calloc(n, sizeof *t->names);
    t->fmts  = calloc(n, 1);
    t->i64   = calloc(n, sizeof *t->i64);
    t->u8s   = calloc(n, sizeof *t->u8s);
    t->u8l   = calloc(n, sizeof *t->u8l);
    if (!t->names || !t->fmts || !t->i64 || !t->u8s || !t->u8l) {
        snprintf(err, err_cap, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch->children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            snprintf(err, err_cap, "column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            return -1;
        }
        t->fmts[i]  = fmt[0];
        t->names[i] = strdup((c && c->name) ? c->name : "");
        if (!t->names[i]) { snprintf(err, err_cap, "out of memory"); return -1; }
    }
    t->n_cols = n;
    return 0;
}

static int jtab_grow_rows(JTab *t) {
    size_t nc = t->row_cap ? t->row_cap * 2 : 64;
    for (size_t c = 0; c < t->n_cols; ++c) {
        if (t->fmts[c] == 'l') {
            int64_t *p = realloc(t->i64[c], nc * sizeof *p);
            if (!p) return -1;
            t->i64[c] = p;
        } else {
            char **sp = realloc(t->u8s[c], nc * sizeof *sp);
            if (!sp) return -1;
            t->u8s[c] = sp;
            size_t *lp = realloc(t->u8l[c], nc * sizeof *lp);
            if (!lp) return -1;
            t->u8l[c] = lp;
        }
    }
    t->row_cap = nc;
    return 0;
}

static int jtab_stash(JTab *t, size_t c,
                      const struct ArrowArray *src,
                      size_t row_idx, size_t r) {
    size_t row = row_idx + (size_t)src->offset;
    if (t->fmts[c] == 'l') {
        t->i64[c][r] = ((const int64_t *)src->buffers[1])[row];
        return 0;
    }
    const int32_t *off = src->buffers[1];
    const char    *dat = src->buffers[2];
    size_t len = (size_t)(off[row + 1] - off[row]);
    char *dup = malloc(len + 1);
    if (!dup) return -1;
    if (len) memcpy(dup, dat + off[row], len);
    dup[len] = '\0';
    t->u8s[c][r] = dup;
    t->u8l[c][r] = len;
    return 0;
}

static int jtab_pull(JTab *t, struct ArrowArrayStream *in,
                     char *err, size_t err_cap) {
    for (;;) {
        struct ArrowArray batch = {0};
        if (in->get_next(in, &batch) != 0) {
            const char *e = in->get_last_error ? in->get_last_error(in) : NULL;
            snprintf(err, err_cap, "get_next failed: %s", e ? e : "(no detail)");
            return -1;
        }
        if (!batch.release) return 0;
        if (batch.n_children != (int64_t)t->n_cols) {
            int64_t got = batch.n_children;
            size_t expected = t->n_cols;
            batch.release(&batch);
            snprintf(err, err_cap, "batch has %lld cols, expected %zu",
                     (long long)got, expected);
            return -1;
        }
        size_t length = (size_t)batch.length;
        for (size_t r = 0; r < length; ++r) {
            if (t->n_rows == t->row_cap && jtab_grow_rows(t) != 0) {
                batch.release(&batch);
                snprintf(err, err_cap, "out of memory");
                return -1;
            }
            for (size_t c = 0; c < t->n_cols; ++c) {
                if (jtab_stash(t, c, batch.children[c], r, t->n_rows) != 0) {
                    batch.release(&batch);
                    snprintf(err, err_cap, "out of memory");
                    return -1;
                }
            }
            ++t->n_rows;
        }
        batch.release(&batch);
    }
}

typedef struct {
    char *left_col;
    char *right_col;
    int   left_idx;
    int   right_idx;
    char  fmt;
} JoinKey;

typedef struct {
    BetlContext *ctx;

    JoinKey *keys;
    size_t   n_keys;

    char *kind;

    struct ArrowArrayStream left_in;
    int                     have_left;
    struct ArrowArrayStream right_in;
    int                     have_right;

    JTab L;
    JTab R;

    int materialized;
    int emitted;

    char last_err[256];
} JoinState;

static void jset_err(JoinState *j, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->last_err, sizeof j->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(j->ctx, "%s", j->last_err);
}

typedef struct { JoinState *j; int err; } JoinOnCtx;

static int join_on_visit(const char *key, const char *value, size_t value_len,
                         void *user) {
    JoinOnCtx *c = user;
    JoinState *j = c->j;
    if (value_len == 0 || value[0] != '"') {
        jset_err(j, "join: on['%s'] must be a string (right column name)", key);
        c->err = 1; return -1;
    }
    char *rcol = NULL;
    if (betl_tx_json_decode_str(value, &rcol) != 0 || !rcol) { c->err = 1; return -1; }

    JoinKey *grow = realloc(j->keys, (j->n_keys + 1) * sizeof *grow);
    if (!grow) { free(rcol); c->err = 1; return -1; }
    j->keys = grow;
    JoinKey *k = &j->keys[j->n_keys++];
    memset(k, 0, sizeof *k);
    k->left_col  = strdup(key);
    k->right_col = rcol;
    k->left_idx  = -1;
    k->right_idx = -1;
    if (!k->left_col) { c->err = 1; return -1; }
    return 0;
}

static int join_init(BetlContext *ctx, const char *cfg, void **state) {
    JoinState *j = calloc(1, sizeof *j);
    if (!j) return BETL_ERR_INTERNAL;
    j->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    char *kind = NULL;
    betl_tx_json_string_at(cfg, "kind", &kind);
    j->kind = kind ? kind : strdup("inner");
    if (!j->kind) { free(j); return BETL_ERR_INTERNAL; }
    if (strcmp(j->kind, "inner") != 0) {
        jset_err(j, "join: only 'inner' supported in v0.1 (got '%s')", j->kind);
        free(j->kind); free(j); return BETL_ERR_UNSUPPORTED;
    }

    const char *on = betl_tx_json_value_after(cfg, "on");
    if (!on || *on != '{') {
        jset_err(j, "join: requires `on:` map of {left_col: right_col}");
        free(j->kind); free(j); return BETL_ERR_INVALID;
    }
    JoinOnCtx oc = { .j = j, .err = 0 };
    if (betl_tx_json_walk_object(on, join_on_visit, &oc) != 0 || oc.err
        || j->n_keys == 0) {
        if (j->last_err[0] == '\0') jset_err(j, "join: `on:` is empty");
        for (size_t i = 0; i < j->n_keys; ++i) {
            free(j->keys[i].left_col); free(j->keys[i].right_col);
        }
        free(j->keys); free(j->kind); free(j);
        return BETL_ERR_INVALID;
    }
    *state = j;
    return BETL_OK;
}

static int join_attach_input(void *state, int port,
                             struct ArrowArrayStream *in) {
    JoinState *j = state;
    if (port == 0) {
        j->left_in   = *in;
        j->have_left = 1;
    } else if (port == 1) {
        j->right_in   = *in;
        j->have_right = 1;
    } else {
        return BETL_ERR_INVALID;
    }
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void join_destroy(void *state) {
    if (!state) return;
    JoinState *j = state;
    if (j->have_left  && j->left_in.release)  j->left_in.release(&j->left_in);
    if (j->have_right && j->right_in.release) j->right_in.release(&j->right_in);
    for (size_t i = 0; i < j->n_keys; ++i) {
        free(j->keys[i].left_col); free(j->keys[i].right_col);
    }
    free(j->keys);
    free(j->kind);
    jtab_free(&j->L);
    jtab_free(&j->R);
    free(j);
}

static int join_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    JoinState *j = st->private_data;
    memset(out, 0, sizeof *out);
    if (!j || !j->have_left || !j->have_right) return EINVAL;

    struct ArrowSchema sl = {0}, sr = {0};
    if (j->left_in.get_schema(&j->left_in, &sl) != 0) return EIO;
    if (j->right_in.get_schema(&j->right_in, &sr) != 0) {
        if (sl.release) sl.release(&sl);
        return EIO;
    }
    int64_t nl = sl.n_children, nr = sr.n_children;
    int64_t n_total = nl + nr;
    struct ArrowSchema **kids = calloc((size_t)n_total, sizeof *kids);
    if (!kids) {
        if (sl.release) sl.release(&sl);
        if (sr.release) sr.release(&sr);
        return ENOMEM;
    }
    for (int64_t i = 0; i < nl; ++i) { kids[i] = sl.children[i]; sl.children[i] = NULL; }
    for (int64_t i = 0; i < nr; ++i) { kids[nl + i] = sr.children[i]; sr.children[i] = NULL; }
    if (sl.release) sl.release(&sl);
    if (sr.release) sr.release(&sr);

    out->format     = "+s";
    out->n_children = n_total;
    out->children   = kids;
    out->release    = betl_tx_release_schema_struct_owned;
    return 0;
}

static int join_materialize(JoinState *j) {
    if (j->materialized) return 0;
    if (!j->have_left || !j->have_right) {
        jset_err(j, "join: both inputs (left and right) must be attached");
        return -1;
    }
    struct ArrowSchema sl = {0}, sr = {0};
    if (j->left_in.get_schema(&j->left_in, &sl) != 0) {
        jset_err(j, "join: left get_schema failed"); return -1;
    }
    char err[160];
    if (jtab_set_schema(&j->L, &sl, err, sizeof err) != 0) {
        if (sl.release) sl.release(&sl);
        jset_err(j, "join: left: %s", err); return -1;
    }
    if (sl.release) sl.release(&sl);
    if (j->right_in.get_schema(&j->right_in, &sr) != 0) {
        jset_err(j, "join: right get_schema failed"); return -1;
    }
    if (jtab_set_schema(&j->R, &sr, err, sizeof err) != 0) {
        if (sr.release) sr.release(&sr);
        jset_err(j, "join: right: %s", err); return -1;
    }
    if (sr.release) sr.release(&sr);

    for (size_t i = 0; i < j->n_keys; ++i) {
        JoinKey *k = &j->keys[i];
        for (size_t c = 0; c < j->L.n_cols; ++c) {
            if (strcmp(j->L.names[c], k->left_col) == 0) { k->left_idx = (int)c; break; }
        }
        for (size_t c = 0; c < j->R.n_cols; ++c) {
            if (strcmp(j->R.names[c], k->right_col) == 0) { k->right_idx = (int)c; break; }
        }
        if (k->left_idx < 0) {
            jset_err(j, "join: left column '%s' not found", k->left_col);
            return -1;
        }
        if (k->right_idx < 0) {
            jset_err(j, "join: right column '%s' not found", k->right_col);
            return -1;
        }
        if (j->L.fmts[k->left_idx] != j->R.fmts[k->right_idx]) {
            jset_err(j, "join: key '%s'/'%s' types differ ('%c' vs '%c')",
                     k->left_col, k->right_col,
                     j->L.fmts[k->left_idx], j->R.fmts[k->right_idx]);
            return -1;
        }
        k->fmt = j->L.fmts[k->left_idx];
    }

    if (jtab_pull(&j->L, &j->left_in,  err, sizeof err) != 0) {
        jset_err(j, "join: left: %s",  err); return -1;
    }
    if (jtab_pull(&j->R, &j->right_in, err, sizeof err) != 0) {
        jset_err(j, "join: right: %s", err); return -1;
    }
    j->materialized = 1;
    return 0;
}

static int row_keys_equal(const JoinState *j, size_t lr, size_t rr) {
    for (size_t k = 0; k < j->n_keys; ++k) {
        const JoinKey *key = &j->keys[k];
        if (key->fmt == 'l') {
            if (j->L.i64[key->left_idx][lr] != j->R.i64[key->right_idx][rr]) {
                return 0;
            }
        } else {
            size_t la = j->L.u8l[key->left_idx][lr];
            size_t lb = j->R.u8l[key->right_idx][rr];
            if (la != lb) return 0;
            if (la && memcmp(j->L.u8s[key->left_idx][lr],
                             j->R.u8s[key->right_idx][rr], la) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int join_build_int64_leaf(struct ArrowArray *out,
                                 JTab *side, int col_idx,
                                 const size_t *rows, size_t n) {
    int64_t *v = malloc((n ? n : 1) * sizeof *v);
    if (!v) return -1;
    for (size_t i = 0; i < n; ++i) v[i] = side->i64[col_idx][rows[i]];
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(v); return -1; }
    bufs[0] = NULL; bufs[1] = v;
    out->length = (int64_t)n;
    out->n_buffers = 2;
    out->buffers = bufs;
    out->release = betl_tx_release_int64_leaf;
    return 0;
}

static int join_build_utf8_leaf(struct ArrowArray *out,
                                JTab *side, int col_idx,
                                const size_t *rows, size_t n) {
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0;
    offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        total += side->u8l[col_idx][rows[i]];
        if (total > (size_t)INT32_MAX) { free(offs); return -1; }
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t len = side->u8l[col_idx][rows[i]];
        if (len) memcpy(data + pos, side->u8s[col_idx][rows[i]], len);
        pos += len;
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); return -1; }
    bufs[0] = NULL; bufs[1] = offs; bufs[2] = data;
    out->length = (int64_t)n;
    out->n_buffers = 3;
    out->buffers = bufs;
    out->release = betl_tx_release_utf8_leaf;
    return 0;
}

static int join_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    JoinState *j = st->private_data;
    memset(out, 0, sizeof *out);
    if (!j) return EINVAL;
    if (join_materialize(j) != 0) return EIO;
    if (j->emitted) return 0;

    /* Find all match pairs (left_row, right_row). Linear scan v0.1. */
    size_t cap = 16, n = 0;
    size_t *lr = malloc(cap * sizeof *lr);
    size_t *rr = malloc(cap * sizeof *rr);
    if (!lr || !rr) { free(lr); free(rr); jset_err(j, "join: out of memory"); return EIO; }

    for (size_t li = 0; li < j->L.n_rows; ++li) {
        if (betl_should_cancel(j->ctx)) {
            free(lr); free(rr);
            jset_err(j, "join: cancelled"); return EIO;
        }
        for (size_t ri = 0; ri < j->R.n_rows; ++ri) {
            if (!row_keys_equal(j, li, ri)) continue;
            if (n == cap) {
                cap *= 2;
                size_t *nlr = realloc(lr, cap * sizeof *lr);
                size_t *nrr = realloc(rr, cap * sizeof *rr);
                if (!nlr || !nrr) {
                    free(nlr ? nlr : lr); free(nrr ? nrr : rr);
                    jset_err(j, "join: out of memory"); return EIO;
                }
                lr = nlr; rr = nrr;
            }
            lr[n] = li; rr[n] = ri; ++n;
        }
    }

    size_t n_total = j->L.n_cols + j->R.n_cols;
    if (n_total == 0) {
        free(lr); free(rr);
        jset_err(j, "join: no output columns");
        return EIO;
    }
    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        free(lr); free(rr);
        jset_err(j, "join: out of memory"); return EIO;
    }
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        int rc;
        if (c < j->L.n_cols) {
            int idx = (int)c;
            rc = (j->L.fmts[idx] == 'l')
                 ? join_build_int64_leaf(kids[c], &j->L, idx, lr, n)
                 : join_build_utf8_leaf (kids[c], &j->L, idx, lr, n);
        } else {
            int idx = (int)(c - j->L.n_cols);
            rc = (j->R.fmts[idx] == 'l')
                 ? join_build_int64_leaf(kids[c], &j->R, idx, rr, n)
                 : join_build_utf8_leaf (kids[c], &j->R, idx, rr, n);
        }
        if (!kids[c] || rc != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); free(lr); free(rr);
            jset_err(j, "join: failed to build output column");
            return EIO;
        }
    }
    free(lr); free(rr);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        jset_err(j, "join: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;

    j->emitted = 1;
    return 0;
}

static const char *join_get_last_error(struct ArrowArrayStream *st) {
    JoinState *j = st->private_data;
    return (j && j->last_err[0]) ? j->last_err : NULL;
}

static void join_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int join_attach_output(void *state, int port,
                              struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = join_get_schema;
    out->get_next       = join_get_next;
    out->get_last_error = join_get_last_error;
    out->release        = join_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef join_inputs[]  = {
    { .name = "left",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "left rows" },
    { .name = "right", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "right rows" },
};
static const BetlPortDef join_outputs[] = {
    { .name = "out",   .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "joined rows" },
};

static const BetlComponentDef join_components[] = {
    { .name               = "join",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = join_inputs,
      .input_count        = 2,
      .outputs            = join_outputs,
      .output_count       = 1,
      .init               = join_init,
      .destroy            = join_destroy,
      .attach_input       = join_attach_input,
      .attach_output      = join_attach_output },
};

static const BetlProvider join_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-join",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = join_components,
    .component_count = sizeof join_components / sizeof join_components[0],
};

int betl_tx_register_join(BetlRegistry *r) {
    return betl_registry_register(r, &join_provider, "<builtin:join>");
}
