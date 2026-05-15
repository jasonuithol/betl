/* json.read — SOURCE that reads JSON Lines (NDJSON) or a JSON array
 * of objects and emits one Arrow row per object. v0.1 emits every
 * column as utf8; downstream `map` + `ssisexpr` casts are the
 * conversion path. */

#ifndef BETL_RUNTIME_JSON_READ_H
#define BETL_RUNTIME_JSON_READ_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_json_read(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
