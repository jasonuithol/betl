/* betl CLI entry point — argv dispatch only. Subcommands live in
 * sibling translation units. */

#include <stdio.h>
#include <string.h>

#include "betl/version.h"
#include "cli/commands.h"

static const struct {
    const char *name;
    int (*fn)(int, char **);
    const char *summary;
} COMMANDS[] = {
    { "validate", cmd_validate,  "Statically check a betl file" },
    { "run",      cmd_run,       "Execute a betl pipeline file" },
    { NULL,       NULL,          NULL                            },
};

static void print_usage(FILE *out) {
    fprintf(out, "betl %s — Better ETL\n\n", BETL_VERSION);
    fprintf(out, "Usage: betl <command> [args...]\n");
    fprintf(out, "       betl --version\n");
    fprintf(out, "       betl --help\n\n");
    fprintf(out, "Commands:\n");
    for (size_t i = 0; COMMANDS[i].name; ++i) {
        fprintf(out, "  %-12s  %s\n", COMMANDS[i].name, COMMANDS[i].summary);
    }
    fprintf(out, "\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(stderr); return 2; }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("betl %s\n", BETL_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(stdout);
        return 0;
    }

    for (size_t i = 0; COMMANDS[i].name; ++i) {
        if (strcmp(COMMANDS[i].name, argv[1]) == 0) {
            return COMMANDS[i].fn(argc - 1, argv + 1);
        }
    }

    fprintf(stderr, "unknown command: %s\n\n", argv[1]);
    print_usage(stderr);
    return 2;
}
