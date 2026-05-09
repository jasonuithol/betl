/* Loader smoke test.
 *
 * argv[1] = path to the test echo provider .so
 *
 * Exercises:
 *   - loading a real provider .so and finding its components
 *   - lookup of a non-existent component returns NULL
 *   - loading the same path twice fails (no double-registration)
 *   - loading a non-existent .so fails with a useful error
 *   - destroy with mixed state (one good provider loaded) is clean
 */

#include <stdio.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-echo-provider.so>\n", argv[0]);
        return 2;
    }
#endif

    /* --- 1: bad path is rejected with non-empty error message. ----------- */
    {
        BetlRegistry *r = betl_registry_create();
        CHECK(r != NULL);
        if (!r) return 1;

        int rc = betl_registry_load(r, "/no/such/file.so");
        CHECK(rc != BETL_OK);
        const char *err = betl_registry_last_error(r);
        CHECK(err != NULL);
        if (err) CHECK(err[0] != '\0');
        CHECK(betl_registry_provider_count(r) == 0);
        CHECK(betl_registry_component_count(r) == 0);

        betl_registry_destroy(r);
    }

    /* --- 2: happy path. ------------------------------------------------- */
    {
        BetlRegistry *r = betl_registry_create();
        CHECK(r != NULL);

        int rc = betl_registry_load(r, plugin_path);
        if (rc != BETL_OK) {
            fprintf(stderr, "load failed: %s\n", betl_registry_last_error(r));
        }
        CHECK(rc == BETL_OK);
        CHECK(betl_registry_provider_count(r) == 1);
        CHECK(betl_registry_component_count(r) == 1);

        const BetlComponentDef *cd = betl_registry_find(r, "test.echo");
        CHECK(cd != NULL);
        if (cd) {
            CHECK(cd->kind == BETL_KIND_TASK);
            CHECK(strcmp(cd->name, "test.echo") == 0);
            CHECK(cd->init != NULL);
            CHECK(cd->destroy != NULL);
            CHECK(cd->task_run != NULL);
        }

        /* Lookup of unknown name. */
        CHECK(betl_registry_find(r, "no.such.component") == NULL);

        /* --- 3: re-loading the same path fails. ------------------------ */
        rc = betl_registry_load(r, plugin_path);
        CHECK(rc != BETL_OK);
        CHECK(strstr(betl_registry_last_error(r), "already loaded") != NULL);
        /* Counts unchanged. */
        CHECK(betl_registry_provider_count(r) == 1);
        CHECK(betl_registry_component_count(r) == 1);

        /* --- 4: actually invoke the task vtable through the registry. -- */
        void *state = NULL;
        rc = cd->init(NULL, "{}", &state);
        CHECK(rc == BETL_OK);
        CHECK(state != NULL);
        rc = cd->task_run(state);
        CHECK(rc == BETL_OK);
        cd->destroy(state);

        betl_registry_destroy(r);
    }

    /* --- 5: destroy(NULL) is safe. ------------------------------------- */
    betl_registry_destroy(NULL);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("loader: all checks passed\n");
    return 0;
}
