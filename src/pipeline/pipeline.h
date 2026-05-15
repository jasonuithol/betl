/* Pipeline parser — turns a betl YAML pipeline file into an in-memory
 * graph of stages and dataflow steps, with cross-reference and cycle
 * checks already performed.
 *
 * Component-config blocks are NOT parsed here (each step has a `type`
 * and an `id`, but its component-specific keys are left in the source
 * YAML for now). Schema validation of those configs is a separate
 * pass. */

#ifndef BETL_PIPELINE_H
#define BETL_PIPELINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BETL_STAGE_DATAFLOW = 1,    /* contains nested data-flow steps */
    BETL_STAGE_TASK     = 2,    /* a single control-flow task */
    BETL_STAGE_FOREACH  = 3     /* iterates `body:` once per `over:` value;
                                 * binds the current value to `${vars.<as>}` */
} BetlStageKind;

typedef struct {
    char    *id;             /* required, unique within its stage */
    char    *type;           /* component type, e.g. "csv.read" */
    char   **inputs;         /* upstream step ids (from `from:`) */
    size_t   input_count;
    /* JSON object containing the entire step mapping. Includes the
     * special keys (id/type/from) for traceability; component init
     * can ignore them. */
    char    *config_json;
    int      line;           /* 1-based source location of the step's id */
    int      column;
} BetlDataflowStep;

typedef struct {
    char    *name;           /* connection name, e.g. "warehouse" */
    /* JSON object containing every key under the connection mapping
     * (`type`, `dsn`, plus provider-specific keys). The runtime applies
     * `${env.X}` substitution before storing this on the BetlContext. */
    char    *config_json;
    int      line;           /* 1-based source location of the name */
    int      column;
} BetlConnectionDecl;

typedef struct {
    char    *name;           /* parameter name */
    char    *type;           /* "string" / "bool" / "int32" / "int64" /
                              * "date" / "timestamp" — Arrow logical types
                              * are accepted but only the basic set above
                              * is currently validated. */
    int      required;       /* 1 if `required: true`, else 0 */
    int      has_default;
    int      is_sentinel;    /* 1 if default_value is "today" or "now" */
    char    *default_value;  /* literal string OR sentinel name */
    char    *doc;            /* may be NULL */
    int      line;           /* 1-based source location of the name */
    int      column;
} BetlParameterDecl;

typedef struct BetlStage BetlStage;

struct BetlStage {
    char            *id;            /* required, unique within file */
    BetlStageKind    kind;

    /* Set when kind == BETL_STAGE_TASK; e.g. "sql.execute". */
    char            *task_type;
    /* JSON of the entire stage mapping, populated when kind == TASK
     * so the executor can hand it to the task component's init. */
    char            *task_config_json;

    /* Set when kind == BETL_STAGE_DATAFLOW. */
    BetlDataflowStep *steps;
    size_t            step_count;

    /* Set when kind == BETL_STAGE_FOREACH:
     *   `over` is the iteration source (currently always a literal list
     *   of strings; future enumerators will set foreach_kind),
     *   `foreach_var` is the variable name the loop binds per iteration
     *   (consumed by `${vars.<foreach_var>}` inside `children`),
     *   `children` is the nested stage list to run once per `over`
     *   element. */
    char           **over;
    size_t           over_count;
    char            *foreach_var;
    BetlStage       *children;
    size_t           child_count;

    /* Stages this stage waits on (from `after:`). */
    char           **after;
    size_t           after_count;

    /* Optional per-stage flow-control attributes. Both are NULL when
     * unset on the source YAML.
     *
     *   `on_failure`: "stop" (default if unset) or "continue". When
     *      "continue", a non-zero return from this stage is logged at
     *      WARN and the executor proceeds to the next stage.
     *   `condition`: a YAML scalar string. The executor passes it
     *      through betl_substitute_refs (so `${params.X}` / `${vars.X}`
     *      resolve) and treats truthy results as "run", falsy as
     *      "skip with WARN". For v1 this is a string check; future
     *      revisions may accept the full `{lang, expr}` shape from
     *      the spec.
     */
    char            *on_failure;
    char            *condition;

    int              line;          /* 1-based, of the stage's id */
    int              column;
};

typedef struct BetlPipeline BetlPipeline;

/* Parse and validate a pipeline file. On success returns a non-NULL
 * pipeline; the caller frees it with `betl_pipeline_free`. On failure
 * returns NULL and writes a description into `err_buf` (which must
 * have capacity >= 2). The format of the error string is:
 *
 *     <path>:<line>:<col>: <reason>
 *
 * when a source location is known, or just `<path>: <reason>` for
 * file-level errors (open / parse). */
BetlPipeline *betl_pipeline_load(const char *path, char *err_buf, size_t err_cap);

void          betl_pipeline_free(BetlPipeline *p);

const char       *betl_pipeline_name        (const BetlPipeline *p);
const char       *betl_pipeline_description (const BetlPipeline *p);
size_t            betl_pipeline_stage_count (const BetlPipeline *p);
const BetlStage  *betl_pipeline_stage       (const BetlPipeline *p, size_t i);
const BetlStage  *betl_pipeline_find_stage  (const BetlPipeline *p, const char *id);

/* Total step count across all dataflow stages. Useful for reporting. */
size_t            betl_pipeline_total_steps (const BetlPipeline *p);

/* Connections declared at the top of the pipeline file under the
 * `connections:` mapping. Returns 0 / NULL if none were declared. */
size_t                    betl_pipeline_connection_count(const BetlPipeline *p);
const BetlConnectionDecl *betl_pipeline_connection      (const BetlPipeline *p,
                                                         size_t i);

/* Parameters declared under the `parameters:` mapping. Returns 0 / NULL
 * if none were declared. The parser validates structure (each must
 * have a `type:`); resolution of defaults / required-ness / CLI
 * overrides happens at apply time, not here. */
size_t                   betl_pipeline_parameter_count(const BetlPipeline *p);
const BetlParameterDecl *betl_pipeline_parameter      (const BetlPipeline *p,
                                                       size_t i);

#ifdef __cplusplus
}
#endif

#endif /* BETL_PIPELINE_H */
