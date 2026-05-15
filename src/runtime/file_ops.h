/* file.copy / file.move / file.delete — control-flow TASKs that
 * implement the common File System Task operations from SSIS.
 * betl-dtsx2yaml emits these shapes; the SSIS source/destination
 * file fields land in `src:`/`dst:`/`path:`. */

#ifndef BETL_RUNTIME_FILE_OPS_H
#define BETL_RUNTIME_FILE_OPS_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_file_ops(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
