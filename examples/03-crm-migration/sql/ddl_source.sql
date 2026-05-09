-- One-time setup for the legacy CRM (MSSQL).

CREATE TABLE dbo.customers (
  customer_id        BIGINT IDENTITY(1,1) PRIMARY KEY,
  first_name         NVARCHAR(100),
  last_name          NVARCHAR(100),
  email              NVARCHAR(320),
  address_line_1     NVARCHAR(200),
  address_line_2     NVARCHAR(200),
  city               NVARCHAR(100),
  postal_code        VARCHAR(20),
  country            CHAR(2),
  legacy_internal_id VARCHAR(40),
  deprecated_flag    BIT       NOT NULL CONSTRAINT df_lcrm_cust_dep DEFAULT 0,
  created_at         DATETIME2 NOT NULL CONSTRAINT df_lcrm_cust_ca  DEFAULT SYSUTCDATETIME(),
  archived           BIT       NOT NULL CONSTRAINT df_lcrm_cust_arc DEFAULT 0
);

CREATE TABLE dbo.orders (
  order_id        BIGINT IDENTITY(1,1) PRIMARY KEY,
  customer_id     BIGINT        NOT NULL,
  order_date      DATE          NOT NULL,
  total_amount    DECIMAL(14,2) NOT NULL,
  currency        CHAR(3)       NOT NULL,
  status          NVARCHAR(40),
  internal_notes  NVARCHAR(MAX),
  archived        BIT           NOT NULL CONSTRAINT df_lcrm_ord_arc DEFAULT 0
);

CREATE TABLE dbo.addresses (
  address_id   BIGINT IDENTITY(1,1) PRIMARY KEY,
  customer_id  BIGINT NOT NULL,
  line_1       NVARCHAR(200),
  line_2       NVARCHAR(200),
  city         NVARCHAR(100),
  postal_code  VARCHAR(20),
  country      CHAR(2),
  archived     BIT NOT NULL CONSTRAINT df_lcrm_addr_arc DEFAULT 0
);
