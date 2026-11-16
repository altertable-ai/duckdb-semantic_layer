CREATE OR REPLACE SEMANTIC VIEW baskets
  TABLES (
    orders AS orders PRIMARY KEY (o_orderkey),
    lineitem AS lineitem PRIMARY KEY (l_orderkey, l_linenumber),
    part AS part PRIMARY KEY (p_partkey)
  )
  RELATIONSHIPS (
    line_order AS lineitem (l_orderkey) REFERENCES orders,
    line_part AS lineitem (l_partkey) REFERENCES part
  )
  DIMENSIONS (
    orders.order_key AS o_orderkey,
    orders.order_date AS o_orderdate,
    part.manufacturer AS p_mfgr
  )
  METRICS (
    orders.order_count AS COUNT(*),
    lineitem.line_count AS COUNT(*)
  );
