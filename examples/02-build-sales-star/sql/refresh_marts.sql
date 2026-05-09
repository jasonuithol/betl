-- Refresh derived marts that sit on top of fact_sales.
-- Run after fact_sales has been updated for the day. Idempotent.

REFRESH MATERIALIZED VIEW CONCURRENTLY olap.mart_revenue_by_day;
REFRESH MATERIALIZED VIEW CONCURRENTLY olap.mart_top_products_30d;
REFRESH MATERIALIZED VIEW CONCURRENTLY olap.mart_customer_lifetime_value;
