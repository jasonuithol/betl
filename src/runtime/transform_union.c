/* `union` TRANSFORM (vertical concat of N input streams).
 *
 * v0.1: every input must produce the same schema. We don't yet do any
 * column-by-name reconciliation; matching is positional + name-equal +
 * format-equal at first get_schema time. Output is the input streams
 * drained in order: input 0 first, then input 1, etc., until all are
 * exhausted. Empty inputs are skipped naturally. */

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

    struct ArrowArrayStream *inputs;
    size_t                   n_inputs;
    size_t                   cap_inputs;

    /* Drain pointer — index of the input we're currently reading from.
     * Advanced when an input's get_next signals end-of-stream. */
    size_t   cur;

    /* Lazy schema verification. Set on the first get_schema call. */
    int      verified;

    char     last_err[256];
} UnionState;

static void uset_err(UnionState *u, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->last_err, sizeof u->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(u->ctx, "%s", u->last_err);
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int union_init(BetlContext *ctx, const char *cfg, void **state) {
    (void)cfg;
    UnionState *u = calloc(1, sizeof *u);
    if (!u) return BETL_ERR_INTERNAL;
    u->ctx = ctx;
    *state = u;
    return BETL_OK;
}

static void union_destroy(void *state) {
    if (!state) return;
    UnionState *u = state;
    for (size_t i = 0; i < u->n_inputs; ++i) {
        if (u->inputs[i].release) u->inputs[i].release(&u->inputs[i]);
    }
    free(u->inputs);
    free(u);
}

static int union_attach_input(void *state, int port,
                              struct ArrowArrayStream *in) {
    (void)port;
    UnionState *u = state;
    if (u->n_inputs == u->cap_inputs) {
        size_t nc = u->cap_inputs ? u->cap_inputs * 2 : 4;
        struct ArrowArrayStream *p = realloc(u->inputs, nc * sizeof *p);
        if (!p) return BETL_ERR_INTERNAL;
        u->inputs = p;
        u->cap_inputs = nc;
    }
    u->inputs[u->n_inputs++] = *in;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  Schema verification                                             *
 * ============================================================== */

static int schemas_match(const struct ArrowSchema *a,
                         const struct ArrowSchema *b) {
    if (!a->format || !b->format || strcmp(a->format, b->format) != 0) return 0;
    if (a->n_children != b->n_children) return 0;
    for (int64_t i = 0; i < a->n_children; ++i) {
        const struct ArrowSchema *ca = a->children[i];
        const struct ArrowSchema *cb = b->children[i];
        if (!ca || !cb) return 0;
        if (!ca->format || !cb->format
            || strcmp(ca->format, cb->format) != 0) return 0;
        const char *na = ca->name ? ca->name : "";
        const char *nb = cb->name ? cb->name : "";
        if (strcmp(na, nb) != 0) return 0;
    }
    return 1;
}

/* On the first get_schema call, fetch each input's schema and check
 * they all agree with input 0. Releases the per-input schemas; caller
 * still gets a fresh schema from input 0 by their own request. */
static int union_verify_schemas(UnionState *u) {
    if (u->verified) return 0;
    if (u->n_inputs == 0) {
        uset_err(u, "union: requires at least one input");
        return -1;
    }
    struct ArrowSchema sch0 = {0};
    if (u->inputs[0].get_schema(&u->inputs[0], &sch0) != 0) {
        uset_err(u, "union: input 0 get_schema failed");
        return -1;
    }
    for (size_t i = 1; i < u->n_inputs; ++i) {
        struct ArrowSchema schi = {0};
        if (u->inputs[i].get_schema(&u->inputs[i], &schi) != 0) {
            uset_err(u, "union: input %zu get_schema failed", i);
            if (sch0.release) sch0.release(&sch0);
            return -1;
        }
        if (!schemas_match(&sch0, &schi)) {
            uset_err(u, "union: input %zu schema differs from input 0 "
                        "(every input must have the same column names + types)", i);
            if (schi.release) schi.release(&schi);
            if (sch0.release) sch0.release(&sch0);
            return -1;
        }
        if (schi.release) schi.release(&schi);
    }
    if (sch0.release) sch0.release(&sch0);
    u->verified = 1;
    return 0;
}

/* ============================================================== *
 *  Output stream                                                   *
 * ============================================================== */

static int union_get_schema(struct ArrowArrayStream *st,
                            struct ArrowSchema *out) {
    UnionState *u = st->private_data;
    memset(out, 0, sizeof *out);
    if (!u) return EINVAL;
    if (union_verify_schemas(u) != 0) return EIO;
    /* Defer to input 0's get_schema — the caller takes ownership of
     * whatever it returns (its release callback is the input's). */
    return u->inputs[0].get_schema(&u->inputs[0], out) == 0 ? 0 : EIO;
}

static int union_get_next(struct ArrowArrayStream *st,
                          struct ArrowArray *out) {
    UnionState *u = st->private_data;
    memset(out, 0, sizeof *out);
    if (!u) return EINVAL;
    if (union_verify_schemas(u) != 0) return EIO;

    while (u->cur < u->n_inputs) {
        struct ArrowArray batch = {0};
        if (u->inputs[u->cur].get_next(&u->inputs[u->cur], &batch) != 0) {
            const char *e = u->inputs[u->cur].get_last_error
                ? u->inputs[u->cur].get_last_error(&u->inputs[u->cur])
                : NULL;
            uset_err(u, "union: input %zu get_next failed: %s",
                     u->cur, e ? e : "(no detail)");
            return EIO;
        }
        if (!batch.release) {
            /* End-of-stream for this input — advance to the next. */
            ++u->cur;
            continue;
        }
        /* Forward the batch verbatim. The buffers + release callback
         * pointed at by `batch` carry over into *out; the caller is
         * the new owner and will eventually call out->release(out). */
        *out = batch;
        return 0;
    }
    /* All inputs drained — leave *out's release as NULL to signal EOF. */
    return 0;
}

static const char *union_get_last_error(struct ArrowArrayStream *st) {
    UnionState *u = st->private_data;
    return (u && u->last_err[0]) ? u->last_err : NULL;
}

static void union_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release = NULL;
}

static int union_attach_output(void *state, int port,
                               struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = union_get_schema;
    out->get_next       = union_get_next;
    out->get_last_error = union_get_last_error;
    out->release        = union_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

/* `input_count` here is the prototype port spec; the executor uses
 * the YAML's `from: [...]` length to drive how many attach_input calls
 * we get. A YAML with `from: [a, b, c]` produces three calls with
 * port indices 0, 1, 2 — union_attach_input grows its inputs[] array
 * to fit. */
static const BetlPortDef union_inputs[] = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows from one of the upstream streams" },
};
static const BetlPortDef union_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "concatenation of all input streams in order" },
};

static const BetlComponentDef union_components[] = {
    { .name               = "union",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = union_inputs,
      .input_count        = 1,
      .outputs            = union_outputs,
      .output_count       = 1,
      .init               = union_init,
      .destroy            = union_destroy,
      .attach_input       = union_attach_input,
      .attach_output      = union_attach_output },
};

static const BetlProvider union_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-union",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = union_components,
    .component_count = sizeof union_components / sizeof union_components[0],
};

int betl_tx_register_union(BetlRegistry *r) {
    return betl_registry_register(r, &union_provider, "<builtin:union>");
}
