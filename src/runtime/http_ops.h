/* http.get / http.post — control-flow TASKs that issue an HTTP
 * request via libcurl and capture the response body to a local
 * file. Pairs with json.read / csv.read downstream. */

#ifndef BETL_RUNTIME_HTTP_OPS_H
#define BETL_RUNTIME_HTTP_OPS_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_http_ops(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
