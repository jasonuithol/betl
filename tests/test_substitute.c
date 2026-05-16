/* Boundary unit tests for substitute.c (`${env.X}` / `${params.X}` /
 * `${vars.X}` placeholder resolution).
 *
 * Covers happy paths, escapes / unknown prefixes, malformed syntax,
 * missing-name errors, and ${vars.X} bound through a context. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "runtime/context.h"
#include "runtime/substitute.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

int main(void) {
    /* ---- plain literal passes through verbatim ----------------------- */
    {
        char err[128];
        char *out = betl_substitute_refs("hello world", NULL,
                                         err, sizeof err);
        CHECK(out != NULL);
        CHECK(out && strcmp(out, "hello world") == 0);
        free(out);
    }

    /* ---- ${env.X} resolves to the env value -------------------------- */
    {
        setenv("BETL_SUB_TEST", "yes", 1);
        char err[128];
        char *out = betl_substitute_refs("X=${env.BETL_SUB_TEST}.",
                                         NULL, err, sizeof err);
        CHECK(out != NULL);
        CHECK(out && strcmp(out, "X=yes.") == 0);
        free(out);
        unsetenv("BETL_SUB_TEST");
    }

    /* ---- missing env var → NULL + clear error message --------------- */
    {
        unsetenv("BETL_SUB_MISSING");
        char err[128] = {0};
        char *out = betl_substitute_refs("${env.BETL_SUB_MISSING}",
                                         NULL, err, sizeof err);
        CHECK(out == NULL);
        CHECK(strstr(err, "BETL_SUB_MISSING") != NULL);
    }

    /* ---- ${params.X} without a ctx errors with "no context" --------- */
    {
        char err[128] = {0};
        char *out = betl_substitute_refs("${params.X}", NULL,
                                         err, sizeof err);
        CHECK(out == NULL);
        CHECK(strstr(err, "context") != NULL);
    }

    /* ---- ${params.X} with ctx + set param resolves ------------------ */
    {
        BetlContext *ctx = betl_context_create();
        CHECK(ctx != NULL);
        CHECK(betl_context_set_param(ctx, "load_date", "2026-05-16") == 0);
        char err[128];
        char *out = betl_substitute_refs("d=${params.load_date}",
                                         ctx, err, sizeof err);
        CHECK(out != NULL);
        CHECK(out && strcmp(out, "d=2026-05-16") == 0);
        free(out);
        betl_context_destroy(ctx);
    }

    /* ---- ${vars.X} with ctx + bound var resolves -------------------- */
    {
        BetlContext *ctx = betl_context_create();
        CHECK(ctx != NULL);
        CHECK(betl_context_set_var(ctx, "row", "alpha") == 0);
        char err[128];
        char *out = betl_substitute_refs("[${vars.row}]", ctx,
                                         err, sizeof err);
        CHECK(out != NULL);
        CHECK(out && strcmp(out, "[alpha]") == 0);
        free(out);
        betl_context_destroy(ctx);
    }

    /* ---- ${vars.X} unbound errors with the variable name ------------ */
    {
        BetlContext *ctx = betl_context_create();
        char err[128] = {0};
        char *out = betl_substitute_refs("${vars.absent}", ctx,
                                         err, sizeof err);
        CHECK(out == NULL);
        CHECK(strstr(err, "absent") != NULL);
        betl_context_destroy(ctx);
    }

    /* ---- unknown prefix passes through as a literal ----------------- */
    {
        /* `${secret.X}` is reserved for a future layer — substitute_refs
         * leaves it untouched (passes the literal `$` through then
         * continues scanning). The doc-string contract says "other forms
         * pass through unchanged". */
        char err[128];
        char *out = betl_substitute_refs("k=${secret.X}", NULL,
                                         err, sizeof err);
        CHECK(out != NULL);
        CHECK(out && strcmp(out, "k=${secret.X}") == 0);
        free(out);
    }

    /* ---- malformed ${env.NAME} (missing close brace) ---------------- */
    {
        char err[128] = {0};
        char *out = betl_substitute_refs("${env.OOPS", NULL,
                                         err, sizeof err);
        CHECK(out == NULL);
        CHECK(err[0] != '\0');
    }

    /* ---- name starting with a digit is rejected --------------------- */
    {
        char err[128] = {0};
        char *out = betl_substitute_refs("${env.1bad}", NULL,
                                         err, sizeof err);
        CHECK(out == NULL);
        CHECK(err[0] != '\0');
    }

    /* ---- substitution doesn't recurse: the resolved value is a literal */
    {
        setenv("BETL_SUB_RECURSIVE", "${env.SHOULD_NOT_EXPAND}", 1);
        char err[128];
        char *out = betl_substitute_refs("${env.BETL_SUB_RECURSIVE}",
                                         NULL, err, sizeof err);
        CHECK(out != NULL);
        CHECK(out && strcmp(out, "${env.SHOULD_NOT_EXPAND}") == 0);
        free(out);
        unsetenv("BETL_SUB_RECURSIVE");
    }

    /* ---- a long replacement triggers internal buffer growth --------- */
    {
        /* 4 KB of A's tests the realloc path in the dynamic buffer. */
        char *big = malloc(4096 + 1);
        CHECK(big != NULL);
        memset(big, 'A', 4096);
        big[4096] = '\0';
        setenv("BETL_SUB_BIG", big, 1);
        char err[128];
        char *out = betl_substitute_refs("[${env.BETL_SUB_BIG}]",
                                         NULL, err, sizeof err);
        CHECK(out != NULL);
        if (out) {
            CHECK(strlen(out) == 4096 + 2);
            CHECK(out[0] == '[');
            CHECK(out[4097] == ']');
            free(out);
        }
        unsetenv("BETL_SUB_BIG");
        free(big);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: substitute unit tests passed\n");
    return 0;
}
