/* `multicast` TRANSFORM — fan one input stream out to N identical outputs.
 *
 * Config:
 *   taps: [csv, db, log]    required, non-empty, max 16 names
 *
 * Each downstream step references a specific tap by name:
 *
 *   pipeline:
 *     - id: fanout
 *       type: multicast
 *       from: source
 *       taps: [a, b, c]
 *     - id: csv_sink
 *       from: fanout:a
 *     - id: db_sink
 *       from: fanout:b
 *     - id: log
 *       from: fanout:c
 *
 * Why a separate transform rather than letting each downstream just
 * reference `from: source` directly: betl's port-ownership model
 * gives each output port a single consumer (attach_input transfers
 * ownership of the ArrowArrayStream). Multicast is the supported way
 * to legitimately route the *same* rows to multiple downstreams —
 * which is also exactly what SSIS' Microsoft.Multicast component
 * does. (See providers/betl-dtsx2yaml: Mappers/Multicast.cs.)
 *
 * Memory model:
 *   Each upstream batch becomes one `SharedBatch` with refcount =
 *   n_taps. We push a node referring to it onto every tap's FIFO
 *   queue. When a tap's downstream releases its handed-out batch, we
 *   decrement refcount; when refcount hits 0, the upstream's release
 *   actually runs. Unconsumed-at-shutdown batches are drained in
 *   mc_destroy.
 *
 *   We re-use the upstream's buffer / children pointers verbatim
 *   (zero-copy fan-out); only the surface ArrowArray + private_data
 *   pointer differ between the views handed to each tap.
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

#define MAX_OUTPUT_PORTS 16

/* ============================================================== *
 *  Shared-batch refcounting                                        *
 * ============================================================== */

typedef struct {
    /* The real ArrowArray as received from upstream. We hand out
     * shallow views of this struct to downstream consumers; only the
     * release callback in the view differs. */
    struct ArrowArray real;
    /* Number of outstanding references: queued + consumer-owned. */
    int refcount;
} SharedBatch;

typedef struct BatchNode {
    SharedBatch      *shared;
    struct BatchNode *next;
} BatchNode;

/* ============================================================== *
 *  Per-tap port state                                              *
 * ============================================================== */

typedef struct {
    char *name;
    BatchNode *q_head;
    BatchNode *q_tail;
    struct {
        void   *parent;        /* McState * */
        size_t  port_idx;
    } port_handle;
} McTap;

typedef struct {
    BetlContext *ctx;

    struct ArrowArrayStream input;
    int                     have_input;

    McTap  *taps;
    size_t  n_taps;

    int     upstream_eof;

    char    last_err[256];
} McState;

static void mset_err(McState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct {
    McState *s;
    int      err;
} TapsCtx;

static int taps_visit(const char *value, size_t value_len, void *user) {
    TapsCtx *tc = user;
    McState *s = tc->s;

    /* Each item must be a JSON string. */
    char *buf = malloc(value_len + 1);
    if (!buf) { mset_err(s, "multicast: out of memory"); tc->err = 1; return -1; }
    memcpy(buf, value, value_len);
    buf[value_len] = '\0';

    char *name = NULL;
    if (betl_tx_json_decode_str(buf, &name) != 0 || !name) {
        mset_err(s, "multicast: each `taps:` entry must be a string");
        free(buf); free(name); tc->err = 1; return -1;
    }
    free(buf);

    if (s->n_taps == MAX_OUTPUT_PORTS) {
        mset_err(s, "multicast: more than %d taps requested", MAX_OUTPUT_PORTS);
        free(name); tc->err = 1; return -1;
    }
    /* Dup-name detection — multicast taps share an output port namespace
     * so collisions would silently misroute downstream consumers. */
    for (size_t i = 0; i < s->n_taps; ++i) {
        if (strcmp(s->taps[i].name, name) == 0) {
            mset_err(s, "multicast: duplicate tap name '%s'", name);
            free(name); tc->err = 1; return -1;
        }
    }
    McTap *t = &s->taps[s->n_taps];
    memset(t, 0, sizeof *t);
    t->name = name;
    t->port_handle.parent   = s;
    t->port_handle.port_idx = s->n_taps;
    ++s->n_taps;
    return 0;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int mc_init(BetlContext *ctx, const char *cfg, void **state) {
    McState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->taps = calloc(MAX_OUTPUT_PORTS, sizeof *s->taps);
    if (!s->taps) { free(s); return BETL_ERR_INTERNAL; }

    cfg = cfg ? cfg : "{}";
    const char *cp = betl_tx_json_value_after(cfg, "taps");
    if (!cp || *cp != '[') {
        mset_err(s, "multicast: `taps:` (a non-empty list of names) is required");
        free(s->taps); free(s);
        return BETL_ERR_INVALID;
    }
    TapsCtx tc = { .s = s, .err = 0 };
    if (betl_tx_json_walk_array(cp, taps_visit, &tc) != 0 || tc.err
        || s->n_taps == 0)
    {
        if (s->last_err[0] == '\0') {
            mset_err(s, "multicast: `taps:` list is empty or malformed");
        }
        for (size_t i = 0; i < s->n_taps; ++i) free(s->taps[i].name);
        free(s->taps); free(s);
        return BETL_ERR_INVALID;
    }

    *state = s;
    return BETL_OK;
}

static void shared_release(SharedBatch *sb) {
    if (--sb->refcount == 0) {
        if (sb->real.release) sb->real.release(&sb->real);
        free(sb);
    }
}

static void queue_drain(McTap *t) {
    while (t->q_head) {
        BatchNode *n = t->q_head;
        t->q_head = n->next;
        shared_release(n->shared);
        free(n);
    }
    t->q_tail = NULL;
}

static void mc_destroy(void *state) {
    if (!state) return;
    McState *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    for (size_t i = 0; i < s->n_taps; ++i) {
        free(s->taps[i].name);
        queue_drain(&s->taps[i]);
    }
    free(s->taps);
    free(s);
}

static int mc_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    McState *s = state;
    s->input = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* ============================================================== *
 *  output_port_index callback                                      *
 * ============================================================== */

static int mc_output_port_index(void *state, const char *name) {
    McState *s = state;
    if (!name) return -1;
    for (size_t i = 0; i < s->n_taps; ++i) {
        if (strcmp(s->taps[i].name, name) == 0) return (int)i;
    }
    return -1;
}

/* ============================================================== *
 *  Pull-and-fan-out                                                *
 * ============================================================== */

static int mc_pull_one(McState *s) {
    if (s->upstream_eof) return 0;
    struct ArrowArray batch = {0};
    if (s->input.get_next(&s->input, &batch) != 0) {
        const char *e = s->input.get_last_error
            ? s->input.get_last_error(&s->input) : NULL;
        mset_err(s, "multicast: upstream get_next failed: %s",
                 e ? e : "(no detail)");
        return -1;
    }
    if (!batch.release) { s->upstream_eof = 1; return 0; }

    /* mc_init guarantees n_taps > 0 (`taps:` list is required); guard
     * the analyzer's leak path here too — calloc'ing sb in a 0-tap
     * universe would leave it orphaned. */
    if (s->n_taps == 0) {
        batch.release(&batch);
        return 0;
    }

    SharedBatch *sb = calloc(1, sizeof *sb);
    if (!sb) {
        batch.release(&batch);
        mset_err(s, "multicast: out of memory");
        return -1;
    }
    sb->real = batch;
    sb->refcount = (int)s->n_taps;

    /* Push a node referencing sb onto every tap. On OOM partway,
     * collapse refcount to whatever we did enqueue and release the
     * shared batch outright if nothing got queued. (The cleaner
     * single-call shape avoids the multi-call-after-free pattern
     * clang-analyzer rightly flags.) */
    for (size_t i = 0; i < s->n_taps; ++i) {
        BatchNode *n = calloc(1, sizeof *n);
        if (!n) {
            mset_err(s, "multicast: out of memory");
            sb->refcount = (int)i;       /* only `i` queued nodes hold refs */
            if (sb->refcount == 0) {
                if (sb->real.release) sb->real.release(&sb->real);
                free(sb);
            }
            return -1;
        }
        n->shared = sb;
        McTap *t = &s->taps[i];
        if (t->q_tail) { t->q_tail->next = n; t->q_tail = n; }
        else           { t->q_head = t->q_tail = n; }
    }
    return 0;
}

/* ============================================================== *
 *  Per-tap output stream                                           *
 * ============================================================== */

static int port_get_schema(struct ArrowArrayStream *st,
                           struct ArrowSchema *out) {
    McTap *t = st->private_data;
    if (!t) return EINVAL;
    McState *s = t->port_handle.parent;
    if (!s || !s->have_input) return EINVAL;
    return s->input.get_schema(&s->input, out) == 0 ? 0 : EIO;
}

static void consumer_release(struct ArrowArray *out) {
    SharedBatch *sb = out->private_data;
    if (!sb) return;
    /* Disconnect the view (its buffers/children pointers alias the
     * shared batch — we must NOT free them here). */
    out->private_data = NULL;
    out->release      = NULL;
    out->buffers      = NULL;
    out->children     = NULL;
    out->n_buffers    = 0;
    out->n_children   = 0;
    shared_release(sb);
}

static int port_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    McTap *t = st->private_data;
    memset(out, 0, sizeof *out);
    if (!t) return EINVAL;
    McState *s = t->port_handle.parent;
    if (!s) return EINVAL;

    while (!t->q_head && !s->upstream_eof) {
        if (mc_pull_one(s) != 0) return EIO;
    }
    if (!t->q_head) return 0;       /* clean EOF for this tap */

    BatchNode *n = t->q_head;
    t->q_head = n->next;
    if (!t->q_head) t->q_tail = NULL;

    SharedBatch *sb = n->shared;
    free(n);

    /* Hand the consumer a shallow view. We copy every field, then
     * swap out release + private_data so our consumer_release
     * decrements the shared refcount instead of freeing upstream
     * memory. */
    *out = sb->real;
    out->release      = consumer_release;
    out->private_data = sb;
    return 0;
}

static const char *port_get_last_error(struct ArrowArrayStream *st) {
    McTap *t = st->private_data;
    if (!t) return NULL;
    McState *s = t->port_handle.parent;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void port_release(struct ArrowArrayStream *st) {
    /* Disconnect the view; the per-tap queue (and any outstanding
     * shared batches) is owned by McState and cleaned by mc_destroy. */
    st->private_data = NULL;
    st->release      = NULL;
}

static int mc_attach_output(void *state, int port,
                            struct ArrowArrayStream *out) {
    McState *s = state;
    if (port < 0 || (size_t)port >= s->n_taps) {
        mset_err(s, "multicast: port %d out of range (have %zu)",
                 port, s->n_taps);
        return BETL_ERR_INVALID;
    }
    out->get_schema     = port_get_schema;
    out->get_next       = port_get_next;
    out->get_last_error = port_get_last_error;
    out->release        = port_release;
    out->private_data   = &s->taps[port];
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef mc_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to fan out" },
};
static const BetlPortDef mc_outputs[MAX_OUTPUT_PORTS] = {
    { .name = "tap_0",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_1",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_2",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_3",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_4",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_5",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_6",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_7",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_8",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_9",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_10", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_11", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_12", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_13", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_14", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "tap_15", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
};

static const BetlComponentDef mc_components[] = {
    { .name               = "multicast",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = mc_inputs,
      .input_count        = 1,
      .outputs            = mc_outputs,
      .output_count       = MAX_OUTPUT_PORTS,
      .init               = mc_init,
      .destroy            = mc_destroy,
      .attach_input       = mc_attach_input,
      .attach_output      = mc_attach_output,
      .output_port_index  = mc_output_port_index },
};

static const BetlProvider mc_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-multicast",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = mc_components,
    .component_count = sizeof mc_components / sizeof mc_components[0],
};

int betl_tx_register_multicast(BetlRegistry *r) {
    return betl_registry_register(r, &mc_provider, "<builtin:multicast>");
}
