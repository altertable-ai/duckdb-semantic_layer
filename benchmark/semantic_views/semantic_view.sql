-- fiscal quarters (non-overlapping ranges) used by a range relationship
CREATE OR REPLACE TABLE fiscal_periods AS
SELECT 'FY' || year(d) || '-Q' || quarter(d) AS name, d AS start_date, d + INTERVAL 3 MONTH AS end_date
FROM range(DATE '2020-01-01', DATE '2025-01-01', INTERVAL 3 MONTH) t(d);

CREATE OR REPLACE SEMANTIC VIEW sales_sv
  TABLES (
    n AS nations PRIMARY KEY (id),
    c AS customers PRIMARY KEY (id),
    p AS products PRIMARY KEY (id),
    s AS sales PRIMARY KEY (id),
    f AS fiscal_periods CONSTRAINT DISTINCT RANGE BETWEEN start_date AND end_date EXCLUSIVE
  )
  RELATIONSHIPS (
    customer_nation AS c (nation_id) REFERENCES n,
    sale_customer AS s (customer_id) REFERENCES c,
    sale_product AS s (product_id) REFERENCES p,
    sale_period AS s (sale_date) REFERENCES f (BETWEEN start_date AND end_date EXCLUSIVE)
  )
  FACTS (
    s.net AS amount * qty,
    c.lifetime_revenue AS SUM(s.amount)
  )
  DIMENSIONS (
    n.nation AS n.name,
    n.region AS n.region,
    c.segment AS c.segment,
    p.category AS p.category,
    f.fiscal_quarter AS f.name,
    s.sale_month AS DATE_TRUNC('month', sale_date),
    s.sale_year AS YEAR(sale_date),
    s.sale_day AS sale_date
  )
  METRICS (
    s.revenue AS SUM(amount),
    s.units AS SUM(qty),
    s.sale_count AS COUNT(*),
    s.net_revenue AS SUM(s.net),
    c.customer_count AS COUNT(*),
    c.avg_lifetime_revenue AS AVG(c.lifetime_revenue),
    s.last_day_revenue NON ADDITIVE BY (s.sale_day) AS SUM(amount),
    s.revenue_ytd CUMULATIVE (s.sale_day TO YEAR) AS SUM(amount),
    s.revenue_running CUMULATIVE (s.sale_day) AS SUM(amount),
    s.active_products_7d CUMULATIVE (s.sale_day OVER INTERVAL 7 DAYS) AS COUNT(DISTINCT product_id),
    revenue_per_customer AS s.revenue / c.customer_count
  );
