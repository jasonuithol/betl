-- One-time setup for the OLAP Postgres warehouse.
-- Creates the star schema and initializes the materialized views so that
-- subsequent CONCURRENTLY refreshes (in refresh_marts.sql) succeed —
-- REFRESH ... CONCURRENTLY requires a previously populated matview.

CREATE SCHEMA IF NOT EXISTS olap;

CREATE TABLE IF NOT EXISTS olap.dim_customer (
  sk_customer  BIGSERIAL    PRIMARY KEY,
  nk_customer  BIGINT       NOT NULL UNIQUE,
  full_name    TEXT         NOT NULL,
  email        TEXT,
  country_code CHAR(2)      NOT NULL,
  created_at   TIMESTAMPTZ  NOT NULL,
  updated_at   TIMESTAMPTZ  NOT NULL
);

CREATE TABLE IF NOT EXISTS olap.dim_product (
  sk_product   BIGSERIAL    PRIMARY KEY,
  nk_product   TEXT         NOT NULL UNIQUE,
  sku          TEXT         NOT NULL,
  product_name TEXT         NOT NULL,
  category     TEXT         NOT NULL,
  list_price   NUMERIC(12,2) NOT NULL,
  updated_at   TIMESTAMPTZ  NOT NULL
);

CREATE TABLE IF NOT EXISTS olap.fact_sales (
  sk_date       INTEGER       NOT NULL,
  sk_customer   BIGINT        NOT NULL REFERENCES olap.dim_customer(sk_customer),
  sk_product    BIGINT        NOT NULL REFERENCES olap.dim_product(sk_product),
  units_sold    INTEGER       NOT NULL,
  gross_revenue NUMERIC(14,2) NOT NULL,
  line_count    INTEGER       NOT NULL,
  PRIMARY KEY (sk_date, sk_customer, sk_product)
);

-- --- Materialized views ----------------------------------------------------
-- Created WITH NO DATA so the CREATE is fast; populated immediately below.
-- After this initial REFRESH (non-CONCURRENTLY), refresh_marts.sql's
-- CONCURRENTLY refreshes will succeed on every subsequent pipeline run.

CREATE MATERIALIZED VIEW IF NOT EXISTS olap.mart_revenue_by_day AS
  SELECT sk_date, SUM(gross_revenue) AS revenue, SUM(units_sold) AS units
  FROM olap.fact_sales
  GROUP BY sk_date
  WITH NO DATA;
CREATE UNIQUE INDEX IF NOT EXISTS ix_mart_revenue_by_day_pk
  ON olap.mart_revenue_by_day(sk_date);

CREATE MATERIALIZED VIEW IF NOT EXISTS olap.mart_top_products_30d AS
  SELECT sk_product, SUM(gross_revenue) AS revenue, SUM(units_sold) AS units
  FROM olap.fact_sales
  GROUP BY sk_product
  WITH NO DATA;
CREATE UNIQUE INDEX IF NOT EXISTS ix_mart_top_products_30d_pk
  ON olap.mart_top_products_30d(sk_product);

CREATE MATERIALIZED VIEW IF NOT EXISTS olap.mart_customer_lifetime_value AS
  SELECT sk_customer, SUM(gross_revenue) AS clv, SUM(line_count) AS lines
  FROM olap.fact_sales
  GROUP BY sk_customer
  WITH NO DATA;
CREATE UNIQUE INDEX IF NOT EXISTS ix_mart_customer_lifetime_value_pk
  ON olap.mart_customer_lifetime_value(sk_customer);

-- One-time non-CONCURRENTLY refresh so each matview is "populated"
-- (with zero rows) and CONCURRENTLY refreshes succeed thereafter.
REFRESH MATERIALIZED VIEW olap.mart_revenue_by_day;
REFRESH MATERIALIZED VIEW olap.mart_top_products_30d;
REFRESH MATERIALIZED VIEW olap.mart_customer_lifetime_value;
