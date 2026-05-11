/* `filter` TRANSFORM (SPEC §4.3).
 *
 * Compiles a predicate via the BetlExprEngine ABI from §7, evaluates
 * it once per upstream batch (desired_format = "b"), and copies through
 * only the rows where the predicate is true. Null predicate values are
 * treated as false and drop the row. v0.1 supports int64 and utf8
 * input columns; the predicate engine itself only needs to produce a
 * bool array.
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
    BetlContext *ctx;
    char        *lang;       /* default "lua" */
    char        *expr_src;

    const BetlExprEngine *engine;
    void                 *engine_handle;
    int                   handle_ready;

    struct ArrowArrayStream input;
    int                     have_input;

    /* Cached schema info. */
    int          schema_cached;
    size_t       n_cols;
    char       **col_names;       /* heap copies */
    char        *col_fmts;        /* one char per col: 'l' or 'u' */

    char         last_err[256];
} FilterState;

static void fset_err(FilterState *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->last_err, sizeof f->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(f->ctx, "%s", f->last_err);
}

static int parse_where(FilterState *f, const char *cfg) {
    /* Try shorthand: where: "string". */
    char *s = NULL;
    if (betl_tx_json_string_at(cfg, "where", &s) == 0 && s) {
        f->expr_src = s;
        f->lang     = strdup("lua");
        return f->lang ? BETL_OK : BETL_ERR_INTERNAL;
    }
    /* Full form: search for "lang" / "expr" / "value" at top level. The
     * strstr-based json_value_after finds them inside the where:{...}
     * object as well as anywhere else; for flat configs that's fine.
     * `value` is accepted in addition to `expr` so the literal engine
     * works (it spells its source field "value"). */
    char *lang = NULL, *src = NULL;
    betl_tx_json_string_at(cfg, "lang", &lang);
    if (betl_tx_json_string_at(cfg, "expr", &src) != 0 || !src) {
        free(src); src = NULL;
        betl_tx_json_value_to_string(cfg, "value", &src);
    }
    if (!lang || !src) {
        free(lang); free(src);
        fset_err(f,
            "filter: 'where' must be a string shorthand or {lang, expr|value} map");
        return BETL_ERR_INVALID;
    }
    f->lang     = lang;
    f->expr_src = src;
    return BETL_OK;
}

static int filter_init(BetlContext *ctx, const char *cfg, void **state) {
    FilterState *f = calloc(1, sizeof *f);
    if (!f) return BETL_ERR_INTERNAL;
    f->ctx = ctx;
    int rc = parse_where(f, cfg ? cfg : "{}");
    if (rc != BETL_OK) { free(f->lang); free(f->expr_src); free(f); return rc; }
    *state = f;
    return BETL_OK;
}

static int filter_attach_input(void *state, int port,
                               struct ArrowArrayStream *in) {
    (void)port;
    FilterState *f = state;
    f->input      = *in;
    f->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void filter_destroy(void *state) {
    if (!state) return;
    FilterState *f = state;
    if (f->have_input && f->input.release) f->input.release(&f->input);
    if (f->handle_ready && f->engine && f->engine_handle) {
        f->engine->release(f->engine_handle);
    }
    free(f->expr_src);
    free(f->lang);
    if (f->col_names) {
        for (size_t i = 0; i < f->n_cols; ++i) free(f->col_names[i]);
        free(f->col_names);
    }
    free(f->col_fmts);
    free(f);
}

/* Fetch upstream schema, validate columns are int64/utf8 only, cache
 * names and types. Idempotent. The caller still owns `*sch_keep` and
 * is responsible for releasing it (we copy what we need). */
static int ensure_schema(FilterState *f, struct ArrowSchema *sch_keep) {
    if (f->schema_cached) return 0;
    if (!f->have_input || !f->input.get_schema) {
        fset_err(f, "filter: input has no get_schema");
        return -1;
    }
    if (f->input.get_schema(&f->input, sch_keep) != 0) {
        fset_err(f, "filter: upstream get_schema failed");
        return -1;
    }
    if (!sch_keep->format || strcmp(sch_keep->format, "+s") != 0
        || sch_keep->n_children <= 0) {
        fset_err(f, "filter: input must be a struct with >=1 child");
        return -1;
    }
    size_t n = (size_t)sch_keep->n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) { free(names); free(fmts);
        fset_err(f, "filter: out of memory"); return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch_keep->children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        int is_fixed = fmt && betl_tx_fixed_width_for_fmt(fmt[0]) != 0
                       && (fmt[0] == 'c' || fmt[0] == 'C' ||
                           fmt[0] == 's' || fmt[0] == 'S' ||
                           fmt[0] == 'i' || fmt[0] == 'I' ||
                           fmt[0] == 'l' || fmt[0] == 'L' ||
                           fmt[0] == 'f' || fmt[0] == 'g');
        int is_utf8  = fmt && strcmp(fmt, "u") == 0;
        if (!is_fixed && !is_utf8) {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            fset_err(f, "filter: column '%s' has unsupported format '%s' "
                        "(supports fixed-width primitive ints/floats and utf8)",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            return -1;
        }
        fmts[i]  = fmt[0];
        names[i] = strdup(c->name ? c->name : "");
        if (!names[i]) {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            fset_err(f, "filter: out of memory"); return -1;
        }
    }
    f->n_cols     = n;
    f->col_names  = names;
    f->col_fmts   = fmts;
    f->schema_cached = 1;
    return 0;
}

static int ensure_compiled(FilterState *f, struct ArrowSchema *schema) {
    if (f->handle_ready) return 0;
    f->engine = betl_get_expr_engine(f->ctx, f->lang);
    if (!f->engine) {
        fset_err(f, "filter: no expression engine for lang '%s'", f->lang);
        return -1;
    }
    int rc = f->engine->compile(f->ctx, f->expr_src, schema, &f->engine_handle);
    if (rc != BETL_OK) return -1;     /* engine has set ctx error */
    f->handle_ready = 1;
    return 0;
}

/* Build a length-N int64 leaf containing only rows from `src` where
 * keep[i] != 0 (n_kept rows total). */
/* Row-mask leaf builders moved to transforms_common.c — shared with
 * distinct, limit, and conditional_split. */

static int filt_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    FilterState *f = st->private_data;
    if (!f || !f->have_input || !f->input.get_schema) return EINVAL;
    return f->input.get_schema(&f->input, out);
}

static int filt_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    FilterState *f = st->private_data;
    memset(out, 0, sizeof *out);
    if (!f) return EINVAL;

    /* Cache schema and compile expression on first call. */
    struct ArrowSchema sch = {0};
    if (ensure_schema(f, &sch) != 0) return EIO;
    if (ensure_compiled(f, &sch) != 0) {
        if (sch.release) sch.release(&sch);
        return EIO;
    }
    if (sch.release) sch.release(&sch);

    /* Pull upstream batch. */
    struct ArrowArray in_arr = {0};
    if (f->input.get_next(&f->input, &in_arr) != 0) {
        const char *e = f->input.get_last_error
                            ? f->input.get_last_error(&f->input) : NULL;
        fset_err(f, "filter: upstream get_next failed: %s",
                 e ? e : "(no detail)");
        return EIO;
    }
    if (!in_arr.release) return 0;          /* end-of-stream */

    if (in_arr.n_children != (int64_t)f->n_cols) {
        long long got = (long long)in_arr.n_children;
        size_t expected = f->n_cols;
        in_arr.release(&in_arr);
        fset_err(f, "filter: upstream batch has %lld cols, expected %zu",
                 got, expected);
        return EIO;
    }
    size_t length = (size_t)in_arr.length;

    /* Evaluate predicate -> bool leaf. */
    struct ArrowArray pred = {0};
    int rc = f->engine->evaluate(f->engine_handle, &in_arr, "b", &pred);
    if (rc != BETL_OK) {
        in_arr.release(&in_arr);
        return EIO;
    }
    if (pred.length != (int64_t)length || pred.n_buffers < 2 || !pred.buffers[1]) {
        if (pred.release) pred.release(&pred);
        in_arr.release(&in_arr);
        fset_err(f, "filter: engine returned malformed bool array");
        return EIO;
    }

    /* Build keep[]: 1=row passes, 0=drop. Null predicate -> drop. */
    uint8_t *keep = calloc(length ? length : 1, 1);
    if (!keep) {
        if (pred.release) pred.release(&pred);
        in_arr.release(&in_arr);
        fset_err(f, "filter: out of memory");
        return EIO;
    }
    const uint8_t *pred_valid = (pred.null_count > 0) ? pred.buffers[0] : NULL;
    const uint8_t *pred_vals  = pred.buffers[1];
    size_t n_kept = 0;
    for (size_t i = 0; i < length; ++i) {
        size_t row = i + (size_t)pred.offset;
        if (pred_valid && !betl_tx_bit_at(pred_valid, row)) continue;
        if (betl_tx_bit_at(pred_vals, row)) { keep[i] = 1; ++n_kept; }
    }
    if (pred.release) pred.release(&pred);

    /* Per-column copy with selection. */
    struct ArrowArray **kids = calloc(f->n_cols, sizeof *kids);
    if (!kids) {
        free(keep); in_arr.release(&in_arr);
        fset_err(f, "filter: out of memory");
        return EIO;
    }
    for (size_t c = 0; c < f->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        int crc;
        size_t w = betl_tx_fixed_width_for_fmt(f->col_fmts[c]);
        if (w != 0) {
            crc = betl_tx_build_fixed_filtered(kids[c], in_arr.children[c],
                                               w, keep, length, n_kept);
        } else {
            crc = betl_tx_build_utf8_filtered (kids[c], in_arr.children[c],
                                               keep, length, n_kept);
        }
        if (crc != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); free(keep); in_arr.release(&in_arr);
            fset_err(f, "filter: build column '%s' failed", f->col_names[c]);
            return EIO;
        }
    }
    free(keep);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < f->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids); in_arr.release(&in_arr);
        fset_err(f, "filter: out of memory");
        return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n_kept;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)f->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;

    in_arr.release(&in_arr);
    return 0;
}

static const char *filt_get_last_error(struct ArrowArrayStream *st) {
    FilterState *f = st->private_data;
    return (f && f->last_err[0]) ? f->last_err : NULL;
}

static void filt_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int filter_attach_output(void *state, int port,
                                struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = filt_get_schema;
    out->get_next       = filt_get_next;
    out->get_last_error = filt_get_last_error;
    out->release        = filt_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef filter_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "any rows" },
};
static const BetlPortDef filter_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "subset of in" },
};

static const BetlComponentDef filter_components[] = {
    { .name               = "filter",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = filter_inputs,
      .input_count        = 1,
      .outputs            = filter_outputs,
      .output_count       = 1,
      .init               = filter_init,
      .destroy            = filter_destroy,
      .attach_input       = filter_attach_input,
      .attach_output      = filter_attach_output },
};

static const BetlProvider filter_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-filter",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = filter_components,
    .component_count = sizeof filter_components / sizeof filter_components[0],
};

int betl_tx_register_filter(BetlRegistry *r) {
    return betl_registry_register(r, &filter_provider, "<builtin:filter>");
}
