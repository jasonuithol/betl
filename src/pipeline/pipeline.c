#include "pipeline/pipeline.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

/* ============================================================== *
 *  Internal types                                                  *
 * ============================================================== */

struct BetlPipeline {
    char               *name;
    char               *description;
    BetlStage          *stages;
    size_t              stage_count;
    size_t              stage_cap;
    BetlConnectionDecl *connections;
    size_t              connection_count;
    size_t              connection_cap;
    BetlParameterDecl  *parameters;
    size_t              parameter_count;
    size_t              parameter_cap;
};

/* Convenience: a parser-context that carries the doc + path + error
 * buffer through the recursive descent so callees can attach a
 * source location to whatever they fail on. */
typedef struct {
    yaml_document_t *doc;
    const char      *path;
    char            *err;
    size_t           err_cap;
} Ctx;

/* ============================================================== *
 *  Error formatting                                                *
 * ============================================================== */

static void verr_at(Ctx *ctx, int line, int col, const char *fmt, va_list ap) {
    if (!ctx->err || ctx->err_cap < 2) return;
    int n = (line > 0)
        ? snprintf(ctx->err, ctx->err_cap, "%s:%d:%d: ", ctx->path, line, col)
        : snprintf(ctx->err, ctx->err_cap, "%s: ", ctx->path);
    if (n < 0 || (size_t)n >= ctx->err_cap) return;
    vsnprintf(ctx->err + n, ctx->err_cap - (size_t)n, fmt, ap);
}

static void err_at(Ctx *ctx, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verr_at(ctx, line, col, fmt, ap);
    va_end(ap);
}

static int yaml_line(const yaml_node_t *n) {
    return n ? (int)n->start_mark.line + 1 : 0;
}
static int yaml_col(const yaml_node_t *n) {
    return n ? (int)n->start_mark.column + 1 : 0;
}

/* ============================================================== *
 *  YAML traversal helpers                                          *
 * ============================================================== */

static yaml_node_t *node(yaml_document_t *d, int id) {
    return id ? yaml_document_get_node(d, id) : NULL;
}

static int is_scalar(const yaml_node_t *n) {
    return n && n->type == YAML_SCALAR_NODE;
}
static int is_seq(const yaml_node_t *n) {
    return n && n->type == YAML_SEQUENCE_NODE;
}
static int is_map(const yaml_node_t *n) {
    return n && n->type == YAML_MAPPING_NODE;
}

static const char *scalar(const yaml_node_t *n) {
    return is_scalar(n) ? (const char *)n->data.scalar.value : NULL;
}

/* Look up a key in a mapping. Returns the value node, or NULL if the
 * mapping doesn't contain that key. */
static yaml_node_t *map_get(yaml_document_t *d, yaml_node_t *m,
                            const char *key) {
    if (!is_map(m)) return NULL;
    for (yaml_node_pair_t *p = m->data.mapping.pairs.start;
         p < m->data.mapping.pairs.top; ++p) {
        yaml_node_t *k = node(d, p->key);
        const char *ks = scalar(k);
        if (ks && strcmp(ks, key) == 0) {
            return node(d, p->value);
        }
    }
    return NULL;
}

/* ============================================================== *
 *  Growable string buffer                                          *
 * ============================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t need) {
    if (b->cap >= need) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) return -1;
    b->data = p;
    b->cap  = nc;
    return 0;
}

static int buf_push(Buf *b, const char *s, size_t n) {
    if (buf_reserve(b, b->len + n + 1) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_pushc(Buf *b, char c) {
    return buf_push(b, &c, 1);
}

static int buf_pushs(Buf *b, const char *s) {
    return buf_push(b, s, strlen(s));
}

/* ============================================================== *
 *  YAML -> JSON                                                    *
 * ============================================================== */

/* Conservative integer detector: optional `-` followed by digits, no
 * leading zeros (unless the value is exactly "0"). We don't try to
 * recognize floats — those fall through to JSON string. */
static int looks_like_int(const char *s) {
    if (!s || !*s) return 0;
    const char *p = s;
    if (*p == '-') ++p;
    if (!*p) return 0;
    if (*p == '0' && p[1] != '\0') return 0;   /* no leading zeros */
    for (; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int json_escape_string(Buf *b, const char *s) {
    if (buf_pushc(b, '"') != 0) return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (buf_pushc(b, '\\') != 0) return -1;
            if (buf_pushc(b, (char)c) != 0) return -1;
        } else if (c == '\n') {
            if (buf_push(b, "\\n", 2) != 0) return -1;
        } else if (c == '\r') {
            if (buf_push(b, "\\r", 2) != 0) return -1;
        } else if (c == '\t') {
            if (buf_push(b, "\\t", 2) != 0) return -1;
        } else if (c == '\b') {
            if (buf_push(b, "\\b", 2) != 0) return -1;
        } else if (c == '\f') {
            if (buf_push(b, "\\f", 2) != 0) return -1;
        } else if (c < 0x20) {
            char esc[8];
            int n = snprintf(esc, sizeof esc, "\\u%04x", c);
            if (n < 0 || buf_push(b, esc, (size_t)n) != 0) return -1;
        } else {
            if (buf_pushc(b, (char)c) != 0) return -1;
        }
    }
    return buf_pushc(b, '"');
}

static int yaml_to_json_node(yaml_document_t *d, yaml_node_t *n, Buf *b);

static int yaml_scalar_to_json(yaml_node_t *n, Buf *b) {
    const char *v = (const char *)n->data.scalar.value;
    /* Plain (unquoted) scalars get type-coerced; quoted scalars stay strings. */
    if (n->data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
        if (!v[0] || strcmp(v, "null") == 0 || strcmp(v, "~") == 0) {
            return buf_pushs(b, "null");
        }
        if (strcmp(v, "true")  == 0) return buf_pushs(b, "true");
        if (strcmp(v, "false") == 0) return buf_pushs(b, "false");
        if (looks_like_int(v))       return buf_pushs(b, v);
    }
    return json_escape_string(b, v);
}

static int yaml_map_to_json(yaml_document_t *d, yaml_node_t *m, Buf *b) {
    if (buf_pushc(b, '{') != 0) return -1;
    int first = 1;
    for (yaml_node_pair_t *p = m->data.mapping.pairs.start;
         p < m->data.mapping.pairs.top; ++p) {
        yaml_node_t *k = node(d, p->key);
        yaml_node_t *v = node(d, p->value);
        const char *ks = scalar(k);
        if (!ks) continue;     /* non-scalar keys: skip in v0.1 */
        if (!first && buf_pushc(b, ',') != 0) return -1;
        first = 0;
        if (json_escape_string(b, ks) != 0) return -1;
        if (buf_pushc(b, ':') != 0) return -1;
        if (yaml_to_json_node(d, v, b) != 0) return -1;
    }
    return buf_pushc(b, '}');
}

static int yaml_seq_to_json(yaml_document_t *d, yaml_node_t *s, Buf *b) {
    if (buf_pushc(b, '[') != 0) return -1;
    int first = 1;
    for (yaml_node_item_t *it = s->data.sequence.items.start;
         it < s->data.sequence.items.top; ++it) {
        if (!first && buf_pushc(b, ',') != 0) return -1;
        first = 0;
        if (yaml_to_json_node(d, node(d, *it), b) != 0) return -1;
    }
    return buf_pushc(b, ']');
}

static int yaml_to_json_node(yaml_document_t *d, yaml_node_t *n, Buf *b) {
    if (!n) return buf_pushs(b, "null");
    switch (n->type) {
        case YAML_SCALAR_NODE:   return yaml_scalar_to_json(n, b);
        case YAML_MAPPING_NODE:  return yaml_map_to_json(d, n, b);
        case YAML_SEQUENCE_NODE: return yaml_seq_to_json(d, n, b);
        case YAML_NO_NODE:       return buf_pushs(b, "null");
    }
    return buf_pushs(b, "null");
}

/* Public-ish: render a YAML node as a freshly-malloc'd JSON string.
 * Returns NULL on OOM (errno-style; caller treats as fatal). */
static char *yaml_node_to_json(yaml_document_t *d, yaml_node_t *n) {
    Buf b = {0};
    if (yaml_to_json_node(d, n, &b) != 0) {
        free(b.data);
        return NULL;
    }
    if (!b.data) {
        /* node was missing; produce "null" explicitly */
        b.data = strdup("null");
    }
    return b.data;
}

/* ============================================================== *
 *  String / list extraction                                        *
 * ============================================================== */

/* Append to a char** array; takes ownership of `s`. Returns 0 on
 * success, -1 on OOM (caller must free `s`). */
static int strlist_push(char ***arr, size_t *count, size_t *cap, char *s) {
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        char **p = realloc(*arr, nc * sizeof *p);
        if (!p) return -1;
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*count)++] = s;
    return 0;
}

static void strlist_free(char **arr, size_t count) {
    for (size_t i = 0; i < count; ++i) free(arr[i]);
    free(arr);
}

/* Extract a list of strings from a node that is either:
 *   - a scalar      → single-element list
 *   - a sequence    → each element must be a scalar
 * On success returns 0 with `*out` and `*out_count` populated.
 * On failure returns -1 with err set. The list is heap-allocated
 * and each entry is strdup'd. */
static int extract_string_list(Ctx *ctx, yaml_node_t *n,
                               const char *what,
                               char ***out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!n) return 0;

    if (is_scalar(n)) {
        char *s = strdup(scalar(n));
        if (!s) return -1;
        char **arr = malloc(sizeof *arr);
        if (!arr) { free(s); return -1; }
        arr[0] = s;
        *out = arr;
        *out_count = 1;
        return 0;
    }
    if (!is_seq(n)) {
        err_at(ctx, yaml_line(n), yaml_col(n),
               "%s: expected a string or list of strings", what);
        return -1;
    }
    size_t cap = 0;
    for (yaml_node_item_t *it = n->data.sequence.items.start;
         it < n->data.sequence.items.top; ++it) {
        yaml_node_t *e = node(ctx->doc, *it);
        const char *s = scalar(e);
        if (!s) {
            err_at(ctx, yaml_line(e), yaml_col(e),
                   "%s: list entries must be strings", what);
            strlist_free(*out, *out_count);
            *out = NULL;
            *out_count = 0;
            return -1;
        }
        char *cp = strdup(s);
        if (!cp || strlist_push(out, out_count, &cap, cp) != 0) {
            free(cp);
            strlist_free(*out, *out_count);
            *out = NULL;
            *out_count = 0;
            return -1;
        }
    }
    return 0;
}

/* ============================================================== *
 *  Step / stage parsing                                            *
 * ============================================================== */

static void free_parameter(BetlParameterDecl *pa) {
    if (!pa) return;
    free(pa->name);
    free(pa->type);
    free(pa->default_value);
    free(pa->doc);
    memset(pa, 0, sizeof *pa);
}

static int append_parameter(BetlPipeline *p, const BetlParameterDecl *pa) {
    if (p->parameter_count == p->parameter_cap) {
        size_t nc = p->parameter_cap ? p->parameter_cap * 2 : 4;
        BetlParameterDecl *arr = realloc(p->parameters, nc * sizeof *arr);
        if (!arr) return -1;
        p->parameters    = arr;
        p->parameter_cap = nc;
    }
    p->parameters[p->parameter_count++] = *pa;
    return 0;
}

static void free_connection(BetlConnectionDecl *c) {
    if (!c) return;
    free(c->name);
    free(c->config_json);
    memset(c, 0, sizeof *c);
}

static int append_connection(BetlPipeline *p, const BetlConnectionDecl *c) {
    if (p->connection_count == p->connection_cap) {
        size_t nc = p->connection_cap ? p->connection_cap * 2 : 4;
        BetlConnectionDecl *arr = realloc(p->connections, nc * sizeof *arr);
        if (!arr) return -1;
        p->connections     = arr;
        p->connection_cap  = nc;
    }
    p->connections[p->connection_count++] = *c;
    return 0;
}

static void free_step(BetlDataflowStep *s) {
    free(s->id);
    free(s->type);
    free(s->config_json);
    strlist_free(s->inputs, s->input_count);
}

static void free_stage(BetlStage *s) {
    free(s->id);
    free(s->task_type);
    free(s->task_config_json);
    strlist_free(s->after, s->after_count);
    for (size_t i = 0; i < s->step_count; ++i) free_step(&s->steps[i]);
    free(s->steps);
    strlist_free(s->over, s->over_count);
    free(s->foreach_var);
    for (size_t i = 0; i < s->child_count; ++i) free_stage(&s->children[i]);
    free(s->children);
    free(s->on_failure);
    free(s->condition);
}

/* Parse one entry in the top-level `connections:` mapping. The key is
 * the connection name; the value is a mapping of attributes. We require
 * a `dsn:` and reject literal credentials (heuristic: a `dsn:` whose
 * value contains `://[^@]*:[^@]*@` and no `${...}` substitution slots).
 * Other attributes are passed through as-is into the stored JSON. */
static int parse_connection(Ctx *ctx, const char *name,
                            int name_line, int name_col,
                            yaml_node_t *map_n,
                            BetlConnectionDecl *out) {
    if (!is_map(map_n)) {
        err_at(ctx, yaml_line(map_n), yaml_col(map_n),
               "connection '%s': expected a mapping", name);
        return -1;
    }
    yaml_node_t *dsn_n = map_get(ctx->doc, map_n, "dsn");
    if (!scalar(dsn_n)) {
        err_at(ctx, name_line, name_col,
               "connection '%s': missing required scalar `dsn`", name);
        return -1;
    }
    const char *dsn = scalar(dsn_n);

    /* Cheap inline-credential check: a URL-style DSN with a password in
     * the userinfo and no substitution placeholder anywhere in the
     * string is almost certainly a literal secret. SPEC §5.3 forbids
     * those. We accept env / secret / params placeholders as evidence
     * the user meant to interpolate. */
    int has_placeholder = strstr(dsn, "${env.")    != NULL
                       || strstr(dsn, "${secret.") != NULL
                       || strstr(dsn, "${params.") != NULL;
    if (!has_placeholder) {
        const char *scheme_end = strstr(dsn, "://");
        if (scheme_end) {
            const char *at = strchr(scheme_end + 3, '@');
            const char *colon = strchr(scheme_end + 3, ':');
            if (at && colon && colon < at) {
                err_at(ctx, yaml_line(dsn_n), yaml_col(dsn_n),
                       "connection '%s': inline literal password in dsn — "
                       "use ${env.X} or ${secret.X} instead", name);
                return -1;
            }
        }
    }

    out->name        = strdup(name);
    out->config_json = yaml_node_to_json(ctx->doc, map_n);
    out->line        = name_line;
    out->column      = name_col;
    if (!out->name || !out->config_json) {
        err_at(ctx, 0, 0, "out of memory parsing connection '%s'", name);
        free_connection(out);
        return -1;
    }
    return 0;
}

/* Parse one entry in the top-level `parameters:` mapping. The key is the
 * parameter name; the value is either a mapping of attributes or a
 * scalar (shorthand: scalar = type, no default, not required). */
static int parse_parameter(Ctx *ctx, const char *name,
                           int name_line, int name_col,
                           yaml_node_t *val_n,
                           BetlParameterDecl *out) {
    out->name   = strdup(name);
    out->line   = name_line;
    out->column = name_col;
    if (!out->name) goto oom;

    /* Shorthand: `param_name: string` is equivalent to `{ type: string }`. */
    if (scalar(val_n)) {
        out->type = strdup(scalar(val_n));
        if (!out->type) goto oom;
        return 0;
    }
    if (!is_map(val_n)) {
        err_at(ctx, yaml_line(val_n), yaml_col(val_n),
               "parameter '%s': expected a mapping or scalar type", name);
        goto fail;
    }

    yaml_node_t *type_n     = map_get(ctx->doc, val_n, "type");
    yaml_node_t *required_n = map_get(ctx->doc, val_n, "required");
    yaml_node_t *default_n  = map_get(ctx->doc, val_n, "default");
    yaml_node_t *doc_n      = map_get(ctx->doc, val_n, "doc");

    if (!scalar(type_n)) {
        err_at(ctx, name_line, name_col,
               "parameter '%s': missing required scalar `type`", name);
        goto fail;
    }
    out->type = strdup(scalar(type_n));
    if (!out->type) goto oom;

    if (scalar(required_n)) {
        const char *r = scalar(required_n);
        if (strcmp(r, "true") == 0)       out->required = 1;
        else if (strcmp(r, "false") == 0) out->required = 0;
        else {
            err_at(ctx, yaml_line(required_n), yaml_col(required_n),
                   "parameter '%s': `required` must be `true` or `false`",
                   name);
            goto fail;
        }
    }

    if (scalar(default_n)) {
        const char *d = scalar(default_n);
        out->default_value = strdup(d);
        if (!out->default_value) goto oom;
        out->has_default = 1;
        if (strcmp(d, "today") == 0 || strcmp(d, "now") == 0) {
            out->is_sentinel = 1;
        }
    } else if (default_n) {
        err_at(ctx, yaml_line(default_n), yaml_col(default_n),
               "parameter '%s': `default` must be a scalar", name);
        goto fail;
    }

    if (scalar(doc_n)) {
        out->doc = strdup(scalar(doc_n));
        if (!out->doc) goto oom;
    }

    if (out->required && out->has_default) {
        err_at(ctx, name_line, name_col,
               "parameter '%s': `required` is incompatible with `default`",
               name);
        goto fail;
    }

    return 0;

oom:
    err_at(ctx, 0, 0, "out of memory parsing parameter '%s'", name);
fail:
    free_parameter(out);
    return -1;
}

static int parse_step(Ctx *ctx, yaml_node_t *step_node, BetlDataflowStep *out) {
    if (!is_map(step_node)) {
        err_at(ctx, yaml_line(step_node), yaml_col(step_node),
               "step: expected a mapping");
        return -1;
    }
    yaml_node_t *id_n   = map_get(ctx->doc, step_node, "id");
    yaml_node_t *type_n = map_get(ctx->doc, step_node, "type");
    yaml_node_t *from_n = map_get(ctx->doc, step_node, "from");

    const char *id   = scalar(id_n);
    const char *type = scalar(type_n);
    if (!id) {
        err_at(ctx, yaml_line(step_node), yaml_col(step_node),
               "step: missing required scalar `id`");
        return -1;
    }
    if (!type) {
        err_at(ctx, yaml_line(id_n), yaml_col(id_n),
               "step '%s': missing required scalar `type`", id);
        return -1;
    }

    out->id     = strdup(id);
    out->type   = strdup(type);
    out->line   = yaml_line(id_n);
    out->column = yaml_col(id_n);
    if (!out->id || !out->type) return -1;

    if (extract_string_list(ctx, from_n, "step `from`",
                            &out->inputs, &out->input_count) != 0) {
        return -1;
    }

    out->config_json = yaml_node_to_json(ctx->doc, step_node);
    if (!out->config_json) {
        err_at(ctx, yaml_line(step_node), yaml_col(step_node),
               "step '%s': out of memory serializing config", id);
        return -1;
    }
    return 0;
}

static int parse_stage(Ctx *ctx, yaml_node_t *stage_node, BetlStage *out) {
    if (!is_map(stage_node)) {
        err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
               "stage: expected a mapping");
        return -1;
    }
    yaml_node_t *id_n      = map_get(ctx->doc, stage_node, "id");
    yaml_node_t *type_n    = map_get(ctx->doc, stage_node, "type");
    yaml_node_t *after_n   = map_get(ctx->doc, stage_node, "after");
    yaml_node_t *steps_n   = map_get(ctx->doc, stage_node, "steps");
    yaml_node_t *over_n    = map_get(ctx->doc, stage_node, "over");
    yaml_node_t *as_n      = map_get(ctx->doc, stage_node, "as");
    yaml_node_t *body_n    = map_get(ctx->doc, stage_node, "body");
    yaml_node_t *onfail_n  = map_get(ctx->doc, stage_node, "on_failure");
    yaml_node_t *cond_n    = map_get(ctx->doc, stage_node, "condition");

    const char *id   = scalar(id_n);
    const char *type = scalar(type_n);
    if (!id) {
        err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
               "stage: missing required scalar `id`");
        return -1;
    }
    if (!type) {
        err_at(ctx, yaml_line(id_n), yaml_col(id_n),
               "stage '%s': missing required scalar `type`", id);
        return -1;
    }

    out->id     = strdup(id);
    out->line   = yaml_line(id_n);
    out->column = yaml_col(id_n);
    if (!out->id) return -1;

    if (strcmp(type, "dataflow") == 0) {
        out->kind = BETL_STAGE_DATAFLOW;
        if (!steps_n || !is_seq(steps_n)) {
            err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
                   "stage '%s': type=dataflow requires a non-empty `steps:` sequence",
                   id);
            return -1;
        }
        size_t cap = 0;
        for (yaml_node_item_t *it = steps_n->data.sequence.items.start;
             it < steps_n->data.sequence.items.top; ++it) {
            if (out->step_count == cap) {
                size_t nc = cap ? cap * 2 : 4;
                BetlDataflowStep *p = realloc(out->steps, nc * sizeof *p);
                if (!p) return -1;
                out->steps = p;
                cap = nc;
            }
            BetlDataflowStep *step = &out->steps[out->step_count];
            memset(step, 0, sizeof *step);
            if (parse_step(ctx, node(ctx->doc, *it), step) != 0) {
                free_step(step);
                return -1;
            }
            out->step_count++;
        }
        if (out->step_count == 0) {
            err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
                   "stage '%s': type=dataflow has empty `steps:` sequence", id);
            return -1;
        }
    } else if (strcmp(type, "foreach") == 0) {
        out->kind = BETL_STAGE_FOREACH;
        if (steps_n) {
            err_at(ctx, yaml_line(steps_n), yaml_col(steps_n),
                   "stage '%s' (type=foreach): use `body:` for nested stages, "
                   "not `steps:`", id);
            return -1;
        }
        /* `over:` — required, must be a sequence of scalar strings (v1
         * supports only literal-list iteration; future enumerators will
         * gate on additional keys). */
        if (!over_n) {
            err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
                   "stage '%s' (type=foreach): missing required `over:` list", id);
            return -1;
        }
        if (extract_string_list(ctx, over_n, "stage `over`",
                                &out->over, &out->over_count) != 0) {
            return -1;
        }
        if (out->over_count == 0) {
            err_at(ctx, yaml_line(over_n), yaml_col(over_n),
                   "stage '%s' (type=foreach): `over:` must have at least "
                   "one element", id);
            return -1;
        }
        /* `as:` — required, the variable name bound per iteration. */
        const char *as_name = scalar(as_n);
        if (!as_name || !*as_name) {
            err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
                   "stage '%s' (type=foreach): missing required `as:` "
                   "variable name", id);
            return -1;
        }
        out->foreach_var = strdup(as_name);
        if (!out->foreach_var) return -1;
        /* `body:` — required, a sequence of nested stages. */
        if (!body_n || !is_seq(body_n)) {
            err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
                   "stage '%s' (type=foreach): missing required `body:` "
                   "stage list", id);
            return -1;
        }
        size_t cap = 0;
        for (yaml_node_item_t *it = body_n->data.sequence.items.start;
             it < body_n->data.sequence.items.top; ++it) {
            if (out->child_count == cap) {
                size_t nc = cap ? cap * 2 : 4;
                BetlStage *p = realloc(out->children, nc * sizeof *p);
                if (!p) return -1;
                out->children = p;
                cap = nc;
            }
            BetlStage *child = &out->children[out->child_count];
            memset(child, 0, sizeof *child);
            if (parse_stage(ctx, node(ctx->doc, *it), child) != 0) {
                free_stage(child);
                return -1;
            }
            out->child_count++;
        }
        if (out->child_count == 0) {
            err_at(ctx, yaml_line(body_n), yaml_col(body_n),
                   "stage '%s' (type=foreach): `body:` must have at least "
                   "one stage", id);
            return -1;
        }
    } else {
        out->kind = BETL_STAGE_TASK;
        out->task_type = strdup(type);
        if (!out->task_type) return -1;
        /* Tasks must NOT have `steps:` / `body:`. */
        if (steps_n) {
            err_at(ctx, yaml_line(steps_n), yaml_col(steps_n),
                   "stage '%s' (type=%s): `steps:` is only allowed for type=dataflow",
                   id, type);
            return -1;
        }
        if (body_n) {
            err_at(ctx, yaml_line(body_n), yaml_col(body_n),
                   "stage '%s' (type=%s): `body:` is only allowed for type=foreach",
                   id, type);
            return -1;
        }
        out->task_config_json = yaml_node_to_json(ctx->doc, stage_node);
        if (!out->task_config_json) {
            err_at(ctx, yaml_line(stage_node), yaml_col(stage_node),
                   "stage '%s': out of memory serializing config", id);
            return -1;
        }
    }

    if (extract_string_list(ctx, after_n, "stage `after`",
                            &out->after, &out->after_count) != 0) {
        return -1;
    }

    /* `on_failure:` (optional, scalar, one of "stop"/"continue"). */
    if (onfail_n) {
        const char *v = scalar(onfail_n);
        if (!v || (strcmp(v, "stop") != 0 && strcmp(v, "continue") != 0)) {
            err_at(ctx, yaml_line(onfail_n), yaml_col(onfail_n),
                   "stage '%s': `on_failure:` must be \"stop\" or "
                   "\"continue\" (got '%s')",
                   id, v ? v : "(non-scalar)");
            return -1;
        }
        out->on_failure = strdup(v);
        if (!out->on_failure) return -1;
    }

    /* `condition:` (optional, scalar string). The executor passes it
     * through betl_substitute_refs at run-time and treats truthy strings
     * as "run". Object-form conditions ({lang, expr}) are not yet
     * accepted — return a clear error instead of silently ignoring. */
    if (cond_n) {
        const char *v = scalar(cond_n);
        if (!v) {
            err_at(ctx, yaml_line(cond_n), yaml_col(cond_n),
                   "stage '%s': `condition:` must be a scalar string in v1 "
                   "(object form `{lang, expr}` is reserved)", id);
            return -1;
        }
        out->condition = strdup(v);
        if (!out->condition) return -1;
    }

    return 0;
}

/* ============================================================== *
 *  Cross-reference and cycle checks                                *
 * ============================================================== */

/* Walk every stage in the tree (top-level + foreach bodies) and check
 * IDs are unique globally. A foreach body running N times reuses the
 * same stage IDs across iterations — that's fine at run-time — but two
 * separately-declared stages with the same ID would make `after:` /
 * log messages ambiguous, so we reject those. */
typedef struct {
    const BetlStage *stage;
    int              dup_line;
    int              dup_column;
} IdEntry;

typedef struct {
    IdEntry *items;
    size_t   count;
    size_t   cap;
} IdSet;

static int id_set_add(IdSet *s, const BetlStage *st) {
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        IdEntry *grow = realloc(s->items, nc * sizeof *grow);
        if (!grow) return -1;
        s->items = grow;
        s->cap   = nc;
    }
    s->items[s->count++] = (IdEntry){ .stage = st };
    return 0;
}

static int collect_ids(Ctx *ctx, BetlStage *arr, size_t n, IdSet *seen) {
    for (size_t i = 0; i < n; ++i) {
        BetlStage *s = &arr[i];
        for (size_t k = 0; k < seen->count; ++k) {
            if (strcmp(seen->items[k].stage->id, s->id) == 0) {
                err_at(ctx, s->line, s->column,
                       "duplicate stage id '%s' (first defined at line %d)",
                       s->id, seen->items[k].stage->line);
                return -1;
            }
        }
        if (id_set_add(seen, s) != 0) return -1;
        if (s->kind == BETL_STAGE_FOREACH) {
            if (collect_ids(ctx, s->children, s->child_count, seen) != 0)
                return -1;
        }
    }
    return 0;
}

static int check_unique_stage_ids(Ctx *ctx, const BetlPipeline *p) {
    IdSet seen = {0};
    int rc = collect_ids(ctx, p->stages, p->stage_count, &seen);
    free(seen.items);
    return rc;
}

static int check_unique_step_ids(Ctx *ctx, const BetlStage *s) {
    for (size_t i = 0; i < s->step_count; ++i) {
        for (size_t j = i + 1; j < s->step_count; ++j) {
            if (strcmp(s->steps[i].id, s->steps[j].id) == 0) {
                err_at(ctx, s->steps[j].line, s->steps[j].column,
                       "stage '%s': duplicate step id '%s' (first defined at line %d)",
                       s->id, s->steps[i].id, s->steps[i].line);
                return -1;
            }
        }
    }
    return 0;
}

static int find_stage_index_in(const BetlStage *arr, size_t n, const char *id) {
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(arr[i].id, id) == 0) return (int)i;
    }
    return -1;
}

static int find_step_index(const BetlStage *s, const char *id) {
    for (size_t i = 0; i < s->step_count; ++i) {
        if (strcmp(s->steps[i].id, id) == 0) return (int)i;
    }
    return -1;
}

/* `from:` items are either "step_id" or "step_id:port_name" — the
 * port suffix selects a specific output of a multi-output upstream
 * (canonical example: conditional_split). Strip the suffix and look
 * up the step part. Returns -1 on shape errors (empty step, oversized
 * step name) so the caller reports a clean error. */
static int find_step_index_ref(const BetlStage *s, const char *ref) {
    const char *colon = strchr(ref, ':');
    if (!colon) return find_step_index(s, ref);
    size_t step_len = (size_t)(colon - ref);
    if (step_len == 0 || step_len >= 128) return -1;
    char buf[128];
    memcpy(buf, ref, step_len);
    buf[step_len] = '\0';
    return find_step_index(s, buf);
}

/* `after:` is scoped — a stage can only reference siblings at the same
 * level (top-level stages reference top-level; stages inside a foreach
 * body reference siblings of that body). This both keeps the topo-sort
 * straightforward and reflects what dtsx2yaml emits: container
 * children with constraints pointing only at their own siblings. */
static int check_after_refs_in(Ctx *ctx, const BetlStage *arr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const BetlStage *s = &arr[i];
        for (size_t k = 0; k < s->after_count; ++k) {
            const char *ref = s->after[k];
            if (strcmp(ref, s->id) == 0) {
                err_at(ctx, s->line, s->column,
                       "stage '%s' depends on itself in `after:`", s->id);
                return -1;
            }
            if (find_stage_index_in(arr, n, ref) < 0) {
                err_at(ctx, s->line, s->column,
                       "stage '%s' depends on undefined sibling stage '%s' "
                       "in `after:` (refs are scoped to same-level siblings)",
                       s->id, ref);
                return -1;
            }
        }
        if (s->kind == BETL_STAGE_FOREACH) {
            if (check_after_refs_in(ctx, s->children, s->child_count) != 0)
                return -1;
        }
    }
    return 0;
}

static int check_after_refs(Ctx *ctx, const BetlPipeline *p) {
    return check_after_refs_in(ctx, p->stages, p->stage_count);
}

static int check_from_refs(Ctx *ctx, const BetlStage *s) {
    for (size_t i = 0; i < s->step_count; ++i) {
        const BetlDataflowStep *step = &s->steps[i];
        for (size_t k = 0; k < step->input_count; ++k) {
            const char *ref = step->inputs[k];
            int up = find_step_index_ref(s, ref);
            if (up < 0) {
                err_at(ctx, step->line, step->column,
                       "stage '%s': step '%s' references undefined sibling '%s' in `from:`",
                       s->id, step->id, ref);
                return -1;
            }
            if (up == (int)i) {
                err_at(ctx, step->line, step->column,
                       "stage '%s': step '%s' references itself in `from:`",
                       s->id, step->id);
                return -1;
            }
        }
    }
    return 0;
}

/* DFS-based cycle detection within a single `after:` scope (top-level
 * or one foreach body). `state` per node: 0 = unvisited, 1 = on stack,
 * 2 = done. */
static int dfs_stage_in(Ctx *ctx, const BetlStage *arr, size_t n,
                        int idx, unsigned char *state) {
    if (state[idx] == 2) return 0;
    if (state[idx] == 1) {
        const BetlStage *s = &arr[idx];
        err_at(ctx, s->line, s->column,
               "cycle detected in stage `after:` involving '%s'", s->id);
        return -1;
    }
    state[idx] = 1;
    const BetlStage *s = &arr[idx];
    for (size_t k = 0; k < s->after_count; ++k) {
        int nxt = find_stage_index_in(arr, n, s->after[k]);
        if (nxt >= 0 && dfs_stage_in(ctx, arr, n, nxt, state) != 0) return -1;
    }
    state[idx] = 2;
    return 0;
}

static int check_no_stage_cycles_in(Ctx *ctx, const BetlStage *arr, size_t n) {
    if (n == 0) return 0;
    unsigned char *state = calloc(n, 1);
    if (!state) return -1;
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; ++i) {
        rc = dfs_stage_in(ctx, arr, n, (int)i, state);
    }
    free(state);
    if (rc != 0) return rc;
    /* Recurse into each foreach body — each is its own scope. */
    for (size_t i = 0; i < n; ++i) {
        const BetlStage *s = &arr[i];
        if (s->kind == BETL_STAGE_FOREACH) {
            if (check_no_stage_cycles_in(ctx, s->children, s->child_count) != 0)
                return -1;
        }
    }
    return 0;
}

static int check_no_stage_cycles(Ctx *ctx, const BetlPipeline *p) {
    return check_no_stage_cycles_in(ctx, p->stages, p->stage_count);
}

/* Forward decl: defined further down (alongside dfs_step). */
static int check_no_step_cycles(Ctx *ctx, const BetlStage *s);

/* Walk every dataflow stage in the tree (top-level and inside foreach
 * bodies) and run the dataflow-specific validators on each. */
static int check_dataflow_stages(Ctx *ctx, const BetlStage *arr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const BetlStage *s = &arr[i];
        if (s->kind == BETL_STAGE_DATAFLOW) {
            if (check_unique_step_ids(ctx, s) != 0) return -1;
            if (check_from_refs(ctx, s) != 0)       return -1;
            if (check_no_step_cycles(ctx, s) != 0)  return -1;
        } else if (s->kind == BETL_STAGE_FOREACH) {
            if (check_dataflow_stages(ctx, s->children, s->child_count) != 0)
                return -1;
        }
    }
    return 0;
}

static int dfs_step(Ctx *ctx, const BetlStage *s, int idx,
                    unsigned char *state) {
    if (state[idx] == 2) return 0;
    if (state[idx] == 1) {
        const BetlDataflowStep *step = &s->steps[idx];
        err_at(ctx, step->line, step->column,
               "stage '%s': cycle in step `from:` involving '%s'",
               s->id, step->id);
        return -1;
    }
    state[idx] = 1;
    const BetlDataflowStep *step = &s->steps[idx];
    for (size_t k = 0; k < step->input_count; ++k) {
        int nxt = find_step_index_ref(s, step->inputs[k]);
        if (nxt >= 0 && dfs_step(ctx, s, nxt, state) != 0) return -1;
    }
    state[idx] = 2;
    return 0;
}

static int check_no_step_cycles(Ctx *ctx, const BetlStage *s) {
    if (s->step_count == 0) return 0;
    unsigned char *state = calloc(s->step_count, 1);
    if (!state) return -1;
    int rc = 0;
    for (size_t i = 0; i < s->step_count && rc == 0; ++i) {
        rc = dfs_step(ctx, s, (int)i, state);
    }
    free(state);
    return rc;
}

/* ============================================================== *
 *  Top-level orchestration                                         *
 * ============================================================== */

static int append_stage(BetlPipeline *p, const BetlStage *s) {
    if (p->stage_count == p->stage_cap) {
        size_t nc = p->stage_cap ? p->stage_cap * 2 : 4;
        BetlStage *arr = realloc(p->stages, nc * sizeof *arr);
        if (!arr) return -1;
        p->stages = arr;
        p->stage_cap = nc;
    }
    p->stages[p->stage_count++] = *s;
    return 0;
}

void betl_pipeline_free(BetlPipeline *p) {
    if (!p) return;
    free(p->name);
    free(p->description);
    for (size_t i = 0; i < p->stage_count; ++i) free_stage(&p->stages[i]);
    free(p->stages);
    for (size_t i = 0; i < p->connection_count; ++i) {
        free_connection(&p->connections[i]);
    }
    free(p->connections);
    for (size_t i = 0; i < p->parameter_count; ++i) {
        free_parameter(&p->parameters[i]);
    }
    free(p->parameters);
    free(p);
}

/* ============================================================== *
 *  Block parsers                                                   *
 *                                                                  *
 *  parse_connections_block / parse_pipeline_block extract the work *
 *  of reading those top-level keys so load_one can call them on    *
 *  the right document without code duplication.                    *
 * ============================================================== */

/* Parse a `connections:` mapping into p->connections, appending. A
 * collision with an already-loaded connection (whether from this file
 * or an earlier include) is reported as an error using the new
 * declaration's source location. */
static int parse_connections_block(Ctx *ctx, yaml_node_t *conns_n,
                                   BetlPipeline *p) {
    if (!is_map(conns_n)) {
        err_at(ctx, yaml_line(conns_n), yaml_col(conns_n),
               "`connections:` must be a mapping");
        return -1;
    }
    for (yaml_node_pair_t *pr = conns_n->data.mapping.pairs.start;
         pr < conns_n->data.mapping.pairs.top; ++pr) {
        yaml_node_t *key_n = node(ctx->doc, pr->key);
        yaml_node_t *val_n = node(ctx->doc, pr->value);
        const char  *name  = scalar(key_n);
        if (!name) {
            err_at(ctx, yaml_line(key_n), yaml_col(key_n),
                   "connection name must be a scalar");
            return -1;
        }
        for (size_t i = 0; i < p->connection_count; ++i) {
            if (strcmp(p->connections[i].name, name) == 0) {
                err_at(ctx, yaml_line(key_n), yaml_col(key_n),
                       "duplicate connection name '%s' "
                       "(already declared at line %d:%d)",
                       name,
                       p->connections[i].line, p->connections[i].column);
                return -1;
            }
        }
        BetlConnectionDecl c = {0};
        if (parse_connection(ctx, name,
                             yaml_line(key_n), yaml_col(key_n),
                             val_n, &c) != 0) {
            return -1;
        }
        if (append_connection(p, &c) != 0) {
            free_connection(&c);
            err_at(ctx, 0, 0, "out of memory appending connection");
            return -1;
        }
    }
    return 0;
}

/* Parse a `parameters:` mapping into p->parameters, appending. */
static int parse_parameters_block(Ctx *ctx, yaml_node_t *params_n,
                                  BetlPipeline *p) {
    if (!is_map(params_n)) {
        err_at(ctx, yaml_line(params_n), yaml_col(params_n),
               "`parameters:` must be a mapping");
        return -1;
    }
    for (yaml_node_pair_t *pr = params_n->data.mapping.pairs.start;
         pr < params_n->data.mapping.pairs.top; ++pr) {
        yaml_node_t *key_n = node(ctx->doc, pr->key);
        yaml_node_t *val_n = node(ctx->doc, pr->value);
        const char  *name  = scalar(key_n);
        if (!name) {
            err_at(ctx, yaml_line(key_n), yaml_col(key_n),
                   "parameter name must be a scalar");
            return -1;
        }
        for (size_t i = 0; i < p->parameter_count; ++i) {
            if (strcmp(p->parameters[i].name, name) == 0) {
                err_at(ctx, yaml_line(key_n), yaml_col(key_n),
                       "duplicate parameter name '%s' "
                       "(already declared at line %d:%d)",
                       name,
                       p->parameters[i].line, p->parameters[i].column);
                return -1;
            }
        }
        BetlParameterDecl pa = {0};
        if (parse_parameter(ctx, name,
                            yaml_line(key_n), yaml_col(key_n),
                            val_n, &pa) != 0) {
            return -1;
        }
        if (append_parameter(p, &pa) != 0) {
            free_parameter(&pa);
            err_at(ctx, 0, 0, "out of memory appending parameter");
            return -1;
        }
    }
    return 0;
}

/* Parse a `pipeline:` sequence into p->stages, appending. */
static int parse_pipeline_block(Ctx *ctx, yaml_node_t *pipeline_n,
                                BetlPipeline *p) {
    if (!is_seq(pipeline_n)) {
        err_at(ctx, yaml_line(pipeline_n), yaml_col(pipeline_n),
               "`pipeline:` must be a sequence");
        return -1;
    }
    for (yaml_node_item_t *it = pipeline_n->data.sequence.items.start;
         it < pipeline_n->data.sequence.items.top; ++it) {
        BetlStage stage = {0};
        if (parse_stage(ctx, node(ctx->doc, *it), &stage) != 0) {
            free_stage(&stage);
            return -1;
        }
        if (append_stage(p, &stage) != 0) {
            free_stage(&stage);
            err_at(ctx, 0, 0, "out of memory appending stage");
            return -1;
        }
    }
    return 0;
}

/* ============================================================== *
 *  Path set (used to track in-progress and completed includes)     *
 * ============================================================== */

typedef struct {
    char  **paths;
    size_t  count;
    size_t  cap;
} PathSet;

static int  path_set_contains(const PathSet *s, const char *path) {
    for (size_t i = 0; i < s->count; ++i) {
        if (strcmp(s->paths[i], path) == 0) return 1;
    }
    return 0;
}

static int  path_set_add(PathSet *s, const char *path) {
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4;
        char **arr = realloc(s->paths, nc * sizeof *arr);
        if (!arr) return -1;
        s->paths = arr;
        s->cap   = nc;
    }
    char *dup = strdup(path);
    if (!dup) return -1;
    s->paths[s->count++] = dup;
    return 0;
}

static void path_set_remove(PathSet *s, const char *path) {
    /* Order doesn't matter for membership tests, so swap-and-pop. */
    for (size_t i = 0; i < s->count; ++i) {
        if (strcmp(s->paths[i], path) == 0) {
            free(s->paths[i]);
            s->paths[i] = s->paths[--s->count];
            return;
        }
    }
}

static void path_set_free(PathSet *s) {
    for (size_t i = 0; i < s->count; ++i) free(s->paths[i]);
    free(s->paths);
    s->paths = NULL;
    s->count = s->cap = 0;
}

/* Resolve `rel` against the directory part of `base_abs` and canonicalize.
 * Writes the result to out (cap bytes). Returns 0 on success, -1 on
 * truncation or realpath failure. The base path must already be absolute. */
static int resolve_relative(const char *base_abs, const char *rel,
                            char *out, size_t cap) {
    char buf[PATH_MAX * 2];
    if (rel[0] == '/') {
        if ((size_t)snprintf(buf, sizeof buf, "%s", rel) >= sizeof buf) {
            return -1;
        }
    } else {
        const char *slash = strrchr(base_abs, '/');
        size_t dir_len = slash ? (size_t)(slash - base_abs + 1) : 0;
        if ((size_t)snprintf(buf, sizeof buf, "%.*s%s",
                             (int)dir_len, base_abs, rel) >= sizeof buf) {
            return -1;
        }
    }
    if (!realpath(buf, out)) return -1;
    return 0;
}

/* ============================================================== *
 *  Recursive loader                                                *
 *                                                                  *
 *  load_one parses one file into `p`. It opens, reads, validates   *
 *  the discriminator, recursively processes any `include:` paths   *
 *  first, then merges the file's connections / pipeline content.   *
 *                                                                  *
 *  `loading` is the in-progress set used for cycle detection;     *
 *  `loaded` is the completed set that makes diamond inclusion a    *
 *  silent no-op. The caller frees both.                            *
 * ============================================================== */

typedef enum {
    KIND_PIPELINE  = 1,    /* `betl: <ver>` */
    KIND_CONN_BUNDLE = 2   /* `betl_connections: <ver>` */
} FileKind;

static int load_one(BetlPipeline *p, const char *path,
                    int is_root,
                    PathSet *loading, PathSet *loaded,
                    Ctx *ctx);

static int process_includes(BetlPipeline *p, yaml_node_t *include_n,
                            const char *base_abs,
                            PathSet *loading, PathSet *loaded,
                            Ctx *ctx) {
    if (!is_seq(include_n)) {
        err_at(ctx, yaml_line(include_n), yaml_col(include_n),
               "`include:` must be a sequence");
        return -1;
    }
    for (yaml_node_item_t *it = include_n->data.sequence.items.start;
         it < include_n->data.sequence.items.top; ++it) {
        yaml_node_t *item = node(ctx->doc, *it);
        const char  *rel  = scalar(item);
        if (!rel) {
            err_at(ctx, yaml_line(item), yaml_col(item),
                   "`include:` items must be scalar paths");
            return -1;
        }
        char abs[PATH_MAX];
        if (resolve_relative(base_abs, rel, abs, sizeof abs) != 0) {
            err_at(ctx, yaml_line(item), yaml_col(item),
                   "include '%s': cannot resolve: %s", rel, strerror(errno));
            return -1;
        }
        if (load_one(p, abs, /*is_root=*/0, loading, loaded, ctx) != 0) {
            return -1;
        }
    }
    return 0;
}

static int load_one(BetlPipeline *p, const char *path,
                    int is_root,
                    PathSet *loading, PathSet *loaded,
                    Ctx *ctx) {
    /* Canonicalize for de-dup / cycle keys. */
    char abs[PATH_MAX];
    if (!realpath(path, abs)) {
        err_at(ctx, 0, 0, "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }

    if (path_set_contains(loading, abs)) {
        err_at(ctx, 0, 0, "include cycle detected at %s", abs);
        return -1;
    }
    if (path_set_contains(loaded, abs)) {
        return 0;     /* idempotent diamond inclusion */
    }
    if (path_set_add(loading, abs) != 0) {
        err_at(ctx, 0, 0, "out of memory tracking includes");
        return -1;
    }

    /* Switch ctx.path so any error inside this file's parse points at
     * the right file. Save the previous value to restore on exit. */
    const char *prev_path = ctx->path;
    yaml_document_t *prev_doc = ctx->doc;
    ctx->path = abs;
    ctx->doc  = NULL;

    int rc = -1;

    FILE *fp = fopen(abs, "rb");
    if (!fp) {
        err_at(ctx, 0, 0, "cannot open: %s", strerror(errno));
        goto out;
    }
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        err_at(ctx, 0, 0, "yaml_parser_initialize failed");
        goto out;
    }
    yaml_parser_set_input_file(&parser, fp);
    yaml_document_t doc;
    int ok = yaml_parser_load(&parser, &doc);
    if (!ok) {
        err_at(ctx,
               (int)parser.problem_mark.line + 1,
               (int)parser.problem_mark.column + 1,
               "YAML parse error: %s",
               parser.problem ? parser.problem : "(unknown)");
        yaml_parser_delete(&parser);
        fclose(fp);
        goto out;
    }
    yaml_parser_delete(&parser);
    fclose(fp);
    ctx->doc = &doc;

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!is_map(root)) {
        err_at(ctx, yaml_line(root), yaml_col(root),
               "top-level node must be a mapping");
        yaml_document_delete(&doc);
        goto out;
    }

    /* Discriminator: accept `betl:` (pipeline) or `betl_connections:`. */
    FileKind kind;
    yaml_node_t *betl_n  = map_get(&doc, root, "betl");
    yaml_node_t *bconn_n = map_get(&doc, root, "betl_connections");
    if (scalar(betl_n)) {
        kind = KIND_PIPELINE;
    } else if (scalar(bconn_n)) {
        kind = KIND_CONN_BUNDLE;
    } else {
        err_at(ctx, yaml_line(root), yaml_col(root),
               "missing required `betl:` or `betl_connections:` discriminator");
        yaml_document_delete(&doc);
        goto out;
    }

    /* Process includes first. Their content lands in `p` before we add
     * this file's connections / pipeline, so the parent file's local
     * declarations win when reporting source locations on collision. */
    yaml_node_t *include_n = map_get(&doc, root, "include");
    if (include_n) {
        if (process_includes(p, include_n, abs, loading, loaded, ctx) != 0) {
            yaml_document_delete(&doc);
            goto out;
        }
    }

    /* Top-level keys we may consume. Bundle files restrict the set. */
    if (kind == KIND_CONN_BUNDLE) {
        if (map_get(&doc, root, "pipeline") != NULL
         || map_get(&doc, root, "parameters") != NULL)
        {
            err_at(ctx, yaml_line(root), yaml_col(root),
                "`betl_connections:` bundle may only contribute connections");
            yaml_document_delete(&doc);
            goto out;
        }
    }

    /* Name / description: only the root pipeline file owns these. */
    if (is_root && kind == KIND_PIPELINE) {
        yaml_node_t *name_n = map_get(&doc, root, "name");
        yaml_node_t *desc_n = map_get(&doc, root, "description");
        if (scalar(name_n)) p->name = strdup(scalar(name_n));
        if (scalar(desc_n)) p->description = strdup(scalar(desc_n));
    }

    yaml_node_t *conns_n = map_get(&doc, root, "connections");
    if (conns_n && parse_connections_block(ctx, conns_n, p) != 0) {
        yaml_document_delete(&doc);
        goto out;
    }

    if (kind == KIND_PIPELINE) {
        yaml_node_t *params_n = map_get(&doc, root, "parameters");
        if (params_n && parse_parameters_block(ctx, params_n, p) != 0) {
            yaml_document_delete(&doc);
            goto out;
        }
        yaml_node_t *pipeline_n = map_get(&doc, root, "pipeline");
        /* Only the root file is required to have a pipeline; an
         * included pipeline file may legally contain just connections,
         * just stages, or both. */
        if (is_root && !pipeline_n) {
            err_at(ctx, yaml_line(root), yaml_col(root),
                   "missing required `pipeline:` sequence");
            yaml_document_delete(&doc);
            goto out;
        }
        if (pipeline_n
            && parse_pipeline_block(ctx, pipeline_n, p) != 0)
        {
            yaml_document_delete(&doc);
            goto out;
        }
    }

    yaml_document_delete(&doc);
    rc = 0;

out:
    ctx->doc  = prev_doc;
    ctx->path = prev_path;
    path_set_remove(loading, abs);
    if (rc == 0) {
        if (path_set_add(loaded, abs) != 0) {
            err_at(ctx, 0, 0, "out of memory tracking loaded files");
            return -1;
        }
    }
    return rc;
}

BetlPipeline *betl_pipeline_load(const char *path,
                                 char *err_buf, size_t err_cap) {
    if (err_buf && err_cap > 0) err_buf[0] = '\0';

    Ctx ctx = { .doc = NULL, .path = path, .err = err_buf, .err_cap = err_cap };

    BetlPipeline *p = calloc(1, sizeof *p);
    if (!p) {
        err_at(&ctx, 0, 0, "out of memory");
        return NULL;
    }

    PathSet loading = {0}, loaded = {0};
    int rc = load_one(p, path, /*is_root=*/1, &loading, &loaded, &ctx);
    path_set_free(&loading);
    path_set_free(&loaded);

    if (rc != 0) {
        betl_pipeline_free(p);
        return NULL;
    }

    if (p->stage_count == 0) {
        err_at(&ctx, 0, 0, "pipeline has no stages");
        betl_pipeline_free(p);
        return NULL;
    }

    /* Cross-ref + cycle checks. The yaml doc is freed by now; `ctx.doc`
     * is NULL but errors only need line/col we already captured. */
    if (check_unique_stage_ids(&ctx, p) != 0) {
        betl_pipeline_free(p);
        return NULL;
    }
    if (check_after_refs(&ctx, p) != 0) {
        betl_pipeline_free(p);
        return NULL;
    }
    if (check_no_stage_cycles(&ctx, p) != 0) {
        betl_pipeline_free(p);
        return NULL;
    }
    if (check_dataflow_stages(&ctx, p->stages, p->stage_count) != 0) {
        betl_pipeline_free(p);
        return NULL;
    }
    return p;
}

/* ============================================================== *
 *  Public accessors                                                *
 * ============================================================== */

const char *betl_pipeline_name(const BetlPipeline *p) {
    return p ? p->name : NULL;
}
const char *betl_pipeline_description(const BetlPipeline *p) {
    return p ? p->description : NULL;
}
size_t betl_pipeline_stage_count(const BetlPipeline *p) {
    return p ? p->stage_count : 0;
}
const BetlStage *betl_pipeline_stage(const BetlPipeline *p, size_t i) {
    return (p && i < p->stage_count) ? &p->stages[i] : NULL;
}
const BetlStage *betl_pipeline_find_stage(const BetlPipeline *p,
                                          const char *id) {
    if (!p || !id) return NULL;
    for (size_t i = 0; i < p->stage_count; ++i) {
        if (strcmp(p->stages[i].id, id) == 0) return &p->stages[i];
    }
    return NULL;
}
static size_t total_steps_in(const BetlStage *arr, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        total += arr[i].step_count;
        if (arr[i].kind == BETL_STAGE_FOREACH) {
            total += total_steps_in(arr[i].children, arr[i].child_count);
        }
    }
    return total;
}

size_t betl_pipeline_total_steps(const BetlPipeline *p) {
    if (!p) return 0;
    return total_steps_in(p->stages, p->stage_count);
}

size_t betl_pipeline_connection_count(const BetlPipeline *p) {
    return p ? p->connection_count : 0;
}
const BetlConnectionDecl *betl_pipeline_connection(const BetlPipeline *p,
                                                   size_t i) {
    return (p && i < p->connection_count) ? &p->connections[i] : NULL;
}

size_t betl_pipeline_parameter_count(const BetlPipeline *p) {
    return p ? p->parameter_count : 0;
}
const BetlParameterDecl *betl_pipeline_parameter(const BetlPipeline *p,
                                                 size_t i) {
    return (p && i < p->parameter_count) ? &p->parameters[i] : NULL;
}
