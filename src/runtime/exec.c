#include "runtime/exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "runtime/async_stream.h"
#include "runtime/context.h"
#include "runtime/substitute.h"

/* ============================================================== *
 *  Topological sort                                                *
 *                                                                  *
 *  Both the stage-DAG (`after:` edges) and the per-stage step-DAG  *
 *  (`from:` edges) get sorted with a tiny iterative Kahn's-style   *
 *  pass. Edges are resolved by string lookup since v0.1 graphs     *
 *  are small (≪ 50 nodes); a hashmap would be premature.            *
 * ============================================================== */

/* Generic edge resolver. `n` nodes, function `incoming(i, j_out, n_out)`
 * fills j_out[0..*n_out-1] with the indices of `i`'s incoming neighbours.
 * Writes the topo order into `order` (must be cap >= n). Returns 0 on
 * success, -1 if a cycle is detected (shouldn't happen — parser already
 * rejects cycles, but we double-check). */
typedef void (*incoming_fn)(int i, int *out, size_t *out_count, void *user);

static int topo_sort(int n, incoming_fn incoming, void *user, int *order) {
    if (n == 0) return 0;
    int *indeg = calloc((size_t)n, sizeof *indeg);
    int *adj   = NULL;
    int  produced = 0;
    if (!indeg) return -1;

    for (int i = 0; i < n; ++i) {
        int neigh[64]; size_t cnt = 0;
        incoming(i, neigh, &cnt, user);
        indeg[i] = (int)cnt;
    }

    int *queue = malloc((size_t)n * sizeof *queue);
    if (!queue) { free(indeg); return -1; }
    int qhead = 0, qtail = 0;
    for (int i = 0; i < n; ++i) if (indeg[i] == 0) queue[qtail++] = i;

    while (qhead < qtail) {
        int v = queue[qhead++];
        order[produced++] = v;
        /* For every node u, if u has v as an incoming neighbour,
         * decrement u's indeg. We re-scan because we don't keep
         * a forward adjacency list. n is small. */
        for (int u = 0; u < n; ++u) {
            int neigh[64]; size_t cnt = 0;
            incoming(u, neigh, &cnt, user);
            for (size_t k = 0; k < cnt; ++k) {
                if (neigh[k] == v) {
                    if (--indeg[u] == 0) queue[qtail++] = u;
                    break;
                }
            }
        }
    }

    free(indeg);
    free(queue);
    (void)adj;
    return (produced == n) ? 0 : -1;
}

/* ---- Stage-level edges (from `after:`) --------------------------
 *
 * `after:` is scoped — references resolve to siblings in the same
 * stage array (top-level or one foreach body). So the topo-sort
 * operates on a flat slice; recursion into foreach bodies happens
 * from the caller, not the sort. */

typedef struct {
    const BetlStage *arr;
    size_t           n;
} StageCtx;

static int stage_index_in(const BetlStage *arr, size_t n, const char *id) {
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(arr[i].id, id) == 0) return (int)i;
    }
    return -1;
}

static void stage_incoming(int i, int *out, size_t *out_count, void *user) {
    const StageCtx *c = user;
    const BetlStage *s = &c->arr[i];
    *out_count = 0;
    for (size_t k = 0; k < s->after_count; ++k) {
        int idx = stage_index_in(c->arr, c->n, s->after[k]);
        if (idx >= 0 && *out_count < 64) out[(*out_count)++] = idx;
    }
}

/* ---- Step-level edges (from `from:`) ---------------------------- */

typedef struct {
    const BetlStage *s;
} StepCtx;

static int step_index(const BetlStage *s, const char *id) {
    for (size_t i = 0; i < s->step_count; ++i) {
        if (strcmp(s->steps[i].id, id) == 0) return (int)i;
    }
    return -1;
}

/* Split a `from:` ref of the form "step_id" or "step_id:port_name".
 * Writes the step part into `step_buf` (NUL-terminated, must hold at
 * least 128 chars), and sets *port_name_out to the port suffix or NULL
 * if absent. Returns 0 on success, -1 on truncation. */
static int split_input_ref(const char *ref, char *step_buf,
                           const char **port_name_out) {
    const char *colon = strchr(ref, ':');
    if (!colon) {
        size_t len = strlen(ref);
        if (len >= 128) return -1;
        memcpy(step_buf, ref, len + 1);
        *port_name_out = NULL;
        return 0;
    }
    size_t step_len = (size_t)(colon - ref);
    if (step_len == 0 || step_len >= 128) return -1;
    memcpy(step_buf, ref, step_len);
    step_buf[step_len] = '\0';
    const char *port = colon + 1;
    if (*port == '\0') return -1;       /* "step:" with empty port */
    *port_name_out = port;
    return 0;
}

static int step_index_ref(const BetlStage *s, const char *ref) {
    char step_buf[128];
    const char *port = NULL;
    if (split_input_ref(ref, step_buf, &port) != 0) return -1;
    return step_index(s, step_buf);
}

static void step_incoming(int i, int *out, size_t *out_count, void *user) {
    const StepCtx *c = user;
    const BetlDataflowStep *step = &c->s->steps[i];
    *out_count = 0;
    for (size_t k = 0; k < step->input_count; ++k) {
        int idx = step_index_ref(c->s, step->inputs[k]);
        if (idx >= 0 && *out_count < 64) out[(*out_count)++] = idx;
    }
}

/* ============================================================== *
 *  Stage runners                                                   *
 * ============================================================== */

static int run_task_stage(BetlContext *ctx, BetlRegistry *reg,
                          const BetlStage *s) {
    const BetlComponentDef *def = betl_registry_find(reg, s->task_type);
    if (!def) {
        betl_set_error(ctx,
            "stage '%s': no provider registered for task type '%s'",
            s->id, s->task_type);
        return BETL_ERR_NOT_FOUND;
    }
    if (def->kind != BETL_KIND_TASK) {
        betl_set_error(ctx,
            "stage '%s': component '%s' is not a TASK (kind=%d)",
            s->id, s->task_type, (int)def->kind);
        return BETL_ERR_TYPE;
    }
    if (!def->init || !def->task_run || !def->destroy) {
        betl_set_error(ctx,
            "stage '%s': task component '%s' has incomplete vtable",
            s->id, s->task_type);
        return BETL_ERR_INVALID;
    }
    /* Resolve ${env.X}/${params.X} on the task config before init sees it. */
    char sub_err[256];
    char *resolved = betl_substitute_refs(
        s->task_config_json ? s->task_config_json : "{}",
        ctx, sub_err, sizeof sub_err);
    if (!resolved) {
        betl_set_error(ctx, "stage '%s': %s", s->id, sub_err);
        return BETL_ERR_INVALID;
    }

    void *state = NULL;
    int rc = def->init(ctx, resolved, &state);
    free(resolved);
    if (rc != BETL_OK) return rc;
    rc = def->task_run(state);
    def->destroy(state);
    return rc;
}

typedef struct {
    const BetlComponentDef *def;
    void                   *state;
    /* One stream slot per declared output port. attach_output() is
     * called lazily — only when a downstream step actually references
     * a given port via `from: step:port`. Slots stay zeroed for ports
     * with no consumer. After ownership is transferred to a downstream
     * (attach_input), output_attached[i] flips back to 0 so cleanup
     * doesn't double-release. */
    struct ArrowArrayStream *outputs;        /* [n_output_ports] */
    int                     *output_attached;/* [n_output_ports] */
    size_t                   n_output_ports;
    int                      destroyed;
} StepRunner;

static int run_dataflow_stage(BetlContext *ctx, BetlRegistry *reg,
                              const BetlStage *s) {
    if (s->step_count == 0) return BETL_OK;

    int *order = malloc(s->step_count * sizeof *order);
    if (!order) return BETL_ERR_INTERNAL;
    StepCtx sctx = { .s = s };
    if (topo_sort((int)s->step_count, step_incoming, &sctx, order) != 0) {
        free(order);
        betl_set_error(ctx, "stage '%s': cycle or unresolved input in steps",
                       s->id);
        return BETL_ERR_INVALID;
    }

    StepRunner *runners = calloc(s->step_count, sizeof *runners);
    if (!runners) { free(order); return BETL_ERR_INTERNAL; }

    int rc = BETL_OK;

    /* Resolve every component up-front so we don't half-init. */
    for (size_t i = 0; i < s->step_count; ++i) {
        const BetlDataflowStep *st = &s->steps[i];
        const BetlComponentDef *def = betl_registry_find(reg, st->type);
        if (!def) {
            betl_set_error(ctx,
                "stage '%s' step '%s': no provider for type '%s'",
                s->id, st->id, st->type);
            rc = BETL_ERR_NOT_FOUND;
            goto cleanup;
        }
        if (def->kind == BETL_KIND_TASK) {
            betl_set_error(ctx,
                "stage '%s' step '%s': component '%s' is a TASK; only TASKs at top level",
                s->id, st->id, st->type);
            rc = BETL_ERR_TYPE;
            goto cleanup;
        }
        runners[i].def = def;
    }

    /* Init every component (the lookup ensures init/destroy exist). */
    for (size_t i = 0; i < s->step_count; ++i) {
        const BetlDataflowStep *st = &s->steps[i];
        const BetlComponentDef *def = runners[i].def;
        if (!def->init || !def->destroy) {
            betl_set_error(ctx,
                "stage '%s' step '%s': component '%s' missing init/destroy",
                s->id, st->id, st->type);
            rc = BETL_ERR_INVALID;
            goto cleanup;
        }
        /* Pre-allocate per-output slots (lazily attached). For sinks
         * with output_count == 0 we leave outputs == NULL. */
        if (def->output_count > 0) {
            runners[i].outputs = calloc(def->output_count,
                                        sizeof *runners[i].outputs);
            runners[i].output_attached = calloc(def->output_count,
                                                sizeof *runners[i].output_attached);
            if (!runners[i].outputs || !runners[i].output_attached) {
                rc = BETL_ERR_INTERNAL;
                goto cleanup;
            }
            runners[i].n_output_ports = def->output_count;
        }
        char sub_err[256];
        char *resolved = betl_substitute_refs(
            st->config_json ? st->config_json : "{}",
            ctx, sub_err, sizeof sub_err);
        if (!resolved) {
            betl_set_error(ctx,
                "stage '%s' step '%s': %s", s->id, st->id, sub_err);
            rc = BETL_ERR_INVALID;
            goto cleanup;
        }
        rc = def->init(ctx, resolved, &runners[i].state);
        free(resolved);
        if (rc != BETL_OK) goto cleanup;
    }

    /* Walk in topo order: for each step, lazy-attach the specific
     * upstream port each input refers to, then forward it as input. */
    for (int oi = 0; oi < (int)s->step_count; ++oi) {
        int i = order[oi];
        const BetlDataflowStep *st = &s->steps[i];
        StepRunner *r = &runners[i];

        for (size_t pi = 0; pi < st->input_count; ++pi) {
            char step_buf[128];
            const char *port_name = NULL;
            if (split_input_ref(st->inputs[pi], step_buf, &port_name) != 0) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': malformed `from:` ref '%s'",
                    s->id, st->id, st->inputs[pi]);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }
            int up = step_index(s, step_buf);
            if (up < 0) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': upstream '%s' missing",
                    s->id, st->id, step_buf);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }
            StepRunner *upr = &runners[up];

            /* Resolve port_name → port_idx on the upstream. Empty /
             * absent → port 0 (the default first output). Otherwise
             * defer to the component's output_port_index callback if
             * set, else accept the name only when it equals the static
             * outputs[0].name. */
            int port_idx = 0;
            if (port_name) {
                if (upr->def->output_port_index) {
                    port_idx = upr->def->output_port_index(upr->state, port_name);
                } else if (upr->def->outputs && upr->def->output_count > 0
                           && upr->def->outputs[0].name
                           && strcmp(upr->def->outputs[0].name, port_name) == 0) {
                    port_idx = 0;
                } else {
                    port_idx = -1;
                }
                if (port_idx < 0) {
                    betl_set_error(ctx,
                        "stage '%s' step '%s': upstream '%s' has no output port '%s'",
                        s->id, st->id, s->steps[up].id, port_name);
                    rc = BETL_ERR_INVALID;
                    goto cleanup;
                }
            }
            if ((size_t)port_idx >= upr->n_output_ports) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': upstream '%s' port index %d out of range",
                    s->id, st->id, s->steps[up].id, port_idx);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }

            /* Lazy attach this specific port if not yet attached. A
             * port may be referenced by multiple downstream consumers
             * — but since attach_input transfers ownership, only one
             * consumer can own a given upstream port. We catch the
             * second consumer below with a clear error. */
            if (!upr->output_attached[port_idx]) {
                if (!upr->def->attach_output) {
                    betl_set_error(ctx,
                        "stage '%s' step '%s': upstream '%s' lacks attach_output",
                        s->id, st->id, s->steps[up].id);
                    rc = BETL_ERR_INVALID;
                    goto cleanup;
                }
                rc = upr->def->attach_output(upr->state, port_idx,
                                             &upr->outputs[port_idx]);
                if (rc != BETL_OK) goto cleanup;
                upr->output_attached[port_idx] = 1;
            } else if (upr->outputs[port_idx].private_data == NULL) {
                /* Already transferred to an earlier consumer. */
                betl_set_error(ctx,
                    "stage '%s' step '%s': upstream '%s' port '%s' already "
                    "consumed (each output port can have at most one consumer)",
                    s->id, st->id, s->steps[up].id,
                    port_name ? port_name : "<default>");
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }

            if (!r->def->attach_input) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': component lacks attach_input",
                    s->id, st->id);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }

            /* Pipeline parallelism: wrap each upstream-to-downstream
             * edge with an async_stream so the producer (upstream) runs
             * on its own thread. Wrapper takes ownership of the
             * upstream's slot; if attach_input fails, the wrapper's
             * release tears down the producer thread cleanly. */
            struct ArrowArrayStream wrapped = {0};
            struct ArrowArrayStream *to_pass = &upr->outputs[port_idx];
            int wrapped_in_use = 0;
            if (betl_pipeline_parallel_enabled()) {
                int wrap_rc = betl_async_wrap(
                    &upr->outputs[port_idx],
                    betl_pipeline_parallel_depth(), ctx, &wrapped);
                if (wrap_rc != BETL_OK) {
                    betl_set_error(ctx,
                        "stage '%s' step '%s': failed to wrap upstream "
                        "'%s' for pipeline parallelism",
                        s->id, st->id, s->steps[up].id);
                    rc = wrap_rc;
                    goto cleanup;
                }
                /* upstream slot was zeroed by betl_async_wrap; the
                 * wrapper now owns it. */
                upr->output_attached[port_idx] = 0;
                to_pass = &wrapped;
                wrapped_in_use = 1;
            }
            rc = r->def->attach_input(r->state, (int)pi, to_pass);
            if (rc != BETL_OK) {
                /* attach_input may or may not have taken ownership; if
                 * the component is well-behaved it zeroed the stream on
                 * error too, but we can't rely on that. Release the
                 * wrapper here only if the component clearly didn't take
                 * it (release callback still set). */
                if (wrapped_in_use && wrapped.release) {
                    wrapped.release(&wrapped);
                }
                goto cleanup;
            }
            /* attach_input transferred ownership; mark the slot empty
             * so cleanup doesn't double-release. */
            if (!wrapped_in_use) {
                upr->output_attached[port_idx] = 0;
                memset(&upr->outputs[port_idx], 0,
                       sizeof upr->outputs[port_idx]);
            }
        }
    }

    /* Drive every sink (in topo order). */
    for (int oi = 0; oi < (int)s->step_count; ++oi) {
        int i = order[oi];
        StepRunner *r = &runners[i];
        if (r->def->kind != BETL_KIND_SINK) continue;
        if (!r->def->sink_run) {
            betl_set_error(ctx,
                "stage '%s' step '%s': SINK has no sink_run",
                s->id, s->steps[i].id);
            rc = BETL_ERR_INVALID;
            goto cleanup;
        }
        rc = r->def->sink_run(r->state);
        if (rc != BETL_OK) goto cleanup;
    }

cleanup:
    /* Release any per-port outputs that were attached but never
     * consumed by a downstream step. */
    for (size_t i = 0; i < s->step_count; ++i) {
        for (size_t p = 0; p < runners[i].n_output_ports; ++p) {
            if (runners[i].output_attached[p]
                && runners[i].outputs[p].release) {
                runners[i].outputs[p].release(&runners[i].outputs[p]);
            }
        }
    }
    /* Destroy every initialized state, in reverse-init order. */
    for (size_t k = s->step_count; k-- > 0; ) {
        if (runners[k].state && runners[k].def && runners[k].def->destroy
            && !runners[k].destroyed) {
            runners[k].def->destroy(runners[k].state);
            runners[k].destroyed = 1;
        }
        free(runners[k].outputs);
        free(runners[k].output_attached);
    }
    free(runners);
    free(order);
    return rc;
}

/* ============================================================== *
 *  Top-level                                                       *
 * ============================================================== */

/* Forward decl: foreach calls back into the generic runner. */
static int run_stages(BetlContext *ctx, BetlRegistry *reg,
                      const BetlStage *arr, size_t n);

/* Evaluate a stage's `condition:` (NULL if absent). Returns 1 to run,
 * 0 to skip, -1 on substitution / parse error (with betl_set_error). */
static int eval_condition(BetlContext *ctx, const BetlStage *s) {
    if (!s->condition) return 1;
    char sub_err[256] = {0};
    char *resolved = betl_substitute_refs(s->condition, ctx,
                                          sub_err, sizeof sub_err);
    if (!resolved) {
        betl_set_error(ctx, "stage '%s': condition substitution failed: %s",
                       s->id, sub_err);
        return -1;
    }
    /* Trim leading / trailing ASCII whitespace so `condition: " true "`
     * works under YAML's flow-style quoting. */
    char *start = resolved;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
        ++start;
    size_t L = strlen(start);
    while (L > 0 && (start[L-1] == ' '  || start[L-1] == '\t' ||
                     start[L-1] == '\n' || start[L-1] == '\r')) {
        start[--L] = '\0';
    }
    int result;
    if (*start == '\0'
        || strcmp(start, "0")     == 0
        || strcmp(start, "false") == 0
        || strcmp(start, "FALSE") == 0
        || strcmp(start, "False") == 0
        || strcmp(start, "no")    == 0
        || strcmp(start, "NO")    == 0) {
        result = 0;
    } else if (strcmp(start, "1")    == 0
            || strcmp(start, "true") == 0
            || strcmp(start, "TRUE") == 0
            || strcmp(start, "True") == 0
            || strcmp(start, "yes")  == 0
            || strcmp(start, "YES")  == 0) {
        result = 1;
    } else {
        betl_set_error(ctx,
            "stage '%s': condition evaluates to non-boolean '%s' "
            "(supported: true/false/yes/no/1/0)",
            s->id, start);
        result = -1;
    }
    free(resolved);
    return result;
}

static int run_foreach_stage(BetlContext *ctx, BetlRegistry *reg,
                             const BetlStage *s) {
    /* Save and restore any prior binding of `as:` so a nested foreach
     * with the same loop variable name doesn't permanently shadow it. */
    const char *prior = betl_context_get_var(ctx, s->foreach_var);
    char *saved = prior ? strdup(prior) : NULL;
    if (prior && !saved) return BETL_ERR_INTERNAL;

    int rc = BETL_OK;
    for (size_t i = 0; i < s->over_count && rc == BETL_OK; ++i) {
        if (betl_should_cancel(ctx)) {
            betl_set_error(ctx, "cancelled inside foreach '%s' at iter %zu",
                           s->id, i);
            rc = BETL_ERR_CANCELLED;
            break;
        }
        int sr = betl_context_set_var(ctx, s->foreach_var, s->over[i]);
        if (sr != BETL_OK) {
            betl_set_error(ctx,
                "foreach '%s': failed to bind iteration variable", s->id);
            rc = sr;
            break;
        }
        betl_log(ctx, BETL_LOG_INFO,
                 "foreach %s iter %zu/%zu: %s=\"%s\"",
                 s->id, i + 1, s->over_count, s->foreach_var, s->over[i]);
        rc = run_stages(ctx, reg, s->children, s->child_count);
    }

    if (saved) {
        betl_context_set_var(ctx, s->foreach_var, saved);
        free(saved);
    } else {
        betl_context_unset_var(ctx, s->foreach_var);
    }
    return rc;
}

static int run_stages(BetlContext *ctx, BetlRegistry *reg,
                      const BetlStage *arr, size_t n) {
    if (n == 0) return BETL_OK;

    int *order = malloc(n * sizeof *order);
    if (!order) return BETL_ERR_INTERNAL;
    StageCtx sc = { .arr = arr, .n = n };
    if (topo_sort((int)n, stage_incoming, &sc, order) != 0) {
        free(order);
        betl_set_error(ctx, "stage `after:` cycle (parser should have caught)");
        return BETL_ERR_INVALID;
    }

    int rc = BETL_OK;
    for (size_t oi = 0; oi < n && rc == BETL_OK; ++oi) {
        const BetlStage *s = &arr[order[oi]];

        if (betl_should_cancel(ctx)) {
            betl_set_error(ctx, "cancelled before stage '%s'", s->id);
            rc = BETL_ERR_CANCELLED;
            break;
        }

        int gate = eval_condition(ctx, s);
        if (gate < 0) {
            rc = BETL_ERR_INVALID;
            break;
        }
        if (gate == 0) {
            betl_log(ctx, BETL_LOG_INFO,
                     "stage skip: %s (condition is falsy)", s->id);
            continue;
        }

        const char *kind_label =
            (s->kind == BETL_STAGE_DATAFLOW) ? "dataflow" :
            (s->kind == BETL_STAGE_FOREACH)  ? "foreach"  : "task";
        betl_log(ctx, BETL_LOG_INFO, "stage start: %s (%s)",
                 s->id, kind_label);

        int srcrc;
        if (s->kind == BETL_STAGE_TASK) {
            srcrc = run_task_stage(ctx, reg, s);
        } else if (s->kind == BETL_STAGE_FOREACH) {
            srcrc = run_foreach_stage(ctx, reg, s);
        } else {
            srcrc = run_dataflow_stage(ctx, reg, s);
        }

        betl_log(ctx,
            (srcrc == BETL_OK) ? BETL_LOG_INFO : BETL_LOG_ERROR,
            "stage end: %s rc=%d", s->id, srcrc);

        if (srcrc == BETL_OK) continue;

        /* Honor on_failure:continue (only for non-cancellation failures
         * — Ctrl-C / host cancel always halts). */
        if (srcrc != BETL_ERR_CANCELLED
            && s->on_failure
            && strcmp(s->on_failure, "continue") == 0) {
            betl_log(ctx, BETL_LOG_WARN,
                     "stage '%s' failed (rc=%d) but on_failure=continue: %s",
                     s->id, srcrc, betl_context_last_error(ctx));
            continue;
        }
        rc = srcrc;
    }
    free(order);
    return rc;
}

int betl_run(BetlContext *ctx, BetlRegistry *reg, const BetlPipeline *p) {
    if (!ctx || !reg || !p) return BETL_ERR_INVALID;

    /* Make the registry reachable to components via the context — used
     * by core transforms (filter / map) and any plugin that needs to
     * find an expression engine by lang. */
    betl_context_set_registry(ctx, reg);

    size_t n = betl_pipeline_stage_count(p);
    if (n == 0) return BETL_OK;
    return run_stages(ctx, reg, betl_pipeline_stage(p, 0), n);
}
