/* xlsx.write — SINK that writes Arrow batches to an .xlsx file via
 * libxlsxwriter. SSIS Excel Destination parity. Conditionally compiled
 * when libxlsxwriter is available. */

#ifndef BETL_RUNTIME_XLSX_WRITE_H
#define BETL_RUNTIME_XLSX_WRITE_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_xlsx_write(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
