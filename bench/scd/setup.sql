-- bench/scd/setup.sql — schema for the SCD type-2 bench. Mirrors
-- examples/05-scd-type2/sql/dim_setup.sql, with two adjustments:
--   - schemas are `scdbench_*` so the bench doesn't collide with the
--     example's `dim`/`stg` schemas
--   - the dim seed table holds N rows seeded by seed.sql so the
--     bench can drive an SCD against a non-empty current image

DROP SCHEMA IF EXISTS scdbench_dim CASCADE;
DROP SCHEMA IF EXISTS scdbench_stg CASCADE;
CREATE SCHEMA scdbench_dim;
CREATE SCHEMA scdbench_stg;

CREATE TABLE scdbench_dim.customer (
    customer_sk   BIGSERIAL    PRIMARY KEY,
    customer_id   INTEGER      NOT NULL,
    name          TEXT,
    email         TEXT,
    address       TEXT,
    valid_from    TIMESTAMPTZ  NOT NULL,
    valid_to      TIMESTAMPTZ,
    is_current    BOOLEAN      NOT NULL DEFAULT TRUE
);
CREATE INDEX scdbench_dim_current_bk
    ON scdbench_dim.customer (customer_id) WHERE is_current;

CREATE VIEW scdbench_dim.customer_current AS
    SELECT customer_sk, customer_id, name, email, address,
           valid_from, valid_to
      FROM scdbench_dim.customer
     WHERE is_current;

CREATE TABLE scdbench_stg.customer (
    customer_id   INTEGER  NOT NULL,
    name          TEXT,
    email         TEXT,
    address       TEXT
);
