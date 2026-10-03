-- name: q1_total
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv METRICS s.revenue, s.sale_count)
-- handwritten
SELECT SUM(amount) AS revenue, COUNT(*) AS sale_count FROM sales

-- name: q2_by_category
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS p.category METRICS s.revenue, s.units)
-- handwritten
SELECT p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units FROM sales s LEFT JOIN products p ON s.product_id = p.id GROUP BY 1

-- name: q3_by_region_two_hops
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS n.region, s.sale_year METRICS s.revenue, s.net_revenue)
-- handwritten
SELECT n.region, YEAR(s.sale_date) AS sale_year, SUM(s.amount) AS revenue, SUM(s.amount * s.qty) AS net_revenue FROM sales s LEFT JOIN customers c ON s.customer_id = c.id LEFT JOIN nations n ON c.nation_id = n.id GROUP BY 1, 2

-- name: q4_two_grains
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS n.nation METRICS s.revenue, c.customer_count, revenue_per_customer)
-- handwritten
WITH r AS (SELECT n.name AS nation, SUM(s.amount) AS revenue FROM sales s LEFT JOIN customers c ON s.customer_id = c.id LEFT JOIN nations n ON c.nation_id = n.id GROUP BY 1),
     k AS (SELECT n.name AS nation, COUNT(*) AS customer_count FROM customers c LEFT JOIN nations n ON c.nation_id = n.id GROUP BY 1)
SELECT COALESCE(r.nation, k.nation) AS nation, r.revenue, k.customer_count, r.revenue / k.customer_count AS revenue_per_customer FROM r FULL OUTER JOIN k ON r.nation IS NOT DISTINCT FROM k.nation

-- name: q5_filter_remote_dimension
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS s.sale_month METRICS s.revenue WHERE n.region = 'EMEA' AND c.segment = 'BUILDING')
-- handwritten
SELECT DATE_TRUNC('month', s.sale_date) AS sale_month, SUM(s.amount) AS revenue FROM sales s LEFT JOIN customers c ON s.customer_id = c.id LEFT JOIN nations n ON c.nation_id = n.id WHERE n.region = 'EMEA' AND c.segment = 'BUILDING' GROUP BY 1

-- name: q6_child_aggregate
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment METRICS c.avg_lifetime_revenue)
-- handwritten
SELECT c.segment, AVG(l.lifetime_revenue) AS avg_lifetime_revenue FROM customers c LEFT JOIN (SELECT customer_id, SUM(amount) AS lifetime_revenue FROM sales GROUP BY 1) l ON l.customer_id = c.id GROUP BY 1

-- name: q7_standard_sql_form
-- semantic
SELECT category, segment, AGG(revenue), AGG(units) FROM sales_sv WHERE region = 'APAC' GROUP BY category, segment HAVING AGG(revenue) > 0
-- handwritten
SELECT p.category, c.segment, SUM(s.amount), SUM(s.qty) FROM sales s LEFT JOIN products p ON s.product_id = p.id LEFT JOIN customers c ON s.customer_id = c.id LEFT JOIN nations n ON c.nation_id = n.id WHERE n.region = 'APAC' GROUP BY 1, 2 HAVING SUM(s.amount) > 0

-- name: q8_semi_additive
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS p.category METRICS s.last_day_revenue)
-- handwritten
SELECT category, SUM(amount) AS last_day_revenue FROM (SELECT p.category, s.amount, s.sale_date, max(s.sale_date) OVER (PARTITION BY p.category) AS last_day FROM sales s LEFT JOIN products p ON s.product_id = p.id) WHERE sale_date = last_day GROUP BY 1

-- name: q9_range_relationship
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS f.fiscal_quarter METRICS s.revenue)
-- handwritten
SELECT f.name AS fiscal_quarter, SUM(s.amount) AS revenue FROM sales s LEFT JOIN fiscal_periods f ON s.sale_date >= f.start_date AND s.sale_date < f.end_date GROUP BY 1
