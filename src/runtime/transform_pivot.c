/* `pivot` TRANSFORM. Inverse of unpivot: takes rows that name a column
 * + value and folds them into wide rows keyed by id_cols.
 *
 * YAML:
 *   - id: p
 *     type: pivot
 *     from: source
 *     id_cols:    [region, year]        # group-by keys
 *     name_col:   month                  # holds the future column name
 *     value_col:  amount                 # holds the future cell value
 *     pivot_keys: [jan, feb, mar]        # declared up-front
 *
 * Requires `pivot_keys` declared so the output schema is fixed at
 * compile time and we don't have to buffer the entire stream to learn
 * the unique column-name values. Requires the upstream to be sorted /
 * grouped on id_cols — matches SSIS Pivot's contract.
 *
 * Implementation: maintain a single "current group" — buffer all rows
 * with the same id-tuple, emit one wide row when the id-tuple changes
 * (or upstream ends). Sorted-input assumption means at most one row
 * carry-over between batches.
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
    char **pivot_keys;       size_t n_pivot;
    char  *name_col;
    char  *value_col;

    /* schema */
    int    schema_resolved;
    size_t n_input_cols;
    char **input_col_names;
    char  *input_col_fmts;
    int   *id_idx;
    int    name_idx;
    int    value_idx;
    char   value_fmt;
    char  *value_fmt_str;

    /* output-row staging */
    int        have_open_group;
    /* current id-tuple slot values (int64 stash + str for utf8) */
    int64_t   *id_slot_i64;
    char     **id_slot_strs;
    size_t    *id_slot_lens;
    uint8_t   *id_slot_null;
    /* pivot slots: per-key one int64 / float-bits, or one utf8 cell. */
    int64_t   *pivot_slot_i64;
    char     **pivot_slot_strs;
    size_t    *pivot_slot_lens;
    uint8_t   *pivot_slot_null;     /* 1 = unset, 0 = filled */

    /* completed output rows accumulator (one batch at a time). */
    size_t     out_n;
    size_t     out_cap;
    int64_t   *out_id_i64;          /* [n_id][out_cap] flattened: c*cap + r */
    char     **out_id_strs;
    size_t    *out_id_lens;
    uint8_t   *out_id_null;
    int64_t   *out_pivot_i64;       /* [n_pivot][out_cap] flattened */
    char     **out_pivot_strs;
    size_t    *out_pivot_lens;
    uint8_t   *out_pivot_null;

    int    emitted_eof;
    char   last_err[256];
} PivotState;

static void pset_err(PivotState *p, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->last_err, sizeof p->last_err, fmt, ap); va_end(ap);
    betl_set_error(p->ctx, "%s", p->last_err);
}

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

static int pivot_init(BetlContext *ctx, const char *cfg, void **state) {
    PivotState *p = calloc(1, sizeof *p);
    if (!p) return BETL_ERR_INTERNAL;
    p->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    const char *pp;
    if ((pp = betl_tx_json_value_after(cfg, "id_cols")) && *pp == '[') {
        StrlistCtx sc = { .arr = &p->id_col_names, .n = &p->n_id };
        if (betl_tx_json_walk_array(pp, strlist_visit, &sc) != 0 || sc.err) {
            betl_set_error(ctx, "pivot: `id_cols:` must be a list of strings");
            goto fail;
        }
    }
    if ((pp = betl_tx_json_value_after(cfg, "pivot_keys")) && *pp == '[') {
        StrlistCtx sc = { .arr = &p->pivot_keys, .n = &p->n_pivot };
        if (betl_tx_json_walk_array(pp, strlist_visit, &sc) != 0 || sc.err) {
            betl_set_error(ctx, "pivot: `pivot_keys:` must be a list of strings");
            goto fail;
        }
    }
    if (p->n_pivot == 0) {
        betl_set_error(ctx, "pivot: `pivot_keys:` is required (no streaming-friendly inference)");
        goto fail;
    }
    if (betl_tx_json_string_at(cfg, "name_col", &p->name_col) != 0 || !p->name_col) {
        betl_set_error(ctx, "pivot: `name_col:` is required"); goto fail;
    }
    if (betl_tx_json_string_at(cfg, "value_col", &p->value_col) != 0 || !p->value_col) {
        betl_set_error(ctx, "pivot: `value_col:` is required"); goto fail;
    }
    *state = p;
    return BETL_OK;
fail:
    for (size_t i = 0; i < p->n_id; ++i) free(p->id_col_names[i]);
    free(p->id_col_names);
    for (size_t i = 0; i < p->n_pivot; ++i) free(p->pivot_keys[i]);
    free(p->pivot_keys);
    free(p->name_col); free(p->value_col);
    free(p);
    return BETL_ERR_INVALID;
}

static void pivot_destroy(void *state) {
    if (!state) return;
    PivotState *p = state;
    if (p->have_input && p->input.release) p->input.release(&p->input);
    for (size_t i = 0; i < p->n_id; ++i) free(p->id_col_names[i]);
    free(p->id_col_names);
    for (size_t i = 0; i < p->n_pivot; ++i) free(p->pivot_keys[i]);
    free(p->pivot_keys);
    if (p->input_col_names) {
        for (size_t i = 0; i < p->n_input_cols; ++i) free(p->input_col_names[i]);
        free(p->input_col_names);
    }
    free(p->input_col_fmts);
    free(p->id_idx);
    free(p->value_fmt_str);
    free(p->id_slot_i64);
    if (p->id_slot_strs) {
        for (size_t i = 0; i < p->n_id; ++i) free(p->id_slot_strs[i]);
        free(p->id_slot_strs);
    }
    free(p->id_slot_lens);
    free(p->id_slot_null);
    free(p->pivot_slot_i64);
    if (p->pivot_slot_strs) {
        for (size_t i = 0; i < p->n_pivot; ++i) free(p->pivot_slot_strs[i]);
        free(p->pivot_slot_strs);
    }
    free(p->pivot_slot_lens);
    free(p->pivot_slot_null);
    free(p->out_id_i64);
    if (p->out_id_strs) {
        for (size_t i = 0; i < p->n_id * p->out_cap; ++i) free(p->out_id_strs[i]);
        free(p->out_id_strs);
    }
    free(p->out_id_lens);
    free(p->out_id_null);
    free(p->out_pivot_i64);
    if (p->out_pivot_strs) {
        for (size_t i = 0; i < p->n_pivot * p->out_cap; ++i) free(p->out_pivot_strs[i]);
        free(p->out_pivot_strs);
    }
    free(p->out_pivot_lens);
    free(p->out_pivot_null);
    free(p->name_col); free(p->value_col);
    free(p);
}

static int pivot_attach_input(void *state, int port,
                              struct ArrowArrayStream *in) {
    (void)port;
    PivotState *p = state;
    p->input = *in;
    p->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  Schema resolution                                               *
 * ============================================================== */

static int pivot_resolve_schema(PivotState *p) {
    if (p->schema_resolved) return 0;
    if (!p->have_input || !p->input.get_schema) {
        pset_err(p, "pivot: no input attached"); return -1;
    }
    struct ArrowSchema sch = {0};
    if (p->input.get_schema(&p->input, &sch) != 0) {
        pset_err(p, "pivot: upstream get_schema failed"); return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        pset_err(p, "pivot: input must be a struct with >=1 child"); goto done;
    }
    size_t n = (size_t)sch.n_children;
    p->input_col_names = calloc(n, sizeof *p->input_col_names);
    p->input_col_fmts  = calloc(n, 1);
    if (!p->input_col_names || !p->input_col_fmts) {
        pset_err(p, "pivot: out of memory"); goto done;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt) { pset_err(p, "pivot: column %zu has no format", i); goto done; }
        p->input_col_fmts[i] = fmt[0];
        p->input_col_names[i] = strdup(c->name ? c->name : "");
        if (!p->input_col_names[i]) { pset_err(p, "pivot: out of memory"); goto done; }
    }
    p->n_input_cols = n;

    p->id_idx = calloc(p->n_id ? p->n_id : 1, sizeof *p->id_idx);
    if (p->n_id && !p->id_idx) { pset_err(p, "pivot: OOM"); goto done; }
    for (size_t i = 0; i < p->n_id; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(p->input_col_names[k], p->id_col_names[i]) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) { pset_err(p, "pivot: id_col '%s' not found", p->id_col_names[i]); goto done; }
        p->id_idx[i] = idx;
    }
    p->name_idx = -1; p->value_idx = -1;
    for (size_t k = 0; k < n; ++k) {
        if (strcmp(p->input_col_names[k], p->name_col)  == 0) p->name_idx  = (int)k;
        if (strcmp(p->input_col_names[k], p->value_col) == 0) p->value_idx = (int)k;
    }
    if (p->name_idx  < 0) { pset_err(p, "pivot: name_col '%s' not found", p->name_col);  goto done; }
    if (p->value_idx < 0) { pset_err(p, "pivot: value_col '%s' not found", p->value_col); goto done; }
    if (p->input_col_fmts[p->name_idx] != 'u') {
        pset_err(p, "pivot: name_col must be utf8"); goto done;
    }
    p->value_fmt = p->input_col_fmts[p->value_idx];
    const char *vfs = sch.children[p->value_idx]->format;
    p->value_fmt_str = strdup(vfs ? vfs : "");
    if (!p->value_fmt_str) { pset_err(p, "pivot: OOM"); goto done; }
    if (betl_tx_fixed_width_for_fmt(p->value_fmt) == 0 && p->value_fmt != 'u') {
        pset_err(p, "pivot: value_col format '%c' not supported", p->value_fmt);
        goto done;
    }

    /* Allocate group-row staging. */
    if (p->n_id > 0) {
        p->id_slot_i64  = calloc(p->n_id, sizeof *p->id_slot_i64);
        p->id_slot_strs = calloc(p->n_id, sizeof *p->id_slot_strs);
        p->id_slot_lens = calloc(p->n_id, sizeof *p->id_slot_lens);
        p->id_slot_null = calloc(p->n_id, 1);
        if (!p->id_slot_i64 || !p->id_slot_strs || !p->id_slot_lens
            || !p->id_slot_null) { pset_err(p, "pivot: OOM"); goto done; }
    }
    p->pivot_slot_i64  = calloc(p->n_pivot, sizeof *p->pivot_slot_i64);
    p->pivot_slot_strs = calloc(p->n_pivot, sizeof *p->pivot_slot_strs);
    p->pivot_slot_lens = calloc(p->n_pivot, sizeof *p->pivot_slot_lens);
    p->pivot_slot_null = calloc(p->n_pivot, 1);
    if (!p->pivot_slot_i64 || !p->pivot_slot_strs || !p->pivot_slot_lens
        || !p->pivot_slot_null) { pset_err(p, "pivot: OOM"); goto done; }

    p->schema_resolved = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

/* ============================================================== *
 *  Per-cell readers                                                *
 * ============================================================== */

static int cell_is_null(const struct ArrowArray *col, size_t r) {
    size_t row = r + (size_t)col->offset;
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *v = col->buffers[0];
        return !((v[row / 8] >> (row % 8)) & 1u);
    }
    return 0;
}

/* Read any fixed-width int/float into the int64 slot (bit-reinterpret floats). */
static int64_t read_fixed_widened(const struct ArrowArray *col, char fmt, size_t r) {
    size_t row = r + (size_t)col->offset;
    int64_t out = 0;
    switch (fmt) {
        case 'l': out = ((const int64_t  *)col->buffers[1])[row]; break;
        case 'L': out = (int64_t)((const uint64_t *)col->buffers[1])[row]; break;
        case 'c': out = ((const int8_t   *)col->buffers[1])[row]; break;
        case 'C': out = ((const uint8_t  *)col->buffers[1])[row]; break;
        case 's': out = ((const int16_t  *)col->buffers[1])[row]; break;
        case 'S': out = ((const uint16_t *)col->buffers[1])[row]; break;
        case 'i': out = ((const int32_t  *)col->buffers[1])[row]; break;
        case 'I': out = (int64_t)((const uint32_t *)col->buffers[1])[row]; break;
        case 'g': { double d = ((const double *)col->buffers[1])[row];
                    memcpy(&out, &d, sizeof d); break; }
        case 'f': { float f = ((const float *)col->buffers[1])[row]; double d = (double)f;
                    memcpy(&out, &d, sizeof d); break; }
        default: break;
    }
    return out;
}

static void read_utf8(const struct ArrowArray *col, size_t r,
                      const char **out_p, size_t *out_n) {
    size_t row = r + (size_t)col->offset;
    const int32_t *off = col->buffers[1];
    const char    *dat = col->buffers[2];
    *out_p = dat + off[row];
    *out_n = (size_t)(off[row + 1] - off[row]);
}

/* Compare two id-tuple slots for equality. Both sides represent
 * "current group" semantics so we compare slot vs incoming row. */
static int id_matches(const PivotState *p, const struct ArrowArray *batch, size_t r) {
    for (size_t k = 0; k < p->n_id; ++k) {
        const struct ArrowArray *col = batch->children[p->id_idx[k]];
        int n_now = cell_is_null(col, r);
        if (n_now != (int)p->id_slot_null[k]) return 0;
        if (n_now) continue;
        char fmt = p->input_col_fmts[p->id_idx[k]];
        if (fmt == 'u') {
            const char *s; size_t sl;
            read_utf8(col, r, &s, &sl);
            if (sl != p->id_slot_lens[k]) return 0;
            if (sl && memcmp(p->id_slot_strs[k], s, sl) != 0) return 0;
        } else {
            int64_t v = read_fixed_widened(col, fmt, r);
            if (v != p->id_slot_i64[k]) return 0;
        }
    }
    return 1;
}

static int capture_group_id(PivotState *p, const struct ArrowArray *batch, size_t r) {
    for (size_t k = 0; k < p->n_id; ++k) {
        const struct ArrowArray *col = batch->children[p->id_idx[k]];
        p->id_slot_null[k] = (uint8_t)cell_is_null(col, r);
        free(p->id_slot_strs[k]); p->id_slot_strs[k] = NULL;
        p->id_slot_lens[k] = 0;
        if (p->id_slot_null[k]) continue;
        char fmt = p->input_col_fmts[p->id_idx[k]];
        if (fmt == 'u') {
            const char *s; size_t sl;
            read_utf8(col, r, &s, &sl);
            char *dup = malloc(sl + 1);
            if (!dup) return -1;
            if (sl) memcpy(dup, s, sl);
            dup[sl] = '\0';
            p->id_slot_strs[k] = dup;
            p->id_slot_lens[k] = sl;
        } else {
            p->id_slot_i64[k] = read_fixed_widened(col, fmt, r);
        }
    }
    for (size_t k = 0; k < p->n_pivot; ++k) {
        p->pivot_slot_null[k] = 1;
        free(p->pivot_slot_strs[k]); p->pivot_slot_strs[k] = NULL;
        p->pivot_slot_lens[k] = 0;
        p->pivot_slot_i64[k] = 0;
    }
    p->have_open_group = 1;
    return 0;
}

/* Slot one pivot value from the current input row. */
static int slot_pivot_value(PivotState *p, const struct ArrowArray *batch, size_t r) {
    const struct ArrowArray *nc = batch->children[p->name_idx];
    if (cell_is_null(nc, r)) return 0;     /* NULL name → ignored */
    const char *nm; size_t nml;
    read_utf8(nc, r, &nm, &nml);
    int slot = -1;
    for (size_t k = 0; k < p->n_pivot; ++k) {
        const char *pk = p->pivot_keys[k];
        size_t pkl = strlen(pk);
        if (pkl == nml && (nml == 0 || memcmp(nm, pk, nml) == 0)) { slot = (int)k; break; }
    }
    if (slot < 0) return 0;        /* unknown name → silently dropped */
    const struct ArrowArray *vc = batch->children[p->value_idx];
    if (cell_is_null(vc, r)) {
        p->pivot_slot_null[slot] = 1;
        return 0;
    }
    p->pivot_slot_null[slot] = 0;
    if (p->value_fmt == 'u') {
        const char *s; size_t sl;
        read_utf8(vc, r, &s, &sl);
        char *dup = malloc(sl + 1);
        if (!dup) return -1;
        if (sl) memcpy(dup, s, sl);
        dup[sl] = '\0';
        free(p->pivot_slot_strs[slot]);
        p->pivot_slot_strs[slot] = dup;
        p->pivot_slot_lens[slot] = sl;
    } else {
        p->pivot_slot_i64[slot] = read_fixed_widened(vc, p->value_fmt, r);
    }
    return 0;
}

/* ============================================================== *
 *  Output staging                                                  *
 * ============================================================== */

static int out_reserve(PivotState *p) {
    if (p->out_cap > p->out_n) return 0;
    size_t nc = p->out_cap ? p->out_cap * 2 : 64;
    int64_t *ni = realloc(p->out_id_i64, p->n_id * nc * sizeof *ni);
    if (p->n_id && !ni) return -1;
    if (ni) p->out_id_i64 = ni;
    char **ns = realloc(p->out_id_strs, p->n_id * nc * sizeof *ns);
    if (p->n_id && !ns) return -1;
    if (ns) {
        /* Zero the newly allocated slots. */
        for (size_t i = p->n_id * p->out_cap; i < p->n_id * nc; ++i) ns[i] = NULL;
        p->out_id_strs = ns;
    }
    size_t *nl = realloc(p->out_id_lens, p->n_id * nc * sizeof *nl);
    if (p->n_id && !nl) return -1;
    if (nl) p->out_id_lens = nl;
    uint8_t *nn = realloc(p->out_id_null, p->n_id * nc);
    if (p->n_id && !nn) return -1;
    if (nn) p->out_id_null = nn;

    int64_t *pi = realloc(p->out_pivot_i64, p->n_pivot * nc * sizeof *pi);
    if (!pi) return -1;
    p->out_pivot_i64 = pi;
    char **ps = realloc(p->out_pivot_strs, p->n_pivot * nc * sizeof *ps);
    if (!ps) return -1;
    for (size_t i = p->n_pivot * p->out_cap; i < p->n_pivot * nc; ++i) ps[i] = NULL;
    p->out_pivot_strs = ps;
    size_t *pl = realloc(p->out_pivot_lens, p->n_pivot * nc * sizeof *pl);
    if (!pl) return -1;
    p->out_pivot_lens = pl;
    uint8_t *pn = realloc(p->out_pivot_null, p->n_pivot * nc);
    if (!pn) return -1;
    p->out_pivot_null = pn;

    p->out_cap = nc;
    return 0;
}

/* Flush current open group into the output row accumulator. Resets
 * pivot_slot_* (id_slot_* is overwritten on the next group open). */
static int flush_group(PivotState *p) {
    if (!p->have_open_group) return 0;
    if (out_reserve(p) != 0) return -1;
    size_t r = p->out_n++;
    for (size_t k = 0; k < p->n_id; ++k) {
        size_t idx = k * p->out_cap + r;
        p->out_id_null[idx] = p->id_slot_null[k];
        if (p->id_slot_null[k]) continue;
        if (p->input_col_fmts[p->id_idx[k]] == 'u') {
            char *dup = malloc(p->id_slot_lens[k] + 1);
            if (!dup) return -1;
            if (p->id_slot_lens[k]) memcpy(dup, p->id_slot_strs[k], p->id_slot_lens[k]);
            dup[p->id_slot_lens[k]] = '\0';
            free(p->out_id_strs[idx]);
            p->out_id_strs[idx] = dup;
            p->out_id_lens[idx] = p->id_slot_lens[k];
        } else {
            p->out_id_i64[idx] = p->id_slot_i64[k];
        }
    }
    for (size_t k = 0; k < p->n_pivot; ++k) {
        size_t idx = k * p->out_cap + r;
        p->out_pivot_null[idx] = p->pivot_slot_null[k];
        if (p->pivot_slot_null[k]) continue;
        if (p->value_fmt == 'u') {
            char *dup = malloc(p->pivot_slot_lens[k] + 1);
            if (!dup) return -1;
            if (p->pivot_slot_lens[k]) memcpy(dup, p->pivot_slot_strs[k], p->pivot_slot_lens[k]);
            dup[p->pivot_slot_lens[k]] = '\0';
            free(p->out_pivot_strs[idx]);
            p->out_pivot_strs[idx] = dup;
            p->out_pivot_lens[idx] = p->pivot_slot_lens[k];
        } else {
            p->out_pivot_i64[idx] = p->pivot_slot_i64[k];
        }
    }
    p->have_open_group = 0;
    return 0;
}

/* ============================================================== *
 *  Output schema + batch emission                                  *
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

static int pivot_get_schema(struct ArrowArrayStream *st,
                            struct ArrowSchema *out) {
    PivotState *p = st->private_data;
    memset(out, 0, sizeof *out);
    if (!p) return EINVAL;
    if (pivot_resolve_schema(p) != 0) return EIO;

    struct ArrowSchema up = {0};
    if (p->input.get_schema(&p->input, &up) != 0) return EIO;

    size_t n_total = p->n_id + p->n_pivot;
    struct ArrowSchema **kids = calloc(n_total ? n_total : 1, sizeof *kids);
    if (!kids) { if (up.release) up.release(&up); return ENOMEM; }
    for (size_t i = 0; i < p->n_id; ++i) {
        struct ArrowSchema *src = up.children[p->id_idx[i]];
        kids[i] = new_leaf(p->id_col_names[i], src->format);
        if (!kids[i]) goto oom;
    }
    for (size_t i = 0; i < p->n_pivot; ++i) {
        kids[p->n_id + i] = new_leaf(p->pivot_keys[i], p->value_fmt_str);
        if (!kids[p->n_id + i]) goto oom;
    }
    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = release_schema_struct_owned;
    if (up.release) up.release(&up);
    return 0;
oom:
    for (size_t i = 0; i < n_total; ++i) {
        if (kids[i]) { if (kids[i]->release) kids[i]->release(kids[i]); free(kids[i]); }
    }
    free(kids);
    if (up.release) up.release(&up);
    return ENOMEM;
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

static int build_fixed_from_stash(struct ArrowArray *out, char fmt,
                                  const int64_t *stash, const uint8_t *nulls,
                                  size_t n) {
    if (!out) return -1;
    size_t w = betl_tx_fixed_width_for_fmt(fmt);
    if (w == 0) return -1;
    uint8_t *vals = malloc((n ? n : 1) * w);
    if (!vals) return -1;
    size_t bytes = (n + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!vmap) { free(vals); return -1; }
    memset(vmap, 0xFF, bytes ? bytes : 1);
    int64_t null_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i]) {
            memset(vals + i * w, 0, w);
            vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
            ++null_count;
            continue;
        }
        int64_t v = stash[i];
        switch (fmt) {
            case 'l': case 'L': case 'g': memcpy(vals + i * 8, &v, 8); break;
            case 'c': case 'C':           vals[i] = (uint8_t)v; break;
            case 's': case 'S': { int16_t b = (int16_t)v; memcpy(vals + i * 2, &b, 2); break; }
            case 'i': case 'I': { int32_t b = (int32_t)v; memcpy(vals + i * 4, &b, 4); break; }
            case 'f': { double d; memcpy(&d, &v, sizeof d);
                        float f = (float)d; memcpy(vals + i * 4, &f, 4); break; }
            default: free(vals); free(vmap); return -1;
        }
    }
    if (null_count == 0) { free(vmap); vmap = NULL; }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = vals;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 2;
    out->buffers    = bufs;
    out->release    = release_fixed_leaf;
    return 0;
}

static int build_utf8_from_stash(struct ArrowArray *out,
                                 char *const *strs, const size_t *lens,
                                 const uint8_t *nulls, size_t n) {
    if (!out) return -1;
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0; offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!nulls[i]) total += lens[i];
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    int64_t null_count = 0;
    size_t bytes = (n + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!vmap) { free(offs); free(data); return -1; }
    memset(vmap, 0xFF, bytes ? bytes : 1);
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i]) {
            vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
            ++null_count;
            continue;
        }
        if (lens[i]) memcpy(data + pos, strs[i], lens[i]);
        pos += lens[i];
    }
    if (null_count == 0) { free(vmap); vmap = NULL; }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap; bufs[1] = offs; bufs[2] = data;
    out->length     = (int64_t)n;
    out->null_count = null_count;
    out->n_buffers  = 3;
    out->buffers    = bufs;
    out->release    = release_utf8_leaf;
    return 0;
}

/* Build an output batch from p->out_*. Resets out_n on success. */
static int emit_batch(PivotState *p, struct ArrowArray *out) {
    size_t n = p->out_n;
    size_t n_total = p->n_id + p->n_pivot;
    if (n == 0) { memset(out, 0, sizeof *out); return 0; }
    struct ArrowArray **kids = calloc(n_total ? n_total : 1, sizeof *kids);
    if (!kids) return -1;
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) goto fail;
    }
    for (size_t k = 0; k < p->n_id; ++k) {
        char fmt = p->input_col_fmts[p->id_idx[k]];
        const int64_t *stash = p->out_id_i64 + k * p->out_cap;
        const uint8_t *nulls = p->out_id_null + k * p->out_cap;
        if (fmt == 'u') {
            char  **strs = p->out_id_strs + k * p->out_cap;
            size_t *lens = p->out_id_lens + k * p->out_cap;
            if (build_utf8_from_stash(kids[k], strs, lens, nulls, n) != 0) goto fail;
        } else {
            if (build_fixed_from_stash(kids[k], fmt, stash, nulls, n) != 0) goto fail;
        }
    }
    for (size_t k = 0; k < p->n_pivot; ++k) {
        const int64_t *stash = p->out_pivot_i64 + k * p->out_cap;
        const uint8_t *nulls = p->out_pivot_null + k * p->out_cap;
        if (p->value_fmt == 'u') {
            char  **strs = p->out_pivot_strs + k * p->out_cap;
            size_t *lens = p->out_pivot_lens + k * p->out_cap;
            if (build_utf8_from_stash(kids[p->n_id + k], strs, lens,
                                      nulls, n) != 0) goto fail;
        } else {
            if (build_fixed_from_stash(kids[p->n_id + k], p->value_fmt,
                                       stash, nulls, n) != 0) goto fail;
        }
    }
    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) goto fail;
    outer[0] = NULL;
    out->length     = (int64_t)n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;

    /* Reset accumulator: per-cell strdup'd entries were transferred into
     * kids via build_utf8_from_stash → memcpy into a packed buffer; we
     * still own the per-slot dup pointers and must free them. */
    for (size_t i = 0; i < p->n_id * p->out_cap; ++i) {
        free(p->out_id_strs[i]); p->out_id_strs[i] = NULL;
    }
    for (size_t i = 0; i < p->n_pivot * p->out_cap; ++i) {
        free(p->out_pivot_strs[i]); p->out_pivot_strs[i] = NULL;
    }
    p->out_n = 0;
    return 0;

fail:
    for (size_t c = 0; c < n_total; ++c) {
        if (kids[c]) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
    }
    free(kids);
    return -1;
}

static int pivot_get_next(struct ArrowArrayStream *st,
                          struct ArrowArray *out) {
    PivotState *p = st->private_data;
    memset(out, 0, sizeof *out);
    if (!p) return EINVAL;
    if (pivot_resolve_schema(p) != 0) return EIO;
    if (p->emitted_eof) return 0;

    /* Stream one input batch, process all its rows, emit if any complete
     * groups were produced. Loop until at least one row is queued or
     * upstream is exhausted. */
    while (p->out_n == 0) {
        struct ArrowArray in_arr = {0};
        if (p->input.get_next(&p->input, &in_arr) != 0) {
            pset_err(p, "pivot: upstream get_next failed"); return EIO;
        }
        if (!in_arr.release) {
            /* End-of-stream: flush any open group and signal EOF. */
            if (flush_group(p) != 0) { pset_err(p, "pivot: OOM at EOF flush"); return EIO; }
            p->emitted_eof = 1;
            if (p->out_n == 0) return 0;
            break;
        }
        size_t length = (size_t)in_arr.length;
        for (size_t r = 0; r < length; ++r) {
            if (p->have_open_group && !id_matches(p, &in_arr, r)) {
                if (flush_group(p) != 0) {
                    in_arr.release(&in_arr);
                    pset_err(p, "pivot: OOM at flush"); return EIO;
                }
            }
            if (!p->have_open_group) {
                if (capture_group_id(p, &in_arr, r) != 0) {
                    in_arr.release(&in_arr);
                    pset_err(p, "pivot: OOM at group open"); return EIO;
                }
            }
            if (slot_pivot_value(p, &in_arr, r) != 0) {
                in_arr.release(&in_arr);
                pset_err(p, "pivot: OOM at slot"); return EIO;
            }
        }
        in_arr.release(&in_arr);
    }
    if (emit_batch(p, out) != 0) {
        pset_err(p, "pivot: emit failed"); return EIO;
    }
    return 0;
}

static const char *pivot_get_last_error(struct ArrowArrayStream *st) {
    PivotState *p = st->private_data;
    return (p && p->last_err[0]) ? p->last_err : NULL;
}
static void pivot_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int pivot_attach_output(void *state, int port,
                               struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = pivot_get_schema;
    out->get_next       = pivot_get_next;
    out->get_last_error = pivot_get_last_error;
    out->release        = pivot_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef pivot_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "long rows; must be sorted on id_cols" },
};
static const BetlPortDef pivot_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "wide rows" },
};

static const BetlComponentDef pivot_components[] = {
    { .name               = "pivot",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = pivot_inputs,
      .input_count        = 1,
      .outputs            = pivot_outputs,
      .output_count       = 1,
      .init               = pivot_init,
      .destroy            = pivot_destroy,
      .attach_input       = pivot_attach_input,
      .attach_output      = pivot_attach_output },
};

static const BetlProvider pivot_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-pivot",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = pivot_components,
    .component_count = sizeof pivot_components / sizeof pivot_components[0],
};

int betl_tx_register_pivot(BetlRegistry *r) {
    return betl_registry_register(r, &pivot_provider, "<builtin:pivot>");
}
