/* Test provider that exercises every host-side helper from inside a
 * component. The TASK component "test.ctx_probe":
 *
 *   1. logs at INFO     (verifiable via the host's log stream)
 *   2. checks cancel    (must be 0 — the host hasn't cancelled)
 *   3. reads a param    ("greeting" → "hello")
 *   4. reads a conn     ("warehouse" → some JSON)
 *   5. sets an error    ("probe completed")
 *
 * Returns BETL_OK if every observation matches expectation; otherwise
 * sets an error describing the first mismatch and returns
 * BETL_ERR_INTERNAL. */

#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"

typedef struct {
    BetlContext *ctx;
} ProbeState;

static int probe_init(BetlContext *ctx, const char *cfg_json, void **state) {
    (void)cfg_json;
    ProbeState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    *state = s;
    return BETL_OK;
}

static void probe_destroy(void *state) {
    free(state);
}

static int probe_task_run(void *state) {
    ProbeState *s = state;
    BetlContext *ctx = s->ctx;

    betl_log(ctx, BETL_LOG_INFO, "probe alive: ts=%d", 1234);

    if (betl_should_cancel(ctx) != 0) {
        betl_set_error(ctx, "probe: cancel was unexpectedly set");
        return BETL_ERR_INTERNAL;
    }

    const char *g = betl_get_param(ctx, "greeting");
    if (!g || strcmp(g, "hello") != 0) {
        betl_set_error(ctx, "probe: greeting param wrong: %s",
                       g ? g : "(null)");
        return BETL_ERR_INTERNAL;
    }

    const char *missing = betl_get_param(ctx, "no.such.param");
    if (missing != NULL) {
        betl_set_error(ctx, "probe: missing param returned non-NULL");
        return BETL_ERR_INTERNAL;
    }

    const char *conn = betl_get_connection(ctx, "warehouse");
    if (!conn || !strstr(conn, "dsn")) {
        betl_set_error(ctx, "probe: warehouse connection wrong");
        return BETL_ERR_INTERNAL;
    }

    betl_set_error(ctx, "probe completed");
    return BETL_OK;
}

static const BetlComponentDef probe_components[] = {
    { .name               = "test.ctx_probe",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_DETERMINISTIC,
      .init               = probe_init,
      .destroy            = probe_destroy,
      .task_run           = probe_task_run },
};

static const BetlProvider probe_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-test-ctx-probe",
    .version         = "0.0.1",
    .license         = "Apache-2.0",
    .components      = probe_components,
    .component_count = sizeof probe_components / sizeof probe_components[0],
};

BETL_EXPORT const BetlProvider *betl_provider_entry(void) {
    return &probe_provider;
}
