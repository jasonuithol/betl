-- Seed the legacy CRM with messy realistic-ish data.
-- Quirks:
--   - Trailing whitespace in some names (1, 2)
--   - Mixed-case email (1, 2) - lua transform should lowercase
--   - deprecated_flag set on a few; deprecated != archived
--   - rows 7 and 10 are archived; should be filtered out by example 03
--   - Mixed currencies on orders (USD/EUR/GBP/JPY) for the FX path
--   - One null status on orders (defaults to 'UNKNOWN' in the transform)
--   - Some addresses without line_2

INSERT INTO dbo.customers
  (first_name, last_name, email, address_line_1, address_line_2, city, postal_code, country, legacy_internal_id, deprecated_flag, created_at, archived) VALUES
  ('John ',  'Doe',      'JOHN.DOE@EXAMPLE.COM',     '1 Main St',         NULL,      'Springfield','12345',   'US', 'L00001', 0, '2020-03-15', 0),
  (' Jane',  ' Smith ',  'jane.smith@EXAMPLE.com',   '742 Evergreen Terr','Apt 2',   'Springfield','12345',   'US', 'L00002', 0, '2020-05-22', 0),
  ('Wei',    'Chen',     'wei.chen@example.cn',      'Beijing Rd 100',    NULL,      'Shanghai',   '200000',  'CN', 'L00003', 1, '2021-01-10', 0),
  ('Marie',  'Curie',    'marie@example.fr',         'Rue de Paris 5',    NULL,      'Paris',      '75001',   'FR', 'L00004', 0, '2021-06-30', 0),
  ('Sven',   'Larsson',  'sven@example.se',          'Storgatan 1',       NULL,      'Stockholm',  '11122',   'SE', 'L00005', 0, '2022-02-14', 0),
  ('Ada',    'Lovelace', 'ada@example.uk',           'Mayfair 10',        NULL,      'London',     'W1K',     'GB', 'L00006', 0, '2022-09-01', 0),
  ('Old',    'User',     'old@example.com',          'Nowhere 0',         NULL,      'Atlantis',   '00000',   'US', 'L00007', 1, '2018-01-01', 1),
  ('Akira',  'Tanaka',   'akira@example.jp',         'Shibuya 1',         NULL,      'Tokyo',      '150-0001','JP', 'L00008', 0, '2023-04-10', 0),
  ('Maria',  'Garcia',   'maria@example.es',         'Gran Via 100',      NULL,      'Madrid',     '28013',   'ES', 'L00009', 0, '2023-11-20', 0),
  ('Removed','Customer', 'removed@example.com',      NULL,                NULL,      NULL,         NULL,      NULL, 'L00010', 1, '2017-05-05', 1);

INSERT INTO dbo.orders (customer_id, order_date, total_amount, currency, status, internal_notes, archived) VALUES
  (1,  '2025-12-01',   100.00, 'USD', 'completed', 'ship asap',          0),
  (1,  '2026-01-15',   250.50, 'USD', 'shipped',   NULL,                 0),
  (2,  '2025-11-10',    80.00, 'EUR', 'pending',   'wait for stock',     0),
  (2,  '2026-02-20',  1500.00, 'GBP', 'COMPLETED', NULL,                 0),
  (3,  '2025-12-25',    50.00, 'USD', NULL,        NULL,                 0),
  (4,  '2026-03-01',   200.00, 'EUR', 'shipped',   'fragile',            0),
  (5,  '2026-03-15',    75.00, 'EUR', 'completed', NULL,                 0),
  (6,  '2025-09-01',   999.99, 'GBP', 'completed', 'VIP',                0),
  (7,  '2025-08-01',    50.00, 'USD', 'completed', 'do not migrate',     1),
  (8,  '2026-04-01', 12000.00, 'JPY', 'shipped',   NULL,                 0),
  (9,  '2026-04-15',    60.00, 'EUR', 'pending',   NULL,                 0),
  (10, '2026-04-20',   100.00, 'USD', 'completed', 'for migration test', 1);

INSERT INTO dbo.addresses (customer_id, line_1, line_2, city, postal_code, country, archived) VALUES
  (1, '1 Main St',          NULL,     'Springfield','12345',   'US', 0),
  (1, 'Work Office',        'Floor 3','Springfield','12345',   'US', 0),
  (2, '742 Evergreen Terr', 'Apt 2',  'Springfield','12345',   'US', 0),
  (3, 'Beijing Rd 100',     NULL,     'Shanghai',   '200000',  'CN', 0),
  (4, 'Rue de Paris 5',     NULL,     'Paris',      '75001',   'FR', 0),
  (5, 'Storgatan 1',        NULL,     'Stockholm',  '11122',   'SE', 0),
  (6, 'Mayfair 10',         NULL,     'London',     'W1K',     'GB', 0),
  (7, 'Nowhere 0',          NULL,     'Atlantis',   '00000',   'US', 1),
  (8, 'Shibuya 1',          NULL,     'Tokyo',      '150-0001','JP', 0),
  (9, 'Gran Via 100',       NULL,     'Madrid',     '28013',   'ES', 0),
  (10,'Old',                NULL,     NULL,         NULL,      NULL, 1);
