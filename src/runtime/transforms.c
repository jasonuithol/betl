/* Standard data-flow transforms (SPEC §4.3) — registration entry point.
 *
 * Each transform component lives in its own translation unit:
 *
 *   transforms_common.c        shared helpers (JSON parse + walkers,
 *                              Arrow leaf releases, schema helpers)
 *   transform_filter.c         `filter`
 *   transform_map.c            `map` (add: + select:)
 *   transform_aggregate.c      `aggregate`
 *   transform_sort.c           `sort`
 *   transform_join.c           `join`
 *
 * This file is just the master register fn.
 */

#include "runtime/transforms.h"
#include "runtime/transforms_internal.h"

int betl_register_transforms(BetlRegistry *r) {
    int rc;
    rc = betl_tx_register_filter(r);    if (rc != BETL_OK) return rc;
    rc = betl_tx_register_map(r);       if (rc != BETL_OK) return rc;
    rc = betl_tx_register_aggregate(r); if (rc != BETL_OK) return rc;
    rc = betl_tx_register_sort(r);      if (rc != BETL_OK) return rc;
    rc = betl_tx_register_join(r);      if (rc != BETL_OK) return rc;
    rc = betl_tx_register_union(r);     if (rc != BETL_OK) return rc;
    rc = betl_tx_register_distinct(r);  if (rc != BETL_OK) return rc;
    rc = betl_tx_register_limit(r);     if (rc != BETL_OK) return rc;
    rc = betl_tx_register_split(r);     if (rc != BETL_OK) return rc;
    rc = betl_tx_register_unpivot(r);   if (rc != BETL_OK) return rc;
    rc = betl_tx_register_pivot(r);     if (rc != BETL_OK) return rc;
    return BETL_OK;
}
