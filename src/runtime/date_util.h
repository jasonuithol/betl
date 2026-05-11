#ifndef BETL_RUNTIME_DATE_UTIL_H
#define BETL_RUNTIME_DATE_UTIL_H

/* Small shared helpers for Arrow date32 / timestamp_us columns.
 *
 * date32: int32 days since 1970-01-01.
 * ts_us:  int64 microseconds since 1970-01-01 00:00:00 UTC, no tz.
 *
 * Civil ↔ days math uses Howard Hinnant's algorithm — full Arrow
 * date32 range. Parsers accept ISO 8601 textual forms ("YYYY-MM-DD"
 * and "YYYY-MM-DD HH:MM:SS[.uuuuuu]" with optional 'T' separator).
 * Postgres returns these as-is in text mode; mssql via ODBC has
 * structured types but we stringify those to the same form for the
 * csv-style reader paths. */

#include <stdint.h>
#include <stddef.h>

/* Civil → days since 1970-01-01. y is the full year (no offset). */
int32_t betl_days_from_civil(int y, unsigned m, unsigned d);

/* Days since 1970-01-01 → civil. */
void betl_civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d);

/* Split a ts_us into (days, micros-of-day) with Euclidean (floor) division. */
void betl_split_ts(int64_t us, int32_t *out_days, int64_t *out_us_of_day);

/* Parse "YYYY-MM-DD" → days. Returns 0 on success, -1 on parse error. */
int betl_parse_iso_date(const char *s, size_t n, int32_t *out_days);

/* Parse "YYYY-MM-DD HH:MM:SS[.uuuuuu]" or with 'T' separator → micros.
 * Fractional seconds: up to 6 digits, missing digits treated as zero. */
int betl_parse_iso_ts(const char *s, size_t n, int64_t *out_us);

/* Parse a timestamp with an optional trailing tz suffix and normalize to
 * UTC micros. Accepted forms:
 *   YYYY-MM-DD HH:MM:SS[.uuuuuu]            — no tz, treated as already UTC
 *   YYYY-MM-DDTHH:MM:SS[.uuuuuu]Z           — UTC sentinel
 *   YYYY-MM-DD HH:MM:SS[.uuuuuu]+HH         — offset, no minutes
 *   YYYY-MM-DD HH:MM:SS[.uuuuuu]+HH:MM      — offset with minutes
 *   YYYY-MM-DD HH:MM:SS[.uuuuuu]+HHMM       — offset, no separator
 * The offset is subtracted to convert into UTC. Returns 0 on success,
 * -1 on parse error. */
int betl_parse_iso_tstz(const char *s, size_t n, int64_t *out_us);

#endif
