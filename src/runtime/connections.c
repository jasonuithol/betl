#include "runtime/connections.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/context.h"
#include "runtime/substitute.h"

int betl_apply_connections(BetlContext *ctx, const BetlPipeline *p,
                           char *err_buf, size_t err_cap) {
    if (err_buf && err_cap > 0) err_buf[0] = '\0';
    if (!ctx || !p) return BETL_ERR_INVALID;

    size_t n = betl_pipeline_connection_count(p);
    for (size_t i = 0; i < n; ++i) {
        const BetlConnectionDecl *c = betl_pipeline_connection(p, i);
        char sub_err[256];
        char *resolved = betl_substitute_refs(c->config_json, ctx,
                                              sub_err, sizeof sub_err);
        if (!resolved) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                    "connection '%s' (line %d:%d): %s",
                    c->name, c->line, c->column, sub_err);
            }
            return BETL_ERR_INVALID;
        }
        int rc = betl_context_set_connection(ctx, c->name, resolved);
        free(resolved);
        if (rc != BETL_OK) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                    "connection '%s': set_connection failed (rc=%d)",
                    c->name, rc);
            }
            return rc;
        }
    }
    return BETL_OK;
}
