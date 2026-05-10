/* `distinct` TRANSFORM — drops duplicate rows.
 *
 * Config:
 *   keys: [col1, col2, ...]   optional; defaults to all input columns
 *
 * Strategy: streaming with a growing "seen" tuple set. For each input
 * batch, compare each row's key tuple against every previously-seen
 * tuple; if not seen, add to seen[] and mark the row kept. Output is
 * the input filtered down to first-occurrence rows. Order-preserving
 * (first occurrence wins). int64 + utf8 keys; nulls match nulls per
 * SQL DISTINCT semantics. */

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
    int   col_idx;     /* index into the input batch's children[] */
    char  fmt;         /* 'l' or 'u' */
    char *name;        /* heap; for error messages */
} DistKey;

/* Per-key seen-value buckets. Layout is parallel arrays indexed by
 * [key_idx][seen_idx]. For int64 keys: i64[k][i] is the value, used
 * when null_mask[k][i] == 0. For utf8 keys: u8s[k][i] is a heap copy
 * of the bytes, u8l[k][i] the length. null_mask[k][i] == 1 means the
 * tuple's k-th cell was NULL. */
typedef struct {
    int64_t  **i64;
    char    ***u8s;
    size_t   **u8l;
    uint8_t  **null_mask;
    size_t     n;
    size_t     cap;
} SeenSet;

typedef struct {
    BetlContext *ctx;

    /* Configured names; key_names == NULL means "all columns". */
    char     **key_names;
    size_t     n_key_names;

    struct ArrowArrayStream input;
    int                     have_input;

    /* Resolved on first batch. */
    int       schema_resolved;
    size_t    n_cols;        /* total input cols */
    char    **col_names;     /* heap copies of every input col name */
    char     *col_fmts;      /* per-col 'l' / 'u' */
    DistKey  *keys;
    size_t    n_keys;

    SeenSet   seen;

    char      last_err[256];
} DistinctState;

static void dset_err(DistinctState *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->last_err, sizeof d->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(d->ctx, "%s", d->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { char ***arr; size_t *n; int err; } StrlistCtx;

static int strlist_visit(const char *value, size_t value_len, void *user) {
    StrlistCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        c->err = 1; return -1;
    }
    char *s = NULL;
    if (betl_tx_json_decode_str(value, &s) != 0 || !s) {
        c->err = 1; return -1;
    }
    char **grow = realloc(*c->arr, (*c->n + 1) * sizeof *grow);
    if (!grow) { free(s); c->err = 1; return -1; }
    *c->arr = grow;
    (*c->arr)[(*c->n)++] = s;
    return 0;
}

static int distinct_init(BetlContext *ctx, const char *cfg, void **state) {
    DistinctState *d = calloc(1, sizeof *d);
    if (!d) return BETL_ERR_INTERNAL;
    d->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    /* Optional `keys: [name, ...]`. */
    const char *kp = betl_tx_json_value_after(cfg, "keys");
    if (kp && *kp == '[') {
        StrlistCtx sc = { .arr = &d->key_names, .n = &d->n_key_names, .err = 0 };
        if (betl_tx_json_walk_array(kp, strlist_visit, &sc) != 0 || sc.err) {
            dset_err(d, "distinct: `keys:` must be a list of column names");
            for (size_t i = 0; i < d->n_key_names; ++i) free(d->key_names[i]);
            free(d->key_names);
            free(d);
            return BETL_ERR_INVALID;
        }
        if (d->n_key_names == 0) {
            dset_err(d, "distinct: `keys:` is empty (omit it to dedupe on all columns)");
            free(d->key_names);
            free(d);
            return BETL_ERR_INVALID;
        }
    }

    *state = d;
    return BETL_OK;
}

static int distinct_attach_input(void *state, int port,
                                 struct ArrowArrayStream *in) {
    (void)port;
    DistinctState *d = state;
    d->input = *in;
    d->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void seen_free(SeenSet *s, size_t n_keys) {
    if (s->u8s) {
        for (size_t k = 0; k < n_keys; ++k) {
            if (s->u8s[k]) {
                for (size_t i = 0; i < s->n; ++i) free(s->u8s[k][i]);
                free(s->u8s[k]);
            }
        }
        free(s->u8s);
    }
    if (s->u8l) {
        for (size_t k = 0; k < n_keys; ++k) free(s->u8l[k]);
        free(s->u8l);
    }
    if (s->i64) {
        for (size_t k = 0; k < n_keys; ++k) free(s->i64[k]);
        free(s->i64);
    }
    if (s->null_mask) {
        for (size_t k = 0; k < n_keys; ++k) free(s->null_mask[k]);
        free(s->null_mask);
    }
    memset(s, 0, sizeof *s);
}

static void distinct_destroy(void *state) {
    if (!state) return;
    DistinctState *d = state;
    if (d->have_input && d->input.release) d->input.release(&d->input);
    for (size_t i = 0; i < d->n_key_names; ++i) free(d->key_names[i]);
    free(d->key_names);
    if (d->col_names) {
        for (size_t i = 0; i < d->n_cols; ++i) free(d->col_names[i]);
        free(d->col_names);
    }
    free(d->col_fmts);
    if (d->keys) {
        for (size_t i = 0; i < d->n_keys; ++i) free(d->keys[i].name);
        free(d->keys);
    }
    seen_free(&d->seen, d->n_keys);
    free(d);
}

/* ============================================================== *
 *  Schema resolution + seen-set growth                             *
 * ============================================================== */

static int distinct_resolve_schema(DistinctState *d) {
    if (d->schema_resolved) return 0;
    if (!d->have_input || !d->input.get_schema) {
        dset_err(d, "distinct: input has no get_schema");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (d->input.get_schema(&d->input, &sch) != 0) {
        dset_err(d, "distinct: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        dset_err(d, "distinct: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    d->col_names = calloc(n, sizeof *d->col_names);
    d->col_fmts  = calloc(n, 1);
    if (!d->col_names || !d->col_fmts) {
        dset_err(d, "distinct: out of memory");
        goto done;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            dset_err(d, "distinct: input column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            goto done;
        }
        d->col_fmts[i]  = fmt[0];
        d->col_names[i] = strdup((c && c->name) ? c->name : "");
        if (!d->col_names[i]) { dset_err(d, "distinct: out of memory"); goto done; }
    }
    d->n_cols = n;

    /* Resolve the key list. If no explicit keys, dedupe on all columns. */
    size_t want_n_keys = d->n_key_names ? d->n_key_names : n;
    d->keys = calloc(want_n_keys, sizeof *d->keys);
    if (!d->keys) { dset_err(d, "distinct: out of memory"); goto done; }
    for (size_t i = 0; i < want_n_keys; ++i) {
        const char *want = d->n_key_names ? d->key_names[i] : d->col_names[i];
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(d->col_names[k], want) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            dset_err(d, "distinct: key column '%s' not found in input", want);
            goto done;
        }
        d->keys[i].col_idx = idx;
        d->keys[i].fmt     = d->col_fmts[idx];
        d->keys[i].name    = strdup(d->col_names[idx]);
        if (!d->keys[i].name) { dset_err(d, "distinct: out of memory"); goto done; }
    }
    d->n_keys = want_n_keys;

    /* Allocate seen-set per-key arrays (rows grow as we see them). */
    d->seen.i64       = calloc(d->n_keys, sizeof *d->seen.i64);
    d->seen.u8s       = calloc(d->n_keys, sizeof *d->seen.u8s);
    d->seen.u8l       = calloc(d->n_keys, sizeof *d->seen.u8l);
    d->seen.null_mask = calloc(d->n_keys, sizeof *d->seen.null_mask);
    if (!d->seen.i64 || !d->seen.u8s || !d->seen.u8l || !d->seen.null_mask) {
        dset_err(d, "distinct: out of memory"); goto done;
    }

    d->schema_resolved = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

static int seen_grow(SeenSet *s, size_t n_keys) {
    size_t nc = s->cap ? s->cap * 2 : 32;
    for (size_t k = 0; k < n_keys; ++k) {
        int64_t *p = realloc(s->i64[k], nc * sizeof *p);
        if (!p) return -1;
        s->i64[k] = p;
        char **sp = realloc(s->u8s[k], nc * sizeof *sp);
        if (!sp) return -1;
        s->u8s[k] = sp;
        size_t *lp = realloc(s->u8l[k], nc * sizeof *lp);
        if (!lp) return -1;
        s->u8l[k] = lp;
        uint8_t *np = realloc(s->null_mask[k], nc);
        if (!np) return -1;
        s->null_mask[k] = np;
    }
    s->cap = nc;
    return 0;
}

/* Read key cell from input batch's column at row r. Sets *is_null,
 * fills *i64v for int64 or *u8p / *u8len for utf8 (pointing into
 * batch data; caller must copy if persisting). */
static void read_key_cell(const struct ArrowArray *col, char fmt, size_t r,
                          int *is_null, int64_t *i64v,
                          const char **u8p, size_t *u8len) {
    size_t row = r + (size_t)col->offset;
    *is_null = 0;
    /* Any leaf array we accept here has at least 2 buffers (validity +
     * data); utf8 has 3. The analyzer can't see that invariant on
     * arbitrary input, so guard `buffers` even though a non-NULL
     * release callback (already checked upstream) implies it. */
    if (!col->buffers) { *is_null = 1; return; }
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *valid = col->buffers[0];
        if (!betl_tx_bit_at(valid, row)) { *is_null = 1; return; }
    }
    if (fmt == 'l') {
        *i64v = ((const int64_t *)col->buffers[1])[row];
    } else {
        const int32_t *off = col->buffers[1];
        const char    *dat = col->buffers[2];
        *u8p   = dat + off[row];
        *u8len = (size_t)(off[row + 1] - off[row]);
    }
}

/* Linear scan: is the key tuple at `row` already in seen[]? */
static int seen_contains(const DistinctState *d, const struct ArrowArray *batch,
                         size_t row) {
    for (size_t i = 0; i < d->seen.n; ++i) {
        int all_eq = 1;
        for (size_t k = 0; k < d->n_keys; ++k) {
            const struct ArrowArray *col = batch->children[d->keys[k].col_idx];
            int  is_null = 0;
            int64_t v_i64 = 0;
            const char *v_u8 = NULL; size_t v_u8l = 0;
            read_key_cell(col, d->keys[k].fmt, row,
                          &is_null, &v_i64, &v_u8, &v_u8l);
            uint8_t seen_null = d->seen.null_mask[k][i];
            if (is_null != (int)seen_null) { all_eq = 0; break; }
            if (is_null) continue;       /* both null → equal */
            if (d->keys[k].fmt == 'l') {
                if (d->seen.i64[k][i] != v_i64) { all_eq = 0; break; }
            } else {
                if (d->seen.u8l[k][i] != v_u8l) { all_eq = 0; break; }
                if (v_u8l && memcmp(d->seen.u8s[k][i], v_u8, v_u8l) != 0) {
                    all_eq = 0; break;
                }
            }
        }
        if (all_eq) return 1;
    }
    return 0;
}

/* Append the key tuple at `row` to seen[]. */
static int seen_add(DistinctState *d, const struct ArrowArray *batch,
                    size_t row) {
    if (d->seen.n == d->seen.cap && seen_grow(&d->seen, d->n_keys) != 0) {
        dset_err(d, "distinct: out of memory");
        return -1;
    }
    size_t i = d->seen.n;
    for (size_t k = 0; k < d->n_keys; ++k) {
        const struct ArrowArray *col = batch->children[d->keys[k].col_idx];
        int  is_null = 0;
        int64_t v_i64 = 0;
        const char *v_u8 = NULL; size_t v_u8l = 0;
        read_key_cell(col, d->keys[k].fmt, row,
                      &is_null, &v_i64, &v_u8, &v_u8l);
        d->seen.null_mask[k][i] = (uint8_t)is_null;
        /* Initialize every slot regardless of key type — the cleanup
         * loop in seen_free walks all four parallel arrays for every
         * key index up to n, and would read uninitialized pointers if
         * we left utf8 slots untouched on int64 keys (or vice versa). */
        d->seen.i64[k][i] = 0;
        d->seen.u8s[k][i] = NULL;
        d->seen.u8l[k][i] = 0;
        if (is_null) continue;
        if (d->keys[k].fmt == 'l') {
            d->seen.i64[k][i] = v_i64;
        } else {
            char *dup = malloc(v_u8l + 1);
            if (!dup) { dset_err(d, "distinct: out of memory"); return -1; }
            if (v_u8l) memcpy(dup, v_u8, v_u8l);
            dup[v_u8l] = '\0';
            d->seen.u8s[k][i] = dup;
            d->seen.u8l[k][i] = v_u8l;
        }
    }
    ++d->seen.n;
    return 0;
}

/* ============================================================== *
 *  Stream — get_schema + get_next                                  *
 * ============================================================== */

static int dist_get_schema(struct ArrowArrayStream *st,
                           struct ArrowSchema *out) {
    DistinctState *d = st->private_data;
    if (!d || !d->have_input) return EINVAL;
    return d->input.get_schema(&d->input, out) == 0 ? 0 : EIO;
}

static int dist_get_next(struct ArrowArrayStream *st,
                         struct ArrowArray *out) {
    DistinctState *d = st->private_data;
    memset(out, 0, sizeof *out);
    if (!d) return EINVAL;
    if (distinct_resolve_schema(d) != 0) return EIO;

    /* Pull batches until we get one with at least one new (kept) row,
     * or the upstream is exhausted. */
    for (;;) {
        struct ArrowArray batch = {0};
        if (d->input.get_next(&d->input, &batch) != 0) {
            const char *e = d->input.get_last_error
                ? d->input.get_last_error(&d->input) : NULL;
            dset_err(d, "distinct: upstream get_next failed: %s",
                     e ? e : "(no detail)");
            return EIO;
        }
        if (!batch.release) return 0;        /* clean EOF */
        size_t length = (size_t)batch.length;
        if (length == 0) { batch.release(&batch); continue; }

        uint8_t *keep = calloc(length, 1);
        if (!keep) {
            batch.release(&batch);
            dset_err(d, "distinct: out of memory");
            return EIO;
        }
        size_t n_kept = 0;
        for (size_t r = 0; r < length; ++r) {
            if (seen_contains(d, &batch, r)) continue;
            if (seen_add(d, &batch, r) != 0) {
                free(keep); batch.release(&batch); return EIO;
            }
            keep[r] = 1;
            ++n_kept;
        }
        if (n_kept == 0) {
            free(keep);
            batch.release(&batch);
            continue;       /* every row was a duplicate — pull next batch */
        }

        struct ArrowArray **kids = calloc(d->n_cols, sizeof *kids);
        if (!kids) {
            free(keep); batch.release(&batch);
            dset_err(d, "distinct: out of memory");
            return EIO;
        }
        int build_failed = 0;
        for (size_t c = 0; c < d->n_cols; ++c) {
            kids[c] = calloc(1, sizeof **kids);
            if (!kids[c]) { build_failed = 1; break; }
            int crc = (d->col_fmts[c] == 'l')
                ? betl_tx_build_int64_filtered(kids[c], batch.children[c],
                                               keep, length, n_kept)
                : betl_tx_build_utf8_filtered (kids[c], batch.children[c],
                                               keep, length, n_kept);
            if (crc != 0) { build_failed = 1; break; }
        }
        if (build_failed) {
            for (size_t c = 0; c < d->n_cols; ++c) {
                if (kids[c]) {
                    if (kids[c]->release) kids[c]->release(kids[c]);
                    free(kids[c]);
                }
            }
            free(kids); free(keep); batch.release(&batch);
            dset_err(d, "distinct: failed to build output column");
            return EIO;
        }
        const void **outer = malloc(1 * sizeof *outer);
        if (!outer) {
            for (size_t c = 0; c < d->n_cols; ++c) {
                if (kids[c]->release) kids[c]->release(kids[c]);
                free(kids[c]);
            }
            free(kids); free(keep); batch.release(&batch);
            dset_err(d, "distinct: out of memory");
            return EIO;
        }
        outer[0] = NULL;
        out->length     = (int64_t)n_kept;
        out->null_count = 0;
        out->n_buffers  = 1;
        out->n_children = (int64_t)d->n_cols;
        out->buffers    = outer;
        out->children   = kids;
        out->release    = betl_tx_release_struct;
        free(keep);
        batch.release(&batch);
        return 0;
    }
}

static const char *dist_get_last_error(struct ArrowArrayStream *st) {
    DistinctState *d = st->private_data;
    return (d && d->last_err[0]) ? d->last_err : NULL;
}

static void dist_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release = NULL;
}

static int distinct_attach_output(void *state, int port,
                                  struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = dist_get_schema;
    out->get_next       = dist_get_next;
    out->get_last_error = dist_get_last_error;
    out->release        = dist_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef distinct_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to dedupe" },
};
static const BetlPortDef distinct_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "first-occurrence rows" },
};

static const BetlComponentDef distinct_components[] = {
    { .name               = "distinct",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = distinct_inputs,
      .input_count        = 1,
      .outputs            = distinct_outputs,
      .output_count       = 1,
      .init               = distinct_init,
      .destroy            = distinct_destroy,
      .attach_input       = distinct_attach_input,
      .attach_output      = distinct_attach_output },
};

static const BetlProvider distinct_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-distinct",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = distinct_components,
    .component_count = sizeof distinct_components / sizeof distinct_components[0],
};

int betl_tx_register_distinct(BetlRegistry *r) {
    return betl_registry_register(r, &distinct_provider, "<builtin:distinct>");
}
