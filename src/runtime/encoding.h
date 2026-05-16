/* Text-encoding wrappers for I/O sources/sinks (csv.read, csv.write,
 * eventually xml.read / xlsx.read / json.read where they expose an
 * `encoding:` parameter).
 *
 * Strategy: Arrow `"u"` is canonical UTF-8 across the whole pipeline.
 * The only place encodings exist is at the file boundary — we iconv on
 * the way in (raw bytes → UTF-8) and on the way out (UTF-8 → raw
 * bytes). The rest of the runtime never sees a codepage label.
 *
 * Wraps a FILE* via glibc's fopencookie() so existing fgetc/fputs/
 * fprintf/fwrite calls work unchanged once they're pointed at the
 * wrapper. The wrapper takes ownership of the underlying FILE*; closing
 * the wrapper closes the original.
 *
 * Encoding names: case-insensitive. SSIS-style Windows codepage numbers
 * ("1252", "932") are translated to iconv-friendly names ("CP1252",
 * "SHIFT_JIS"). Other names pass through to iconv_open verbatim so any
 * encoding glibc knows about works. "utf-8" / "utf8" / "65001" /
 * empty / NULL is the no-op pass-through (returns the original FILE*).
 *
 * BOM handling:
 *  - betl_textread_wrap auto-skips a leading UTF-8 BOM (EF BB BF) when
 *    `encoding` is UTF-8 (or unset).
 *  - For UTF-16, callers should pass "UTF-16" — glibc's iconv handles
 *    the BOM and detects endianness automatically.
 *
 * Failure modes:
 *  - Unknown encoding → returns NULL, writes to err_buf.
 *  - iconv malformed-input on read → wrapper returns EOF and the
 *    caller's fgetc loop terminates cleanly; check ferror() for
 *    distinction from clean EOF.
 *  - iconv malformed-output on write (a UTF-8 character has no
 *    representation in the target codepage) → writer substitutes a
 *    '?' so the operation keeps progressing. Mirrors SSIS's "Use
 *    code page substitution" default.
 */

#ifndef BETL_RUNTIME_ENCODING_H
#define BETL_RUNTIME_ENCODING_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap `fp` so reads from the returned FILE* yield UTF-8 bytes. If
 * `encoding` is NULL/empty/UTF-8 the function returns `fp` unchanged
 * after consuming any leading UTF-8 BOM. On error returns NULL and
 * writes a description into err_buf (capacity >= 1); `fp` is closed
 * by the caller on failure. Closing the returned FILE* closes the
 * underlying FILE*. */
FILE *betl_textread_wrap(FILE *fp, const char *encoding,
                         char *err_buf, size_t err_cap);

/* Wrap `fp` so writes through the returned FILE* convert UTF-8 input
 * to the target encoding before hitting the underlying FILE*. Same
 * pass-through and ownership rules as betl_textread_wrap. */
FILE *betl_textwrite_wrap(FILE *fp, const char *encoding,
                          char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
