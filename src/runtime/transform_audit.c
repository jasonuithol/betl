/* `audit` TRANSFORM — passes input rows through, appending constant
 * columns. SSIS Audit-component parity.
 *
 * Config:
 *   columns:
 *     <out_name>: <string-or-number-literal>
 *
 * Each output column gets the same scalar value broadcast across the
 * batch. Column types are inferred:
 *
 *   - JSON-number literals →  int64 (`l`)   if no decimal point
 *                          →  float64 (`g`) otherwise
 *   - JSON-string literals →  utf8 (`u`)
 *
 * Values that need to vary per run (hostname, pipeline name, run id,
 * start time) come from the YAML loader via ${env.X} / ${params.X}
 * substitution — by the time init() runs the config JSON contains
 * resolved string literals.
 *
 * Output schema = input columns + audit columns (in that order).
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

typedef enum {
    AUDIT_INT64   = 1,
    AUDIT_FLOAT64 = 2,
    AUDIT_UTF8    = 3,
} AuditValKind;

typedef struct {
    char        *name;       /* output column name (strdup) */
    AuditValKind kind;
    int64_t      i64;
    double       f64;
    char        *str;        /* utf8 payload (strdup), NULL otherwise */
    size_t       str_len;
} AuditCol;

typedef struct {
    BetlContext *ctx;
    struct ArrowArrayStream input;
    int                     have_input;

    AuditCol *cols;
    size_t    n_cols;

    int      schema_cached;
    size_t   n_input_cols;
    struct ArrowSchema input_schema;
    int                input_schema_owned;

    char     last_err[256];
} AuditState;

static void aset_err(AuditState *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->last_err, sizeof a->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(a->ctx, "%s", a->last_err);
}

/* ============================================================== *
 *  Config parsing                                                  *
 * ============================================================== */

typedef struct { AuditState *a; int err; } AuditCtx;

static int audit_visit(const char *key, const char *value, size_t value_len,
                       void *user) {
    AuditCtx *c = user;
    AuditState *a = c->a;
    if (value_len == 0) {
        aset_err(a, "audit: column '%s' has no value", key);
        c->err = 1; return -1;
    }

    AuditCol *grow = realloc(a->cols, (a->n_cols + 1) * sizeof *grow);
    if (!grow) { c->err = 1; return -1; }
    a->cols = grow;
    AuditCol *col = &a->cols[a->n_cols++];
    memset(col, 0, sizeof *col);
    col->name = strdup(key);
    if (!col->name) { c->err = 1; return -1; }

    if (value[0] == '"') {
        char *vbuf = malloc(value_len + 1);
        if (!vbuf) { c->err = 1; return -1; }
        memcpy(vbuf, value, value_len);
        vbuf[value_len] = '\0';
        char *decoded = NULL;
        if (betl_tx_json_decode_str(vbuf, &decoded) != 0 || !decoded) {
            free(vbuf);
            aset_err(a, "audit: column '%s' malformed string literal", key);
            c->err = 1; return -1;
        }
        free(vbuf);
        col->kind    = AUDIT_UTF8;
        col->str     = decoded;
        col->str_len = strlen(decoded);
        return 0;
    }

    /* JSON number — copy into a NUL-terminated buffer and parse. */
    char numbuf[64];
    if (value_len >= sizeof numbuf) {
        aset_err(a, "audit: column '%s' number literal too long", key);
        c->err = 1; return -1;
    }
    memcpy(numbuf, value, value_len);
    numbuf[value_len] = '\0';

    int is_float = 0;
    for (size_t i = 0; i < value_len; ++i) {
        char ch = numbuf[i];
        if (ch == '.' || ch == 'e' || ch == 'E') { is_float = 1; break; }
    }

    char *end = NULL;
    if (is_float) {
        double v = strtod(numbuf, &end);
        if (end == numbuf || *end != '\0') {
            aset_err(a, "audit: column '%s' bad number '%s'", key, numbuf);
            c->err = 1; return -1;
        }
        col->kind = AUDIT_FLOAT64;
        col->f64  = v;
    } else {
        long long v = strtoll(numbuf, &end, 10);
        if (end == numbuf || *end != '\0') {
            aset_err(a, "audit: column '%s' bad integer '%s'", key, numbuf);
            c->err = 1; return -1;
        }
        col->kind = AUDIT_INT64;
        col->i64  = (int64_t)v;
    }
    return 0;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int audit_init(BetlContext *ctx, const char *cfg, void **state) {
    AuditState *a = calloc(1, sizeof *a);
    if (!a) return BETL_ERR_INTERNAL;
    a->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    const char *cols = betl_tx_json_value_after(cfg, "columns");
    if (!cols || *cols != '{') {
        aset_err(a, "audit: requires a `columns:` mapping");
        free(a);
        return BETL_ERR_INVALID;
    }
    AuditCtx c = { .a = a, .err = 0 };
    if (betl_tx_json_walk_object(cols, audit_visit, &c) != 0 || c.err) {
        for (size_t i = 0; i < a->n_cols; ++i) {
            free(a->cols[i].name); free(a->cols[i].str);
        }
        free(a->cols); free(a);
        return BETL_ERR_INVALID;
    }
    if (a->n_cols == 0) {
        aset_err(a, "audit: `columns:` is empty");
        free(a);
        return BETL_ERR_INVALID;
    }

    *state = a;
    return BETL_OK;
}

static int audit_attach_input(void *state, int port,
                              struct ArrowArrayStream *in) {
    (void)port;
    AuditState *a = state;
    a->input      = *in;
    a->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void audit_destroy(void *state) {
    if (!state) return;
    AuditState *a = state;
    if (a->have_input && a->input.release) a->input.release(&a->input);
    if (a->input_schema_owned && a->input_schema.release) {
        a->input_schema.release(&a->input_schema);
    }
    for (size_t i = 0; i < a->n_cols; ++i) {
        free(a->cols[i].name); free(a->cols[i].str);
    }
    free(a->cols);
    free(a);
}

static int audit_ensure_schema(AuditState *a) {
    if (a->schema_cached) return 0;
    if (!a->have_input || !a->input.get_schema) {
        aset_err(a, "audit: input has no get_schema");
        return -1;
    }
    if (a->input.get_schema(&a->input, &a->input_schema) != 0) {
        aset_err(a, "audit: upstream get_schema failed");
        return -1;
    }
    a->input_schema_owned = 1;
    if (!a->input_schema.format || strcmp(a->input_schema.format, "+s") != 0) {
        aset_err(a, "audit: input must be a struct array");
        return -1;
    }
    a->n_input_cols = (size_t)a->input_schema.n_children;
    a->schema_cached = 1;
    return 0;
}

/* ============================================================== *
 *  Audit-column leaf builders (constant-broadcast)                 *
 * ============================================================== */

static const char *audit_format_for(AuditValKind k) {
    switch (k) {
        case AUDIT_INT64:   return "l";
        case AUDIT_FLOAT64: return "g";
        case AUDIT_UTF8:    return "u";
    }
    return "u";
}

/* Build an int64/float64 leaf of `n_rows` rows, each set to the same
 * 8-byte scalar value. */
static int build_fixed8_const(struct ArrowArray *out, int64_t n_rows,
                              const void *scalar8) {
    size_t bytes = (size_t)(n_rows > 0 ? n_rows : 1) * 8;
    uint8_t *vals = malloc(bytes);
    if (!vals) return -1;
    for (int64_t i = 0; i < n_rows; ++i) {
        memcpy(vals + (size_t)i * 8, scalar8, 8);
    }
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); return -1; }
    bufs[0] = NULL;
    bufs[1] = vals;
    out->length     = n_rows;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = betl_tx_release_int64_leaf;
    return 0;
}

/* Build a utf8 leaf of `n_rows` rows, each set to the same string `s`
 * (length `slen`, may be empty). */
static int build_utf8_const(struct ArrowArray *out, int64_t n_rows,
                            const char *s, size_t slen) {
    size_t total = slen * (size_t)(n_rows > 0 ? n_rows : 0);
    int32_t *offs = malloc((size_t)(n_rows + 1) * sizeof *offs);
    char    *data = malloc(total ? total : 1);
    if (!offs || !data) { free(offs); free(data); return -1; }
    offs[0] = 0;
    for (int64_t i = 0; i < n_rows; ++i) {
        if (slen) memcpy(data + (size_t)i * slen, s, slen);
        offs[i + 1] = (int32_t)((i + 1) * slen);
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); return -1; }
    bufs[0] = NULL;
    bufs[1] = offs;
    bufs[2] = data;
    out->length     = n_rows;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 3;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = betl_tx_release_utf8_leaf;
    return 0;
}

static int build_audit_leaf(struct ArrowArray *out, int64_t n_rows,
                            const AuditCol *col) {
    switch (col->kind) {
        case AUDIT_INT64:
            return build_fixed8_const(out, n_rows, &col->i64);
        case AUDIT_FLOAT64:
            return build_fixed8_const(out, n_rows, &col->f64);
        case AUDIT_UTF8:
            return build_utf8_const(out, n_rows,
                                    col->str ? col->str : "",
                                    col->str_len);
    }
    return -1;
}

/* ============================================================== *
 *  Stream                                                          *
 * ============================================================== */

static int aud_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    AuditState *a = st->private_data;
    if (!a) return EINVAL;
    if (audit_ensure_schema(a) != 0) return EIO;
    memset(out, 0, sizeof *out);

    struct ArrowSchema up = {0};
    if (a->input.get_schema(&a->input, &up) != 0) {
        aset_err(a, "audit: upstream get_schema failed");
        return EIO;
    }
    size_t n_total = a->n_input_cols + a->n_cols;
    struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
    if (!kids) { if (up.release) up.release(&up); return ENOMEM; }

    for (size_t i = 0; i < a->n_input_cols; ++i) {
        kids[i] = up.children[i];
        up.children[i] = NULL;
    }
    if (up.release) up.release(&up);

    for (size_t j = 0; j < a->n_cols; ++j) {
        const char *fmt = audit_format_for(a->cols[j].kind);
        struct ArrowSchema *c = betl_tx_new_leaf_schema(a->cols[j].name, fmt);
        if (!c) {
            for (size_t k = 0; k < a->n_input_cols + j; ++k) {
                if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
        kids[a->n_input_cols + j] = c;
    }

    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = betl_tx_release_schema_struct_owned;
    return 0;
}

static void free_kids_partial(struct ArrowArray **kids, size_t k, size_t n) {
    for (size_t i = k; i < n; ++i) {
        if (kids[i] && kids[i]->release) kids[i]->release(kids[i]);
        free(kids[i]);
    }
    free(kids);
}

static int aud_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    AuditState *a = st->private_data;
    memset(out, 0, sizeof *out);
    if (!a) return EINVAL;
    if (audit_ensure_schema(a) != 0) return EIO;

    struct ArrowArray in_arr = {0};
    if (a->input.get_next(&a->input, &in_arr) != 0) {
        const char *e = a->input.get_last_error
                            ? a->input.get_last_error(&a->input) : NULL;
        aset_err(a, "audit: upstream get_next failed: %s",
                 e ? e : "(no detail)");
        return EIO;
    }
    if (!in_arr.release) return 0;   /* EOF */

    if (in_arr.n_children != (int64_t)a->n_input_cols) {
        long long got = (long long)in_arr.n_children;
        in_arr.release(&in_arr);
        aset_err(a, "audit: upstream batch has %lld cols, expected %zu",
                 got, a->n_input_cols);
        return EIO;
    }

    size_t n_total = a->n_input_cols + a->n_cols;
    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        in_arr.release(&in_arr);
        aset_err(a, "audit: out of memory");
        return EIO;
    }

    /* Take ownership of upstream leaves. */
    for (size_t i = 0; i < a->n_input_cols; ++i) {
        kids[i] = in_arr.children[i];
        in_arr.children[i] = NULL;
    }

    /* Build constant audit leaves. */
    for (size_t j = 0; j < a->n_cols; ++j) {
        struct ArrowArray *leaf = calloc(1, sizeof *leaf);
        if (!leaf) {
            free_kids_partial(kids, 0, a->n_input_cols + j);
            in_arr.release(&in_arr);
            aset_err(a, "audit: out of memory");
            return EIO;
        }
        if (build_audit_leaf(leaf, in_arr.length, &a->cols[j]) != 0) {
            free(leaf);
            free_kids_partial(kids, 0, a->n_input_cols + j);
            in_arr.release(&in_arr);
            aset_err(a, "audit: failed to build column '%s'", a->cols[j].name);
            return EIO;
        }
        kids[a->n_input_cols + j] = leaf;
    }

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        free_kids_partial(kids, 0, n_total);
        in_arr.release(&in_arr);
        aset_err(a, "audit: out of memory");
        return EIO;
    }
    outer[0] = NULL;

    out->length     = in_arr.length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;

    in_arr.release(&in_arr);
    return 0;
}

static const char *aud_get_last_error(struct ArrowArrayStream *st) {
    AuditState *a = st->private_data;
    return (a && a->last_err[0]) ? a->last_err : NULL;
}

static void aud_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int audit_attach_output(void *state, int port,
                               struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = aud_get_schema;
    out->get_next       = aud_get_next;
    out->get_last_error = aud_get_last_error;
    out->release        = aud_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef audit_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "any rows" },
};
static const BetlPortDef audit_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "in + constant audit cols" },
};

static const BetlComponentDef audit_components[] = {
    { .name               = "audit",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = audit_inputs,
      .input_count        = 1,
      .outputs            = audit_outputs,
      .output_count       = 1,
      .init               = audit_init,
      .destroy            = audit_destroy,
      .attach_input       = audit_attach_input,
      .attach_output      = audit_attach_output },
};

static const BetlProvider audit_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-audit",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = audit_components,
    .component_count = sizeof audit_components / sizeof audit_components[0],
};

int betl_tx_register_audit(BetlRegistry *r) {
    return betl_registry_register(r, &audit_provider, "<builtin:audit>");
}
