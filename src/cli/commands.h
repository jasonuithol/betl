/* Internal: declarations for each `betl <subcommand>` entry point.
 * One function per source file in src/cli/. */

#ifndef BETL_CLI_COMMANDS_H
#define BETL_CLI_COMMANDS_H

int cmd_validate(int argc, char **argv);
int cmd_run     (int argc, char **argv);

#endif /* BETL_CLI_COMMANDS_H */
