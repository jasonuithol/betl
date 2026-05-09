/* Smoke test for the public ABI header. Confirms every advertised
 * macro, type, and enum is visible and the included Arrow C Data /
 * Stream Interface structs have the expected shape. */

#include "betl/provider.h"

#include <stdint.h>
#include <stddef.h>

int main(void) {
    /* Macros */
    uint32_t  abi    = BETL_ABI_VERSION;
    int64_t   afflag = ARROW_FLAG_NULLABLE | ARROW_FLAG_DICTIONARY_ORDERED
                                          | ARROW_FLAG_MAP_KEYS_SORTED;

    /* Enums */
    BetlStatus        s = BETL_OK;
    BetlLogLevel      l = BETL_LOG_INFO;
    BetlComponentKind k = BETL_KIND_TRANSFORM;
    BetlSchemaMode    m = BETL_SCHEMA_DERIVED;
    uint32_t          f = BETL_FLAG_THREADSAFE
                        | BETL_FLAG_DETERMINISTIC
                        | BETL_FLAG_TRANSACTIONAL;

    /* Arrow C structs — ensure layout fields are accessible. */
    struct ArrowSchema      sch = {0};
    struct ArrowArray       arr = {0};
    struct ArrowArrayStream st  = {0};
    sch.format = "i";
    arr.length = 0;
    st.private_data = NULL;

    /* Component / port / provider structs. */
    BetlPortDef       port = { .name = "out", .schema_mode = BETL_SCHEMA_STATIC };
    BetlComponentDef  comp = { .name = "x", .kind = BETL_KIND_SOURCE };
    BetlProvider      prov = { .abi_version = BETL_ABI_VERSION,
                               .name = "p", .version = "0", .license = "Apache-2.0" };

    (void)abi; (void)afflag; (void)s; (void)l; (void)k; (void)m; (void)f;
    (void)sch; (void)arr; (void)st; (void)port; (void)comp; (void)prov;
    return 0;
}
