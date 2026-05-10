/* `limit` TRANSFORM — keep the first N rows, drop the rest.
 *
 * Config:
 *   n: <int>     required, must be > 0
 *
 * Streaming: pass batches through unchanged until we'd exceed `n`; for
 * the cusp batch, slice it down to the remaining-row count and emit a
 * fresh trimmed batch. Subsequent get_next calls return EOF without
 * pulling more upstream batches. */

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
    struct ArrowArrayStream input;
    int                     have_input;

    int64_t  n_max;            /* configured cap */
    int64_t  n_emitted;        /* rows pushed downstream so far */

    /* Schema cache — column count + per-col format. We need formats
     * to call the right slicing helper for the cusp batch. Resolved
     * lazily on the first get_next call (after upstream is queryable). */
    int      schema_resolved;
    size_t   n_cols;
    char    *col_fmts;

    char     last_err[256];
} LimitState;

static void lset_err(LimitState *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->last_err, sizeof l->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(l->ctx, "%s", l->last_err);
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int limit_init(BetlContext *ctx, const char *cfg, void **state) {
    LimitState *l = calloc(1, sizeof *l);
    if (!l) return BETL_ERR_INTERNAL;
    l->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    char *n_str = NULL;
    if (betl_tx_json_value_to_string(cfg, "n", &n_str) != 0 || !n_str) {
        lset_err(l, "limit: required `n:` (positive integer) is missing");
        free(l);
        return BETL_ERR_INVALID;
    }
    char *end = NULL;
    long long v = strtoll(n_str, &end, 10);
    if (end == n_str || *end != '\0' || v <= 0) {
        lset_err(l, "limit: `n:` must be a positive integer (got '%s')", n_str);
        free(n_str); free(l);
        return BETL_ERR_INVALID;
    }
    free(n_str);
    l->n_max = (int64_t)v;

    *state = l;
    return BETL_OK;
}

static int limit_attach_input(void *state, int port,
                              struct ArrowArrayStream *in) {
    (void)port;
    LimitState *l = state;
    l->input = *in;
    l->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void limit_destroy(void *state) {
    if (!state) return;
    LimitState *l = state;
    if (l->have_input && l->input.release) l->input.release(&l->input);
    free(l->col_fmts);
    free(l);
}

/* ============================================================== *
 *  Schema resolution                                               *
 * ============================================================== */

static int limit_resolve_schema(LimitState *l) {
    if (l->schema_resolved) return 0;
    if (!l->have_input || !l->input.get_schema) {
        lset_err(l, "limit: input has no get_schema");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (l->input.get_schema(&l->input, &sch) != 0) {
        lset_err(l, "limit: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        lset_err(l, "limit: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    l->col_fmts = calloc(n, 1);
    if (!l->col_fmts) { lset_err(l, "limit: out of memory"); goto done; }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            lset_err(l, "limit: input column has unsupported format '%s'",
                     fmt ? fmt : "(none)");
            goto done;
        }
        l->col_fmts[i] = fmt[0];
    }
    l->n_cols = n;
    l->schema_resolved = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

/* Slice the first n_take rows out of `src` into a fresh batch in
 * `out`. Releases `src` after building. */
static int slice_batch(LimitState *l, struct ArrowArray *src, size_t n_take,
                      struct ArrowArray *out) {
    size_t length = (size_t)src->length;
    uint8_t *keep = calloc(length, 1);
    if (!keep) {
        src->release(src);
        lset_err(l, "limit: out of memory");
        return -1;
    }
    for (size_t i = 0; i < n_take; ++i) keep[i] = 1;

    struct ArrowArray **kids = calloc(l->n_cols, sizeof *kids);
    if (!kids) {
        free(keep); src->release(src);
        lset_err(l, "limit: out of memory");
        return -1;
    }
    int build_failed = 0;
    for (size_t c = 0; c < l->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) { build_failed = 1; break; }
        int crc = (l->col_fmts[c] == 'l')
            ? betl_tx_build_int64_filtered(kids[c], src->children[c],
                                           keep, length, n_take)
            : betl_tx_build_utf8_filtered (kids[c], src->children[c],
                                           keep, length, n_take);
        if (crc != 0) { build_failed = 1; break; }
    }
    if (build_failed) {
        for (size_t c = 0; c < l->n_cols; ++c) {
            if (kids[c]) {
                if (kids[c]->release) kids[c]->release(kids[c]);
                free(kids[c]);
            }
        }
        free(kids); free(keep); src->release(src);
        lset_err(l, "limit: failed to slice cusp batch");
        return -1;
    }
    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < l->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids); free(keep); src->release(src);
        lset_err(l, "limit: out of memory");
        return -1;
    }
    outer[0] = NULL;
    out->length     = (int64_t)n_take;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)l->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;
    free(keep);
    src->release(src);
    return 0;
}

/* ============================================================== *
 *  Stream — get_schema + get_next                                  *
 * ============================================================== */

static int lim_get_schema(struct ArrowArrayStream *st,
                          struct ArrowSchema *out) {
    LimitState *l = st->private_data;
    if (!l || !l->have_input) return EINVAL;
    return l->input.get_schema(&l->input, out) == 0 ? 0 : EIO;
}

static int lim_get_next(struct ArrowArrayStream *st,
                        struct ArrowArray *out) {
    LimitState *l = st->private_data;
    memset(out, 0, sizeof *out);
    if (!l) return EINVAL;
    if (limit_resolve_schema(l) != 0) return EIO;

    int64_t remaining = l->n_max - l->n_emitted;
    if (remaining <= 0) return 0;     /* cap reached → end-of-stream */

    struct ArrowArray batch = {0};
    if (l->input.get_next(&l->input, &batch) != 0) {
        const char *e = l->input.get_last_error
            ? l->input.get_last_error(&l->input) : NULL;
        lset_err(l, "limit: upstream get_next failed: %s",
                 e ? e : "(no detail)");
        return EIO;
    }
    if (!batch.release) return 0;     /* upstream EOF before cap */

    if (batch.length <= remaining) {
        /* Whole batch fits — forward unchanged. The buffers and
         * release callback transfer through `out`; the caller becomes
         * the new owner. */
        l->n_emitted += batch.length;
        *out = batch;
        return 0;
    }

    /* Cusp batch: slice down to `remaining` rows. */
    size_t n_take = (size_t)remaining;
    if (slice_batch(l, &batch, n_take, out) != 0) return EIO;
    l->n_emitted += (int64_t)n_take;
    return 0;
}

static const char *lim_get_last_error(struct ArrowArrayStream *st) {
    LimitState *l = st->private_data;
    return (l && l->last_err[0]) ? l->last_err : NULL;
}

static void lim_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release = NULL;
}

static int limit_attach_output(void *state, int port,
                               struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = lim_get_schema;
    out->get_next       = lim_get_next;
    out->get_last_error = lim_get_last_error;
    out->release        = lim_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef limit_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to limit" },
};
static const BetlPortDef limit_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "first n rows" },
};

static const BetlComponentDef limit_components[] = {
    { .name               = "limit",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = limit_inputs,
      .input_count        = 1,
      .outputs            = limit_outputs,
      .output_count       = 1,
      .init               = limit_init,
      .destroy            = limit_destroy,
      .attach_input       = limit_attach_input,
      .attach_output      = limit_attach_output },
};

static const BetlProvider limit_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-limit",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = limit_components,
    .component_count = sizeof limit_components / sizeof limit_components[0],
};

int betl_tx_register_limit(BetlRegistry *r) {
    return betl_registry_register(r, &limit_provider, "<builtin:limit>");
}
