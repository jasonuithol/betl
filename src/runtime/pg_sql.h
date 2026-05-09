/* SQL string-building helpers for the postgres.upsert sink.
 *
 * Pure string code with no dependency on libpq — extracted so it can
 * be unit-tested without a database in the loop. The libpq-using parts
 * of the sink live in postgres_upsert.c and call into here. */

#ifndef BETL_RUNTIME_PG_SQL_H
#define BETL_RUNTIME_PG_SQL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} BetlBuf;

int  betl_buf_reserve(BetlBuf *b, size_t extra);
int  betl_buf_append (BetlBuf *b, const char *s, size_t n);
int  betl_buf_appendf(BetlBuf *b, const char *fmt, ...);

typedef enum {
    BETL_OC_UPDATE = 0,           /* default — overwrite all non-key columns */
    BETL_OC_UPDATE_IF_CHANGED,    /* update only when at least one non-key col differs */
    BETL_OC_IGNORE,               /* DO NOTHING */
    BETL_OC_ERROR                 /* no ON CONFLICT clause; conflict raises */
} BetlOnConflict;

/* Parse "update" / "update_if_changed" / "ignore" / "error" / NULL into
 * `*out`. NULL is accepted and resolves to BETL_OC_UPDATE. Returns 0 on
 * success, -1 if the string isn't recognized. */
int betl_parse_on_conflict(const char *s, BetlOnConflict *out);

/* Build the INSERT...ON CONFLICT SQL for the given column / key list and
 * conflict mode into `out`. Returns:
 *     0   success — out->data holds the NUL-terminated SQL.
 *    -1   identifier contains an embedded `"`.
 *    -2   out of memory.
 *    -3   a key column is not present in the column list.
 * Caller frees `out->data` on success or failure. */
int betl_build_upsert_sql(BetlBuf *out,
                          const char *table,
                          char **cols, size_t n_cols,
                          char **keys, size_t n_keys,
                          BetlOnConflict mode);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_PG_SQL_H */
