# DuckDB semantic views

A DuckDB extension that implements [Snowflake semantic views](https://docs.snowflake.com/en/user-guide/views-semantic/overview):
define logical tables, relationships, facts, dimensions and metrics once with `CREATE SEMANTIC VIEW`, then
query any combination of them with `SEMANTIC_VIEW(...)`. The extension writes the joins and the
aggregation for you, at the right grain, so metrics are never double counted.

```sql
CREATE SEMANTIC VIEW sales
  TABLES (
    orders    AS orders    PRIMARY KEY (o_orderkey),
    customers AS customer  PRIMARY KEY (c_custkey),
    lines     AS lineitem  PRIMARY KEY (l_orderkey, l_linenumber)
  )
  RELATIONSHIPS (
    orders_to_customers AS orders (o_custkey) REFERENCES customers,
    lines_to_orders     AS lines (l_orderkey) REFERENCES orders
  )
  FACTS (
    lines.net_price       AS l_extendedprice * (1 - l_discount),
    orders.line_count     AS COUNT(lines.l_linenumber)
  )
  DIMENSIONS (
    customers.segment     AS c_mktsegment,
    orders.order_year     AS YEAR(o_orderdate)
  )
  METRICS (
    lines.revenue         AS SUM(lines.net_price),
    orders.avg_order      AS AVG(o_totalprice),
    orders.avg_lines      AS AVG(orders.line_count),
    customers.customers   AS COUNT(*),
    revenue_per_customer  AS lines.revenue / customers.customers
  );

SELECT * FROM SEMANTIC_VIEW(
  sales
  DIMENSIONS customers.segment, orders.order_year
  METRICS lines.revenue, orders.avg_order, orders.avg_lines
  WHERE orders.order_year >= 1995
)
ORDER BY segment, order_year;

-- metrics of different grains combine; revenue_per_customer counts customers per segment
SELECT * FROM SEMANTIC_VIEW(sales DIMENSIONS customers.segment METRICS revenue_per_customer);
```

A metric can only be broken down by dimensions of its own table or of tables it references
(Snowflake's granularity rule): `revenue_per_customer` cannot be grouped by `orders.order_year`, because a
customer count has no order year; such queries are rejected rather than silently fanned out.

The syntax is Snowflake's. Pasting a Snowflake `CREATE SEMANTIC VIEW` statement works: clauses that only make
sense in Snowflake (`AI_SQL_GENERATION`, `AI_VERIFIED_QUERIES`, `WITH CORTEX SEARCH SERVICE`, tags, `COPY GRANTS`,
`MAX_STALENESS`, `SAMPLE_VALUES`, `IS_ENUM`) are accepted and kept as metadata.

## Statements

| Statement | Notes |
|---|---|
| `CREATE [OR REPLACE \| OR ALTER] SEMANTIC VIEW [IF NOT EXISTS] name TABLES (...) [RELATIONSHIPS (...)] [FACTS (...)] [DIMENSIONS (...)] [METRICS (...)] [COMMENT = '...']` | validated at creation (tables, columns, keys, relationship graph, expressions, aggregation rules, types) |
| `DROP SEMANTIC VIEW [IF EXISTS] name` | |
| `ALTER SEMANTIC VIEW [IF EXISTS] name RENAME TO new \| SET COMMENT = '...' \| UNSET COMMENT` | |
| `DESCRIBE SEMANTIC VIEW name` | Snowflake layout: `object_kind, object_name, parent_entity, property, property_value` |
| `SHOW SEMANTIC VIEWS [LIKE 'pattern'] [IN SCHEMA s \| IN DATABASE d]` | |
| `SHOW SEMANTIC DIMENSIONS \| METRICS \| FACTS [LIKE 'pattern'] [IN name]` | includes the data type of every member |
| `SELECT ... FROM SEMANTIC_VIEW(name [DIMENSIONS ...] [METRICS ...] [FACTS ...] [WHERE ...]) [AS alias]` | composes with the rest of the query (joins, CTEs, `CREATE TABLE AS`, `EXPLAIN`, prepared statements) |
| `SELECT sql FROM semantic_view_sql('<query>')` | shows the SQL a query compiles to |

All statements can be used as table expressions, e.g. `SELECT name FROM (SHOW SEMANTIC METRICS IN sales)`.

### Querying

- `DIMENSIONS`, `METRICS` and `FACTS` take comma-separated expressions with optional aliases (`AS` optional);
  output columns appear in the order of the clauses and use the unqualified member name by default.
- Members are referenced as `table.name`, or by bare name when unambiguous. `table.*` selects every public
  member of a logical table.
- Dimension items can be expressions over dimensions and facts (`YEAR(orders.order_date) AS year`); metric items
  can be expressions over metrics or aggregates of dimensions/facts (`COUNT(DISTINCT customers.segment)`).
- `WHERE` filters rows before aggregation and may reference dimensions and facts of any table reachable from every
  metric's table; uncorrelated subqueries are allowed.
- `FACTS` without `DIMENSIONS` return row-level values. `FACTS` and `METRICS` cannot be combined.
- `PRIVATE` facts and metrics can be used in definitions but not queried.

## How queries are evaluated (and why they scale)

A `SEMANTIC_VIEW(...)` clause is expanded at bind time into an ordinary DuckDB query; there is no custom
operator, so the optimizer and the parallel hash aggregates/joins apply unchanged.

1. Every metric is decomposed into *aggregate atoms* (`SUM(amount)`, `COUNT(*)`, ...). Each atom has a home table:
   the logical table whose rows it aggregates (`lines` for `SUM(lines.net_price)`, `customers` for `COUNT(*)` of a
   customer metric).
2. Atoms with the same home table form a *block*:
   `SELECT dims, aggregates FROM home LEFT JOIN <tables reached through many-to-one relationships> WHERE filter GROUP BY dims`.
   Joins only follow relationships towards the "one" side, so they can never multiply the home table's rows, and
   only the tables actually referenced are joined.
3. A dimension (or filter) must be reachable from every metric's home table through many-to-one relationships
   (Snowflake's "equal or lower granularity" rule); otherwise the query is rejected instead of silently fanning out.
4. Blocks are merged on the dimension values (`UNION ALL` + `GROUP BY`, NULL dimension values match), and derived
   metrics are computed on the merged rows.
5. Facts or dimensions that aggregate finer rows (`orders.line_count AS COUNT(lines.l_linenumber)`) and nested
   aggregates (`AVG(SUM(orders.o_totalprice))` on customers) become correlated scalar subqueries, which DuckDB
   decorrelates into a hash aggregate + join.

On a 100M-row fact table the generated queries run within a few percent of equivalent hand-written SQL; see
[docs/benchmarks.md](docs/benchmarks.md).

## Limitations

- Semantic views live in memory, per database instance: they are not persisted in the database file and are
  not transactional (a `CREATE` inside a rolled-back transaction stays). Re-create them after restarting.
- Not supported yet: the `SELECT dim, AGG(metric) FROM semantic_view GROUP BY dim` query form, `USING (...)`
  relationship selection, `NON ADDITIVE BY`, window-function metrics, `ASOF` and range relationships,
  `SHOW SEMANTIC DIMENSIONS ... FOR METRIC`. These are parsed and rejected with a clear error.
- A single statement cannot combine `SEMANTIC_VIEW(...)` with syntax of another parser extension such as
  `MATCH_RECOGNIZE`.
- Physical columns of another logical table can be referenced directly in expressions (Snowflake requires a fact).

## Building

```bash
GEN=ninja make
make test
```

The PEG grammar lives in `third_party/duckdb_peg_parser` (a vendored copy of DuckDB's PEG parser); see
[AGENTS.md](AGENTS.md) for the maintenance scripts. Design notes: `docs/superpowers/specs/`.

## Benchmark

```bash
python3 scripts/bench_semantic_views.py --db /tmp/semantic_views_bench.duckdb --rows 100000000
```
