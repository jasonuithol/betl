/* `aggregate` TRANSFORM (SPEC §4.3).
 *
 * v0.1: int64 group_by columns + count/sum/min/max aggregations
 * producing int64 outputs. Materialize → emit-once: pull every upstream
 * batch first, fold each row into a linear-search group table, then on
 * the first downstream get_next emit one struct array with n_groups
 * rows.
 *
 * Nulls: a null in any group_by column is rejected as v0.1 limitation.
 * A null in an `over:` column silently skips that contribution.
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


typedef enum {
    AGG_COUNT = 1,
    AGG_SUM   = 2,
    AGG_MIN   = 3,
    AGG_MAX   = 4,
} AggKind;

typedef struct {
    char  *out_name;
    AggKind kind;
    char  *over_name;     /* NULL for count */
    int    over_idx;      /* resolved at first batch */
} ComputeSpec;

typedef struct {
    int64_t count;
    int64_t sum;
    int64_t mn;
    int64_t mx;
    int     seen;
} Accum;

typedef struct {
    int64_t *key_vals;
    Accum   *accs;
} Group;

typedef struct {
    BetlContext *ctx;

    char **group_by_names;
    size_t n_group_by;

    ComputeSpec *computes;
    size_t n_computes;

    struct ArrowArrayStream input;
    int                     have_input;

    int          schema_cached;
    size_t       n_input_cols;
    char       **input_col_names;
    char        *input_col_fmts;
    int         *group_by_idx;

    Group  *groups;
    size_t  n_groups;
    size_t  groups_cap;
    int     materialized;
    int     emitted;

    char    last_err[256];
} AggState;

static void aset_err(AggState *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->last_err, sizeof a->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(a->ctx, "%s", a->last_err);
}

typedef struct { AggState *a; int err; } AggCtx;

static int compute_visit(const char *key, const char *value, size_t value_len,
                         void *user) {
    AggCtx *c = user;
    AggState *a = c->a;
    if (value_len == 0 || value[0] != '{') {
        aset_err(a, "aggregate: compute['%s'] must be an object", key);
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *agg = NULL, *over = NULL;
    betl_tx_json_string_at(vbuf, "agg",  &agg);
    betl_tx_json_string_at(vbuf, "over", &over);
    free(vbuf);

    if (!agg) {
        free(over);
        aset_err(a, "aggregate: compute['%s'] missing 'agg'", key);
        c->err = 1; return -1;
    }
    AggKind kind;
    if      (strcmp(agg, "count") == 0) kind = AGG_COUNT;
    else if (strcmp(agg, "sum")   == 0) kind = AGG_SUM;
    else if (strcmp(agg, "min")   == 0) kind = AGG_MIN;
    else if (strcmp(agg, "max")   == 0) kind = AGG_MAX;
    else {
        aset_err(a, "aggregate: compute['%s'] unsupported agg '%s' "
                    "(v0.1: count, sum, min, max)", key, agg);
        free(agg); free(over); c->err = 1; return -1;
    }
    free(agg);
    if (kind != AGG_COUNT && !over) {
        aset_err(a, "aggregate: compute['%s'] requires 'over:' for non-count agg", key);
        c->err = 1; return -1;
    }
    if (kind == AGG_COUNT) { free(over); over = NULL; }

    ComputeSpec *grow = realloc(a->computes,
                                (a->n_computes + 1) * sizeof *grow);
    if (!grow) { free(over); c->err = 1; return -1; }
    a->computes = grow;
    ComputeSpec *cs = &a->computes[a->n_computes++];
    cs->out_name  = strdup(key);
    cs->kind      = kind;
    cs->over_name = over;
    cs->over_idx  = -1;
    if (!cs->out_name) { c->err = 1; return -1; }
    return 0;
}

typedef struct { AggState *a; int err; } GbCtx;

static int group_by_visit(const char *value, size_t value_len, void *user) {
    GbCtx *c = user;
    AggState *a = c->a;
    if (value_len == 0 || value[0] != '"') {
        aset_err(a, "aggregate: group_by entries must be column-name strings");
        c->err = 1; return -1;
    }
    char *s = NULL;
    if (betl_tx_json_decode_str(value, &s) != 0 || !s) { c->err = 1; return -1; }
    char **grow = realloc(a->group_by_names,
                          (a->n_group_by + 1) * sizeof *grow);
    if (!grow) { free(s); c->err = 1; return -1; }
    a->group_by_names = grow;
    a->group_by_names[a->n_group_by++] = s;
    return 0;
}

static int agg_init(BetlContext *ctx, const char *cfg, void **state) {
    AggState *a = calloc(1, sizeof *a);
    if (!a) return BETL_ERR_INTERNAL;
    a->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    const char *gb = betl_tx_json_value_after(cfg, "group_by");
    if (!gb || *gb != '[') {
        aset_err(a, "aggregate: `group_by` must be a list of column names");
        free(a); return BETL_ERR_INVALID;
    }
    GbCtx gbc = { .a = a, .err = 0 };
    if (betl_tx_json_walk_array(gb, group_by_visit, &gbc) != 0 || gbc.err
        || a->n_group_by == 0) {
        if (a->last_err[0] == '\0')
            aset_err(a, "aggregate: `group_by` is empty");
        for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
        free(a->group_by_names);
        free(a);
        return BETL_ERR_INVALID;
    }

    const char *cmp = betl_tx_json_value_after(cfg, "compute");
    if (!cmp || *cmp != '{') {
        aset_err(a, "aggregate: `compute` must be a map");
        for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
        free(a->group_by_names);
        free(a);
        return BETL_ERR_INVALID;
    }
    AggCtx cc = { .a = a, .err = 0 };
    if (betl_tx_json_walk_object(cmp, compute_visit, &cc) != 0 || cc.err
        || a->n_computes == 0) {
        if (a->last_err[0] == '\0')
            aset_err(a, "aggregate: `compute` is empty");
        for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
        free(a->group_by_names);
        for (size_t i = 0; i < a->n_computes; ++i) {
            free(a->computes[i].out_name);
            free(a->computes[i].over_name);
        }
        free(a->computes);
        free(a);
        return BETL_ERR_INVALID;
    }
    *state = a;
    return BETL_OK;
}

static int agg_attach_input(void *state, int port,
                            struct ArrowArrayStream *in) {
    (void)port;
    AggState *a = state;
    a->input      = *in;
    a->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void agg_destroy(void *state) {
    if (!state) return;
    AggState *a = state;
    if (a->have_input && a->input.release) a->input.release(&a->input);
    for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
    free(a->group_by_names);
    for (size_t i = 0; i < a->n_computes; ++i) {
        free(a->computes[i].out_name);
        free(a->computes[i].over_name);
    }
    free(a->computes);
    if (a->input_col_names) {
        for (size_t i = 0; i < a->n_input_cols; ++i) free(a->input_col_names[i]);
        free(a->input_col_names);
    }
    free(a->input_col_fmts);
    free(a->group_by_idx);
    for (size_t i = 0; i < a->n_groups; ++i) {
        free(a->groups[i].key_vals);
        free(a->groups[i].accs);
    }
    free(a->groups);
    free(a);
}

static int agg_resolve_schema(AggState *a) {
    if (a->schema_cached) return 0;
    if (!a->have_input || !a->input.get_schema) {
        aset_err(a, "aggregate: input has no get_schema");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (a->input.get_schema(&a->input, &sch) != 0) {
        aset_err(a, "aggregate: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        aset_err(a, "aggregate: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) {
        free(names); free(fmts);
        aset_err(a, "aggregate: out of memory");
        goto done;
    }
    int ok = 1;
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        names[i] = strdup((c && c->name) ? c->name : "");
        if (!names[i] || !fmt) {
            aset_err(a, "aggregate: input column %zu malformed", i);
            ok = 0; break;
        }
        fmts[i] = fmt[0];
    }
    if (!ok) {
        for (size_t i = 0; i < n; ++i) free(names[i]);
        free(names); free(fmts);
        goto done;
    }
    a->n_input_cols    = n;
    a->input_col_names = names;
    a->input_col_fmts  = fmts;

    a->group_by_idx = calloc(a->n_group_by, sizeof *a->group_by_idx);
    if (!a->group_by_idx) { aset_err(a, "aggregate: out of memory"); goto done; }
    for (size_t i = 0; i < a->n_group_by; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], a->group_by_names[i]) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            aset_err(a, "aggregate: group_by column '%s' not found",
                     a->group_by_names[i]);
            goto done;
        }
        if (fmts[idx] != 'l') {
            aset_err(a, "aggregate: group_by column '%s' must be int64 (got '%c')",
                     a->group_by_names[i], fmts[idx]);
            goto done;
        }
        a->group_by_idx[i] = idx;
    }
    for (size_t i = 0; i < a->n_computes; ++i) {
        ComputeSpec *cs = &a->computes[i];
        if (cs->kind == AGG_COUNT) { cs->over_idx = -1; continue; }
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], cs->over_name) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            aset_err(a, "aggregate: compute['%s'] over: '%s' not found",
                     cs->out_name, cs->over_name);
            goto done;
        }
        if (fmts[idx] != 'l') {
            aset_err(a, "aggregate: compute['%s'] over: '%s' must be int64",
                     cs->out_name, cs->over_name);
            goto done;
        }
        cs->over_idx = idx;
    }
    a->schema_cached = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

static int agg_find_or_create(AggState *a, const int64_t *key, size_t *out_idx) {
    for (size_t i = 0; i < a->n_groups; ++i) {
        int eq = 1;
        for (size_t k = 0; k < a->n_group_by; ++k) {
            if (a->groups[i].key_vals[k] != key[k]) { eq = 0; break; }
        }
        if (eq) { *out_idx = i; return 0; }
    }
    if (a->n_groups == a->groups_cap) {
        size_t nc = a->groups_cap ? a->groups_cap * 2 : 8;
        Group *g = realloc(a->groups, nc * sizeof *g);
        if (!g) return -1;
        a->groups = g;
        a->groups_cap = nc;
    }
    Group *g = &a->groups[a->n_groups];
    /* n_group_by is validated > 0 in init, but the analyzer can't see
     * that — guard the malloc size to keep it quiet. */
    g->key_vals = malloc((a->n_group_by ? a->n_group_by : 1) * sizeof *g->key_vals);
    g->accs     = calloc(a->n_computes, sizeof *g->accs);
    if (!g->key_vals || !g->accs) {
        free(g->key_vals); free(g->accs);
        return -1;
    }
    memcpy(g->key_vals, key, a->n_group_by * sizeof *key);
    for (size_t c = 0; c < a->n_computes; ++c) {
        g->accs[c].mn = INT64_MAX;
        g->accs[c].mx = INT64_MIN;
    }
    *out_idx = a->n_groups++;
    return 0;
}

static int read_int64_cell(const struct ArrowArray *col, size_t row_idx,
                           int64_t *out) {
    size_t row = row_idx + (size_t)col->offset;
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *valid = col->buffers[0];
        if (!(valid[row / 8] & (1u << (row % 8)))) return 1;
    }
    *out = ((const int64_t *)col->buffers[1])[row];
    return 0;
}

static int agg_materialize(AggState *a) {
    if (a->materialized) return 0;
    if (agg_resolve_schema(a) != 0) return -1;

    int64_t *key = malloc(a->n_group_by * sizeof *key);
    if (!key) { aset_err(a, "aggregate: out of memory"); return -1; }

    for (;;) {
        if (betl_should_cancel(a->ctx)) {
            free(key);
            aset_err(a, "aggregate: cancelled"); return -1;
        }
        struct ArrowArray batch = {0};
        if (a->input.get_next(&a->input, &batch) != 0) {
            const char *e = a->input.get_last_error
                                ? a->input.get_last_error(&a->input) : NULL;
            free(key);
            aset_err(a, "aggregate: upstream get_next failed: %s",
                     e ? e : "(no detail)");
            return -1;
        }
        if (!batch.release) break;
        if (batch.n_children != (int64_t)a->n_input_cols) {
            int64_t got = batch.n_children;
            size_t expected = a->n_input_cols;
            batch.release(&batch); free(key);
            aset_err(a, "aggregate: batch has %lld cols, expected %zu",
                     (long long)got, expected);
            return -1;
        }
        size_t length = (size_t)batch.length;
        for (size_t r = 0; r < length; ++r) {
            int row_failed = 0;
            for (size_t k = 0; k < a->n_group_by; ++k) {
                int64_t v;
                if (read_int64_cell(batch.children[a->group_by_idx[k]], r, &v) != 0) {
                    aset_err(a,
                        "aggregate: null in group_by column '%s' (row %zu) "
                        "is not supported in v0.1",
                        a->group_by_names[k], r);
                    row_failed = 1;
                    break;
                }
                key[k] = v;
            }
            if (row_failed) {
                batch.release(&batch); free(key); return -1;
            }
            size_t gi;
            if (agg_find_or_create(a, key, &gi) != 0) {
                batch.release(&batch); free(key);
                aset_err(a, "aggregate: out of memory");
                return -1;
            }
            Group *g = &a->groups[gi];
            for (size_t c = 0; c < a->n_computes; ++c) {
                ComputeSpec *cs = &a->computes[c];
                Accum *acc = &g->accs[c];
                if (cs->kind == AGG_COUNT) { ++acc->count; acc->seen = 1; continue; }
                int64_t v;
                if (read_int64_cell(batch.children[cs->over_idx], r, &v) != 0) {
                    continue;
                }
                ++acc->count;
                acc->seen = 1;
                switch (cs->kind) {
                    case AGG_SUM: acc->sum += v; break;
                    case AGG_MIN: if (v < acc->mn) acc->mn = v; break;
                    case AGG_MAX: if (v > acc->mx) acc->mx = v; break;
                    case AGG_COUNT: break;     /* unreachable */
                }
            }
        }
        batch.release(&batch);
    }
    free(key);
    a->materialized = 1;
    return 0;
}

static int agg_get_schema(struct ArrowArrayStream *st,
                          struct ArrowSchema *out) {
    AggState *a = st->private_data;
    memset(out, 0, sizeof *out);
    if (!a) return EINVAL;
    if (agg_resolve_schema(a) != 0) return EIO;

    size_t n_total = a->n_group_by + a->n_computes;
    struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < a->n_group_by; ++i) {
        kids[i] = betl_tx_new_leaf_schema(a->group_by_names[i], "l");
        if (!kids[i]) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    for (size_t i = 0; i < a->n_computes; ++i) {
        kids[a->n_group_by + i] = betl_tx_new_leaf_schema(a->computes[i].out_name, "l");
        if (!kids[a->n_group_by + i]) {
            for (size_t k = 0; k < a->n_group_by + i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = betl_tx_release_schema_struct_owned;
    return 0;
}

static int build_int64_const_leaf(struct ArrowArray *out,
                                  const int64_t *vals, size_t length) {
    int64_t *v = malloc((length ? length : 1) * sizeof *v);
    if (!v) return -1;
    if (length) memcpy(v, vals, length * sizeof *v);
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(v); return -1; }
    bufs[0] = NULL;
    bufs[1] = v;
    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = betl_tx_release_int64_leaf;
    return 0;
}

static int agg_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    AggState *a = st->private_data;
    memset(out, 0, sizeof *out);
    if (!a) return EINVAL;
    if (agg_materialize(a) != 0) return EIO;
    if (a->emitted) return 0;

    size_t length  = a->n_groups;
    size_t n_total = a->n_group_by + a->n_computes;
    if (n_total == 0) { aset_err(a, "aggregate: no output columns"); return EIO; }

    size_t cells = n_total * (length ? length : 1);
    int64_t *flat = malloc(cells * sizeof *flat);
    if (!flat) { aset_err(a, "aggregate: out of memory"); return EIO; }

    for (size_t r = 0; r < length; ++r) {
        for (size_t k = 0; k < a->n_group_by; ++k) {
            flat[k * length + r] = a->groups[r].key_vals[k];
        }
        for (size_t c = 0; c < a->n_computes; ++c) {
            const ComputeSpec *cs = &a->computes[c];
            const Accum *acc = &a->groups[r].accs[c];
            int64_t v = 0;
            switch (cs->kind) {
                case AGG_COUNT: v = acc->count; break;
                case AGG_SUM:   v = acc->sum;   break;
                case AGG_MIN:   v = acc->seen ? acc->mn : 0; break;
                case AGG_MAX:   v = acc->seen ? acc->mx : 0; break;
            }
            flat[(a->n_group_by + c) * length + r] = v;
        }
    }

    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        free(flat);
        aset_err(a, "aggregate: out of memory"); return EIO;
    }
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c] || build_int64_const_leaf(kids[c],
                                               flat + c * length,
                                               length) != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            free(flat);
            aset_err(a, "aggregate: out of memory"); return EIO;
        }
    }
    free(flat);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        aset_err(a, "aggregate: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;

    a->emitted = 1;
    return 0;
}

static const char *agg_get_last_error(struct ArrowArrayStream *st) {
    AggState *a = st->private_data;
    return (a && a->last_err[0]) ? a->last_err : NULL;
}

static void agg_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int agg_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = agg_get_schema;
    out->get_next       = agg_get_next;
    out->get_last_error = agg_get_last_error;
    out->release        = agg_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef agg_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to group" },
};
static const BetlPortDef agg_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "one row per group" },
};

static const BetlComponentDef agg_components[] = {
    { .name               = "aggregate",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = agg_inputs,
      .input_count        = 1,
      .outputs            = agg_outputs,
      .output_count       = 1,
      .init               = agg_init,
      .destroy            = agg_destroy,
      .attach_input       = agg_attach_input,
      .attach_output      = agg_attach_output },
};

static const BetlProvider agg_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-aggregate",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = agg_components,
    .component_count = sizeof agg_components / sizeof agg_components[0],
};

int betl_tx_register_aggregate(BetlRegistry *r) {
    return betl_registry_register(r, &agg_provider, "<builtin:aggregate>");
}
