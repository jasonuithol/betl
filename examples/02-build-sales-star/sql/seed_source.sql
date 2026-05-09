-- Seed the reporting source with deterministic test data.
-- 12 transactions on 2026-05-04 (the load_date used by the pipeline)
-- with deliberate (customer, product) duplicates so the aggregate step
-- has groups to roll up.

INSERT INTO dbo.customers (full_name, email, country_code, created_at, updated_at) VALUES
  ('Alice Tan',   'alice@example.com', 'SG', '2025-01-15', '2025-01-15'),
  ('Bob Mueller', 'bob@example.com',   'DE', '2025-02-20', '2025-04-01'),
  ('Chen Wei',    'chen@example.com',  'CN', '2025-03-05', '2025-03-05'),
  ('Dana Smith',  'dana@example.com',  'US', '2025-04-12', '2025-04-12'),
  ('Eli Peretz',  NULL,                'IL', '2025-04-30', '2025-04-30');

INSERT INTO dbo.products (sku, product_name, category, list_price, is_discontinued, updated_at) VALUES
  ('WIDGET-A', 'Widget A',        'Hardware',  9.99, 0, '2025-01-01'),
  ('WIDGET-B', 'Widget B',        'Hardware', 14.50, 0, '2025-01-01'),
  ('GIZMO-1',  'Gizmo',           'Gadgets',  29.95, 0, '2025-01-01'),
  ('LEGACY-X', 'Legacy Widget X', 'Hardware',  4.99, 1, '2024-06-01'); -- discontinued; example filters out

INSERT INTO dbo.transactions (customer_id, sku, quantity, unit_price, ordered_at) VALUES
  -- 2026-05-03 (off-day; correct date filter excludes these)
  (1, 'WIDGET-A', 1,  9.99, '2026-05-03 10:00:00'),
  (2, 'GIZMO-1',  1, 29.95, '2026-05-03 14:00:00'),
  -- 2026-05-04 (load_date; 12 rows, with deliberate duplicates per (customer, product))
  (1, 'WIDGET-A', 1,  9.99, '2026-05-04 09:15:00'),
  (1, 'WIDGET-A', 2,  9.99, '2026-05-04 11:30:00'),
  (1, 'GIZMO-1',  1, 29.95, '2026-05-04 12:00:00'),
  (2, 'WIDGET-B', 1, 14.50, '2026-05-04 08:00:00'),
  (2, 'WIDGET-B', 1, 14.50, '2026-05-04 09:45:00'),
  (2, 'WIDGET-B', 2, 14.50, '2026-05-04 16:20:00'),
  (3, 'WIDGET-A', 5,  9.99, '2026-05-04 10:00:00'),
  (4, 'GIZMO-1',  2, 29.95, '2026-05-04 13:10:00'),
  (4, 'GIZMO-1',  1, 29.95, '2026-05-04 17:00:00'),
  (4, 'WIDGET-B', 3, 14.50, '2026-05-04 17:30:00'),
  (5, 'WIDGET-B', 1, 14.50, '2026-05-04 19:00:00'),
  (5, 'WIDGET-A', 1,  9.99, '2026-05-04 19:05:00'),
  -- 2026-05-05 (off-day)
  (3, 'WIDGET-B', 1, 14.50, '2026-05-05 08:00:00');
