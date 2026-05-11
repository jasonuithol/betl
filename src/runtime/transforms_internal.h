/* Private header shared by the standard transform implementations
 * (filter / map / aggregate / sort / join). Not part of the public
 * provider ABI — symbols here are linked into betl_core and reachable
 * to in-tree code via the "runtime/transforms_internal.h" include path.
 */

#ifndef BETL_RUNTIME_TRANSFORMS_INTERNAL_H
#define BETL_RUNTIME_TRANSFORMS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "betl/provider.h"
#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JSON helpers (string-based; no proper parser yet) ----------------- */

const char *betl_tx_json_value_after(const char *json, const char *key);
int  betl_tx_json_decode_str(const char *p, char **out);
int  betl_tx_json_string_at(const char *json, const char *key, char **out);
int  betl_tx_json_value_to_string(const char *json, const char *key, char **out);

/* JSON object walker. cb is called for each top-level key/value pair;
 * value points into the original JSON, value_len is the substring
 * length. Returns 0 on success, -1 on malformed input or cb-returned
 * non-zero. */
typedef int (*betl_tx_kv_visit_fn)(const char *key,
                                   const char *value, size_t value_len,
                                   void *user);
int betl_tx_json_walk_object(const char *p,
                             betl_tx_kv_visit_fn cb, void *user);

/* JSON array walker. cb is called for each element. */
typedef int (*betl_tx_item_visit_fn)(const char *value, size_t value_len,
                                     void *user);
int betl_tx_json_walk_array(const char *p,
                            betl_tx_item_visit_fn cb, void *user);


/* ---- Arrow leaf releases ---------------------------------------------- */

void betl_tx_release_int64_leaf(struct ArrowArray *arr);
void betl_tx_release_utf8_leaf (struct ArrowArray *arr);
void betl_tx_release_struct    (struct ArrowArray *arr);

/* Element width in bytes for fixed-width primitive Arrow formats.
 * Returns 0 for non-fixed-width / unknown formats, and for formats
 * that need element-specific handling (e.g. utf8). */
static inline size_t betl_tx_fixed_width_for_fmt(char fmt) {
    switch (fmt) {
        case 'c': case 'C': case 'b': return 1;
        case 's': case 'S':           return 2;
        case 'i': case 'I': case 'f': return 4;
        case 'l': case 'L': case 'g': return 8;
        default: return 0;
    }
}

/* Schema leaf with a strdup'd `name` and a static-literal `format`. */
void betl_tx_release_schema_named_leaf(struct ArrowSchema *sch);

/* Schema struct that owns its children pointer array; recursively
 * frees each child (with their own release) plus the array. */
void betl_tx_release_schema_struct_owned(struct ArrowSchema *sch);

/* Build a leaf schema struct with a strdup'd name and the given (static)
 * format string. Returns NULL on OOM. */
struct ArrowSchema *betl_tx_new_leaf_schema(const char *name,
                                            const char *format);


/* ---- Row-mask leaf builders ------------------------------------------- */

/* Build a fresh int64 / utf8 leaf containing only the rows of `src`
 * for which `keep[i]` != 0. n_rows is the input length, n_kept the
 * count of rows kept (caller-precomputed). Validity bits are
 * preserved from src. Returns 0 on success, -1 on OOM.
 *
 * Used by filter, distinct, limit, and conditional_split — all four
 * follow the "input batch + keep mask → smaller output batch" shape.
 */
int betl_tx_build_int64_filtered(struct ArrowArray *out,
                                 const struct ArrowArray *src,
                                 const uint8_t *keep, size_t n_rows,
                                 size_t n_kept);
int betl_tx_build_utf8_filtered (struct ArrowArray *out,
                                 const struct ArrowArray *src,
                                 const uint8_t *keep, size_t n_rows,
                                 size_t n_kept);

/* Same shape, but for any fixed-width primitive leaf (1/2/4/8-byte
 * element). Element width given by `elem_size`. */
int betl_tx_build_fixed_filtered(struct ArrowArray *out,
                                 const struct ArrowArray *src,
                                 size_t elem_size,
                                 const uint8_t *keep, size_t n_rows,
                                 size_t n_kept);


/* ---- Bit helpers ------------------------------------------------------ */

static inline int betl_tx_bit_at(const uint8_t *bm, size_t i) {
    return (bm[i / 8] >> (i % 8)) & 1u;
}


/* ---- Per-component registration entry points -------------------------- */

int betl_tx_register_filter   (BetlRegistry *r);
int betl_tx_register_map      (BetlRegistry *r);
int betl_tx_register_aggregate(BetlRegistry *r);
int betl_tx_register_sort     (BetlRegistry *r);
int betl_tx_register_join     (BetlRegistry *r);
int betl_tx_register_union    (BetlRegistry *r);
int betl_tx_register_distinct (BetlRegistry *r);
int betl_tx_register_limit    (BetlRegistry *r);
int betl_tx_register_split    (BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_TRANSFORMS_INTERNAL_H */
