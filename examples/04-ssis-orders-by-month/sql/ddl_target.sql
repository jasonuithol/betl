-- One-time setup for the warehouse Postgres instance.
-- Target schema for example 04 — orders_by_month with derived columns.

CREATE SCHEMA IF NOT EXISTS analytics;

CREATE TABLE IF NOT EXISTS analytics.orders_by_month (
  order_id        BIGINT     PRIMARY KEY,
  customer_name   TEXT       NOT NULL,
  order_date      DATE       NOT NULL,
  delivered_at    TIMESTAMP,
  total_cents     BIGINT     NOT NULL,
  -- Derived columns produced by the SSIS-EL `enrich` step:
  order_month     BIGINT     NOT NULL,   -- 1..12
  order_year      BIGINT     NOT NULL,
  due_date        DATE       NOT NULL,
  is_overdue      BOOLEAN    NOT NULL,
  customer_upper  TEXT       NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_orders_by_month_ym
  ON analytics.orders_by_month (order_year, order_month);
