-- One-time setup for the warehouse Postgres instance.
-- Run before the pipeline's first run.

CREATE SCHEMA IF NOT EXISTS stg;

-- Note: betl's current type set is int64 / utf8 / date / timestamp, so:
--  * unit_price is stored as whole cents (BIGINT). Convert to dollars in
--    reporting queries with `unit_price / 100.0`.
--  * ordered_at uses TIMESTAMP (no tz) — pair with TIMESTAMPTZ once betl
--    grows tz support.
CREATE TABLE IF NOT EXISTS stg.orders (
  order_id     BIGINT       PRIMARY KEY,
  customer_id  BIGINT       NOT NULL,
  sku          TEXT         NOT NULL,
  quantity     BIGINT       NOT NULL,
  unit_price   BIGINT       NOT NULL,   -- cents
  ordered_at   TIMESTAMP    NOT NULL,
  notes        TEXT,
  load_date    DATE         NOT NULL,
  source_file  TEXT         NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_stg_orders_load_date ON stg.orders(load_date);
