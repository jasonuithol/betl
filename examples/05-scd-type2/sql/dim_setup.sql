-- Schema for example 05 — SCD type-2 dimension load.
--
-- Run once against the warehouse: `psql -f sql/dim_setup.sql`.
-- Truncates first so re-running is idempotent. The view
-- `dim.customer_current` is what the betl pipeline reads to build
-- its "current state" image; an index on (customer_id) WHERE is_current
-- keeps lookups fast on a big dim.

DROP SCHEMA IF EXISTS stg CASCADE;
DROP SCHEMA IF EXISTS dim CASCADE;
CREATE SCHEMA stg;
CREATE SCHEMA dim;

CREATE TABLE dim.customer (
    customer_sk   BIGSERIAL    PRIMARY KEY,
    customer_id   INTEGER      NOT NULL,        -- business key
    name          TEXT,                          -- tracked attribute
    email         TEXT,                          -- tracked attribute
    address       TEXT,                          -- tracked attribute
    valid_from    TIMESTAMPTZ  NOT NULL,
    valid_to      TIMESTAMPTZ,                   -- NULL while is_current
    is_current    BOOLEAN      NOT NULL DEFAULT TRUE
);
CREATE INDEX dim_customer_current_bk
    ON dim.customer (customer_id) WHERE is_current;

-- Convenience view: the pipeline reads from this instead of the raw
-- dim table so it sees only the current image. Materialised as a
-- proper view (not a CTE in the pipeline YAML) so the SCD recipe
-- stays portable across engines and easier to inspect.
CREATE VIEW dim.customer_current AS
    SELECT customer_sk, customer_id, name, email, address,
           valid_from, valid_to
      FROM dim.customer
     WHERE is_current;

-- Staging table — what the pipeline consumes as the "new image" of
-- the source-of-truth customer list each batch. Application would
-- usually populate this by some other process (CDC export, S3 dump,
-- file landing). We seed it explicitly for the test.
CREATE TABLE stg.customer (
    customer_id   INTEGER  NOT NULL,
    name          TEXT,
    email         TEXT,
    address       TEXT
);
