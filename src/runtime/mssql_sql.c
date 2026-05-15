#include "runtime/mssql_sql.h"

#include <stdlib.h>
#include <string.h>

/* MSSQL bracket-quoting: [Name]. The `]` character inside a bracketed
 * identifier needs to be doubled (`]]`), but we conservatively reject
 * such names — the parser already requires names to be plain SQL
 * identifiers, so accepting `]` here would mostly let weird configs
 * through silently. */
static int append_bracket(BetlBuf *b, const char *ident) {
    if (strchr(ident, ']') != NULL) return -1;
    if (betl_buf_append(b, "[", 1) != 0) return -2;
    if (betl_buf_append(b, ident, strlen(ident)) != 0) return -2;
    if (betl_buf_append(b, "]", 1) != 0) return -2;
    return 0;
}

static int append_table(BetlBuf *b, const char *table) {
    const char *dot = strchr(table, '.');
    if (!dot) return append_bracket(b, table);
    size_t left_len = (size_t)(dot - table);
    char *left = strndup(table, left_len);
    if (!left) return -2;
    int rc = append_bracket(b, left);
    free(left);
    if (rc != 0) return rc;
    if (betl_buf_append(b, ".", 1) != 0) return -2;
    return append_bracket(b, dot + 1);
}

static int is_in(const char *name, char **list, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(name, list[i]) == 0) return 1;
    }
    return 0;
}

/* INSERT only — the path used for OC_ERROR, mssql.bulkinsert, and as a
 * building block. */
static int append_plain_insert(BetlBuf *out,
                               const char *table,
                               char **cols, size_t n_cols) {
    if (betl_buf_append(out, "INSERT INTO ", 12) != 0) return -2;
    int rc = append_table(out, table);
    if (rc < 0) return rc;
    if (betl_buf_append(out, " (", 2) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        rc = append_bracket(out, cols[i]);
        if (rc < 0) return rc;
    }
    if (betl_buf_append(out, ") VALUES (", 10) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        if (betl_buf_append(out, "?", 1) != 0) return -2;
    }
    if (betl_buf_append(out, ")", 1) != 0) return -2;
    return 0;
}

int betl_build_mssql_insert_sql(BetlBuf *out,
                                const char *table,
                                char **cols, size_t n_cols) {
    return append_plain_insert(out, table, cols, n_cols);
}

int betl_build_mssql_merge_sql(BetlBuf *out,
                               const char *table,
                               char **cols, size_t n_cols,
                               char **keys, size_t n_keys,
                               BetlOnConflict mode) {
    /* Verify every key is in `cols`. */
    for (size_t i = 0; i < n_keys; ++i) {
        if (!is_in(keys[i], cols, n_cols)) return -3;
    }

    /* OC_ERROR: just INSERT — the PK violation surfaces naturally. */
    if (mode == BETL_OC_ERROR) {
        return append_plain_insert(out, table, cols, n_cols);
    }

    /* MERGE [target] AS T
     *   USING (SELECT ? AS [c1], ? AS [c2], ...) AS S
     *   ON T.[k1] = S.[k1] AND ...
     *   WHEN MATCHED [AND change-pred] THEN UPDATE SET T.[c]=S.[c], ...
     *   WHEN NOT MATCHED THEN INSERT (...) VALUES (S.c1, S.c2, ...);
     *
     * For OC_IGNORE we omit the WHEN MATCHED clause entirely. */
    if (betl_buf_append(out, "MERGE INTO ", 11) != 0) return -2;
    int rc = append_table(out, table);
    if (rc < 0) return rc;
    if (betl_buf_append(out, " AS T USING (SELECT ", 20) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        if (betl_buf_append(out, "? AS ", 5) != 0) return -2;
        rc = append_bracket(out, cols[i]);
        if (rc < 0) return rc;
    }
    if (betl_buf_append(out, ") AS S ON ", 10) != 0) return -2;
    for (size_t i = 0; i < n_keys; ++i) {
        if (i && betl_buf_append(out, " AND ", 5) != 0) return -2;
        if (betl_buf_append(out, "T.", 2) != 0) return -2;
        rc = append_bracket(out, keys[i]);
        if (rc < 0) return rc;
        if (betl_buf_append(out, " = S.", 5) != 0) return -2;
        rc = append_bracket(out, keys[i]);
        if (rc < 0) return rc;
    }

    /* Count non-key columns; if there are none, MERGE has nothing to
     * update or insert beyond the keys themselves — fall back to a
     * straightforward "skip on conflict" shape. */
    size_t n_nonkey = 0;
    for (size_t i = 0; i < n_cols; ++i) {
        if (!is_in(cols[i], keys, n_keys)) ++n_nonkey;
    }

    if (mode != BETL_OC_IGNORE && n_nonkey > 0) {
        if (betl_buf_append(out, " WHEN MATCHED", 13) != 0) return -2;
        if (mode == BETL_OC_UPDATE_IF_CHANGED) {
            /* AND ((T.col <> S.col) OR (T.col IS NULL AND S.col IS NOT NULL)
             * OR (T.col IS NOT NULL AND S.col IS NULL)) — null-safe.
             * MSSQL's `<>` doesn't compare nulls, so the explicit
             * IS-NULL pairs handle the "one side became null" case. */
            if (betl_buf_append(out, " AND (", 6) != 0) return -2;
            int wrote = 0;
            for (size_t i = 0; i < n_cols; ++i) {
                if (is_in(cols[i], keys, n_keys)) continue;
                if (wrote && betl_buf_append(out, " OR ", 4) != 0) return -2;
                wrote = 1;
                if (betl_buf_append(out, "(T.", 3) != 0) return -2;
                rc = append_bracket(out, cols[i]);
                if (rc < 0) return rc;
                if (betl_buf_append(out, " <> S.", 6) != 0) return -2;
                rc = append_bracket(out, cols[i]);
                if (rc < 0) return rc;
                if (betl_buf_append(out, " OR (T.", 7) != 0) return -2;
                rc = append_bracket(out, cols[i]);
                if (rc < 0) return rc;
                if (betl_buf_append(out, " IS NULL AND S.", 15) != 0) return -2;
                rc = append_bracket(out, cols[i]);
                if (rc < 0) return rc;
                if (betl_buf_append(out, " IS NOT NULL) OR (T.", 20) != 0) return -2;
                rc = append_bracket(out, cols[i]);
                if (rc < 0) return rc;
                if (betl_buf_append(out, " IS NOT NULL AND S.", 19) != 0) return -2;
                rc = append_bracket(out, cols[i]);
                if (rc < 0) return rc;
                if (betl_buf_append(out, " IS NULL))", 10) != 0) return -2;
            }
            if (betl_buf_append(out, ")", 1) != 0) return -2;
        }
        if (betl_buf_append(out, " THEN UPDATE SET ", 17) != 0) return -2;
        int wrote_set = 0;
        for (size_t i = 0; i < n_cols; ++i) {
            if (is_in(cols[i], keys, n_keys)) continue;
            if (wrote_set && betl_buf_append(out, ", ", 2) != 0) return -2;
            wrote_set = 1;
            if (betl_buf_append(out, "T.", 2) != 0) return -2;
            rc = append_bracket(out, cols[i]);
            if (rc < 0) return rc;
            if (betl_buf_append(out, " = S.", 5) != 0) return -2;
            rc = append_bracket(out, cols[i]);
            if (rc < 0) return rc;
        }
    }

    if (betl_buf_append(out, " WHEN NOT MATCHED THEN INSERT (", 31) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        rc = append_bracket(out, cols[i]);
        if (rc < 0) return rc;
    }
    if (betl_buf_append(out, ") VALUES (", 10) != 0) return -2;
    for (size_t i = 0; i < n_cols; ++i) {
        if (i && betl_buf_append(out, ", ", 2) != 0) return -2;
        if (betl_buf_append(out, "S.", 2) != 0) return -2;
        rc = append_bracket(out, cols[i]);
        if (rc < 0) return rc;
    }
    if (betl_buf_append(out, ");", 2) != 0) return -2;
    return 0;
}
