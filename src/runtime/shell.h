/* shell — control-flow TASK that runs an external process with a
 * literal argv list (no shell expansion). SSIS Execute Process Task
 * parity; betl-dtsx2yaml emits this shape. */

#ifndef BETL_RUNTIME_SHELL_H
#define BETL_RUNTIME_SHELL_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_shell(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
