/* xlsx.read — SOURCE that reads rows from an .xlsx file via libxlsxio.
 * SSIS Excel Source parity. Conditionally compiled when libxlsxio is
 * available. */

#ifndef BETL_RUNTIME_XLSX_READ_H
#define BETL_RUNTIME_XLSX_READ_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_xlsx_read(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
