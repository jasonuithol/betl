/* libFuzzer harness for the CSV parser.
 *
 * Drives csv.read directly via the component API: init() reads a tiny
 * config JSON pointing at a temp file containing the fuzz input, then
 * we drain via attach_output's ArrowArrayStream until end-of-stream
 * or error. The point is to exercise csv_read_record / csv_parse_field
 * / csv_parse_record_typed with adversarial bytes.
 *
 * Two configs are tried per input: header=true (default) and
 * header=false with a schema, so both code paths get coverage. */

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
static const BetlComponentDef *g_csv_def = NULL;

static void __attribute__((constructor)) fuzz_init(void) {
    snprintf(g_path, sizeof g_path, "/tmp/betl-fuzz-csv-%d.csv",
             (int)getpid());
    g_ctx = betl_context_create();
    g_reg = betl_registry_create();
    if (!g_ctx || !g_reg) abort();
    if (betl_register_builtins(g_reg) != BETL_OK) abort();
    g_csv_def = betl_registry_find(g_reg, "csv.read");
    if (!g_csv_def) abort();
}

static void __attribute__((destructor)) fuzz_shutdown(void) {
    if (g_reg) betl_registry_destroy(g_reg);
    if (g_ctx) betl_context_destroy(g_ctx);
    unlink(g_path);
}

/* Drive one csv.read with the given config. Drains the output stream
 * until either get_next returns end-of-stream (release == NULL on the
 * batch) or it returns an error. Either way the harness must not
 * crash. */
static void drive_one(const char *cfg_json) {
    void *state = NULL;
    if (g_csv_def->init(g_ctx, cfg_json, &state) != BETL_OK || !state) {
        return;
    }
    struct ArrowArrayStream stream = {0};
    if (g_csv_def->attach_output(state, 0, &stream) == BETL_OK) {
        /* Drain. Cap iterations so adversarial input can't infinite-
         * loop the fuzzer (libFuzzer also has a wall-clock timeout). */
        for (int i = 0; i < 1000; ++i) {
            struct ArrowArray batch = {0};
            int rc = stream.get_next(&stream, &batch);
            if (rc != 0) break;
            if (!batch.release) break;
            batch.release(&batch);
        }
        if (stream.release) stream.release(&stream);
    }
    g_csv_def->destroy(state);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FILE *f = fopen(g_path, "wb");
    if (!f) return 0;
    if (size) fwrite(data, 1, size, f);
    fclose(f);

    /* Config 1: header=true (default). Schema inferred from header row. */
    {
        char cfg[256];
        snprintf(cfg, sizeof cfg,
                 "{\"path\": \"%s\", \"batch_size\": 64}", g_path);
        drive_one(cfg);
    }

    /* Config 2: header=false, schema declared explicitly. Exercises the
     * typed-coercion path (int64 / utf8) on data the fuzzer may have
     * pointed at non-numeric bytes. */
    {
        char cfg[512];
        snprintf(cfg, sizeof cfg,
                 "{\"path\": \"%s\", \"header\": false, \"batch_size\": 64,"
                 " \"schema\": {\"columns\": ["
                 "{\"name\": \"a\", \"type\": \"int64\"},"
                 "{\"name\": \"b\", \"type\": \"utf8\"}"
                 "]}}", g_path);
        drive_one(cfg);
    }

    return 0;
}
