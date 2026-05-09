/* Lua expression engine test (SPEC §7).
 *
 * argv[1] = path to betl-lua.so
 *
 * Uses the betl.gen_strings builtin (id int64, name utf8) to materialise
 * a 3-row input batch, then drives the lua expression engine through:
 *
 *   - "row.id * 10"             producing int64    -> 0, 10, 20
 *   - "string.upper(row.name)"  producing utf8     -> "ROW_0".."ROW_2"
 *   - "row.id > 0"              producing bool     -> 0, 1, 1
 *
 * Plus error paths:
 *   - syntax error at compile time
 *   - request a desired_format the engine doesn't support
 *   - schema mismatch (input batch has wrong column count at evaluate)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/context.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

/* Build a (schema, batch) pair from gen_strings. Caller releases both
 * via their respective release callbacks, then destroys src_state. */
static int build_input(BetlRegistry *r,
                       BetlContext *ctx,
                       struct ArrowSchema *schema_out,
                       struct ArrowArray  *batch_out,
                       void **src_state_out,
                       const BetlComponentDef **src_def_out) {
    const BetlComponentDef *src = betl_registry_find(r, "betl.gen_strings");
    if (!src) return 1;

    void *st = NULL;
    int rc = src->init(ctx, "{\"row_count\":3}", &st);
    if (rc != BETL_OK) return rc;

    struct ArrowArrayStream stream = {0};
    rc = src->attach_output(st, 0, &stream);
    if (rc != BETL_OK) { src->destroy(st); return rc; }

    if (stream.get_schema(&stream, schema_out) != 0) {
        if (stream.release) stream.release(&stream);
        src->destroy(st);
        return 1;
    }
    if (stream.get_next(&stream, batch_out) != 0) {
        if (schema_out->release) schema_out->release(schema_out);
        if (stream.release) stream.release(&stream);
        src->destroy(st);
        return 1;
    }
    /* The stream just points back into the same gen_strings state; it's
     * safe to release it now — the schema and batch we extracted own
     * their own buffers. */
    if (stream.release) stream.release(&stream);

    *src_state_out = st;
    *src_def_out   = src;
    return 0;
}

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-lua.so>\n", argv[0]);
        return 2;
    }
#endif

    BetlContext  *ctx = betl_context_create();
    BetlRegistry *r   = betl_registry_create();
    CHECK(ctx && r);
    if (!ctx || !r) return 1;

    int rc = betl_register_builtins(r);
    CHECK(rc == BETL_OK);
    rc = betl_registry_load(r, plugin_path);
    if (rc != BETL_OK) {
        fprintf(stderr, "load failed: %s\n", betl_registry_last_error(r));
    }
    CHECK(rc == BETL_OK);

    /* Engine discovery. The host registers `literal` as a builtin, then
     * we dlopen betl-lua which registers `lua` — so two engines total. */
    CHECK(betl_registry_expr_count(r) == 2);
    const BetlExprEngine *eng = betl_registry_find_expr(r, "lua");
    CHECK(eng != NULL);
    CHECK(betl_registry_find_expr(r, "literal") != NULL);
    CHECK(betl_registry_find_expr(r, "no.such.lang") == NULL);
    if (!eng) { betl_registry_destroy(r); betl_context_destroy(ctx); return 1; }
    CHECK(eng->compile && eng->evaluate && eng->release);

    /* Build a 3-row input batch via gen_strings. */
    struct ArrowSchema schema = {0};
    struct ArrowArray  batch  = {0};
    void *src_state = NULL;
    const BetlComponentDef *src_def = NULL;
    rc = build_input(r, ctx, &schema, &batch, &src_state, &src_def);
    CHECK(rc == 0);
    CHECK(batch.length == 3);
    CHECK(batch.n_children == 2);

    /* --- 1: int64 result ------------------------------------------- */
    {
        void *handle = NULL;
        rc = eng->compile(ctx, "row.id * 10", &schema, &handle);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(handle, &batch, "l", &out);
        CHECK(rc == BETL_OK);
        CHECK(out.length    == 3);
        CHECK(out.n_buffers == 2);
        if (out.length == 3 && out.n_buffers == 2) {
            const int64_t *vals = out.buffers[1];
            CHECK(vals[0] == 0);
            CHECK(vals[1] == 10);
            CHECK(vals[2] == 20);
        }
        if (out.release) out.release(&out);
        eng->release(handle);
    }

    /* --- 2: utf8 result via string.upper --------------------------- */
    {
        void *handle = NULL;
        rc = eng->compile(ctx, "string.upper(row.name)", &schema, &handle);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(handle, &batch, "u", &out);
        CHECK(rc == BETL_OK);
        CHECK(out.length    == 3);
        CHECK(out.n_buffers == 3);
        if (out.length == 3 && out.n_buffers == 3) {
            const int32_t *off = out.buffers[1];
            const char    *dat = out.buffers[2];
            char tmp[32];
            for (int i = 0; i < 3; ++i) {
                size_t len = (size_t)(off[i + 1] - off[i]);
                if (len + 1 > sizeof tmp) { CHECK(0); continue; }
                memcpy(tmp, dat + off[i], len);
                tmp[len] = '\0';
                char want[8]; snprintf(want, sizeof want, "ROW_%d", i);
                CHECK(strcmp(tmp, want) == 0);
            }
        }
        if (out.release) out.release(&out);
        eng->release(handle);
    }

    /* --- 3: bool result (predicate row.id > 0) --------------------- */
    {
        void *handle = NULL;
        rc = eng->compile(ctx, "row.id > 0", &schema, &handle);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(handle, &batch, "b", &out);
        CHECK(rc == BETL_OK);
        CHECK(out.length    == 3);
        CHECK(out.n_buffers == 2);
        if (out.length == 3 && out.n_buffers == 2) {
            const uint8_t *bm = out.buffers[1];
            CHECK((bm[0] & 0x01) == 0);   /* row 0: false */
            CHECK((bm[0] & 0x02) != 0);   /* row 1: true  */
            CHECK((bm[0] & 0x04) != 0);   /* row 2: true  */
        }
        if (out.release) out.release(&out);
        eng->release(handle);
    }

    /* --- 4: syntax error at compile -------------------------------- */
    {
        void *handle = NULL;
        rc = eng->compile(ctx, "row.id +!@#", &schema, &handle);
        CHECK(rc != BETL_OK);
        CHECK(handle == NULL);
        const char *err = betl_context_last_error(ctx);
        CHECK(strstr(err, "compile error") != NULL);
    }

    /* --- 5: unsupported desired_format ----------------------------- */
    {
        void *handle = NULL;
        rc = eng->compile(ctx, "row.id", &schema, &handle);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(handle, &batch, "d", &out);  /* "d" = float32 */
        CHECK(rc == BETL_ERR_TYPE);
        CHECK(out.release == NULL);  /* not populated on error */
        eng->release(handle);
    }

    /* --- 6: input schema with a column the engine can't handle ----- */
    {
        /* Hand-build a schema with one float64 column ("g") to confirm
         * compile rejects unsupported input formats. */
        struct ArrowSchema bad     = {0};
        struct ArrowSchema *child  = calloc(1, sizeof *child);
        char *cname                = strdup("amount");
        struct ArrowSchema **kids  = malloc(sizeof *kids);
        if (!child || !cname || !kids) {
            CHECK(0);
            free(child); free(cname); free(kids);
        } else {
            child->format   = "g";
            child->name     = cname;
            bad.format      = "+s";
            bad.n_children  = 1;
            bad.children    = kids;
            kids[0]         = child;
            bad.release     = NULL;  /* we own the buffers, free manually */

            void *handle = NULL;
            rc = eng->compile(ctx, "row.amount > 0", &bad, &handle);
            CHECK(rc == BETL_ERR_TYPE);
            CHECK(handle == NULL);
            const char *err = betl_context_last_error(ctx);
            CHECK(strstr(err, "unsupported format") != NULL);

            free(cname);
            free(child);
            free(kids);
        }
    }

    /* Cleanup: release the batch and schema, then destroy src state. */
    if (batch.release)  batch.release(&batch);
    if (schema.release) schema.release(&schema);
    if (src_def && src_state) src_def->destroy(src_state);

    betl_registry_destroy(r);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("lua_expr: all checks passed\n");
    return 0;
}
