/* Built-in `literal` expression engine.
 *
 * The source string is the user's `value:` field; we store it verbatim
 * at compile time and reproduce it row by row at evaluate time, parsed
 * into the requested Arrow type.
 */

#include "runtime/literal_expr.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"


/* ---- Per-handle state ---------------------------------------------------- */

typedef struct {
    char *source;     /* heap copy of the literal value as a string */
} LitHandle;

static int lit_compile(BetlContext *ctx,
                       const char *source,
                       const struct ArrowSchema *schema,
                       void **out_handle) {
    (void)schema;
    if (!source) {
        betl_set_error(ctx, "literal: source is NULL");
        return BETL_ERR_INVALID;
    }
    LitHandle *h = calloc(1, sizeof *h);
    if (!h) return BETL_ERR_INTERNAL;
    h->source = strdup(source);
    if (!h->source) { free(h); return BETL_ERR_INTERNAL; }
    *out_handle = h;
    return BETL_OK;
}

static void lit_release(void *handle) {
    if (!handle) return;
    LitHandle *h = handle;
    free(h->source);
    free(h);
}


/* ---- Output leaf builders ----------------------------------------------- */

static void release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[1]);   /* values */
        /* buffers[0] (validity) is always NULL for literal — no nulls. */
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[1]);   /* offsets */
        free((void *)arr->buffers[2]);   /* data */
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_bool_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[1]);   /* values bitmap */
    }
    free(arr->buffers);
    arr->release = NULL;
}

static int build_int64(struct ArrowArray *out, size_t length, int64_t v) {
    int64_t *vals = malloc((length ? length : 1) * sizeof *vals);
    if (!vals) return -1;
    for (size_t i = 0; i < length; ++i) vals[i] = v;

    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); return -1; }
    bufs[0] = NULL;       /* no nulls */
    bufs[1] = vals;

    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_int64_leaf;
    return 0;
}

static int build_utf8(struct ArrowArray *out, size_t length, const char *s) {
    size_t slen = strlen(s);
    /* offsets: length+1 entries, monotonically increasing by slen. */
    int32_t *offsets = malloc((length + 1) * sizeof *offsets);
    if (!offsets) return -1;
    for (size_t i = 0; i <= length; ++i) {
        offsets[i] = (int32_t)(i * slen);
    }
    /* Concatenate `length` copies of the source bytes. */
    size_t bytes = slen * (length ? length : 1);
    char *data = malloc(bytes ? bytes : 1);
    if (!data) { free(offsets); return -1; }
    for (size_t i = 0; i < length; ++i) {
        memcpy(data + i * slen, s, slen);
    }

    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offsets); free(data); return -1; }
    bufs[0] = NULL;
    bufs[1] = offsets;
    bufs[2] = data;

    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 3;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_utf8_leaf;
    return 0;
}

static int build_bool(struct ArrowArray *out, size_t length, int v) {
    size_t nbytes = (length + 7) / 8;
    uint8_t *bitmap = calloc(nbytes ? nbytes : 1, 1);
    if (!bitmap) return -1;
    if (v) {
        for (size_t i = 0; i < length; ++i) bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
    }

    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(bitmap); return -1; }
    bufs[0] = NULL;
    bufs[1] = bitmap;

    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_bool_leaf;
    return 0;
}

/* Trim leading/trailing whitespace in a writable buffer. Returns p. */
static char *strip(char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return p;
}

static int lit_evaluate(void *handle,
                        const struct ArrowArray *input,
                        const char *desired_format,
                        struct ArrowArray *out) {
    LitHandle *h = handle;
    if (!h || !input || !out || !desired_format) return BETL_ERR_INVALID;
    memset(out, 0, sizeof *out);
    size_t length = (size_t)input->length;

    if (strcmp(desired_format, "l") == 0) {
        char *s = strdup(h->source);
        if (!s) return BETL_ERR_INTERNAL;
        char *t = strip(s);
        char *end = NULL;
        errno = 0;
        long long v = strtoll(t, &end, 10);
        int parse_ok = (end != t && *end == '\0' && errno == 0);
        free(s);
        if (!parse_ok) return BETL_ERR_TYPE;
        return build_int64(out, length, (int64_t)v) == 0 ? BETL_OK : BETL_ERR_INTERNAL;
    }
    if (strcmp(desired_format, "u") == 0) {
        return build_utf8(out, length, h->source) == 0 ? BETL_OK : BETL_ERR_INTERNAL;
    }
    if (strcmp(desired_format, "b") == 0) {
        char *s = strdup(h->source);
        if (!s) return BETL_ERR_INTERNAL;
        char *t = strip(s);
        int v;
        if      (strcmp(t, "true")  == 0 || strcmp(t, "1") == 0) v = 1;
        else if (strcmp(t, "false") == 0 || strcmp(t, "0") == 0) v = 0;
        else { free(s); return BETL_ERR_TYPE; }
        free(s);
        return build_bool(out, length, v) == 0 ? BETL_OK : BETL_ERR_INTERNAL;
    }
    return BETL_ERR_TYPE;
}


/* ---- Provider scaffolding ----------------------------------------------- */

static const BetlExprEngine literal_engine = {
    .lang     = "literal",
    .compile  = lit_compile,
    .evaluate = lit_evaluate,
    .release  = lit_release,
};

static const BetlProvider literal_provider = {
    .abi_version = BETL_ABI_VERSION,
    .name        = "betl-builtins-literal",
    .version     = "0.1.0",
    .license     = "Apache-2.0",
    .expr_engine = &literal_engine,
};

int betl_register_literal_engine(BetlRegistry *r) {
    return betl_registry_register(r, &literal_provider, "<builtin:literal>");
}
