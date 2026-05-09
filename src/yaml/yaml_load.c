#include "yaml/yaml_load.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int betl_yaml_load_file(const char *path, BetlYamlDoc *out) {
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(out->error, sizeof(out->error),
                 "cannot open: %s", strerror(errno));
        return 1;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        snprintf(out->error, sizeof(out->error),
                 "yaml_parser_initialize failed");
        fclose(fp);
        return 1;
    }
    yaml_parser_set_input_file(&parser, fp);

    const int ok = yaml_parser_load(&parser, &out->doc);
    if (!ok) {
        snprintf(out->error, sizeof(out->error),
                 "YAML parse error at line %lu, column %lu: %s",
                 (unsigned long)parser.problem_mark.line + 1,
                 (unsigned long)parser.problem_mark.column + 1,
                 parser.problem ? parser.problem : "(unknown)");
    } else {
        out->loaded = 1;
    }

    yaml_parser_delete(&parser);
    fclose(fp);
    return ok ? 0 : 1;
}

int betl_yaml_root_has_key(const BetlYamlDoc *doc, const char *key) {
    if (!doc->loaded) return 0;

    /* yaml_document_get_root_node / get_node are not declared as taking
     * const pointers, so cast away const to call them. We do not mutate. */
    yaml_document_t *d = (yaml_document_t *)&doc->doc;
    yaml_node_t *root = yaml_document_get_root_node(d);
    if (!root || root->type != YAML_MAPPING_NODE) return 0;

    for (yaml_node_pair_t *p = root->data.mapping.pairs.start;
         p < root->data.mapping.pairs.top; ++p) {
        yaml_node_t *k = yaml_document_get_node(d, p->key);
        if (k && k->type == YAML_SCALAR_NODE
            && strcmp((const char *)k->data.scalar.value, key) == 0) {
            return 1;
        }
    }
    return 0;
}

void betl_yaml_free(BetlYamlDoc *doc) {
    if (doc->loaded) {
        yaml_document_delete(&doc->doc);
        doc->loaded = 0;
    }
}
