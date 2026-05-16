/* libFuzzer harness for the YAML pipeline loader.
 *
 * Writes each input to a temp path, then drives betl_pipeline_load.
 * The loader must reject malformed input cleanly — no crash, no leak,
 * no infinite loop. libFuzzer + AddressSanitizer flag those for us.
 *
 * Build via: cmake -DBETL_FUZZ=ON, then `ninja yaml_fuzz` from build/.
 *
 * Run: build/fuzz/yaml_fuzz fuzz/seeds/yaml -max_total_time=300 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipeline/pipeline.h"

/* Reuse a single temp path across invocations to avoid /tmp churn. */
static char g_path[64];

static void __attribute__((constructor)) init_temp_path(void) {
    snprintf(g_path, sizeof g_path, "/tmp/betl-fuzz-yaml-%d.yml",
             (int)getpid());
}

static void __attribute__((destructor)) cleanup_temp_path(void) {
    unlink(g_path);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FILE *f = fopen(g_path, "wb");
    if (!f) return 0;
    if (size) fwrite(data, 1, size, f);
    fclose(f);

    char err[1024];
    BetlPipeline *p = betl_pipeline_load(g_path, err, sizeof err);
    if (p) betl_pipeline_free(p);
    return 0;
}
