/* SSIS Expression Language engine test.
 *
 * argv[1] = path to betl-ssisexpr.so
 *
 * Drives the engine through compile + evaluate against the same
 * gen_strings(3)-shaped input batch used by test_lua_expr: a 3-row
 * struct with (id: int64, name: utf8) = (0, "row_0"), (1, "row_1"),
 * (2, "row_2"). Then exercises:
 *   - integer arithmetic + bracketed colref
 *   - comparison + boolean operators with three-valued logic
 *   - typed NULL propagation
 *   - cast (DT_R8) and (DT_WSTR, N)
 *   - string concatenation
 *   - ternary
 *   - case-insensitive column resolution
 *   - error paths: syntax, unknown column, unsupported desired_format
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
    if (stream.release) stream.release(&stream);

    *src_state_out = st;
    *src_def_out   = src;
    return 0;
}

static int64_t get_i64(const struct ArrowArray *a, size_t i) {
    const int64_t *v = a->buffers[1];
    return v[i];
}

static int bit_is_set(const uint8_t *bm, size_t i) {
    return (bm[i / 8] >> (i % 8)) & 1u;
}

static int compile_eval(const BetlExprEngine *eng,
                        BetlContext *ctx,
                        const struct ArrowSchema *schema,
                        const struct ArrowArray *batch,
                        const char *src,
                        const char *fmt,
                        struct ArrowArray *out) {
    void *h = NULL;
    int rc = eng->compile(ctx, src, schema, &h);
    if (rc != BETL_OK) {
        fprintf(stderr, "compile(%s): %s\n", src, betl_context_last_error(ctx));
        return rc;
    }
    rc = eng->evaluate(h, batch, fmt, out);
    if (rc != BETL_OK) {
        fprintf(stderr, "evaluate(%s -> %s): %s\n",
                src, fmt, betl_context_last_error(ctx));
    }
    eng->release(h);
    return rc;
}

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-ssisexpr.so>\n", argv[0]);
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

    const BetlExprEngine *eng = betl_registry_find_expr(r, "ssisexpr");
    CHECK(eng != NULL);
    CHECK(betl_registry_find_expr(r, "literal") != NULL);
    if (!eng) { betl_registry_destroy(r); betl_context_destroy(ctx); return 1; }
    CHECK(eng->compile && eng->evaluate && eng->release);

    struct ArrowSchema schema = {0};
    struct ArrowArray  batch  = {0};
    void *src_state = NULL;
    const BetlComponentDef *src_def = NULL;
    rc = build_input(r, ctx, &schema, &batch, &src_state, &src_def);
    CHECK(rc == 0);
    CHECK(batch.length == 3);
    CHECK(batch.n_children == 2);

    /* --- 1: arithmetic + bracketed colref ----------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "[id] * 10 + 1", "l", &out);
        CHECK(rc == BETL_OK);
        CHECK(out.length == 3);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 1);
            CHECK(get_i64(&out, 1) == 11);
            CHECK(get_i64(&out, 2) == 21);
        }
        if (out.release) out.release(&out);
    }

    /* --- 2: comparison + && with bool result -------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "[id] >= 1 && [id] <= 2", "b", &out);
        CHECK(rc == BETL_OK);
        CHECK(out.length == 3);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const uint8_t *bm = out.buffers[1];
            CHECK(bit_is_set(bm, 0) == 0);
            CHECK(bit_is_set(bm, 1) == 1);
            CHECK(bit_is_set(bm, 2) == 1);
        }
        if (out.release) out.release(&out);
    }

    /* --- 3: NULL(DT_I8) propagation ----------------------------------- */
    {
        struct ArrowArray out = {0};
        /* [id] + NULL(DT_I8) → all rows null */
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "[id] + NULL(DT_I8)", "l", &out);
        CHECK(rc == BETL_OK);
        CHECK(out.length == 3);
        CHECK(out.null_count == 3);
        if (out.release) out.release(&out);
    }

    /* --- 4: three-valued OR — true || null = true --------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "[id] == 1 || NULL(DT_BOOL)", "b", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            /* row 0: false || null = null
             * row 1: true  || null = true
             * row 2: false || null = null */
            CHECK(out.null_count == 2);
            const uint8_t *valid = out.buffers[0];
            const uint8_t *bm    = out.buffers[1];
            CHECK(valid && !bit_is_set(valid, 0));
            CHECK(valid &&  bit_is_set(valid, 1));
            CHECK(valid && !bit_is_set(valid, 2));
            if (valid && bit_is_set(valid, 1)) CHECK(bit_is_set(bm, 1) == 1);
        }
        if (out.release) out.release(&out);
    }

    /* --- 5: cast (DT_R8) and string concat ---------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_R8) [id] + 0.5", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] == 0.5);
            CHECK(v[1] == 1.5);
            CHECK(v[2] == 2.5);
        }
        if (out.release) out.release(&out);
    }

    /* --- 6: string concatenation -------------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "[name] + \"!\"", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            const int32_t *off = out.buffers[1];
            const char    *dat = out.buffers[2];
            const char *want[] = { "row_0!", "row_1!", "row_2!" };
            for (int i = 0; i < 3; ++i) {
                size_t n = (size_t)(off[i + 1] - off[i]);
                char tmp[32];
                if (n >= sizeof tmp) { CHECK(0); continue; }
                memcpy(tmp, dat + off[i], n); tmp[n] = '\0';
                CHECK(strcmp(tmp, want[i]) == 0);
            }
        }
        if (out.release) out.release(&out);
    }

    /* --- 7: ternary --------------------------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "[id] == 1 ? 999 : [id]", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 0);
            CHECK(get_i64(&out, 1) == 999);
            CHECK(get_i64(&out, 2) == 2);
        }
        if (out.release) out.release(&out);
    }

    /* --- 8: case-insensitive column resolution ------------------------ */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "[ID]", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 0);
            CHECK(get_i64(&out, 1) == 1);
            CHECK(get_i64(&out, 2) == 2);
        }
        if (out.release) out.release(&out);
    }

    /* --- 9: cast (DT_WSTR, N) stringification of int ------------------ */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 16) [id]", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            const int32_t *off = out.buffers[1];
            const char    *dat = out.buffers[2];
            const char *want[] = { "0", "1", "2" };
            for (int i = 0; i < 3; ++i) {
                size_t n = (size_t)(off[i + 1] - off[i]);
                char tmp[8]; memcpy(tmp, dat + off[i], n); tmp[n] = '\0';
                CHECK(strcmp(tmp, want[i]) == 0);
            }
        }
        if (out.release) out.release(&out);
    }

    /* Helper for string-output asserts. Compares output[row] to `want`. */
#define EXPECT_STR(out, row, want) do {                                  \
    const int32_t *_off = (out).buffers[1];                              \
    const char    *_dat = (out).buffers[2];                              \
    size_t _n = (size_t)(_off[(row) + 1] - _off[(row)]);                 \
    char _tmp[64];                                                       \
    CHECK(_n < sizeof _tmp);                                             \
    if (_n < sizeof _tmp) {                                              \
        memcpy(_tmp, _dat + _off[(row)], _n); _tmp[_n] = '\0';           \
        CHECK(strcmp(_tmp, (want)) == 0);                                \
    }                                                                    \
} while (0)

    /* --- 10: LEN / SUBSTRING / LEFT / RIGHT --------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "LEN([name])", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 5);
            CHECK(get_i64(&out, 1) == 5);
            CHECK(get_i64(&out, 2) == 5);
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "SUBSTRING([name], 1, 3)", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "row");
            EXPECT_STR(out, 1, "row");
            EXPECT_STR(out, 2, "row");
        }
        if (out.release) out.release(&out);
    }
    {
        /* SUBSTRING past end clamps length, returning trailing fragment. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "SUBSTRING([name], 5, 100)", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "0");
            EXPECT_STR(out, 1, "1");
            EXPECT_STR(out, 2, "2");
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "LEFT([name], 3)", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "row");
            EXPECT_STR(out, 1, "row");
            EXPECT_STR(out, 2, "row");
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "RIGHT([name], 1)", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "0");
            EXPECT_STR(out, 1, "1");
            EXPECT_STR(out, 2, "2");
        }
        if (out.release) out.release(&out);
    }

    /* --- 11: UPPER / LOWER / REPLACE ---------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "UPPER([name])", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "ROW_0");
            EXPECT_STR(out, 1, "ROW_1");
            EXPECT_STR(out, 2, "ROW_2");
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "LOWER(\"HELLO\")", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "hello");
            EXPECT_STR(out, 1, "hello");
            EXPECT_STR(out, 2, "hello");
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "REPLACE([name], \"row_\", \"X-\")", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "X-0");
            EXPECT_STR(out, 1, "X-1");
            EXPECT_STR(out, 2, "X-2");
        }
        if (out.release) out.release(&out);
    }

    /* --- 12: TRIM / LTRIM / RTRIM ------------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "TRIM(\"  hi  \")", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) EXPECT_STR(out, 0, "hi");
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "LTRIM(\"  hi  \")", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) EXPECT_STR(out, 0, "hi  ");
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "RTRIM(\"  hi  \")", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) EXPECT_STR(out, 0, "  hi");
        if (out.release) out.release(&out);
    }

    /* --- 13: REVERSE + FINDSTRING ------------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "REVERSE([name])", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "0_wor");
            EXPECT_STR(out, 1, "1_wor");
            EXPECT_STR(out, 2, "2_wor");
        }
        if (out.release) out.release(&out);
    }
    {
        /* "_" is at byte position 4 (1-based: 4). */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "FINDSTRING([name], \"_\", 1)", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 4);
            CHECK(get_i64(&out, 1) == 4);
            CHECK(get_i64(&out, 2) == 4);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Not found → 0. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "FINDSTRING([name], \"zz\", 1)", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 0);
            CHECK(get_i64(&out, 1) == 0);
            CHECK(get_i64(&out, 2) == 0);
        }
        if (out.release) out.release(&out);
    }

    /* --- 14: ISNULL + REPLACENULL ------------------------------------- */
    {
        /* ISNULL of a typed NULL should be TRUE for every row. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "ISNULL(NULL(DT_I8))", "b", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const uint8_t *bm = out.buffers[1];
            CHECK(bit_is_set(bm, 0));
            CHECK(bit_is_set(bm, 1));
            CHECK(bit_is_set(bm, 2));
        }
        if (out.release) out.release(&out);
    }
    {
        /* ISNULL of [id] (which is never null in this batch) → FALSE. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "ISNULL([id])", "b", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const uint8_t *bm = out.buffers[1];
            CHECK(!bit_is_set(bm, 0));
            CHECK(!bit_is_set(bm, 1));
            CHECK(!bit_is_set(bm, 2));
        }
        if (out.release) out.release(&out);
    }
    {
        /* REPLACENULL on a typed NULL returns the replacement value. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "REPLACENULL(NULL(DT_I8), 42)", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(out.null_count == 0);
            CHECK(get_i64(&out, 0) == 42);
            CHECK(get_i64(&out, 1) == 42);
            CHECK(get_i64(&out, 2) == 42);
        }
        if (out.release) out.release(&out);
    }
    {
        /* REPLACENULL passes the value through when not null. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "REPLACENULL([id], 99)", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(out.null_count == 0);
            CHECK(get_i64(&out, 0) == 0);
            CHECK(get_i64(&out, 1) == 1);
            CHECK(get_i64(&out, 2) == 2);
        }
        if (out.release) out.release(&out);
    }

    /* --- 15: ABS + SIGN ----------------------------------------------- */
    {
        /* ABS([id] - 1) → 1, 0, 1 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "ABS([id] - 1)", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 1);
            CHECK(get_i64(&out, 1) == 0);
            CHECK(get_i64(&out, 2) == 1);
        }
        if (out.release) out.release(&out);
    }
    {
        /* SIGN([id] - 1) → -1, 0, 1 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "SIGN([id] - 1)", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == -1);
            CHECK(get_i64(&out, 1) == 0);
            CHECK(get_i64(&out, 2) == 1);
        }
        if (out.release) out.release(&out);
    }

    /* --- 16: POWER + SQRT + SQUARE ------------------------------------ */
    {
        /* POWER(2, [id]) → 1.0, 2.0, 4.0 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "POWER(2, [id])", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] == 1.0);
            CHECK(v[1] == 2.0);
            CHECK(v[2] == 4.0);
        }
        if (out.release) out.release(&out);
    }
    {
        /* SQUARE([id] + 1) → 1.0, 4.0, 9.0 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "SQUARE([id] + 1)", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] == 1.0);
            CHECK(v[1] == 4.0);
            CHECK(v[2] == 9.0);
        }
        if (out.release) out.release(&out);
    }
    {
        /* SQRT(SQUARE([id] + 1)) → 1, 2, 3 (within fp tolerance) */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "SQRT(SQUARE([id] + 1))", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 0.999999 && v[0] < 1.000001);
            CHECK(v[1] > 1.999999 && v[1] < 2.000001);
            CHECK(v[2] > 2.999999 && v[2] < 3.000001);
        }
        if (out.release) out.release(&out);
    }

    /* --- 17: ROUND / CEILING / FLOOR ---------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "ROUND(3.14159, 2)", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 3.1399 && v[0] < 3.1401);
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "CEILING(1.4)", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] == 2.0);
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch, "FLOOR(1.6)", "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] == 1.0);
        }
        if (out.release) out.release(&out);
    }

    /* --- 18: composition + case-insensitive function name ------------- */
    {
        /* upper(left([name], 3)) — function names lowercased too. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "upper(left([name], 3))", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "ROW");
            EXPECT_STR(out, 1, "ROW");
            EXPECT_STR(out, 2, "ROW");
        }
        if (out.release) out.release(&out);
    }

    /* --- 19: NULL propagates through a function ----------------------- */
    {
        /* LEN(NULL(DT_WSTR)) — all rows null. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "LEN(NULL(DT_WSTR))", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(out.null_count == 3);
        if (out.release) out.release(&out);
    }

    /* --- 20: error paths for functions -------------------------------- */
    {
        /* Unknown function. */
        void *h = NULL;
        rc = eng->compile(ctx, "DOES_NOT_EXIST(1)", &schema, &h);
        CHECK(rc != BETL_OK);
        CHECK(h == NULL);
        CHECK(strstr(betl_context_last_error(ctx), "unknown function") != NULL);
    }
    {
        /* Wrong arity. */
        void *h = NULL;
        rc = eng->compile(ctx, "LEN([id], [id])", &schema, &h);
        CHECK(rc != BETL_OK);
        CHECK(h == NULL);
    }
    {
        /* Type error: ABS on a string. */
        void *h = NULL;
        rc = eng->compile(ctx, "ABS([name])", &schema, &h);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(h, &batch, "l", &out);
        CHECK(rc != BETL_OK);
        CHECK(out.release == NULL);
        eng->release(h);
    }

    /* --- 24: cast string -> DT_DBDATE, stringify back ----------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 32) (DT_DBDATE) \"2026-05-11\"", "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "2026-05-11");
            EXPECT_STR(out, 1, "2026-05-11");
            EXPECT_STR(out, 2, "2026-05-11");
        }
        if (out.release) out.release(&out);
    }

    /* --- 25: cast string -> DT_DBTIMESTAMP with fractional seconds ---- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 40) (DT_DBTIMESTAMP) \"2026-05-11 10:30:00.123456\"",
                          "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) {
            EXPECT_STR(out, 0, "2026-05-11 10:30:00.123456");
        }
        if (out.release) out.release(&out);
    }

    /* --- 26: NULL(DT_DBDATE) propagates through YEAR ------------------ */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "YEAR(NULL(DT_DBDATE))", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(out.null_count == 3);
        if (out.release) out.release(&out);
    }

    /* --- 27: YEAR / MONTH / DAY on a timestamp literal ---------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "YEAR((DT_DBTIMESTAMP) \"2026-05-11 10:30:00\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) {
            CHECK(get_i64(&out, 0) == 2026);
        }
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "MONTH((DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 5);
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DAY((DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 11);
        if (out.release) out.release(&out);
    }

    /* --- 28: DATEPART variants ---------------------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEPART(\"quarter\", (DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 2);
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEPART(\"dayofyear\", (DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        /* 2026-05-11 = Jan 31 + Feb 28 + Mar 31 + Apr 30 + 11 = 131 */
        if (out.length == 3) CHECK(get_i64(&out, 0) == 131);
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        /* 2026-05-11 was a Monday → SSIS DW=2 (1=Sunday). */
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEPART(\"weekday\", (DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 2);
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEPART(\"hour\", (DT_DBTIMESTAMP) \"2026-05-11 10:30:00\")",
                          "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 10);
        if (out.release) out.release(&out);
    }

    /* --- 29: DATEADD month + DATEADD year ----------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 32) DATEADD(\"month\", 1, (DT_DBDATE) \"2026-01-31\")",
                          "u", &out);
        CHECK(rc == BETL_OK);
        /* Jan 31 + 1 month should clamp to Feb 28 (2026 isn't a leap year). */
        if (out.length == 3 && out.n_buffers == 3) EXPECT_STR(out, 0, "2026-02-28");
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 32) DATEADD(\"year\", -1, (DT_DBDATE) \"2026-05-11\")",
                          "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) EXPECT_STR(out, 0, "2025-05-11");
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 40) DATEADD(\"hour\", 25, "
                          "(DT_DBTIMESTAMP) \"2026-05-11 10:30:00\")",
                          "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3) EXPECT_STR(out, 0, "2026-05-12 11:30:00");
        if (out.release) out.release(&out);
    }

    /* --- 30: DATEDIFF day + DATEDIFF year ----------------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEDIFF(\"day\", (DT_DBDATE) \"2026-05-01\", "
                                            "(DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 10);
        if (out.release) out.release(&out);
    }
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEDIFF(\"year\", (DT_DBDATE) \"2020-01-01\", "
                                             "(DT_DBDATE) \"2026-05-11\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 6);
        if (out.release) out.release(&out);
    }
    {
        /* DATEDIFF in hours between two timestamps spanning ~1.5 days. */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "DATEDIFF(\"hour\", "
                          "(DT_DBTIMESTAMP) \"2026-05-11 10:00:00\", "
                          "(DT_DBTIMESTAMP) \"2026-05-12 22:30:00\")", "l", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3) CHECK(get_i64(&out, 0) == 36);  /* 1.5 days = 36h */
        if (out.release) out.release(&out);
    }

    /* --- 31: date comparison ------------------------------------------ */
    {
        /* (DT_DBDATE) "2026-05-11" < (DT_DBDATE) "2026-05-12" → TRUE */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_DBDATE) \"2026-05-11\" < (DT_DBDATE) \"2026-05-12\"",
                          "b", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const uint8_t *bm = out.buffers[1];
            CHECK(bit_is_set(bm, 0));
        }
        if (out.release) out.release(&out);
    }

    /* --- 32: GETDATE returns a non-null timestamp --------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 40) GETDATE()", "u", &out);
        CHECK(rc == BETL_OK);
        /* Don't check the value — just verify the row landed and length>=19
         * (the iso "YYYY-MM-DD HH:MM:SS" prefix). */
        if (out.length == 3 && out.n_buffers == 3) {
            const int32_t *off = out.buffers[1];
            size_t n0 = (size_t)(off[1] - off[0]);
            CHECK(n0 >= 19);
            CHECK(out.null_count == 0);
        }
        if (out.release) out.release(&out);
    }

    /* --- 33: cast date -> timestamp gives midnight -------------------- */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
                          "(DT_WSTR, 40) (DT_DBTIMESTAMP) (DT_DBDATE) \"2026-05-11\"",
                          "u", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 3)
            EXPECT_STR(out, 0, "2026-05-11 00:00:00");
        if (out.release) out.release(&out);
    }

    /* --- 34: error: YEAR on a string ---------------------------------- */
    {
        void *h = NULL;
        rc = eng->compile(ctx, "YEAR(\"hello\")", &schema, &h);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(h, &batch, "l", &out);
        CHECK(rc != BETL_OK);
        CHECK(out.release == NULL);
        eng->release(h);
    }

#undef EXPECT_STR

    /* --- 35: decimal arithmetic --------------------------------------- *
     * Exercises add/sub/mul/div/mod on DT_NUMERIC values through
     * op_arith. Result is cast to DT_R8 for easy double comparison;
     * the decimal layer does the work in i128 with scale alignment. */
    {
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 2) \"1.50\" + (DT_NUMERIC, 18, 2) \"2.25\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 3.7499 && v[0] < 3.7501);  /* 3.75 */
        }
        if (out.release) out.release(&out);
    }
    {
        /* Scale alignment: 10.0000 (scale 4) - 3.50 (scale 2) → 6.5000 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 4) \"10.0000\" - (DT_NUMERIC, 18, 2) \"3.50\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 6.4999 && v[0] < 6.5001);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Multiply: 2.50 * 0.500 → 1.25000 (scale 5 = 2+3) */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 2) \"2.50\" * (DT_NUMERIC, 18, 3) \"0.500\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 1.2499 && v[0] < 1.2501);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Divide: 10.00 / 4.00 → 2.500000 (scale = max(2,6) = 6) */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 2) \"10.00\" / (DT_NUMERIC, 18, 2) \"4.00\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 2.4999 && v[0] < 2.5001);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Divide producing repeating decimal: 1 / 3 at scale 6 → 0.333333 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 0) \"1\" / (DT_NUMERIC, 18, 0) \"3\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 0.333332 && v[0] < 0.333334);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Modulo: 10.00 % 3.00 → 1.00 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 2) \"10.00\" % (DT_NUMERIC, 18, 2) \"3.00\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 0.9999 && v[0] < 1.0001);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Decimal + int: 1.50 + 2 → 3.50  (int promoted to scale-0 dec) */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 2) \"1.50\" + 2)",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 3.4999 && v[0] < 3.5001);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Decimal + float: 1.50 + 0.25 (DT_R8) → goes via doubles → 1.75 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_NUMERIC, 18, 2) \"1.50\" + (DT_R8) 0.25",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > 1.7499 && v[0] < 1.7501);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Negative result: 1.00 - 5.00 → -4.00 */
        struct ArrowArray out = {0};
        rc = compile_eval(eng, ctx, &schema, &batch,
            "(DT_R8) ((DT_NUMERIC, 18, 2) \"1.00\" - (DT_NUMERIC, 18, 2) \"5.00\")",
            "g", &out);
        CHECK(rc == BETL_OK);
        if (out.length == 3 && out.n_buffers == 2 && out.buffers[1]) {
            const double *v = out.buffers[1];
            CHECK(v[0] > -4.0001 && v[0] < -3.9999);
        }
        if (out.release) out.release(&out);
    }
    {
        /* Divide by zero is a run-time error. */
        void *h = NULL;
        rc = eng->compile(ctx,
            "(DT_NUMERIC, 18, 2) \"1.00\" / (DT_NUMERIC, 18, 2) \"0\"",
            &schema, &h);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(h, &batch, "d:18,2", &out);
        CHECK(rc != BETL_OK);
        CHECK(out.release == NULL);
        eng->release(h);
    }
    {
        /* Overflow on multiply: ~10^37 * 100 won't fit in i128 even
         * after scale clipping (i128 max ≈ 1.7 * 10^38). */
        void *h = NULL;
        rc = eng->compile(ctx,
            "(DT_NUMERIC, 38, 0) \"9999999999999999999999999999999999999\" "
            "* (DT_NUMERIC, 18, 0) \"100\"",
            &schema, &h);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(h, &batch, "d:38,0", &out);
        CHECK(rc != BETL_OK);
        CHECK(out.release == NULL);
        eng->release(h);
    }

    /* --- 21: syntax error at compile time ----------------------------- */
    {
        void *h = NULL;
        rc = eng->compile(ctx, "[id] +", &schema, &h);
        CHECK(rc != BETL_OK);
        CHECK(h == NULL);
    }

    /* --- 22: unknown column ------------------------------------------- */
    {
        void *h = NULL;
        rc = eng->compile(ctx, "[no_such_column]", &schema, &h);
        CHECK(rc != BETL_OK);
        CHECK(h == NULL);
        const char *err = betl_context_last_error(ctx);
        CHECK(strstr(err, "unknown column") != NULL);
    }

    /* --- 23: unsupported desired_format ------------------------------- */
    {
        void *h = NULL;
        rc = eng->compile(ctx, "[id]", &schema, &h);
        CHECK(rc == BETL_OK);
        struct ArrowArray out = {0};
        rc = eng->evaluate(h, &batch, "d", &out);   /* float32, unsupported */
        CHECK(rc == BETL_ERR_TYPE);
        CHECK(out.release == NULL);
        eng->release(h);
    }

    if (batch.release)  batch.release(&batch);
    if (schema.release) schema.release(&schema);
    if (src_def && src_state) src_def->destroy(src_state);

    betl_registry_destroy(r);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ssisexpr: all checks passed\n");
    return 0;
}
