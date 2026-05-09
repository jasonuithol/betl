-- One-time setup for the warehouse Postgres instance.
-- Run before the pipeline's first run.

CREATE SCHEMA IF NOT EXISTS stg;

CREATE TABLE IF NOT EXISTS stg.orders (
  order_id     BIGINT        PRIMARY KEY,
  customer_id  BIGINT        NOT NULL,
  sku          TEXT          NOT NULL,
  quantity     INTEGER       NOT NULL,
  unit_price   NUMERIC(12,2) NOT NULL,
  ordered_at   TIMESTAMPTZ   NOT NULL,
  notes        TEXT,
  load_date    DATE          NOT NULL,
  source_file  TEXT          NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_stg_orders_load_date ON stg.orders(load_date);
