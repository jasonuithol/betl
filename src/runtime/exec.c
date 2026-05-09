#include "runtime/exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
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

/* ---- Stage-level edges (from `after:`) -------------------------- */

typedef struct {
    const BetlPipeline *p;
} StageCtx;

static int stage_index(const BetlPipeline *p, const char *id) {
    for (size_t i = 0; i < betl_pipeline_stage_count(p); ++i) {
        if (strcmp(betl_pipeline_stage(p, i)->id, id) == 0) return (int)i;
    }
    return -1;
}

static void stage_incoming(int i, int *out, size_t *out_count, void *user) {
    const StageCtx *c = user;
    const BetlStage *s = betl_pipeline_stage(c->p, (size_t)i);
    *out_count = 0;
    for (size_t k = 0; k < s->after_count; ++k) {
        int idx = stage_index(c->p, s->after[k]);
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

static void step_incoming(int i, int *out, size_t *out_count, void *user) {
    const StepCtx *c = user;
    const BetlDataflowStep *step = &c->s->steps[i];
    *out_count = 0;
    for (size_t k = 0; k < step->input_count; ++k) {
        int idx = step_index(c->s, step->inputs[k]);
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
    struct ArrowArrayStream output;     /* populated if def->attach_output ran */
    int                     output_attached;
    int                     destroyed;
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

    /* Walk in topo order: for each step, attach inputs from upstream
     * outputs, then call attach_output to populate THIS step's output. */
    for (int oi = 0; oi < (int)s->step_count; ++oi) {
        int i = order[oi];
        const BetlDataflowStep *st = &s->steps[i];
        StepRunner *r = &runners[i];

        /* attach_input from each declared upstream, by port order. */
        for (size_t pi = 0; pi < st->input_count; ++pi) {
            int up = step_index(s, st->inputs[pi]);
            if (up < 0) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': upstream '%s' missing",
                    s->id, st->id, st->inputs[pi]);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }
            StepRunner *upr = &runners[up];
            if (!upr->output_attached) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': upstream '%s' has no output stream",
                    s->id, st->id, s->steps[up].id);
                rc = BETL_ERR_INTERNAL;
                goto cleanup;
            }
            if (!r->def->attach_input) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': component lacks attach_input",
                    s->id, st->id);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }
            rc = r->def->attach_input(r->state, (int)pi, &upr->output);
            if (rc != BETL_OK) goto cleanup;
            /* Ownership transferred to downstream. */
            upr->output_attached = 0;
            memset(&upr->output, 0, sizeof upr->output);
        }

        /* attach_output unless this is a sink */
        if (r->def->kind != BETL_KIND_SINK) {
            if (!r->def->attach_output) {
                betl_set_error(ctx,
                    "stage '%s' step '%s': component lacks attach_output",
                    s->id, st->id);
                rc = BETL_ERR_INVALID;
                goto cleanup;
            }
            rc = r->def->attach_output(r->state, 0, &r->output);
            if (rc != BETL_OK) goto cleanup;
            r->output_attached = 1;
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
    /* Release any outputs that were populated but never consumed. */
    for (size_t i = 0; i < s->step_count; ++i) {
        if (runners[i].output_attached && runners[i].output.release) {
            runners[i].output.release(&runners[i].output);
        }
    }
    /* Destroy every initialized state, in reverse-init order. */
    for (size_t k = s->step_count; k-- > 0; ) {
        if (runners[k].state && runners[k].def && runners[k].def->destroy
            && !runners[k].destroyed) {
            runners[k].def->destroy(runners[k].state);
            runners[k].destroyed = 1;
        }
    }
    free(runners);
    free(order);
    return rc;
}

/* ============================================================== *
 *  Top-level                                                       *
 * ============================================================== */

int betl_run(BetlContext *ctx, BetlRegistry *reg, const BetlPipeline *p) {
    if (!ctx || !reg || !p) return BETL_ERR_INVALID;

    /* Make the registry reachable to components via the context — used
     * by core transforms (filter / map) and any plugin that needs to
     * find an expression engine by lang. */
    betl_context_set_registry(ctx, reg);

    int n = (int)betl_pipeline_stage_count(p);
    if (n == 0) return BETL_OK;

    int *order = malloc((size_t)n * sizeof *order);
    if (!order) return BETL_ERR_INTERNAL;
    StageCtx sc = { .p = p };
    if (topo_sort(n, stage_incoming, &sc, order) != 0) {
        free(order);
        betl_set_error(ctx, "stage `after:` cycle (parser should have caught)");
        return BETL_ERR_INVALID;
    }

    int rc = BETL_OK;
    for (int oi = 0; oi < n && rc == BETL_OK; ++oi) {
        const BetlStage *s = betl_pipeline_stage(p, (size_t)order[oi]);
        if (betl_should_cancel(ctx)) {
            betl_set_error(ctx, "cancelled before stage '%s'", s->id);
            rc = BETL_ERR_CANCELLED;
            break;
        }
        betl_log(ctx, BETL_LOG_INFO, "stage start: %s (%s)",
                 s->id,
                 s->kind == BETL_STAGE_DATAFLOW ? "dataflow" : "task");
        if (s->kind == BETL_STAGE_TASK) {
            rc = run_task_stage(ctx, reg, s);
        } else {
            rc = run_dataflow_stage(ctx, reg, s);
        }
        betl_log(ctx,
            (rc == BETL_OK) ? BETL_LOG_INFO : BETL_LOG_ERROR,
            "stage end: %s rc=%d", s->id, rc);
    }
    free(order);
    return rc;
}
