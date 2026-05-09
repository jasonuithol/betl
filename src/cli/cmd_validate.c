/* `betl validate <file>`
 *
 * For pipeline files (top-level `betl: <version>`):
 *   - parse the YAML
 *   - build the in-memory stage / step graph
 *   - check uniqueness of ids, resolve `from:` and `after:` references,
 *     detect cycles
 *   - print a one-line summary on success or `path:line:col: reason`
 *     on failure
 *
 * For connections bundles (`betl_connections: <version>`) and schema
 * files (`betl_schema: <version>`) we still only do the structural
 * pre-check until each gets its own parser. */

#include <stdio.h>
#include <string.h>

#include "cli/commands.h"
#include "pipeline/pipeline.h"
#include "yaml/yaml_load.h"

static int validate_pipeline(const char *path) {
    char err[1024];
    BetlPipeline *p = betl_pipeline_load(path, err, sizeof err);
    if (!p) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    size_t stages = betl_pipeline_stage_count(p);
    size_t steps  = betl_pipeline_total_steps(p);
    const char *name = betl_pipeline_name(p);
    printf("%s: OK (%s, %zu stage%s, %zu step%s)\n",
           path,
           name ? name : "(unnamed)",
           stages, stages == 1 ? "" : "s",
           steps,  steps  == 1 ? "" : "s");
    betl_pipeline_free(p);
    return 0;
}

static int validate_other(const char *path, const char *kind) {
    /* Bundles / schema files don't have a parser yet — keep the
     * structural-OK message for them. */
    BetlYamlDoc doc;
    if (betl_yaml_load_file(path, &doc) != 0) {
        fprintf(stderr, "%s: %s\n", path, doc.error);
        betl_yaml_free(&doc);
        return 1;
    }
    printf("%s: %s — structural check OK\n", path, kind);
    fprintf(stderr,
        "note: full schema validation for %s files is not yet wired up.\n",
        kind);
    betl_yaml_free(&doc);
    return 0;
}

int cmd_validate(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: betl validate <file>\n");
        return 2;
    }
    const char *path = argv[1];

    /* Quick structural sniff to choose the right parser. */
    BetlYamlDoc doc;
    if (betl_yaml_load_file(path, &doc) != 0) {
        fprintf(stderr, "%s: %s\n", path, doc.error);
        betl_yaml_free(&doc);
        return 1;
    }
    int is_pipeline    = betl_yaml_root_has_key(&doc, "betl");
    int is_connections = betl_yaml_root_has_key(&doc, "betl_connections");
    int is_schema      = betl_yaml_root_has_key(&doc, "betl_schema");
    betl_yaml_free(&doc);

    if (is_pipeline)    return validate_pipeline(path);
    if (is_connections) return validate_other(path, "betl_connections");
    if (is_schema)      return validate_other(path, "betl_schema");

    fprintf(stderr,
        "%s: not a betl document — top-level mapping has no recognized "
        "discriminator key (betl / betl_connections / betl_schema)\n",
        path);
    return 1;
}
