-- One-time setup for the new CRM (Postgres).

CREATE SCHEMA IF NOT EXISTS crm;

CREATE TABLE IF NOT EXISTS crm.customers (
  customer_id     BIGINT       PRIMARY KEY,
  full_name       TEXT         NOT NULL,
  email           TEXT,
  address_line_1  TEXT,
  address_line_2  TEXT,
  city            TEXT,
  postal_code     TEXT,
  country         CHAR(2),
  address_hash    TEXT         NOT NULL,
  created_at      TIMESTAMPTZ  NOT NULL
);

CREATE TABLE IF NOT EXISTS crm.orders (
  order_id          BIGINT        PRIMARY KEY,
  customer_id       BIGINT        NOT NULL REFERENCES crm.customers(customer_id),
  order_date        DATE          NOT NULL,
  total_amount      NUMERIC(14,2) NOT NULL,
  currency          CHAR(3)       NOT NULL,
  status            TEXT          NOT NULL,
  total_amount_usd  NUMERIC(14,2) NOT NULL
);

CREATE TABLE IF NOT EXISTS crm.addresses (
  address_id   BIGINT  PRIMARY KEY,
  customer_id  BIGINT  NOT NULL REFERENCES crm.customers(customer_id),
  line_1       TEXT,
  line_2       TEXT,
  city         TEXT,
  postal_code  TEXT,
  country      CHAR(2)
);
