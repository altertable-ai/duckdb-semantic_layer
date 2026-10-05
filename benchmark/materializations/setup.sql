-- Shared pristine star schema for materialization benchmarks.
-- The caller attaches the DuckLake catalog as lake and sets sales_rows.
SET VARIABLE customer_rows = GREATEST(getvariable('sales_rows') // 100, 1000);

CREATE TABLE lake.customers AS
SELECT range AS id,
       ['AUTOMOBILE','BUILDING','FURNITURE','HOUSEHOLD','MACHINERY'][(hash(range) % 5)::BIGINT + 1] AS segment
FROM range(getvariable('customer_rows'));

CREATE TABLE lake.products AS
SELECT range AS id, 'category_' || (range % 50) AS category FROM range(10000);

CREATE TABLE lake.sales AS
SELECT range AS id,
       (hash(range) % getvariable('customer_rows'))::BIGINT AS customer_id,
       (hash(range * 7) % 10000)::BIGINT AS product_id,
       ((hash(range * 17) % 100000) / 100.0)::DECIMAL(12, 2) AS amount,
       (hash(range * 19) % 10 + 1)::INTEGER AS qty
FROM range(getvariable('sales_rows'));

CREATE SEMANTIC VIEW sales_sv
  TABLES (s AS lake.sales PRIMARY KEY (id), c AS lake.customers PRIMARY KEY (id), p AS lake.products PRIMARY KEY (id))
  RELATIONSHIPS (s (customer_id) REFERENCES c, s (product_id) REFERENCES p)
  DIMENSIONS (c.segment AS segment, p.category AS category)
  METRICS (s.revenue AS SUM(amount), s.units AS SUM(qty), s.sale_count AS COUNT(*), s.avg_qty AS AVG(qty));
