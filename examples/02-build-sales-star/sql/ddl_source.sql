-- One-time setup for the reporting MSSQL source.
-- Idempotent enough — drop the database first if you want a clean rebuild.

CREATE TABLE dbo.customers (
  customer_id  BIGINT IDENTITY(1,1) PRIMARY KEY,
  full_name    NVARCHAR(200) NOT NULL,
  email        NVARCHAR(320) NULL,
  country_code CHAR(2)       NOT NULL,
  created_at   DATETIME2     NOT NULL CONSTRAINT df_customers_created_at DEFAULT SYSUTCDATETIME(),
  updated_at   DATETIME2     NOT NULL CONSTRAINT df_customers_updated_at DEFAULT SYSUTCDATETIME()
);

CREATE TABLE dbo.products (
  sku             VARCHAR(40)   NOT NULL PRIMARY KEY,
  product_name    NVARCHAR(200) NOT NULL,
  category        NVARCHAR(80)  NOT NULL,
  list_price      DECIMAL(12,2) NOT NULL,
  is_discontinued BIT           NOT NULL CONSTRAINT df_products_is_disc DEFAULT 0,
  updated_at      DATETIME2     NOT NULL CONSTRAINT df_products_updated_at DEFAULT SYSUTCDATETIME()
);

CREATE TABLE dbo.transactions (
  txn_id      BIGINT IDENTITY(1,1) PRIMARY KEY,
  customer_id BIGINT        NOT NULL,
  sku         VARCHAR(40)   NOT NULL,
  quantity    INT           NOT NULL,
  unit_price  DECIMAL(12,2) NOT NULL,
  ordered_at  DATETIME2     NOT NULL
);
