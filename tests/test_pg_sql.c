/* Unit tests for the postgres.upsert SQL builder.
 *
 * No database, no libpq — just string compares against expected output
 * for each (column-set, key-set, on_conflict) shape we promise to support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/pg_sql.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

#define CHECK_STR(got, want) do { \
    const char *g_ = (got), *w_ = (want); \
    if (!g_ || !w_ || strcmp(g_, w_) != 0) { \
        fprintf(stderr, "FAIL %s:%d:\n  got:  %s\n  want: %s\n", \
                __FILE__, __LINE__, g_ ? g_ : "(null)", w_); \
        ++failures; \
    } \
} while (0)

static char *S(const char *s) {
    /* string literal helper — drop in places that need a non-const char ** */
    return (char *)s;
}

/* --- on_conflict parsing ----------------------------------------------------*/

static void test_on_conflict_parse(void) {
    BetlOnConflict m = (BetlOnConflict)999;
    CHECK(betl_parse_on_conflict(NULL, &m) == 0 && m == BETL_OC_UPDATE);
    CHECK(betl_parse_on_conflict("update", &m) == 0 && m == BETL_OC_UPDATE);
    CHECK(betl_parse_on_conflict("update_if_changed", &m) == 0
          && m == BETL_OC_UPDATE_IF_CHANGED);
    CHECK(betl_parse_on_conflict("ignore", &m) == 0 && m == BETL_OC_IGNORE);
    CHECK(betl_parse_on_conflict("error",  &m) == 0 && m == BETL_OC_ERROR);
    CHECK(betl_parse_on_conflict("nope",   &m) == -1);
    CHECK(betl_parse_on_conflict("",       &m) == -1);
}

/* --- single-key, two non-key cols, all four modes --------------------------*/

static void test_two_cols_update(void) {
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount"), S("note") };
    char *keys[] = { S("id") };
    int rc = betl_build_upsert_sql(&b, "stg.orders",
                                   cols, 3, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO \"stg\".\"orders\" "
        "(\"id\", \"amount\", \"note\") VALUES ($1, $2, $3) "
        "ON CONFLICT (\"id\") "
        "DO UPDATE SET "
        "\"amount\" = EXCLUDED.\"amount\", \"note\" = EXCLUDED.\"note\"");
    free(b.data);
}

static void test_two_cols_update_if_changed(void) {
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount"), S("note") };
    char *keys[] = { S("id") };
    int rc = betl_build_upsert_sql(&b, "stg.orders",
                                   cols, 3, keys, 1,
                                   BETL_OC_UPDATE_IF_CHANGED);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO \"stg\".\"orders\" "
        "(\"id\", \"amount\", \"note\") VALUES ($1, $2, $3) "
        "ON CONFLICT (\"id\") "
        "DO UPDATE SET "
        "\"amount\" = EXCLUDED.\"amount\", \"note\" = EXCLUDED.\"note\" "
        "WHERE \"stg\".\"orders\".\"amount\" IS DISTINCT FROM EXCLUDED.\"amount\""
        " OR \"stg\".\"orders\".\"note\" IS DISTINCT FROM EXCLUDED.\"note\"");
    free(b.data);
}

static void test_two_cols_ignore(void) {
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount") };
    char *keys[] = { S("id") };
    int rc = betl_build_upsert_sql(&b, "orders",
                                   cols, 2, keys, 1, BETL_OC_IGNORE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO \"orders\" "
        "(\"id\", \"amount\") VALUES ($1, $2) "
        "ON CONFLICT (\"id\") DO NOTHING");
    free(b.data);
}

static void test_two_cols_error(void) {
    /* mode=error → no ON CONFLICT clause at all. */
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount") };
    char *keys[] = { S("id") };
    int rc = betl_build_upsert_sql(&b, "orders",
                                   cols, 2, keys, 1, BETL_OC_ERROR);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO \"orders\" "
        "(\"id\", \"amount\") VALUES ($1, $2)");
    free(b.data);
}

/* --- composite key ---------------------------------------------------------*/

static void test_composite_key(void) {
    BetlBuf b = {0};
    char *cols[] = { S("tenant"), S("sku"), S("qty") };
    char *keys[] = { S("tenant"), S("sku") };
    int rc = betl_build_upsert_sql(&b, "inv.stock",
                                   cols, 3, keys, 2, BETL_OC_UPDATE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO \"inv\".\"stock\" "
        "(\"tenant\", \"sku\", \"qty\") VALUES ($1, $2, $3) "
        "ON CONFLICT (\"tenant\", \"sku\") "
        "DO UPDATE SET \"qty\" = EXCLUDED.\"qty\"");
    free(b.data);
}

/* --- "every column is a key" → DO NOTHING ----------------------------------*/

static void test_all_cols_are_keys(void) {
    BetlBuf b = {0};
    char *cols[] = { S("a"), S("b") };
    char *keys[] = { S("a"), S("b") };
    int rc = betl_build_upsert_sql(&b, "t",
                                   cols, 2, keys, 2, BETL_OC_UPDATE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO \"t\" (\"a\", \"b\") VALUES ($1, $2) "
        "ON CONFLICT (\"a\", \"b\") DO NOTHING");
    free(b.data);
}

/* --- error cases -----------------------------------------------------------*/

static void test_bad_identifier(void) {
    /* Embedded `"` in a column name is rejected (we don't try to escape it). */
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("evil\"col") };
    char *keys[] = { S("id") };
    int rc = betl_build_upsert_sql(&b, "t",
                                   cols, 2, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == -1);
    free(b.data);
}

static void test_key_not_in_columns(void) {
    BetlBuf b = {0};
    char *cols[] = { S("a"), S("b") };
    char *keys[] = { S("c") };
    int rc = betl_build_upsert_sql(&b, "t",
                                   cols, 2, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == -3);
    free(b.data);
}

/* --- entry point -----------------------------------------------------------*/

int main(void) {
    test_on_conflict_parse();
    test_two_cols_update();
    test_two_cols_update_if_changed();
    test_two_cols_ignore();
    test_two_cols_error();
    test_composite_key();
    test_all_cols_are_keys();
    test_bad_identifier();
    test_key_not_in_columns();

    if (failures) {
        fprintf(stderr, "FAIL: %d failure(s)\n", failures);
        return 1;
    }
    printf("ok: pg_sql unit tests passed\n");
    return 0;
}
