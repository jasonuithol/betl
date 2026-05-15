/* json.write — SINK that writes incoming rows as NDJSON (one object
 * per line) or a JSON array of objects. */

#ifndef BETL_RUNTIME_JSON_WRITE_H
#define BETL_RUNTIME_JSON_WRITE_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_json_write(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
