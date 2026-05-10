/* `conditional_split` TRANSFORM — route each input row to one of N
 * output ports based on per-port predicates.
 *
 * Config:
 *   cases:                    required, ordered list
 *     - { name: hot,  where: "row.priority == 'hot'" }
 *     - { name: warm, where: "row.priority == 'warm'" }
 *     - { name: cold, where: "row.priority == 'cold'" }
 *   default: rest             optional — bucket for non-matching rows.
 *                             If absent, non-matching rows are dropped.
 *
 * Ports:
 *   The component declares MAX_OUTPUT_PORTS slots statically; the
 *   user's case names map to indices via the `output_port_index`
 *   callback. A YAML reference of the form `from: split:hot` resolves
 *   to whichever port_idx the case `hot` was assigned at init.
 *
 * Routing semantics:
 *   - Earlier cases win: a row that satisfies cases[0] does NOT also
 *     get evaluated for cases[1..]. (Same as SQL CASE / SSIS Conditional
 *     Split.)
 *   - Predicates are compiled lazily at first batch via the §7 expr
 *     engine. Each case's `where:` accepts the same shorthand and
 *     full forms as `filter`'s where.
 *
 * Memory model:
 *   - One pull from upstream produces a sliced batch per case that
 *     received any rows. Sliced batches go onto per-case FIFO queues.
 *   - When ANY consumer asks for a batch, we drain into queues until
 *     either that consumer's queue has something OR upstream EOF. The
 *     other consumers may end up holding queued batches in the meantime
 *     — bounded by upstream batch size × n_cases. */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/transforms_internal.h"

/* Hard cap — typical conditional-split has 2–10 branches; 16 is
 * comfortable headroom and keeps StepRunner.outputs[] modest. */
#define MAX_OUTPUT_PORTS 16

/* ============================================================== *
 *  Per-case state                                                  *
 * ============================================================== */

typedef struct BatchNode {
    struct ArrowArray  batch;
    struct BatchNode  *next;
} BatchNode;

typedef struct {
    /* Configured */
    char *name;
    char *lang;       /* for predicate; NULL on the default case */
    char *expr_src;   /* NULL on the default case */

    /* Compiled */
    const BetlExprEngine *engine;
    void                 *engine_handle;
    int                   handle_ready;

    /* Per-case queue of sliced batches awaiting a downstream pull. */
    BatchNode *q_head;
    BatchNode *q_tail;

    /* The slot this case fills via attach_output. The downstream
     * consumer reaches us through stream.private_data → port_handle →
     * (parent, case_idx). */
    struct {
        void   *parent;        /* SplitState * */
        size_t  case_idx;
    } port_handle;
} SplitCase;

typedef struct {
    BetlContext *ctx;

    struct ArrowArrayStream input;
    int                     have_input;

    SplitCase *cases;
    size_t     n_cases;          /* includes a default case if configured */
    int        has_default;      /* if 1, cases[n_cases-1] is the default */

    /* Cached schema from the first call. */
    int          schema_resolved;
    size_t       n_cols;
    char        *col_fmts;       /* per-col 'l' or 'u' */

    int          upstream_eof;

    char         last_err[256];
} SplitState;

static void sset_err(SplitState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

/* Each entry in cases: must be a map containing `name` and `where`.
 * `where` accepts the same shorthand / full form as filter. */
typedef struct {
    SplitState *s;
    int         err;
} CasesCtx;

/* Parse one case object (supplied as a JSON substring). On success,
 * appends a SplitCase to s->cases and returns 0; on failure, returns
 * -1 with err set. */
static int parse_case_object(SplitState *s, const char *obj_json,
                             size_t obj_len) {
    /* Copy substring so json helpers see a NUL-terminated buffer. */
    char *buf = malloc(obj_len + 1);
    if (!buf) { sset_err(s, "conditional_split: out of memory"); return -1; }
    memcpy(buf, obj_json, obj_len);
    buf[obj_len] = '\0';

    char *name = NULL;
    if (betl_tx_json_string_at(buf, "name", &name) != 0 || !name) {
        sset_err(s, "conditional_split: each `cases:` entry needs a `name`");
        free(buf); free(name); return -1;
    }

    /* Predicate: try shorthand first (where: "string"), then the
     * structured {lang, expr|value} form. */
    char *lang = NULL, *expr = NULL;
    char *where_short = NULL;
    if (betl_tx_json_string_at(buf, "where", &where_short) == 0 && where_short) {
        lang = strdup("lua");
        expr = where_short;
        if (!lang) { free(buf); free(name); free(expr); return -1; }
    } else {
        free(where_short);
        betl_tx_json_string_at(buf, "lang", &lang);
        if (betl_tx_json_string_at(buf, "expr", &expr) != 0 || !expr) {
            free(expr); expr = NULL;
            betl_tx_json_value_to_string(buf, "value", &expr);
        }
        if (!lang || !expr) {
            sset_err(s, "conditional_split: case '%s' needs a `where:` "
                        "(string shorthand or {lang, expr|value})", name);
            free(buf); free(name); free(lang); free(expr);
            return -1;
        }
    }
    free(buf);

    SplitCase *grow = realloc(s->cases, (s->n_cases + 1) * sizeof *grow);
    if (!grow) {
        sset_err(s, "conditional_split: out of memory");
        free(name); free(lang); free(expr); return -1;
    }
    s->cases = grow;
    SplitCase *c = &s->cases[s->n_cases];
    memset(c, 0, sizeof *c);
    c->name     = name;
    c->lang     = lang;
    c->expr_src = expr;
    c->port_handle.parent   = s;
    c->port_handle.case_idx = s->n_cases;
    ++s->n_cases;
    return 0;
}

static int cases_visit(const char *value, size_t value_len, void *user) {
    CasesCtx *c = user;
    if (value_len == 0 || value[0] != '{') {
        sset_err(c->s, "conditional_split: each `cases:` entry must be a map");
        c->err = 1; return -1;
    }
    if (parse_case_object(c->s, value, value_len) != 0) {
        c->err = 1; return -1;
    }
    return 0;
}

static int split_init(BetlContext *ctx, const char *cfg, void **state) {
    SplitState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    const char *cp = betl_tx_json_value_after(cfg, "cases");
    if (!cp || *cp != '[') {
        sset_err(s, "conditional_split: `cases:` (a list) is required");
        free(s);
        return BETL_ERR_INVALID;
    }
    CasesCtx cc = { .s = s, .err = 0 };
    if (betl_tx_json_walk_array(cp, cases_visit, &cc) != 0 || cc.err
        || s->n_cases == 0)
    {
        if (s->last_err[0] == '\0') {
            sset_err(s, "conditional_split: `cases:` list is empty or malformed");
        }
        for (size_t i = 0; i < s->n_cases; ++i) {
            free(s->cases[i].name); free(s->cases[i].lang);
            free(s->cases[i].expr_src);
        }
        free(s->cases);
        free(s);
        return BETL_ERR_INVALID;
    }

    /* Optional `default: <name>` — if present, append a case with no
     * predicate using that name. */
    char *default_name = NULL;
    if (betl_tx_json_string_at(cfg, "default", &default_name) == 0
        && default_name)
    {
        SplitCase *grow = realloc(s->cases,
                                  (s->n_cases + 1) * sizeof *grow);
        if (!grow) {
            free(default_name);
            for (size_t i = 0; i < s->n_cases; ++i) {
                free(s->cases[i].name); free(s->cases[i].lang);
                free(s->cases[i].expr_src);
            }
            free(s->cases); free(s);
            return BETL_ERR_INTERNAL;
        }
        s->cases = grow;
        SplitCase *c = &s->cases[s->n_cases];
        memset(c, 0, sizeof *c);
        c->name     = default_name;
        c->lang     = NULL;
        c->expr_src = NULL;
        c->port_handle.parent   = s;
        c->port_handle.case_idx = s->n_cases;
        ++s->n_cases;
        s->has_default = 1;
    } else {
        free(default_name);
    }

    if (s->n_cases > MAX_OUTPUT_PORTS) {
        sset_err(s, "conditional_split: %zu output ports requested, "
                    "max %d", s->n_cases, MAX_OUTPUT_PORTS);
        for (size_t i = 0; i < s->n_cases; ++i) {
            free(s->cases[i].name); free(s->cases[i].lang);
            free(s->cases[i].expr_src);
        }
        free(s->cases); free(s);
        return BETL_ERR_INVALID;
    }

    *state = s;
    return BETL_OK;
}

static int split_attach_input(void *state, int port,
                              struct ArrowArrayStream *in) {
    (void)port;
    SplitState *s = state;
    s->input = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void queue_drain(SplitCase *c) {
    while (c->q_head) {
        BatchNode *n = c->q_head;
        c->q_head = n->next;
        if (n->batch.release) n->batch.release(&n->batch);
        free(n);
    }
    c->q_tail = NULL;
}

static void split_destroy(void *state) {
    if (!state) return;
    SplitState *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    for (size_t i = 0; i < s->n_cases; ++i) {
        SplitCase *c = &s->cases[i];
        if (c->handle_ready && c->engine && c->engine_handle) {
            c->engine->release(c->engine_handle);
        }
        free(c->name); free(c->lang); free(c->expr_src);
        queue_drain(c);
    }
    free(s->cases);
    free(s->col_fmts);
    free(s);
}

/* ============================================================== *
 *  output_port_index callback                                      *
 * ============================================================== */

static int split_output_port_index(void *state, const char *name) {
    SplitState *s = state;
    if (!name) return -1;
    for (size_t i = 0; i < s->n_cases; ++i) {
        if (strcmp(s->cases[i].name, name) == 0) return (int)i;
    }
    return -1;
}

/* ============================================================== *
 *  Schema + predicate compilation                                  *
 * ============================================================== */

static int split_resolve_schema(SplitState *s) {
    if (s->schema_resolved) return 0;
    if (!s->have_input || !s->input.get_schema) {
        sset_err(s, "conditional_split: input has no get_schema");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (s->input.get_schema(&s->input, &sch) != 0) {
        sset_err(s, "conditional_split: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        sset_err(s, "conditional_split: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    s->col_fmts = calloc(n, 1);
    if (!s->col_fmts) {
        sset_err(s, "conditional_split: out of memory");
        goto done;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            sset_err(s, "conditional_split: column has unsupported "
                        "format '%s'", fmt ? fmt : "(none)");
            goto done;
        }
        s->col_fmts[i] = fmt[0];
    }
    s->n_cols = n;

    /* Compile each case's predicate (default has none). */
    for (size_t i = 0; i < s->n_cases; ++i) {
        SplitCase *c = &s->cases[i];
        if (!c->expr_src) continue;     /* default case */
        c->engine = betl_get_expr_engine(s->ctx, c->lang);
        if (!c->engine) {
            sset_err(s, "conditional_split: case '%s': no expression engine "
                        "for lang '%s'", c->name, c->lang);
            goto done;
        }
        if (c->engine->compile(s->ctx, c->expr_src, &sch,
                               &c->engine_handle) != BETL_OK)
        {
            sset_err(s, "conditional_split: case '%s': compile failed",
                     c->name);
            goto done;
        }
        c->handle_ready = 1;
    }

    s->schema_resolved = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

/* ============================================================== *
 *  Routing                                                         *
 * ============================================================== */

/* Build a sliced batch from `src` containing only rows where keep[i]
 * != 0; push it onto the case's queue. Returns 0 on success, -1 on
 * OOM (with an error already set). */
static int push_slice(SplitState *s, SplitCase *c,
                      const struct ArrowArray *src,
                      const uint8_t *keep, size_t length, size_t n_kept) {
    BatchNode *node = calloc(1, sizeof *node);
    if (!node) { sset_err(s, "conditional_split: out of memory"); return -1; }

    struct ArrowArray **kids = calloc(s->n_cols, sizeof *kids);
    if (!kids) {
        free(node);
        sset_err(s, "conditional_split: out of memory");
        return -1;
    }
    int build_failed = 0;
    for (size_t col = 0; col < s->n_cols; ++col) {
        kids[col] = calloc(1, sizeof **kids);
        if (!kids[col]) { build_failed = 1; break; }
        int crc = (s->col_fmts[col] == 'l')
            ? betl_tx_build_int64_filtered(kids[col], src->children[col],
                                           keep, length, n_kept)
            : betl_tx_build_utf8_filtered (kids[col], src->children[col],
                                           keep, length, n_kept);
        if (crc != 0) { build_failed = 1; break; }
    }
    if (build_failed) {
        for (size_t col = 0; col < s->n_cols; ++col) {
            if (kids[col]) {
                if (kids[col]->release) kids[col]->release(kids[col]);
                free(kids[col]);
            }
        }
        free(kids); free(node);
        sset_err(s, "conditional_split: failed to slice batch for case '%s'",
                 c->name);
        return -1;
    }
    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t col = 0; col < s->n_cols; ++col) {
            if (kids[col]->release) kids[col]->release(kids[col]);
            free(kids[col]);
        }
        free(kids); free(node);
        sset_err(s, "conditional_split: out of memory");
        return -1;
    }
    outer[0] = NULL;
    node->batch.length     = (int64_t)n_kept;
    node->batch.null_count = 0;
    node->batch.n_buffers  = 1;
    node->batch.n_children = (int64_t)s->n_cols;
    node->batch.buffers    = outer;
    node->batch.children   = kids;
    node->batch.release    = betl_tx_release_struct;

    if (c->q_tail) {
        c->q_tail->next = node;
        c->q_tail = node;
    } else {
        c->q_head = c->q_tail = node;
    }
    return 0;
}

/* Pull one batch from upstream and partition it across all cases. On
 * upstream EOF, sets s->upstream_eof = 1 and returns 0 with no work
 * done. Returns -1 on error (with last_err set). */
static int route_one_batch(SplitState *s) {
    if (s->upstream_eof) return 0;

    struct ArrowArray batch = {0};
    if (s->input.get_next(&s->input, &batch) != 0) {
        const char *e = s->input.get_last_error
            ? s->input.get_last_error(&s->input) : NULL;
        sset_err(s, "conditional_split: upstream get_next failed: %s",
                 e ? e : "(no detail)");
        return -1;
    }
    if (!batch.release) { s->upstream_eof = 1; return 0; }
    size_t length = (size_t)batch.length;
    if (length == 0) { batch.release(&batch); return 0; }

    /* Build per-case keep masks. Rules: a row goes to the FIRST case
     * whose predicate is true; if none and a default exists, it goes
     * there; else dropped. We allocate one mask per case (default
     * included) and a `taken` mask tracking rows already routed. */
    uint8_t *taken = calloc(length, 1);
    uint8_t **keep = calloc(s->n_cases, sizeof *keep);
    if (!taken || !keep) {
        free(taken); free(keep);
        batch.release(&batch);
        sset_err(s, "conditional_split: out of memory");
        return -1;
    }
    int oom = 0;
    for (size_t i = 0; i < s->n_cases; ++i) {
        keep[i] = calloc(length, 1);
        if (!keep[i]) { oom = 1; break; }
    }
    if (oom) {
        for (size_t i = 0; i < s->n_cases; ++i) free(keep[i]);
        free(keep); free(taken); batch.release(&batch);
        sset_err(s, "conditional_split: out of memory");
        return -1;
    }

    /* Evaluate each predicate against the whole batch (the engine
     * returns a bool[] aligned to length). For each row, the first
     * case whose bool is true claims the row. */
    for (size_t i = 0; i < s->n_cases; ++i) {
        SplitCase *c = &s->cases[i];
        if (!c->engine_handle) continue;        /* default — handled below */
        struct ArrowArray pred = {0};
        if (c->engine->evaluate(c->engine_handle, &batch, "b", &pred)
            != BETL_OK)
        {
            sset_err(s, "conditional_split: case '%s' evaluate failed",
                     c->name);
            goto fail_keep;
        }
        if (pred.length != (int64_t)length || pred.n_buffers < 2
            || !pred.buffers || !pred.buffers[1])
        {
            if (pred.release) pred.release(&pred);
            sset_err(s, "conditional_split: case '%s' returned malformed bool",
                     c->name);
            goto fail_keep;
        }
        const uint8_t *pv = pred.buffers[1];
        const uint8_t *pn = (pred.null_count > 0 && pred.n_buffers >= 1)
                            ? (const uint8_t *)pred.buffers[0] : NULL;
        for (size_t r = 0; r < length; ++r) {
            if (taken[r]) continue;
            int is_null = pn && !betl_tx_bit_at(pn, r);
            if (is_null) continue;          /* NULL predicate → skip */
            if (betl_tx_bit_at(pv, r)) {
                keep[i][r] = 1;
                taken[r] = 1;
            }
        }
        pred.release(&pred);
    }

    /* Default case: claim everything that nobody else claimed. */
    if (s->has_default) {
        size_t def_idx = s->n_cases - 1;
        for (size_t r = 0; r < length; ++r) {
            if (!taken[r]) { keep[def_idx][r] = 1; taken[r] = 1; }
        }
    }

    /* For each case that claimed at least one row, push a sliced batch
     * onto its queue. The keep[i] entries were all allocated above and
     * the loop earlier `goto fail_keep` if any failed; the explicit
     * NULL guard here is only to satisfy clang-analyzer's path tracker. */
    int rc_inner = 0;
    for (size_t i = 0; i < s->n_cases; ++i) {
        if (!keep[i]) continue;
        size_t n_kept = 0;
        for (size_t r = 0; r < length; ++r) n_kept += keep[i][r];
        if (n_kept == 0) continue;
        if (push_slice(s, &s->cases[i], &batch, keep[i], length, n_kept) != 0) {
            rc_inner = -1;
            break;
        }
    }

    for (size_t i = 0; i < s->n_cases; ++i) free(keep[i]);
    free(keep); free(taken);
    batch.release(&batch);
    return rc_inner;

fail_keep:
    for (size_t i = 0; i < s->n_cases; ++i) free(keep[i]);
    free(keep); free(taken);
    batch.release(&batch);
    return -1;
}

/* ============================================================== *
 *  Per-port output stream                                          *
 * ============================================================== */

static int port_get_schema(struct ArrowArrayStream *st,
                           struct ArrowSchema *out) {
    SplitCase *c = st->private_data;
    if (!c) return EINVAL;
    SplitState *s = c->port_handle.parent;
    if (!s || !s->have_input) return EINVAL;
    /* Defer to upstream — every output port has the same schema as
     * the input. */
    return s->input.get_schema(&s->input, out) == 0 ? 0 : EIO;
}

static int port_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    SplitCase *c = st->private_data;
    memset(out, 0, sizeof *out);
    if (!c) return EINVAL;
    SplitState *s = c->port_handle.parent;
    if (!s) return EINVAL;
    if (split_resolve_schema(s) != 0) return EIO;

    /* Drain upstream until this case has something to emit, OR upstream
     * is exhausted with this case's queue empty. */
    while (!c->q_head && !s->upstream_eof) {
        if (route_one_batch(s) != 0) return EIO;
    }
    if (!c->q_head) return 0;       /* clean EOF for this port */

    BatchNode *node = c->q_head;
    c->q_head = node->next;
    if (!c->q_head) c->q_tail = NULL;
    *out = node->batch;
    free(node);
    return 0;
}

static const char *port_get_last_error(struct ArrowArrayStream *st) {
    SplitCase *c = st->private_data;
    if (!c) return NULL;
    SplitState *s = c->port_handle.parent;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void port_release(struct ArrowArrayStream *st) {
    /* The actual case state is owned by SplitState; releasing the
     * stream just disconnects this view of it. */
    st->private_data = NULL;
    st->release = NULL;
}

static int split_attach_output(void *state, int port,
                               struct ArrowArrayStream *out) {
    SplitState *s = state;
    if (port < 0 || (size_t)port >= s->n_cases) {
        sset_err(s, "conditional_split: port %d out of range (have %zu)",
                 port, s->n_cases);
        return BETL_ERR_INVALID;
    }
    out->get_schema     = port_get_schema;
    out->get_next       = port_get_next;
    out->get_last_error = port_get_last_error;
    out->release        = port_release;
    out->private_data   = &s->cases[port];
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

/* MAX_OUTPUT_PORTS placeholder port descriptors. Names are formal and
 * never appear in YAML — users always reference cases by their config
 * names, which `output_port_index` resolves dynamically. */
static const BetlPortDef split_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to route" },
};
static const BetlPortDef split_outputs[MAX_OUTPUT_PORTS] = {
    { .name = "case_0",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_1",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_2",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_3",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_4",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_5",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_6",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_7",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_8",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_9",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_10", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_11", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_12", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_13", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_14", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
    { .name = "case_15", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "" },
};

static const BetlComponentDef split_components[] = {
    { .name               = "conditional_split",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = split_inputs,
      .input_count        = 1,
      .outputs            = split_outputs,
      .output_count       = MAX_OUTPUT_PORTS,
      .init               = split_init,
      .destroy            = split_destroy,
      .attach_input       = split_attach_input,
      .attach_output      = split_attach_output,
      .output_port_index  = split_output_port_index },
};

static const BetlProvider split_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-conditional_split",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = split_components,
    .component_count = sizeof split_components / sizeof split_components[0],
};

int betl_tx_register_split(BetlRegistry *r) {
    return betl_registry_register(r, &split_provider, "<builtin:conditional_split>");
}
