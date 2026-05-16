/* libFuzzer harness for the JSON SOURCE.
 *
 * Same shape as csv_fuzz.c: drive json.read directly via the component
 * API on fuzzer-supplied bytes. Compiled only when BETL_HAVE_CJSON is
 * defined (top-level CMakeLists guards json_read/json_write behind
 * the same flag). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/context.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static char            g_path[64];
static BetlContext    *g_ctx = NULL;
static BetlRegistry   *g_reg = NULL;
static const BetlComponentDef *g_json_def = NULL;

static void __attribute__((constructor)) fuzz_init(void) {
    snprintf(g_path, sizeof g_path, "/tmp/betl-fuzz-json-%d.json",
             (int)getpid());
    g_ctx = betl_context_create();
    g_reg = betl_registry_create();
    if (!g_ctx || !g_reg) abort();
    if (betl_register_builtins(g_reg) != BETL_OK) abort();
    g_json_def = betl_registry_find(g_reg, "json.read");
    if (!g_json_def) abort();
}

static void __attribute__((destructor)) fuzz_shutdown(void) {
    if (g_reg) betl_registry_destroy(g_reg);
    if (g_ctx) betl_context_destroy(g_ctx);
    unlink(g_path);
}

static void drive_one(const char *cfg_json) {
    void *state = NULL;
    if (g_json_def->init(g_ctx, cfg_json, &state) != BETL_OK || !state) {
        return;
    }
    struct ArrowArrayStream stream = {0};
    if (g_json_def->attach_output(state, 0, &stream) == BETL_OK) {
        for (int i = 0; i < 1000; ++i) {
            struct ArrowArray batch = {0};
            int rc = stream.get_next(&stream, &batch);
            if (rc != 0) break;
            if (!batch.release) break;
            batch.release(&batch);
        }
        if (stream.release) stream.release(&stream);
    }
    g_json_def->destroy(state);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FILE *f = fopen(g_path, "wb");
    if (!f) return 0;
    if (size) fwrite(data, 1, size, f);
    fclose(f);

    /* json.read requires `columns:` in config — without it, init bails
     * before the parser ever runs. Drive two configs so both the array
     * (default) and ndjson paths get exercised. */
    char cfg[512];

    /* Config 1: array format (default). */
    snprintf(cfg, sizeof cfg,
             "{\"path\": \"%s\", \"batch_size\": 64,"
             " \"columns\": [\"id\", \"name\", \"value\"]}", g_path);
    drive_one(cfg);

    /* Config 2: ndjson format. */
    snprintf(cfg, sizeof cfg,
             "{\"path\": \"%s\", \"format\": \"ndjson\", \"batch_size\": 64,"
             " \"columns\": [\"id\", \"name\", \"value\"]}", g_path);
    drive_one(cfg);

    return 0;
}
