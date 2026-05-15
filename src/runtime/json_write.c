/* json.write — SINK that emits incoming rows as NDJSON or a JSON
 * array of objects.
 *
 * Config:
 *   path   string, required
 *   format "ndjson" (default) | "array"
 *
 * Supported input types (Arrow format strings):
 *   "u"  utf8        → JSON string
 *   "l"  int64       → JSON number
 *   "i"  int32
 *   "s"  int16
 *   "c"  int8
 *   "g"  float64     → JSON number (special cases NaN/Inf as `null`)
 *   "b"  bool        → JSON true / false (bit-packed input)
 *
 * Other Arrow types are rejected at schema attach with a clear error
 * (use `map` + `ssisexpr` to coerce upstream).
 */

#include "runtime/json_write.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"

typedef struct {
    BetlContext *ctx;
    char        *path;
    int          ndjson;            /* 1 = NDJSON, 0 = array */
    FILE        *fp;
    struct ArrowArrayStream input;
    int          have_input;
    int64_t      rows_written;
    char         last_err[400];
} JwState;

static void jwset_err(JwState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* Same JSON string-extractor as the other components. */
static int json_string_at(const char *json, const char *key, char **out) {
    *out = NULL;
    if (!json) return -1;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof needle) return -1;
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return -1;
    ++p;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, p, len);
    s[len] = '\0';
    *out = s;
    return 0;
}

static int jw_init(BetlContext *ctx, const char *cfg, void **state) {
    JwState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx    = ctx;
    s->ndjson = 1;
    cfg = cfg ? cfg : "{}";

    if (json_string_at(cfg, "path", &s->path) != 0 || !s->path) {
        jwset_err(s, "json.write: missing required `path`");
        free(s); return BETL_ERR_INVALID;
    }
    char *fmt = NULL;
    if (json_string_at(cfg, "format", &fmt) == 0 && fmt) {
        if (strcmp(fmt, "ndjson") == 0) s->ndjson = 1;
        else if (strcmp(fmt, "array") == 0) s->ndjson = 0;
        else {
            jwset_err(s, "json.write: `format:` must be \"ndjson\" or "
                         "\"array\" (got '%s')", fmt);
            free(fmt); free(s->path); free(s);
            return BETL_ERR_INVALID;
        }
        free(fmt);
    }
    *state = s;
    return BETL_OK;
}

static void jw_destroy(void *state) {
    if (!state) return;
    JwState *s = state;
    if (s->fp) fclose(s->fp);
    if (s->have_input && s->input.release) s->input.release(&s->input);
    free(s->path);
    free(s);
}

static int jw_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    JwState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* Validity helper. */
static int is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* JSON-escape `len` bytes from `data` to `fp`. Surrounds with quotes. */
static int json_emit_string(FILE *fp, const char *data, size_t len) {
    if (fputc('"', fp) == EOF) return -1;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '"':  if (fputs("\\\"", fp) == EOF) return -1; break;
        case '\\': if (fputs("\\\\", fp) == EOF) return -1; break;
        case '\b': if (fputs("\\b",  fp) == EOF) return -1; break;
        case '\f': if (fputs("\\f",  fp) == EOF) return -1; break;
        case '\n': if (fputs("\\n",  fp) == EOF) return -1; break;
        case '\r': if (fputs("\\r",  fp) == EOF) return -1; break;
        case '\t': if (fputs("\\t",  fp) == EOF) return -1; break;
        default:
            if (c < 0x20) {
                if (fprintf(fp, "\\u%04x", c) < 0) return -1;
            } else {
                if (fputc((char)c, fp) == EOF) return -1;
            }
        }
    }
    if (fputc('"', fp) == EOF) return -1;
    return 0;
}

static int render_cell(JwState *s, FILE *fp,
                       const struct ArrowArray *col,
                       const char *fmt, int64_t row) {
    if (is_null(col, row)) {
        return fputs("null", fp) == EOF ? -1 : 0;
    }
    int64_t off = col->offset + row;
    if (strcmp(fmt, "u") == 0) {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        return json_emit_string(fp, data + start, (size_t)(end - start));
    }
    if (strcmp(fmt, "l") == 0) {
        const int64_t *vals = col->buffers[1];
        return fprintf(fp, "%" PRId64, vals[off]) < 0 ? -1 : 0;
    }
    if (strcmp(fmt, "i") == 0) {
        const int32_t *vals = col->buffers[1];
        return fprintf(fp, "%" PRId32, vals[off]) < 0 ? -1 : 0;
    }
    if (strcmp(fmt, "s") == 0) {
        const int16_t *vals = col->buffers[1];
        return fprintf(fp, "%" PRId16, vals[off]) < 0 ? -1 : 0;
    }
    if (strcmp(fmt, "c") == 0) {
        const int8_t *vals = col->buffers[1];
        return fprintf(fp, "%" PRId8, vals[off]) < 0 ? -1 : 0;
    }
    if (strcmp(fmt, "g") == 0) {
        double v = ((const double *)col->buffers[1])[off];
        /* NaN/Inf aren't representable in JSON — emit null. */
        if (!isfinite(v)) {
            return fputs("null", fp) == EOF ? -1 : 0;
        }
        return fprintf(fp, "%.17g", v) < 0 ? -1 : 0;
    }
    if (strcmp(fmt, "b") == 0) {
        const uint8_t *bits = col->buffers[1];
        int v = (bits[off / 8] >> (off % 8)) & 1;
        return fputs(v ? "true" : "false", fp) == EOF ? -1 : 0;
    }
    jwset_err(s, "json.write: unsupported Arrow type '%s' (supported: "
                 "u/l/i/s/c/g/b)", fmt);
    return -2;
}

static int jw_sink_run(void *state) {
    JwState *s = state;
    if (!s->have_input) {
        jwset_err(s, "json.write: sink_run with no input attached");
        return BETL_ERR_INVALID;
    }

    struct ArrowSchema schema = {0};
    if (s->input.get_schema(&s->input, &schema) != 0) {
        jwset_err(s, "json.write: get_schema failed: %s",
                  s->input.get_last_error
                      ? s->input.get_last_error(&s->input)
                      : "(no detail)");
        return BETL_ERR_IO;
    }

    /* Copy column names + formats so we don't depend on schema living
     * past its release. */
    int64_t n_cols = schema.n_children;
    char  **names = calloc((size_t)n_cols, sizeof *names);
    char  **fmts  = calloc((size_t)n_cols, sizeof *fmts);
    if (!names || !fmts) {
        free(names); free(fmts);
        if (schema.release) schema.release(&schema);
        return BETL_ERR_INTERNAL;
    }
    for (int64_t c = 0; c < n_cols; ++c) {
        names[c] = strdup(schema.children[c]->name   ? schema.children[c]->name   : "");
        fmts[c]  = strdup(schema.children[c]->format ? schema.children[c]->format : "");
    }
    if (schema.release) schema.release(&schema);

    s->fp = fopen(s->path, "w");
    if (!s->fp) {
        for (int64_t c = 0; c < n_cols; ++c) { free(names[c]); free(fmts[c]); }
        free(names); free(fmts);
        jwset_err(s, "json.write: open(%s): %s", s->path, strerror(errno));
        return BETL_ERR_IO;
    }

    if (!s->ndjson) {
        if (fputc('[', s->fp) == EOF) goto io_err;
    }

    int rc = BETL_OK;
    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            jwset_err(s, "json.write: cancelled");
            rc = BETL_ERR_CANCELLED;
            break;
        }
        struct ArrowArray arr = {0};
        if (s->input.get_next(&s->input, &arr) != 0) {
            jwset_err(s, "json.write: get_next failed: %s",
                      s->input.get_last_error
                          ? s->input.get_last_error(&s->input)
                          : "(no detail)");
            rc = BETL_ERR_IO;
            break;
        }
        if (arr.release == NULL) break;     /* end-of-stream */
        int64_t n_rows = arr.length;
        for (int64_t r = 0; r < n_rows; ++r) {
            if (!s->ndjson && s->rows_written > 0) {
                if (fputc(',', s->fp) == EOF) { arr.release(&arr); goto io_err; }
            }
            if (s->ndjson && s->rows_written > 0) {
                if (fputc('\n', s->fp) == EOF) { arr.release(&arr); goto io_err; }
            }
            if (fputc('{', s->fp) == EOF) { arr.release(&arr); goto io_err; }
            for (int64_t c = 0; c < n_cols; ++c) {
                if (c > 0 && fputc(',', s->fp) == EOF) {
                    arr.release(&arr); goto io_err;
                }
                if (json_emit_string(s->fp, names[c], strlen(names[c])) != 0) {
                    arr.release(&arr); goto io_err;
                }
                if (fputc(':', s->fp) == EOF) { arr.release(&arr); goto io_err; }
                int cell_rc = render_cell(s, s->fp, arr.children[c], fmts[c], r);
                if (cell_rc != 0) {
                    arr.release(&arr);
                    rc = (cell_rc == -2) ? BETL_ERR_TYPE : BETL_ERR_IO;
                    goto out;
                }
            }
            if (fputc('}', s->fp) == EOF) { arr.release(&arr); goto io_err; }
            ++s->rows_written;
        }
        arr.release(&arr);
    }

    if (!s->ndjson) {
        if (fputc(']', s->fp) == EOF) goto io_err;
    }
    if (s->ndjson && s->rows_written > 0) {
        if (fputc('\n', s->fp) == EOF) goto io_err;
    }
    fflush(s->fp);
    goto out;

io_err:
    jwset_err(s, "json.write: write to %s failed: %s",
              s->path, strerror(errno));
    rc = BETL_ERR_IO;
out:
    for (int64_t c = 0; c < n_cols; ++c) { free(names[c]); free(fmts[c]); }
    free(names); free(fmts);
    return rc;
}

static const char *jw_get_last_error_state(BetlContext *ctx, void *state) {
    (void)ctx;
    JwState *s = state;
    return s ? s->last_err : NULL;
}

static const BetlPortDef jw_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows to serialize; column names become JSON keys" },
};

static const BetlComponentDef jw_components[] = {
    { .name               = "json.write",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = jw_inputs,
      .input_count        = 1,
      .init               = jw_init,
      .destroy            = jw_destroy,
      .attach_input       = jw_attach_input,
      .sink_run           = jw_sink_run },
};

static const BetlProvider jw_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-json-write",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = jw_components,
    .component_count = sizeof jw_components / sizeof jw_components[0],
};

int betl_register_json_write(BetlRegistry *r) {
    (void)jw_get_last_error_state;
    return betl_registry_register(r, &jw_provider, "<builtin:json-write>");
}
