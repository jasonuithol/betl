#include "runtime/context.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"

typedef struct {
    char *key;
    char *value;
} KvPair;

struct BetlContext {
    BetlLogLevel  min_level;
    FILE         *log_stream;
    char         *log_tag;
    int           cancel_flag;
    char          last_error[1024];

    KvPair       *params;
    size_t        params_count;
    size_t        params_cap;

    KvPair       *connections;
    size_t        connections_count;
    size_t        connections_cap;

    /* Loop-iteration variables for `${vars.NAME}` substitution. Pushed
     * on foreach entry, restored on exit. Unlike params/connections,
     * this kv-store is mutated mid-run as stages enter/leave scopes. */
    KvPair       *vars;
    size_t        vars_count;
    size_t        vars_cap;

    /* Borrowed pointer; set by the runtime so transforms can resolve
     * expression engines via betl_get_expr_engine. NULL during isolated
     * unit tests that don't bring a registry — those tests must not
     * call any code path that needs an engine. */
    BetlRegistry *registry;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *level_name(BetlLogLevel l) {
    switch (l) {
        case BETL_LOG_TRACE: return "TRACE";
        case BETL_LOG_DEBUG: return "DEBUG";
        case BETL_LOG_INFO:  return "INFO";
        case BETL_LOG_WARN:  return "WARN";
        case BETL_LOG_ERROR: return "ERROR";
    }
    return "?";
}

static int kv_grow(KvPair **arr, size_t *cap) {
    size_t new_cap = *cap ? *cap * 2 : 8;
    KvPair *p = realloc(*arr, new_cap * sizeof *p);
    if (!p) return BETL_ERR_INTERNAL;
    *arr = p;
    *cap = new_cap;
    return BETL_OK;
}

static int kv_set(KvPair **arr, size_t *count, size_t *cap,
                  const char *k, const char *v) {
    /* Update existing in-place. */
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp((*arr)[i].key, k) == 0) {
            char *vc = strdup(v);
            if (!vc) return BETL_ERR_INTERNAL;
            free((*arr)[i].value);
            (*arr)[i].value = vc;
            return BETL_OK;
        }
    }
    if (*count == *cap) {
        int rc = kv_grow(arr, cap);
        if (rc != BETL_OK) return rc;
    }
    char *kc = strdup(k);
    char *vc = strdup(v);
    if (!kc || !vc) {
        free(kc);
        free(vc);
        return BETL_ERR_INTERNAL;
    }
    (*arr)[*count].key   = kc;
    (*arr)[*count].value = vc;
    (*count)++;
    return BETL_OK;
}

static void kv_free(KvPair *arr, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(arr[i].key);
        free(arr[i].value);
    }
    free(arr);
}

static const char *kv_lookup(const KvPair *arr, size_t count, const char *k) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(arr[i].key, k) == 0) return arr[i].value;
    }
    return NULL;
}

static int kv_unset(KvPair *arr, size_t *count, const char *k) {
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp(arr[i].key, k) == 0) {
            free(arr[i].key);
            free(arr[i].value);
            /* Shift the tail down; the array stays packed so kv_lookup
             * remains O(n) without holes. */
            for (size_t j = i + 1; j < *count; ++j) arr[j - 1] = arr[j];
            (*count)--;
            return BETL_OK;
        }
    }
    return BETL_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

BetlContext *betl_context_create(void) {
    BetlContext *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->min_level  = BETL_LOG_INFO;
    c->log_stream = stderr;
    return c;
}

void betl_context_destroy(BetlContext *ctx) {
    if (!ctx) return;
    free(ctx->log_tag);
    kv_free(ctx->params,      ctx->params_count);
    kv_free(ctx->connections, ctx->connections_count);
    kv_free(ctx->vars,         ctx->vars_count);
    free(ctx);
}

void betl_context_set_min_log_level(BetlContext *ctx, BetlLogLevel level) {
    if (ctx) ctx->min_level = level;
}

void betl_context_set_log_stream(BetlContext *ctx, FILE *stream) {
    if (ctx && stream) ctx->log_stream = stream;
}

void betl_context_set_log_tag(BetlContext *ctx, const char *tag) {
    if (!ctx) return;
    free(ctx->log_tag);
    ctx->log_tag = tag ? strdup(tag) : NULL;
}

void betl_context_request_cancel(BetlContext *ctx) {
    if (ctx) ctx->cancel_flag = 1;
}

void betl_context_clear_cancel(BetlContext *ctx) {
    if (ctx) ctx->cancel_flag = 0;
}

int betl_context_set_param(BetlContext *ctx,
                           const char *key, const char *value) {
    if (!ctx || !key || !value) return BETL_ERR_INVALID;
    return kv_set(&ctx->params, &ctx->params_count, &ctx->params_cap,
                  key, value);
}

int betl_context_set_connection(BetlContext *ctx,
                                const char *name, const char *json) {
    if (!ctx || !name || !json) return BETL_ERR_INVALID;
    return kv_set(&ctx->connections, &ctx->connections_count,
                  &ctx->connections_cap, name, json);
}

void betl_context_set_registry(BetlContext *ctx, struct BetlRegistry *reg) {
    if (ctx) ctx->registry = reg;
}

const char *betl_context_last_error(const BetlContext *ctx) {
    return ctx ? ctx->last_error : "";
}

/* ------------------------------------------------------------------ */
/* ABI externs (called from plugins via host symbol resolution)       */
/* ------------------------------------------------------------------ */

void betl_log(BetlContext *ctx, BetlLogLevel level, const char *fmt, ...) {
    if (!ctx || level < ctx->min_level || !fmt) return;
    FILE *out = ctx->log_stream ? ctx->log_stream : stderr;

    if (ctx->log_tag) {
        fprintf(out, "[%s %s] ", level_name(level), ctx->log_tag);
    } else {
        fprintf(out, "[%s] ",    level_name(level));
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);
}

void betl_set_error(BetlContext *ctx, const char *fmt, ...) {
    if (!ctx || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->last_error, sizeof ctx->last_error, fmt, ap);
    va_end(ap);
}

int betl_should_cancel(BetlContext *ctx) {
    return ctx ? ctx->cancel_flag : 0;
}

const char *betl_get_param(BetlContext *ctx, const char *path) {
    if (!ctx || !path) return NULL;
    return kv_lookup(ctx->params, ctx->params_count, path);
}

const char *betl_get_connection(BetlContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    return kv_lookup(ctx->connections, ctx->connections_count, name);
}

int betl_context_set_var(BetlContext *ctx, const char *name, const char *value) {
    if (!ctx || !name || !value) return BETL_ERR_INVALID;
    return kv_set(&ctx->vars, &ctx->vars_count, &ctx->vars_cap, name, value);
}

int betl_context_unset_var(BetlContext *ctx, const char *name) {
    if (!ctx || !name) return BETL_ERR_INVALID;
    return kv_unset(ctx->vars, &ctx->vars_count, name);
}

const char *betl_context_get_var(BetlContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    return kv_lookup(ctx->vars, ctx->vars_count, name);
}

const struct BetlExprEngine *betl_get_expr_engine(BetlContext *ctx,
                                                  const char *lang) {
    if (!ctx || !ctx->registry || !lang) return NULL;
    return betl_registry_find_expr(ctx->registry, lang);
}
