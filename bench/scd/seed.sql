-- bench/scd/seed.sql — populate the bench schemas with a known mix.
--
-- :n     — staging row count
-- :dimn  — number of pre-existing current-dim rows (overlap with staging)
--
-- The first :dimn staging business keys overlap the dim image:
--   - 50% of those have unchanged tracked columns (UNCHANGED row)
--   - 25% have a changed address      (CHANGED row → INSERT + UPDATE)
--   - 25% are absent from staging      (NEW dim that we don't model
--     here — recipe doesn't handle deletes, those just stay orphaned)
-- The remaining (:n - :dimn) staging keys are new BKs → NEW rows.

TRUNCATE scdbench_dim.customer;
TRUNCATE scdbench_stg.customer;

-- 1. Dim seed: :dimn current rows. customer_id 1..:dimn, address Lane N.
INSERT INTO scdbench_dim.customer
    (customer_id, name, email, address, valid_from, is_current)
SELECT g, 'Name_' || g, 'e' || g || '@x', 'Lane ' || g,
       '2026-01-01T00:00:00Z', TRUE
  FROM generate_series(1, :dimn) AS g;

-- 2. Staging:
--    - rows 1..(:dimn * 0.5)        : unchanged (same address as dim)
--    - rows (:dimn * 0.5 + 1)..:dimn: changed address ("CHANGED " prefix)
--      (skipping every 4th business key to model "deleted from source")
--    - rows :dimn..:n               : new BKs not in dim → NEW rows
INSERT INTO scdbench_stg.customer
    (customer_id, name, email, address)
SELECT g,
       'Name_' || g,
       'e' || g || '@x',
       CASE
         WHEN g <= (:dimn * 0.5)::int          THEN 'Lane ' || g
         WHEN g <= :dimn AND (g % 4) <> 0       THEN 'CHANGED Lane ' || g
         WHEN g > :dimn                         THEN 'Lane ' || g
         ELSE NULL
       END
  FROM generate_series(1, :n) AS g
 WHERE
     -- Drop the every-fourth row in the CHANGED window so that BK
     -- exists in dim but not in staging (recipe ignores; it's not a
     -- delete). Keeps the row count predictable enough for the bench.
     NOT (g > (:dimn * 0.5)::int AND g <= :dimn AND (g % 4) = 0);
