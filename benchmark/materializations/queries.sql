-- name: q1_total
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv METRICS s.revenue AS revenue, s.sale_count AS sale_count)
-- handwritten
SELECT SUM(amount) AS revenue, COUNT(*) AS sale_count FROM lake.sales

-- name: q2_by_segment
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment AS segment METRICS s.revenue AS revenue, s.avg_qty AS avg_qty) ORDER BY ALL
-- handwritten
SELECT c.segment, SUM(s.amount) AS revenue, AVG(s.qty) AS avg_qty
FROM lake.sales s LEFT JOIN lake.customers c ON s.customer_id = c.id
GROUP BY c.segment ORDER BY ALL

-- name: q3_by_category
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS p.category AS category METRICS s.revenue AS revenue, s.units AS units) ORDER BY ALL
-- handwritten
SELECT p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units
FROM lake.sales s LEFT JOIN lake.products p ON s.product_id = p.id
GROUP BY p.category ORDER BY ALL

-- name: q4_segment_category
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment AS segment, p.category AS category METRICS s.revenue AS revenue, s.units AS units, s.sale_count AS sale_count, s.avg_qty AS avg_qty) ORDER BY ALL
-- handwritten
SELECT c.segment, p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units,
       COUNT(*) AS sale_count, AVG(s.qty) AS avg_qty
FROM lake.sales s
LEFT JOIN lake.customers c ON s.customer_id = c.id
LEFT JOIN lake.products p ON s.product_id = p.id
GROUP BY c.segment, p.category ORDER BY ALL

-- name: q5_stored_dimension_filter
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS p.category AS category METRICS s.revenue AS revenue, s.units AS units WHERE c.segment = 'BUILDING') ORDER BY ALL
-- handwritten
SELECT p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units
FROM lake.sales s
LEFT JOIN lake.customers c ON s.customer_id = c.id
LEFT JOIN lake.products p ON s.product_id = p.id
WHERE c.segment = 'BUILDING'
GROUP BY p.category ORDER BY ALL

-- name: q6_source_row_filter
-- semantic
SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment AS segment METRICS s.revenue AS revenue WHERE s.qty >= 5) ORDER BY ALL
-- handwritten
SELECT c.segment, SUM(s.amount) AS revenue
FROM lake.sales s LEFT JOIN lake.customers c ON s.customer_id = c.id
WHERE s.qty >= 5
GROUP BY c.segment ORDER BY ALL
