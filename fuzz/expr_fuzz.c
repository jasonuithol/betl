/* libFuzzer harness for the SSIS expression engine (betl-ssisexpr).
 *
 * The expression language has its own parser (lexer / precedence / type
 * coercion / function dispatch) that's a fertile fuzz target — exactly
 * the class of code where bugs hide in malformed input. This harness
 * feeds the fuzzer's bytes directly as expression source, compiles
 * against a synthetic 3-row 2-column batch, and evaluates with each
 * of the supported desired_format codes ('l' / 'u' / 'b').
 *
 * The plugin path is hard-coded relative to the build dir
 * (build/providers/betl-ssisexpr/betl-ssisexpr.so) since run-fuzz.sh
 * invokes from the repo root with build-fuzz/. If you move the binary
 * the constructor's dlopen will abort and you'll see the path in the
 * abort message. */

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

static BetlContext  *g_ctx = NULL;
static BetlRegistry *g_reg = NULL;
static const BetlExprEngine *g_eng = NULL;
/* The schema + batch are produced once at startup by gen_strings(3),
 * which gives us an `id` (int64) and `name` (utf8) column — same shape
 * as test_ssisexpr. We hold the source state to release at shutdown so
 * the batch buffers stay valid for the harness's lifetime. */
static struct ArrowSchema      g_schema = {0};
static struct ArrowArray       g_batch  = {0};
static void                   *g_src_state = NULL;
static const BetlComponentDef *g_src_def = NULL;

/* Constructor would race with the cmake plugin-build order in some
 * configurations, so we hard-code the relative path. The build-fuzz/
 * tree puts the plugin at providers/betl-ssisexpr/betl-ssisexpr.so. */
static const char *ssisexpr_path(void) {
    /* Try the BETL_FUZZ build-tree relative path first; fall back to a
     * couple of other common spots so the binary works whether you run
     * it from build-fuzz/, build/, or the repo root. */
    static const char *candidates[] = {
        "build-fuzz/providers/betl-ssisexpr/betl-ssisexpr.so",
        "build/providers/betl-ssisexpr/betl-ssisexpr.so",
        "providers/betl-ssisexpr/betl-ssisexpr.so",
        NULL
    };
    for (size_t i = 0; candidates[i]; ++i) {
        if (access(candidates[i], R_OK) == 0) return candidates[i];
    }
    return candidates[0];   /* let dlopen produce the clearest error */
}

static void __attribute__((constructor)) fuzz_init(void) {
    g_ctx = betl_context_create();
    g_reg = betl_registry_create();
    if (!g_ctx || !g_reg) abort();
    if (betl_register_builtins(g_reg) != BETL_OK) abort();
    if (betl_registry_load(g_reg, ssisexpr_path()) != BETL_OK) {
        fprintf(stderr, "fuzz_init: failed to load %s: %s\n",
                ssisexpr_path(), betl_registry_last_error(g_reg));
        abort();
    }
    g_eng = betl_registry_find_expr(g_reg, "ssisexpr");
    if (!g_eng || !g_eng->compile || !g_eng->evaluate || !g_eng->release) {
        abort();
    }

    /* Build the synthetic input batch once. */
    g_src_def = betl_registry_find(g_reg, "betl.gen_strings");
    if (!g_src_def) abort();
    if (g_src_def->init(g_ctx, "{\"row_count\":3}", &g_src_state) != BETL_OK) {
        abort();
    }
    struct ArrowArrayStream stream = {0};
    if (g_src_def->attach_output(g_src_state, 0, &stream) != BETL_OK) abort();
    if (stream.get_schema(&stream, &g_schema) != 0) abort();
    if (stream.get_next(&stream, &g_batch)    != 0) abort();
    if (stream.release) stream.release(&stream);
}

static void __attribute__((destructor)) fuzz_shutdown(void) {
    if (g_batch.release)  g_batch.release(&g_batch);
    if (g_schema.release) g_schema.release(&g_schema);
    if (g_src_state && g_src_def) g_src_def->destroy(g_src_state);
    if (g_reg) betl_registry_destroy(g_reg);
    if (g_ctx) betl_context_destroy(g_ctx);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* compile() expects a NUL-terminated string; copy the fuzzer bytes
     * into a local buffer with an extra '\0'. Any embedded NULs in the
     * input will short-circuit the lexer's strlen, which is exactly
     * the kind of input the parser should handle without crashing. */
    if (size > 8192) return 0;  /* cap source length */
    char src[8193];
    memcpy(src, data, size);
    src[size] = '\0';

    /* Try each supported output format. compile() may fail on parse
     * errors (expected for random input); evaluate() only runs when
     * compile succeeded. */
    static const char *fmts[] = { "l", "u", "b" };
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; ++i) {
        void *handle = NULL;
        if (g_eng->compile(g_ctx, src, &g_schema, &handle) != BETL_OK) {
            /* Parse / type-check error: expected for most fuzzer
             * input. Bail without calling evaluate. */
            continue;
        }
        if (!handle) continue;
        struct ArrowArray out = {0};
        (void)g_eng->evaluate(handle, &g_batch, fmts[i], &out);
        if (out.release) out.release(&out);
        g_eng->release(handle);
    }
    return 0;
}
