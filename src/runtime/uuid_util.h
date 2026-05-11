#ifndef BETL_RUNTIME_UUID_UTIL_H
#define BETL_RUNTIME_UUID_UTIL_H

/* Helpers for Arrow fixed-size-binary(16) UUID columns.
 *
 * Arrow leaf format: "w:16" — 16 raw bytes per cell, no offsets.
 * Values are stored as the canonical big-endian byte order, matching
 * `uuid_to_string` from RFC 4122 (so 0x12345678 0x9abc... renders as
 * "12345678-9abc-..."). */

#include <stdint.h>
#include <stddef.h>

/* Parse the canonical 36-char UUID form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
 * into the 16-byte `out` buffer. Returns 0 on success, -1 on malformed
 * input (wrong length, missing dashes, non-hex digits). */
int betl_uuid_parse(const char *s, size_t n, uint8_t out[16]);

/* Format `in` as a 36-char dashed hex string into `buf` (no NUL).
 * `cap` must be at least 36. Returns 36 on success, -1 on cap overflow. */
int betl_uuid_format(const uint8_t in[16], char *buf, size_t cap);

#endif
