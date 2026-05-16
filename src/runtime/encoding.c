#define _GNU_SOURCE     /* fopencookie */

#include "runtime/encoding.h"

#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================================================== *
 *  Encoding-name normalisation                                     *
 * ============================================================== */

/* Returns 1 if the name resolves to UTF-8 (i.e. no conversion). The
 * pass-through path also handles NULL / empty / whitespace. */
static int is_utf8_name(const char *name) {
    if (!name) return 1;
    while (*name == ' ' || *name == '\t') ++name;
    if (!*name) return 1;
    if (strcasecmp(name, "utf-8") == 0) return 1;
    if (strcasecmp(name, "utf8")  == 0) return 1;
    if (strcasecmp(name, "u8")    == 0) return 1;
    if (strcmp    (name, "65001") == 0) return 1;
    return 0;
}

/* Map a few common shorthand spellings to iconv canonical names so
 * users can write `encoding: cp1252` or `encoding: 1252` and get the
 * obvious thing. Anything we don't recognise is passed verbatim to
 * iconv_open — glibc accepts hundreds of aliases natively. */
static const char *canonical_name(const char *name) {
    if (!name) return "UTF-8";
    /* SSIS often stores CodePage as a bare number. */
    if (strcmp(name, "1250") == 0) return "CP1250";
    if (strcmp(name, "1251") == 0) return "CP1251";
    if (strcmp(name, "1252") == 0) return "CP1252";
    if (strcmp(name, "1253") == 0) return "CP1253";
    if (strcmp(name, "1254") == 0) return "CP1254";
    if (strcmp(name, "1255") == 0) return "CP1255";
    if (strcmp(name, "1256") == 0) return "CP1256";
    if (strcmp(name, "1257") == 0) return "CP1257";
    if (strcmp(name, "1258") == 0) return "CP1258";
    if (strcmp(name, "874")  == 0) return "CP874";
    if (strcmp(name, "932")  == 0) return "SHIFT_JIS";
    if (strcmp(name, "936")  == 0) return "GBK";
    if (strcmp(name, "949")  == 0) return "EUC-KR";
    if (strcmp(name, "950")  == 0) return "BIG5";
    if (strcmp(name, "1200") == 0) return "UTF-16LE";
    if (strcmp(name, "1201") == 0) return "UTF-16BE";
    if (strcasecmp(name, "windows-1252") == 0) return "CP1252";
    if (strcasecmp(name, "windows-1250") == 0) return "CP1250";
    if (strcasecmp(name, "shift-jis")    == 0) return "SHIFT_JIS";
    return name;
}

/* ============================================================== *
 *  Reader cookie — raw bytes in, UTF-8 bytes out                   *
 * ============================================================== */

typedef struct {
    FILE   *raw;
    iconv_t cd;
    /* Hold-back buffer: when iconv hits a partial multi-byte sequence
     * at the end of a read chunk, we keep the trailing bytes here for
     * the next call. */
    char    pending[16];
    size_t  pending_len;
    int     eof_raw;
} ReadCookie;

static ssize_t read_cb(void *cookie, char *buf, size_t size) {
    ReadCookie *rc = cookie;
    if (size == 0) return 0;

    /* Single iconv pass per call. Pulls a chunk from `raw`, optionally
     * prefixes the pending bytes from the last partial conversion, and
     * writes UTF-8 into `buf`. */
    char in_chunk[4096];
    size_t prefix = rc->pending_len;
    if (prefix > sizeof in_chunk) prefix = sizeof in_chunk;
    memcpy(in_chunk, rc->pending, prefix);
    rc->pending_len = 0;

    size_t want = sizeof in_chunk - prefix;
    size_t got  = 0;
    if (!rc->eof_raw && want > 0) {
        got = fread(in_chunk + prefix, 1, want, rc->raw);
        if (got == 0) {
            if (feof(rc->raw)) rc->eof_raw = 1;
            else if (ferror(rc->raw)) return -1;
        }
    }
    size_t in_total = prefix + got;
    if (in_total == 0) {
        /* Final flush of iconv state for stateful encodings. */
        char  *outp = buf;
        size_t out_left = size;
        size_t r = iconv(rc->cd, NULL, NULL, &outp, &out_left);
        if (r == (size_t)-1 && errno != E2BIG) return 0;
        return (ssize_t)(size - out_left);
    }

    char  *inp     = in_chunk;
    size_t in_left = in_total;
    char  *outp    = buf;
    size_t out_left = size;
    size_t r = iconv(rc->cd, &inp, &in_left, &outp, &out_left);
    if (r == (size_t)-1) {
        if (errno == EINVAL) {
            /* Partial multi-byte at chunk end — stash the unconverted
             * tail for next call. Always fits because in_left < 8. */
            if (in_left <= sizeof rc->pending) {
                memcpy(rc->pending, inp, in_left);
                rc->pending_len = in_left;
            } else {
                /* Should be impossible for any single-codepoint encoding,
                 * but guard against runaway. */
                errno = EILSEQ;
                return -1;
            }
        } else if (errno == E2BIG) {
            /* Output buffer full — caller will call us again. Stash the
             * unread input. */
            if (in_left <= sizeof rc->pending) {
                memcpy(rc->pending, inp, in_left);
                rc->pending_len = in_left;
            } else {
                /* Larger leftover than we can stash; the typical chunk-
                 * size mismatch shouldn't reach here, but fall back to
                 * a partial read so the caller can re-drive us. */
                memcpy(rc->pending, inp, sizeof rc->pending);
                rc->pending_len = sizeof rc->pending;
                /* Drop the rest. Conservative — but with `size` being
                 * stdio's BUFSIZ (~4-8KB) and a 4096-byte in_chunk,
                 * this branch is unreachable. */
            }
        } else { /* EILSEQ — malformed input. */
            return -1;
        }
    }
    return (ssize_t)(size - out_left);
}

static int close_cb(void *cookie) {
    ReadCookie *rc = cookie;
    int err = fclose(rc->raw);
    iconv_close(rc->cd);
    free(rc);
    return err;
}

/* Skip a leading UTF-8 BOM (EF BB BF) on `fp` if present. Pure peek-
 * and-replace via ungetc so the parser never sees the BOM bytes. */
static void skip_utf8_bom(FILE *fp) {
    int b1 = fgetc(fp);
    if (b1 != 0xEF) { if (b1 != EOF) ungetc(b1, fp); return; }
    int b2 = fgetc(fp);
    if (b2 != 0xBB) {
        if (b2 != EOF) ungetc(b2, fp);
        ungetc(b1, fp);
        return;
    }
    int b3 = fgetc(fp);
    if (b3 != 0xBF) {
        if (b3 != EOF) ungetc(b3, fp);
        ungetc(b2, fp);
        ungetc(b1, fp);
        return;
    }
    /* All three bytes were the BOM — eaten. */
}

FILE *betl_textread_wrap(FILE *fp, const char *encoding,
                         char *err_buf, size_t err_cap) {
    if (err_buf && err_cap > 0) err_buf[0] = '\0';
    if (!fp) {
        if (err_buf) snprintf(err_buf, err_cap, "null file pointer");
        return NULL;
    }
    if (is_utf8_name(encoding)) {
        /* UTF-8 / unset: the bytes are already canonical. Just clean
         * up the BOM if present. */
        skip_utf8_bom(fp);
        return fp;
    }

    const char *iconv_from = canonical_name(encoding);
    iconv_t cd = iconv_open("UTF-8", iconv_from);
    if (cd == (iconv_t)-1) {
        if (err_buf) {
            snprintf(err_buf, err_cap,
                     "unsupported source encoding '%s' (iconv_open: %s)",
                     encoding, strerror(errno));
        }
        return NULL;
    }

    ReadCookie *rc = calloc(1, sizeof *rc);
    if (!rc) {
        iconv_close(cd);
        if (err_buf) snprintf(err_buf, err_cap, "out of memory");
        return NULL;
    }
    rc->raw = fp;
    rc->cd  = cd;

    cookie_io_functions_t fns = {
        .read  = read_cb,
        .write = NULL,
        .seek  = NULL,
        .close = close_cb,
    };
    FILE *wrapped = fopencookie(rc, "r", fns);
    if (!wrapped) {
        iconv_close(cd);
        free(rc);
        if (err_buf) snprintf(err_buf, err_cap,
                              "fopencookie failed: %s", strerror(errno));
        return NULL;
    }
    return wrapped;
}

/* ============================================================== *
 *  Writer cookie — UTF-8 bytes in, target-encoding bytes out       *
 * ============================================================== */

typedef struct {
    FILE   *raw;
    iconv_t cd;
    /* Hold-back for partial UTF-8 sequences split across two
     * fwrite() calls from the caller. */
    char    pending[8];
    size_t  pending_len;
} WriteCookie;

/* Flush `n` UTF-8 bytes starting at `in` through iconv into `raw`.
 * On unrepresentable characters, substitute '?' and skip the offending
 * input sequence — mirrors SSIS's default "use codepage substitution"
 * behaviour. Returns 0 on success, -1 on iconv-state error. */
static int write_chunk(WriteCookie *wc, const char *in, size_t n) {
    char *inp = (char *)in;
    size_t in_left = n;
    char out_chunk[4096];
    while (in_left > 0) {
        char  *outp = out_chunk;
        size_t out_left = sizeof out_chunk;
        size_t r = iconv(wc->cd, &inp, &in_left, &outp, &out_left);
        size_t produced = sizeof out_chunk - out_left;
        if (produced > 0) {
            if (fwrite(out_chunk, 1, produced, wc->raw) != produced) {
                return -1;
            }
        }
        if (r == (size_t)-1) {
            if (errno == E2BIG) {
                continue;             /* drain again */
            }
            if (errno == EILSEQ) {
                /* Unrepresentable in the target codepage. Substitute
                 * '?' for the offending codepoint and skip its UTF-8
                 * sequence. */
                if (fputc('?', wc->raw) == EOF) return -1;
                if (in_left == 0) break;
                /* Skip one UTF-8 codepoint. Length from lead byte. */
                unsigned char lb = (unsigned char)inp[0];
                size_t skip = 1;
                if ((lb & 0xE0) == 0xC0) skip = 2;
                else if ((lb & 0xF0) == 0xE0) skip = 3;
                else if ((lb & 0xF8) == 0xF0) skip = 4;
                if (skip > in_left) skip = in_left;
                inp += skip;
                in_left -= skip;
                /* Reset iconv state — some stateful encodings need this
                 * after a malformed sequence. */
                iconv(wc->cd, NULL, NULL, NULL, NULL);
                continue;
            }
            if (errno == EINVAL) {
                /* Trailing partial UTF-8 — stash for next call. */
                if (in_left <= sizeof wc->pending) {
                    memcpy(wc->pending, inp, in_left);
                    wc->pending_len = in_left;
                    in_left = 0;
                    break;
                }
                return -1;
            }
            return -1;
        }
    }
    return 0;
}

static ssize_t write_cb(void *cookie, const char *buf, size_t size) {
    WriteCookie *wc = cookie;
    /* Prepend any pending UTF-8 prefix and convert in one shot. */
    if (wc->pending_len) {
        char joined[8 + 4096];
        size_t take = size < (sizeof joined - wc->pending_len)
                          ? size
                          : (sizeof joined - wc->pending_len);
        memcpy(joined, wc->pending, wc->pending_len);
        memcpy(joined + wc->pending_len, buf, take);
        size_t total = wc->pending_len + take;
        wc->pending_len = 0;
        if (write_chunk(wc, joined, total) != 0) return -1;
        if (take < size) {
            if (write_chunk(wc, buf + take, size - take) != 0) return -1;
        }
    } else {
        if (write_chunk(wc, buf, size) != 0) return -1;
    }
    return (ssize_t)size;
}

static int close_write_cb(void *cookie) {
    WriteCookie *wc = cookie;
    /* Flush any iconv state to the underlying file. */
    char out_chunk[16];
    char *outp = out_chunk;
    size_t out_left = sizeof out_chunk;
    iconv(wc->cd, NULL, NULL, &outp, &out_left);
    size_t produced = sizeof out_chunk - out_left;
    if (produced > 0) fwrite(out_chunk, 1, produced, wc->raw);
    int err = fclose(wc->raw);
    iconv_close(wc->cd);
    free(wc);
    return err;
}

FILE *betl_textwrite_wrap(FILE *fp, const char *encoding,
                          char *err_buf, size_t err_cap) {
    if (err_buf && err_cap > 0) err_buf[0] = '\0';
    if (!fp) {
        if (err_buf) snprintf(err_buf, err_cap, "null file pointer");
        return NULL;
    }
    if (is_utf8_name(encoding)) {
        /* UTF-8: bytes pass through. No BOM is emitted; CSV writers
         * traditionally produce BOM-less UTF-8. Callers that want a
         * BOM should write the three bytes themselves. */
        return fp;
    }

    const char *iconv_to = canonical_name(encoding);
    iconv_t cd = iconv_open(iconv_to, "UTF-8");
    if (cd == (iconv_t)-1) {
        if (err_buf) {
            snprintf(err_buf, err_cap,
                     "unsupported target encoding '%s' (iconv_open: %s)",
                     encoding, strerror(errno));
        }
        return NULL;
    }

    WriteCookie *wc = calloc(1, sizeof *wc);
    if (!wc) {
        iconv_close(cd);
        if (err_buf) snprintf(err_buf, err_cap, "out of memory");
        return NULL;
    }
    wc->raw = fp;
    wc->cd  = cd;

    cookie_io_functions_t fns = {
        .read  = NULL,
        .write = write_cb,
        .seek  = NULL,
        .close = close_write_cb,
    };
    FILE *wrapped = fopencookie(wc, "w", fns);
    if (!wrapped) {
        iconv_close(cd);
        free(wc);
        if (err_buf) snprintf(err_buf, err_cap,
                              "fopencookie failed: %s", strerror(errno));
        return NULL;
    }
    return wrapped;
}
