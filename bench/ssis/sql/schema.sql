-- bench/ssis/sql/schema.sql — bootstrap SQL for the SSIS-vs-betl bench.
-- Drives both sides (betl `mssql.read` and SSIS `OLE DB Source`) against
-- the same dataset. Run once before benchmarking.
--
-- Usage:
--   sqlcmd -S <server> -U sa -P '<pwd>' -d master -i bench/ssis/sql/schema.sql
-- or via the mcp-mssql workbench: execute_ddl(database='master', ...) for
-- each block.

-- Database
IF NOT EXISTS (SELECT 1 FROM sys.databases WHERE name = 'betl_bench')
    CREATE DATABASE betl_bench;
GO

USE betl_bench;
GO

-- Tables: 1-col 100k, 10-col 100k, 10-col 1M.
DROP TABLE IF EXISTS dbo.src_1col;
DROP TABLE IF EXISTS dbo.src_10col;
DROP TABLE IF EXISTS dbo.src_10col_1m;
DROP TABLE IF EXISTS dbo.dst_1col;
DROP TABLE IF EXISTS dbo.dst_10col;
GO

CREATE TABLE dbo.src_1col     (a BIGINT NOT NULL);
CREATE TABLE dbo.src_10col    (a BIGINT NOT NULL, b BIGINT NOT NULL, c BIGINT NOT NULL,
                               d BIGINT NOT NULL, e BIGINT NOT NULL, f BIGINT NOT NULL,
                               g BIGINT NOT NULL, h BIGINT NOT NULL, i BIGINT NOT NULL,
                               j BIGINT NOT NULL);
CREATE TABLE dbo.src_10col_1m (a BIGINT NOT NULL, b BIGINT NOT NULL, c BIGINT NOT NULL,
                               d BIGINT NOT NULL, e BIGINT NOT NULL, f BIGINT NOT NULL,
                               g BIGINT NOT NULL, h BIGINT NOT NULL, i BIGINT NOT NULL,
                               j BIGINT NOT NULL);
CREATE TABLE dbo.dst_1col     (a BIGINT NOT NULL);
CREATE TABLE dbo.dst_10col    (a BIGINT NOT NULL, b BIGINT NOT NULL, c BIGINT NOT NULL,
                               d BIGINT NOT NULL, e BIGINT NOT NULL, f BIGINT NOT NULL,
                               g BIGINT NOT NULL, h BIGINT NOT NULL, i BIGINT NOT NULL,
                               j BIGINT NOT NULL);
GO

-- Seed: src_1col (100k), src_10col (100k).
;WITH nums(n) AS (
    SELECT 1 UNION ALL SELECT n+1 FROM nums WHERE n < 100000
)
INSERT INTO dbo.src_1col(a) SELECT n FROM nums OPTION (MAXRECURSION 0);

;WITH nums(n) AS (
    SELECT 1 UNION ALL SELECT n+1 FROM nums WHERE n < 100000
)
INSERT INTO dbo.src_10col(a,b,c,d,e,f,g,h,i,j)
SELECT n, n+1, n+2, n+3, n+4, n+5, n+6, n+7, n+8, n+9 FROM nums
OPTION (MAXRECURSION 0);

-- Seed: src_10col_1m (1M).
;WITH nums(n) AS (
    SELECT 1 UNION ALL SELECT n+1 FROM nums WHERE n < 1000000
)
INSERT INTO dbo.src_10col_1m(a,b,c,d,e,f,g,h,i,j)
SELECT n, n+1, n+2, n+3, n+4, n+5, n+6, n+7, n+8, n+9 FROM nums
OPTION (MAXRECURSION 0);
GO
