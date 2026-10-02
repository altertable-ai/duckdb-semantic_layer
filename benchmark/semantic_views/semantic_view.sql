CREATE OR REPLACE SEMANTIC VIEW sales_sv
  TABLES (
    n AS nations PRIMARY KEY (id),
    c AS customers PRIMARY KEY (id),
    p AS products PRIMARY KEY (id),
    s AS sales PRIMARY KEY (id)
  )
  RELATIONSHIPS (
    customer_nation AS c (nation_id) REFERENCES n,
    sale_customer AS s (customer_id) REFERENCES c,
    sale_product AS s (product_id) REFERENCES p
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
    s.sale_month AS DATE_TRUNC('month', sale_date),
    s.sale_year AS YEAR(sale_date)
  )
  METRICS (
    s.revenue AS SUM(amount),
    s.units AS SUM(qty),
    s.sale_count AS COUNT(*),
    s.net_revenue AS SUM(s.net),
    c.customer_count AS COUNT(*),
    c.avg_lifetime_revenue AS AVG(c.lifetime_revenue),
    revenue_per_customer AS s.revenue / c.customer_count
  );
