-- 100M-row star schema for the semantic_views benchmark.
-- Usage: duckdb <db file> < benchmark/semantic_views/generate.sql   (SCALE = number of sales rows)
SET VARIABLE sales_rows = COALESCE(TRY_CAST(getenv('SV_SALES_ROWS') AS BIGINT), 100000000);
SET VARIABLE customer_rows = GREATEST(getvariable('sales_rows') // 100, 1000);

CREATE OR REPLACE TABLE nations AS
SELECT range AS id, 'nation_' || range AS name, ['EMEA', 'AMER', 'APAC', 'LATAM', 'MEA'][(range % 5)::BIGINT + 1] AS region
FROM range(25);

CREATE OR REPLACE TABLE customers AS
SELECT range AS id, 'customer_' || range AS name, range % 25 AS nation_id,
       ['AUTOMOBILE', 'BUILDING', 'FURNITURE', 'HOUSEHOLD', 'MACHINERY'][(hash(range) % 5)::BIGINT + 1] AS segment
FROM range(getvariable('customer_rows'));

CREATE OR REPLACE TABLE products AS
SELECT range AS id, 'product_' || range AS name, 'category_' || (range % 50) AS category
FROM range(10000);

CREATE OR REPLACE TABLE sales AS
SELECT range AS id,
       (hash(range) % getvariable('customer_rows'))::BIGINT AS customer_id,
       (hash(range * 7) % 10000)::BIGINT AS product_id,
       DATE '2020-01-01' + (hash(range * 13) % 1826)::INTEGER AS sale_date,
       ((hash(range * 17) % 100000) / 100.0)::DECIMAL(12, 2) AS amount,
       (hash(range * 19) % 10 + 1)::INTEGER AS qty
FROM range(getvariable('sales_rows'));

CHECKPOINT;
