/* shell control-flow task tests.
 *
 * Each case wires the task through the registry and runs the vtable
 * directly. Uses standard POSIX utilities (/bin/true, /bin/false,
 * /bin/sleep, /bin/echo) which are present everywhere we ship.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/context.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static int run_task(BetlRegistry *r, BetlContext *ctx,
                    const char *type, const char *cfg) {
    const BetlComponentDef *cd = betl_registry_find(r, type);
    if (!cd) { fprintf(stderr, "no component '%s'\n", type); return -1; }
    void *state = NULL;
    int rc = cd->init(ctx, cfg, &state);
    if (rc != BETL_OK) return rc;
    rc = cd->task_run(state);
    cd->destroy(state);
    return rc;
}

int main(void) {
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    CHECK(betl_register_builtins(reg) == BETL_OK);

    /* --- 1: /bin/true exits 0 → success ------------------------------- */
    CHECK(run_task(reg, ctx, "shell", "{\"argv\":[\"/bin/true\"]}") == BETL_OK);

    /* --- 2: /bin/false exits 1 → IO error with message ---------------- */
    {
        int rc = run_task(reg, ctx, "shell", "{\"argv\":[\"/bin/false\"]}");
        CHECK(rc != BETL_OK);
        const char *e = betl_context_last_error(ctx);
        CHECK(e && strstr(e, "exited with status 1") != NULL);
    }

    /* --- 3: missing executable → error -------------------------------- */
    {
        int rc = run_task(reg, ctx, "shell",
            "{\"argv\":[\"/no/such/binary-here\"]}");
        CHECK(rc != BETL_OK);
    }

    /* --- 4: multiple argv elements --- /bin/sh -c 'exit 0' ------------ */
    CHECK(run_task(reg, ctx, "shell",
        "{\"argv\":[\"/bin/sh\",\"-c\",\"exit 0\"]}") == BETL_OK);

    /* --- 5: timeout kills a long-running process ---------------------- */
    {
        int rc = run_task(reg, ctx, "shell",
            "{\"argv\":[\"/bin/sleep\",\"30\"],\"timeout\":\"2s\"}");
        CHECK(rc != BETL_OK);
        const char *e = betl_context_last_error(ctx);
        CHECK(e && strstr(e, "timeout") != NULL);
    }

    /* --- 6: bad config — empty argv → invalid ------------------------- */
    {
        void *state = NULL;
        const BetlComponentDef *cd = betl_registry_find(reg, "shell");
        int rc = cd->init(ctx, "{\"argv\":[]}", &state);
        CHECK(rc == BETL_ERR_INVALID);
    }

    /* --- 7: bad config — no argv key → invalid ------------------------ */
    {
        void *state = NULL;
        const BetlComponentDef *cd = betl_registry_find(reg, "shell");
        int rc = cd->init(ctx, "{}", &state);
        CHECK(rc == BETL_ERR_INVALID);
    }

    /* --- 8: bad timeout format → invalid ------------------------------ */
    {
        void *state = NULL;
        const BetlComponentDef *cd = betl_registry_find(reg, "shell");
        int rc = cd->init(ctx,
            "{\"argv\":[\"/bin/true\"],\"timeout\":\"forever\"}", &state);
        CHECK(rc == BETL_ERR_INVALID);
    }

    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: shell integration test passed\n");
    return 0;
}
