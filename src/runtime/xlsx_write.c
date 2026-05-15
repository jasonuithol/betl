/* xlsx.write — SINK that writes Arrow batches to a single-sheet
 * .xlsx file via libxlsxwriter. SSIS Excel Destination parity.
 *
 * Config:
 *   path        string, required — output .xlsx path
 *   sheet       string, optional — worksheet name (default "Sheet1")
 *   header      bool,   optional — write column names as row 0 (default true)
 *
 * Type coverage v0.1: int16/int32/int64 (`s`/`i`/`l`) + uint variants,
 * float32/float64 (`f`/`g`), utf8 (`u`), bool (`b`). Other Arrow
 * types return BETL_ERR_UNSUPPORTED — stringify via `map` first.
 *
 * Excel's per-sheet row limit is 1,048,576. libxlsxwriter returns an
 * error if we exceed it; we propagate that as BETL_ERR_IO with the
 * error text. */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xlsxwriter.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/transforms_internal.h"
#include "runtime/xlsx_write.h"

typedef enum {
    XW_INT64   = 1,    /* s/i/l + uint widths, all written via double */
    XW_FLOAT64 = 2,
    XW_UTF8    = 3,
    XW_BOOL    = 4,
} XwColType;

typedef struct {
    XwColType type;
    char       fmt_char;    /* original Arrow format first char — used to
                             * pick the right integer width buffer at
                             * read time */
} XwCol;

typedef struct {
    BetlContext *ctx;

    char        *path;
    char        *sheet_name;
    int          write_header;

    struct ArrowArrayStream input;
    int                     have_input;

    char         last_err[400];
} XwState;

static void xwset_err(XwState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* ============================================================== *
 *  Config + lifecycle                                              *
 * ============================================================== */

static int xw_init(BetlContext *ctx, const char *cfg, void **state) {
    XwState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->write_header = 1;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "path", &s->path) != 0 || !s->path) {
        xwset_err(s, "xlsx.write: missing required `path`");
        free(s); return BETL_ERR_INVALID;
    }
    /* Optional sheet name. */
    if (betl_tx_json_string_at(cfg, "sheet", &s->sheet_name) != 0) {
        s->sheet_name = NULL;
    }
    /* Optional header flag. */
    {
        char *hdr = NULL;
        if (betl_tx_json_value_to_string(cfg, "header", &hdr) == 0 && hdr) {
            s->write_header = (strcmp(hdr, "false") != 0);
            free(hdr);
        }
    }

    *state = s;
    return BETL_OK;
}

static int xw_attach_input(void *state, int port,
                           struct ArrowArrayStream *in) {
    (void)port;
    XwState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void xw_destroy(void *state) {
    if (!state) return;
    XwState *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    free(s->path);
    free(s->sheet_name);
    free(s);
}

/* ============================================================== *
 *  Type mapping                                                    *
 * ============================================================== */

static XwColType fmt_to_xwtype(const char *fmt) {
    if (!fmt) return 0;
    if (strcmp(fmt, "l") == 0) return XW_INT64;
    if (strcmp(fmt, "L") == 0) return XW_INT64;
    if (strcmp(fmt, "i") == 0) return XW_INT64;
    if (strcmp(fmt, "I") == 0) return XW_INT64;
    if (strcmp(fmt, "s") == 0) return XW_INT64;
    if (strcmp(fmt, "S") == 0) return XW_INT64;
    if (strcmp(fmt, "c") == 0) return XW_INT64;
    if (strcmp(fmt, "C") == 0) return XW_INT64;
    if (strcmp(fmt, "g") == 0) return XW_FLOAT64;
    if (strcmp(fmt, "f") == 0) return XW_FLOAT64;
    if (strcmp(fmt, "u") == 0) return XW_UTF8;
    if (strcmp(fmt, "b") == 0) return XW_BOOL;
    return 0;
}

static int validity_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Read a fixed-width primitive value as double — Excel only stores
 * doubles, so widening here is safe (up to int64; values larger than
 * 2^53 lose precision but that's a known Excel limit, not ours). */
static double read_numeric_as_double(const struct ArrowArray *col,
                                     int64_t row, char fmt_char) {
    int64_t off = col->offset + row;
    switch (fmt_char) {
    case 'l': return (double)((const int64_t  *)col->buffers[1])[off];
    case 'L': return (double)((const uint64_t *)col->buffers[1])[off];
    case 'i': return (double)((const int32_t  *)col->buffers[1])[off];
    case 'I': return (double)((const uint32_t *)col->buffers[1])[off];
    case 's': return (double)((const int16_t  *)col->buffers[1])[off];
    case 'S': return (double)((const uint16_t *)col->buffers[1])[off];
    case 'c': return (double)((const int8_t   *)col->buffers[1])[off];
    case 'C': return (double)((const uint8_t  *)col->buffers[1])[off];
    case 'g': return         ((const double   *)col->buffers[1])[off];
    case 'f': return (double)((const float    *)col->buffers[1])[off];
    }
    return 0.0;
}

/* ============================================================== *
 *  sink_run                                                        *
 * ============================================================== */

static int xw_sink_run(void *state) {
    XwState *s = state;
    if (!s->have_input) {
        xwset_err(s, "xlsx.write: sink_run without attached input");
        return BETL_ERR_INVALID;
    }

    struct ArrowSchema schema = {0};
    if (s->input.get_schema(&s->input, &schema) != 0) {
        xwset_err(s, "xlsx.write: upstream get_schema failed");
        return BETL_ERR_IO;
    }
    if (!schema.format || strcmp(schema.format, "+s") != 0
        || schema.n_children <= 0) {
        xwset_err(s, "xlsx.write: input must be a non-empty struct stream");
        if (schema.release) schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    size_t n_cols = (size_t)schema.n_children;

    XwCol *cols = calloc(n_cols, sizeof *cols);
    if (!cols) {
        if (schema.release) schema.release(&schema);
        return BETL_ERR_INTERNAL;
    }
    for (size_t i = 0; i < n_cols; ++i) {
        const char *fmt = schema.children[i]->format;
        XwColType t = fmt_to_xwtype(fmt);
        if (t == 0) {
            xwset_err(s, "xlsx.write: column '%s' has unsupported Arrow type "
                         "'%s' (v0.1 supports integer/float/utf8/bool)",
                      schema.children[i]->name, fmt ? fmt : "(none)");
            free(cols);
            if (schema.release) schema.release(&schema);
            return BETL_ERR_UNSUPPORTED;
        }
        cols[i].type     = t;
        cols[i].fmt_char = fmt ? fmt[0] : 'u';
    }

    /* --- Workbook setup --- */
    lxw_workbook *wb = workbook_new(s->path);
    if (!wb) {
        xwset_err(s, "xlsx.write: workbook_new('%s') failed", s->path);
        free(cols);
        if (schema.release) schema.release(&schema);
        return BETL_ERR_IO;
    }
    lxw_worksheet *ws = workbook_add_worksheet(wb,
        s->sheet_name && *s->sheet_name ? s->sheet_name : NULL);
    if (!ws) {
        xwset_err(s, "xlsx.write: workbook_add_worksheet failed");
        workbook_close(wb);
        free(cols);
        if (schema.release) schema.release(&schema);
        return BETL_ERR_IO;
    }

    lxw_row_t row_idx = 0;
    if (s->write_header) {
        for (size_t c = 0; c < n_cols; ++c) {
            const char *nm = schema.children[c]->name ? schema.children[c]->name : "";
            lxw_error e = worksheet_write_string(ws, row_idx, (lxw_col_t)c,
                                                 nm, NULL);
            if (e != LXW_NO_ERROR) {
                xwset_err(s, "xlsx.write: header write failed for col '%s' (lxw=%d)",
                          nm, (int)e);
                workbook_close(wb);
                free(cols);
                if (schema.release) schema.release(&schema);
                return BETL_ERR_IO;
            }
        }
        ++row_idx;
    }

    /* --- Stream rows --- */
    int rc = BETL_OK;
    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            xwset_err(s, "xlsx.write: cancelled by host");
            rc = BETL_ERR_CANCELLED;
            break;
        }
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            const char *up = s->input.get_last_error
                ? s->input.get_last_error(&s->input) : NULL;
            xwset_err(s, "xlsx.write: upstream get_next failed: %s",
                      up ? up : "(no detail)");
            rc = BETL_ERR_IO;
            break;
        }
        if (!batch.release) break;

        for (int64_t r = 0; r < batch.length; ++r) {
            for (size_t c = 0; c < n_cols; ++c) {
                const struct ArrowArray *col = batch.children[c];
                if (validity_is_null(col, r)) continue;     /* leave blank */
                lxw_error e = LXW_NO_ERROR;
                switch (cols[c].type) {
                case XW_INT64:
                case XW_FLOAT64: {
                    double v = read_numeric_as_double(col, r, cols[c].fmt_char);
                    e = worksheet_write_number(ws, row_idx, (lxw_col_t)c,
                                               v, NULL);
                    break;
                }
                case XW_UTF8: {
                    const int32_t *offs = col->buffers[1];
                    const char    *data = col->buffers[2];
                    int64_t off = col->offset + r;
                    int32_t start = offs[off];
                    int32_t end   = offs[off + 1];
                    size_t  len   = (size_t)(end - start);
                    /* libxlsxwriter wants a NUL-terminated string. */
                    char *tmp = malloc(len + 1);
                    if (!tmp) {
                        xwset_err(s, "xlsx.write: out of memory");
                        rc = BETL_ERR_INTERNAL;
                        e = LXW_ERROR_MEMORY_MALLOC_FAILED;
                        break;
                    }
                    if (len) memcpy(tmp, data + start, len);
                    tmp[len] = '\0';
                    e = worksheet_write_string(ws, row_idx, (lxw_col_t)c,
                                               tmp, NULL);
                    free(tmp);
                    break;
                }
                case XW_BOOL: {
                    const uint8_t *bits = col->buffers[1];
                    int64_t off = col->offset + r;
                    int v = (bits[off / 8] >> (off % 8)) & 1;
                    e = worksheet_write_boolean(ws, row_idx, (lxw_col_t)c,
                                                v, NULL);
                    break;
                }
                }
                if (e != LXW_NO_ERROR && rc == BETL_OK) {
                    xwset_err(s, "xlsx.write: cell write failed at row %u col %zu (lxw=%d)",
                              (unsigned)row_idx, c, (int)e);
                    rc = BETL_ERR_IO;
                    break;
                }
            }
            if (rc != BETL_OK) break;
            ++row_idx;
        }
        batch.release(&batch);
        if (rc != BETL_OK) break;
    }

    /* --- Finalise --- */
    lxw_error close_err = workbook_close(wb);
    if (rc == BETL_OK && close_err != LXW_NO_ERROR) {
        xwset_err(s, "xlsx.write: workbook_close failed (lxw=%d)",
                  (int)close_err);
        rc = BETL_ERR_IO;
    }
    if (rc == BETL_OK) {
        betl_log(s->ctx, BETL_LOG_INFO,
                 "xlsx.write: wrote %u rows to %s",
                 (unsigned)(row_idx - (s->write_header ? 1 : 0)), s->path);
    }
    free(cols);
    if (schema.release) schema.release(&schema);
    return rc;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef xw_inputs[]  = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows to write" },
};

static const BetlComponentDef xw_components[] = {
    { .name               = "xlsx.write",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = xw_inputs,
      .input_count        = 1,
      .init               = xw_init,
      .destroy            = xw_destroy,
      .attach_input       = xw_attach_input,
      .sink_run           = xw_sink_run },
};

static const BetlProvider xw_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-xlsx-write",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = xw_components,
    .component_count = sizeof xw_components / sizeof xw_components[0],
};

int betl_register_xlsx_write(BetlRegistry *r) {
    return betl_registry_register(r, &xw_provider, "<builtin:xlsx-write>");
}
