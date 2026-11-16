-- name: list
SELECT
  UNNEST(list_filter(list_transform(
    flatten(list_transform(
      list_sort(list_distinct(flatten(LIST(LIST(DISTINCT part.manufacturer)) OVER ()))),
      lambda a: list_transform(list_filter(
        list_sort(list_distinct(flatten(LIST(LIST(DISTINCT part.manufacturer)) OVER ()))),
        lambda b: b > a), lambda b: struct_pack(a := a, b := b)))),
    lambda p: struct_pack(
      manufacturer_a := struct_extract(p, 'a'),
      manufacturer_b := struct_extract(p, 'b'),
      orders_with_both := len(list_filter(LIST(LIST(DISTINCT part.manufacturer)) OVER (),
        lambda x: list_contains(x, struct_extract(p, 'a')) AND list_contains(x, struct_extract(p, 'b')))),
      lift := len(list_filter(LIST(LIST(DISTINCT part.manufacturer)) OVER (),
        lambda x: list_contains(x, struct_extract(p, 'a')) AND list_contains(x, struct_extract(p, 'b')))) * 1.0
        / (len(list_filter(LIST(LIST(DISTINCT part.manufacturer)) OVER (),
             lambda x: list_contains(x, struct_extract(p, 'a'))))
           * len(list_filter(LIST(LIST(DISTINCT part.manufacturer)) OVER (),
             lambda x: list_contains(x, struct_extract(p, 'b'))))))),
    lambda r: struct_extract(r, 'orders_with_both') > 0)) AS pair_result,
  struct_extract(pair_result, 'manufacturer_a') AS manufacturer_a,
  struct_extract(pair_result, 'manufacturer_b') AS manufacturer_b,
  struct_extract(pair_result, 'orders_with_both') AS orders_with_both,
  struct_extract(pair_result, 'lift') * (
    SELECT AGG(orders.order_count) FROM baskets
    WHERE orders.order_date >= DATE '1997-01-01' AND orders.order_date < DATE '1998-01-01'
  ) AS lift
FROM baskets
WHERE orders.order_date >= DATE '1997-01-01' AND orders.order_date < DATE '1998-01-01'
GROUP BY orders.order_key
QUALIFY ROW_NUMBER() OVER (ORDER BY orders.order_key) = 1;

-- name: cte
WITH memberships AS (
  SELECT orders.order_key AS order_key, part.manufacturer AS manufacturer
  FROM baskets
  WHERE orders.order_date >= DATE '1997-01-01' AND orders.order_date < DATE '1998-01-01'
  GROUP BY orders.order_key, part.manufacturer
  HAVING AGG(lineitem.line_count) > 0
), counts AS (
  SELECT manufacturer, COUNT(*) AS orders FROM memberships GROUP BY manufacturer
), pairs AS (
  SELECT a.manufacturer AS manufacturer_a, b.manufacturer AS manufacturer_b, COUNT(*) AS orders_with_both
  FROM memberships a JOIN memberships b ON a.order_key = b.order_key AND a.manufacturer < b.manufacturer
  GROUP BY a.manufacturer, b.manufacturer
), totals AS (
  SELECT AGG(orders.order_count) AS orders FROM baskets
  WHERE orders.order_date >= DATE '1997-01-01' AND orders.order_date < DATE '1998-01-01'
)
SELECT p.manufacturer_a, p.manufacturer_b, p.orders_with_both,
       p.orders_with_both * 1.0 * t.orders / (a.orders * b.orders) AS lift
FROM pairs p
JOIN counts a ON a.manufacturer = p.manufacturer_a
JOIN counts b ON b.manufacturer = p.manufacturer_b
CROSS JOIN totals t;

-- name: physical
WITH lines AS (
  SELECT o.o_orderkey, p.p_mfgr
  FROM orders o JOIN lineitem l ON l.l_orderkey = o.o_orderkey JOIN part p ON p.p_partkey = l.l_partkey
  WHERE o.o_orderdate >= DATE '1997-01-01' AND o.o_orderdate < DATE '1998-01-01'
), counts AS (
  SELECT p_mfgr, COUNT(DISTINCT o_orderkey) AS orders FROM lines GROUP BY p_mfgr
), pairs AS (
  SELECT a.p_mfgr AS manufacturer_a, b.p_mfgr AS manufacturer_b, COUNT(DISTINCT a.o_orderkey) AS orders_with_both
  FROM lines a JOIN lines b ON a.o_orderkey = b.o_orderkey AND a.p_mfgr < b.p_mfgr
  GROUP BY a.p_mfgr, b.p_mfgr
)
SELECT p.manufacturer_a, p.manufacturer_b, p.orders_with_both,
       p.orders_with_both * 1.0 * (
         SELECT COUNT(*) FROM orders WHERE o_orderdate >= DATE '1997-01-01' AND o_orderdate < DATE '1998-01-01'
       ) / (a.orders * b.orders) AS lift
FROM pairs p
JOIN counts a ON a.p_mfgr = p.manufacturer_a
JOIN counts b ON b.p_mfgr = p.manufacturer_b;
