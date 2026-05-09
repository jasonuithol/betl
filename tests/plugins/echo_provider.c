/* Minimal test provider: one TASK component "test.echo" that has nothing
 * to do but still exercises the full provider/component declaration path
 * the loader cares about (entry point, ABI version, names, kind, basic
 * lifecycle vtable). */

#include <stdlib.h>

#include "betl/provider.h"

typedef struct {
    int counter;
} EchoState;

static int echo_init(BetlContext *ctx, const char *cfg_json, void **state) {
    (void)ctx; (void)cfg_json;
    EchoState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->counter = 0;
    *state = s;
    return BETL_OK;
}

static void echo_destroy(void *state) {
    free(state);
}

static int echo_task_run(void *state) {
    EchoState *s = state;
    s->counter++;
    return BETL_OK;
}

static const BetlComponentDef echo_components[] = {
    { .name               = "test.echo",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_DETERMINISTIC,
      .init               = echo_init,
      .destroy            = echo_destroy,
      .task_run           = echo_task_run },
};

static const BetlProvider echo_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-test-echo",
    .version         = "0.0.1",
    .license         = "Apache-2.0",
    .components      = echo_components,
    .component_count = sizeof echo_components / sizeof echo_components[0],
};

BETL_EXPORT const BetlProvider *betl_provider_entry(void) {
    return &echo_provider;
}
