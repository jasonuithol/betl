/* xml.read — SOURCE that walks an XML file via libxml2 + XPath.
 * SSIS XML Source parity. Conditionally compiled when libxml2 is
 * available. */

#ifndef BETL_RUNTIME_XML_READ_H
#define BETL_RUNTIME_XML_READ_H

#include "loader/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

int betl_register_xml_read(BetlRegistry *r);

#ifdef __cplusplus
}
#endif

#endif
