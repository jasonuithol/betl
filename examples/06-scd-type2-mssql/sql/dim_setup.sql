-- Schema for example 06 — SCD type-2 on SQL Server.
--
-- Mirrors examples/05-scd-type2/sql/dim_setup.sql, adapted for MSSQL:
--   - IDENTITY column instead of BIGSERIAL
--   - DATETIME2 instead of TIMESTAMPTZ
--   - BIT instead of BOOLEAN
--   - Schema names use SQL Server conventions (still `dim` and `stg`)
--
-- Run once: `sqlcmd -S <server> -d <db> -i sql/dim_setup.sql`.

IF SCHEMA_ID('stg') IS NULL EXEC ('CREATE SCHEMA stg');
IF SCHEMA_ID('dim') IS NULL EXEC ('CREATE SCHEMA dim');

IF OBJECT_ID('dim.customer_current', 'V')  IS NOT NULL DROP VIEW  dim.customer_current;
IF OBJECT_ID('dim.customer',         'U')  IS NOT NULL DROP TABLE dim.customer;
IF OBJECT_ID('stg.customer',         'U')  IS NOT NULL DROP TABLE stg.customer;

CREATE TABLE dim.customer (
    customer_sk   BIGINT       IDENTITY(1,1) PRIMARY KEY,
    customer_id   INT          NOT NULL,           -- business key
    name          NVARCHAR(200),
    email         NVARCHAR(200),
    address       NVARCHAR(400),
    valid_from    DATETIME2(6) NOT NULL,
    valid_to      DATETIME2(6) NULL,                -- NULL while is_current
    is_current    BIT          NOT NULL CONSTRAINT DF_dim_customer_is_current DEFAULT (1)
);
CREATE INDEX dim_customer_current_bk
    ON dim.customer (customer_id) WHERE is_current = 1;

GO

-- Convenience view: the pipeline reads from this instead of the raw
-- dim table so it sees only the current image.
CREATE VIEW dim.customer_current AS
    SELECT customer_sk, customer_id, name, email, address,
           valid_from, valid_to
      FROM dim.customer
     WHERE is_current = 1;

GO

CREATE TABLE stg.customer (
    customer_id   INT          NOT NULL,
    name          NVARCHAR(200),
    email         NVARCHAR(200),
    address       NVARCHAR(400)
);
