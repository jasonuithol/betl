/* SQL string-building helpers for the mssql.upsert sink.
 *
 * The MERGE statement that SQL Server uses for UPSERT is markedly
 * different from postgres' INSERT…ON CONFLICT, so the builder lives
 * in its own translation unit. Reuses BetlBuf / BetlOnConflict /
 * betl_parse_on_conflict from pg_sql.h since those concepts are
 * backend-agnostic. */

#ifndef BETL_RUNTIME_MSSQL_SQL_H
#define BETL_RUNTIME_MSSQL_SQL_H

#include "runtime/pg_sql.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build an upsert statement for SQL Server: a MERGE for UPDATE /
 * UPDATE_IF_CHANGED / IGNORE modes, or a plain INSERT for the ERROR
 * mode. Identifiers are bracket-quoted ([name]); placeholders are
 * positional `?` (ODBC's convention).
 *
 * Returns:
 *     0   success — out->data holds the NUL-terminated SQL.
 *    -1   identifier contains an embedded `]` (which we cannot escape
 *         safely without a richer quoting strategy).
 *    -2   out of memory.
 *    -3   a key column is not present in the column list.
 * Caller frees `out->data` on success or failure. */
int betl_build_mssql_merge_sql(BetlBuf *out,
                               const char *table,
                               char **cols, size_t n_cols,
                               char **keys, size_t n_keys,
                               BetlOnConflict mode);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_MSSQL_SQL_H */
