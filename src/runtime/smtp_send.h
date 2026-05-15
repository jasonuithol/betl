/* smtp.send — control-flow TASK that sends an email via SMTP using
 * libcurl. SSIS Send Mail Task parity. */

#ifndef BETL_RUNTIME_SMTP_SEND_H
#define BETL_RUNTIME_SMTP_SEND_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_smtp_send(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
