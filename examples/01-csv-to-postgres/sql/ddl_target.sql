-- One-time setup for the warehouse Postgres instance.
-- Run before the pipeline's first run.

CREATE SCHEMA IF NOT EXISTS stg;

-- Column types match the betl pipeline's YAML schema exactly:
--   quantity    → int32 (in-memory widened to int64; INT4 here)
--   unit_price  → decimal(12, 2) → NUMERIC(12, 2)
--   ordered_at  → timestamp (no tz). Swap for TIMESTAMPTZ + change the
--                 YAML schema to `timestamptz` if your source data has
--                 a tz suffix.
CREATE TABLE IF NOT EXISTS stg.orders (
  order_id     BIGINT        PRIMARY KEY,
  customer_id  BIGINT        NOT NULL,
  sku          TEXT          NOT NULL,
  quantity     INTEGER       NOT NULL,
  unit_price   NUMERIC(12,2) NOT NULL,
  ordered_at   TIMESTAMP     NOT NULL,
  notes        TEXT,
  load_date    DATE          NOT NULL,
  source_file  TEXT          NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_stg_orders_load_date ON stg.orders(load_date);
