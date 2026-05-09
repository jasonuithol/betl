/* Internal: thin wrapper around libyaml that the rest of the engine
 * uses. Owns a yaml_document_t and exposes the small set of
 * traversal helpers we actually need. NOT public — plugins do not
 * see this header. */

#ifndef BETL_YAML_LOAD_H
#define BETL_YAML_LOAD_H

#include <yaml.h>

typedef struct {
    yaml_document_t doc;
    int             loaded;     /* 1 when `doc` holds a successfully-parsed tree */
    char            error[256]; /* last error message; valid when load returns nonzero */
} BetlYamlDoc;

/* Parse a YAML file. Returns 0 on success, nonzero on failure (with
 * an explanatory message in `out->error`). On failure, `betl_yaml_free`
 * is still safe to call (it's a no-op). */
int  betl_yaml_load_file(const char *path, BetlYamlDoc *out);

/* True iff the root node is a mapping containing the given key. */
int  betl_yaml_root_has_key(const BetlYamlDoc *doc, const char *key);

void betl_yaml_free(BetlYamlDoc *doc);

#endif /* BETL_YAML_LOAD_H */
