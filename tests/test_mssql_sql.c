/* Unit tests for the mssql.upsert SQL (MERGE) builder.
 *
 * No database, no ODBC — string compares only. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/mssql_sql.h"

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

static char *S(const char *s) { return (char *)s; }

static void test_two_cols_update(void) {
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount"), S("note") };
    char *keys[] = { S("id") };
    int rc = betl_build_mssql_merge_sql(&b, "stg.orders",
                                        cols, 3, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "MERGE INTO [stg].[orders] AS T "
        "USING (SELECT ? AS [id], ? AS [amount], ? AS [note]) AS S "
        "ON T.[id] = S.[id] "
        "WHEN MATCHED THEN UPDATE SET T.[amount] = S.[amount], T.[note] = S.[note] "
        "WHEN NOT MATCHED THEN INSERT ([id], [amount], [note]) "
        "VALUES (S.[id], S.[amount], S.[note]);");
    free(b.data);
}

static void test_two_cols_ignore(void) {
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount") };
    char *keys[] = { S("id") };
    int rc = betl_build_mssql_merge_sql(&b, "dbo.t",
                                        cols, 2, keys, 1, BETL_OC_IGNORE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "MERGE INTO [dbo].[t] AS T "
        "USING (SELECT ? AS [id], ? AS [amount]) AS S "
        "ON T.[id] = S.[id] "
        "WHEN NOT MATCHED THEN INSERT ([id], [amount]) "
        "VALUES (S.[id], S.[amount]);");
    free(b.data);
}

static void test_error_mode_is_plain_insert(void) {
    BetlBuf b = {0};
    char *cols[] = { S("id"), S("amount") };
    char *keys[] = { S("id") };
    int rc = betl_build_mssql_merge_sql(&b, "dbo.t",
                                        cols, 2, keys, 1, BETL_OC_ERROR);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "INSERT INTO [dbo].[t] ([id], [amount]) VALUES (?, ?)");
    free(b.data);
}

static void test_keys_only_falls_back(void) {
    /* When every column is a key, MERGE has nothing to update — IGNORE
     * shape should appear (no WHEN MATCHED clause). */
    BetlBuf b = {0};
    char *cols[] = { S("id") };
    char *keys[] = { S("id") };
    int rc = betl_build_mssql_merge_sql(&b, "dbo.t",
                                        cols, 1, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "MERGE INTO [dbo].[t] AS T "
        "USING (SELECT ? AS [id]) AS S "
        "ON T.[id] = S.[id] "
        "WHEN NOT MATCHED THEN INSERT ([id]) VALUES (S.[id]);");
    free(b.data);
}

static void test_compound_key(void) {
    BetlBuf b = {0};
    char *cols[] = { S("a"), S("b"), S("v") };
    char *keys[] = { S("a"), S("b") };
    int rc = betl_build_mssql_merge_sql(&b, "dbo.t",
                                        cols, 3, keys, 2, BETL_OC_UPDATE);
    CHECK(rc == 0);
    CHECK_STR(b.data,
        "MERGE INTO [dbo].[t] AS T "
        "USING (SELECT ? AS [a], ? AS [b], ? AS [v]) AS S "
        "ON T.[a] = S.[a] AND T.[b] = S.[b] "
        "WHEN MATCHED THEN UPDATE SET T.[v] = S.[v] "
        "WHEN NOT MATCHED THEN INSERT ([a], [b], [v]) "
        "VALUES (S.[a], S.[b], S.[v]);");
    free(b.data);
}

static void test_key_not_in_columns_is_error(void) {
    BetlBuf b = {0};
    char *cols[] = { S("a") };
    char *keys[] = { S("z") };
    int rc = betl_build_mssql_merge_sql(&b, "dbo.t",
                                        cols, 1, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == -3);
    free(b.data);
}

static void test_bracket_in_ident_rejected(void) {
    BetlBuf b = {0};
    char *cols[] = { S("ev]il") };
    char *keys[] = { S("ev]il") };
    int rc = betl_build_mssql_merge_sql(&b, "dbo.t",
                                        cols, 1, keys, 1, BETL_OC_UPDATE);
    CHECK(rc == -1);
    free(b.data);
}

int main(void) {
    test_two_cols_update();
    test_two_cols_ignore();
    test_error_mode_is_plain_insert();
    test_keys_only_falls_back();
    test_compound_key();
    test_key_not_in_columns_is_error();
    test_bracket_in_ident_rejected();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: mssql_sql unit tests passed\n");
    return 0;
}
