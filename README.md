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
| `SELECT dim, AGG(metric) FROM name [WHERE ...] GROUP BY dim [HAVING ...]` | the semantic view queried by name, like a regular view |
| `SHOW SEMANTIC DIMENSIONS [LIKE ...] IN name FOR METRIC m` | dimensions `m` can be grouped by; `required` marks dimensions a window metric needs |
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

### Querying by name

```sql
SELECT customers.segment, AGG(lines.revenue), AGG(orders.avg_order) * 2 AS double_avg
FROM sales
WHERE orders.order_year >= 1995
GROUP BY customers.segment
HAVING AGG(lines.revenue) > 1000
ORDER BY 2 DESC;
```

`GROUP BY` expressions become dimensions, metrics must be wrapped in `AGG(...)`, other aggregate functions over
dimensions or facts are ad-hoc metrics, `WHERE` filters before aggregation and `HAVING` after. Without aggregation the
select list is evaluated as facts, row by row. Names can be bare, qualified (`customers.segment`) or quoted as BI tools
do (`"customers.segment"`). Not supported in this form (as in Snowflake): joins with the semantic view, window
functions, `QUALIFY`, `SELECT *`.

### In the catalog (BI tools)

Every semantic view is also listed as a view of the same name in `information_schema.tables` / `columns`,
`duckdb_views()` and `duckdb_columns()`, so BI tools and SQL clients discover it like any view. It has one column per
public fact, dimension and metric (in that order), with the member's type and comment; a name shared by several
members is qualified (`"orders.revenue"`), which the query-by-name form resolves. `DESCRIBE sales` shows those columns.
The view is a placeholder: reading it as a plain table fails with a message pointing to `SEMANTIC_VIEW(...)` and
`AGG(...)`.

Semantic views therefore share the namespace of tables and views: creating one with the name of an existing table or
view fails, and so does creating a table with the name of a semantic view.

### Advanced metrics

- `USING (relationship, ...)` picks the relationship used to reach a table when several connect the same tables
  (role-playing dimensions, e.g. departure vs arrival airport). Without it such queries are rejected as ambiguous.
- `NON ADDITIVE BY (dimension [ASC|DESC] [NULLS FIRST|LAST], ...)` makes a metric semi-additive: within every group,
  only the rows of the last snapshot in that order are aggregated (balances, inventory levels).
- Many-to-many relationships through a bridge table: with `book_authors (author_id) REFERENCES authors` and
  `book_authors (book_id) REFERENCES books`, `books.total_pages` can be grouped or filtered by `authors.country`. Every
  book counts once per group (a book with two French authors counts once for `FR`), and totals without bridged
  dimensions are unchanged. Any table with relationships to both sides acts as a bridge, including fact tables
  (`customers.customer_count` by `products.category` through `sales` counts the customers who bought in each
  category); a dimension stored on the bridge itself is still rejected. When several bridges connect the same tables,
  `USING (relationship)` on the metric picks one. Rows without any bridge row get NULL bridged dimensions, and a
  `WHERE` condition on bridged tables keeps only rows with a matching bridge row.
- `ASOF` relationships, `orders (o_cust_id, o_ord_date) REFERENCES addresses (ca_cust_id, ASOF ca_start_date)`, join
  each row to the latest referenced row at or before its value (`ASOF LEFT JOIN`).
- Range relationships, `events (ts) REFERENCES periods (BETWEEN start_time AND end_time EXCLUSIVE)` with
  `CONSTRAINT DISTINCT RANGE BETWEEN start_time AND end_time EXCLUSIVE` on `periods`, look up the range containing each
  value (NULL bounds are open). They are evaluated on the distinct values of the referencing column, which makes them
  much cheaper than a range join.
- Window function metrics, e.g.
  `AVG(total_sales_quantity) OVER (PARTITION BY EXCLUDING date.date ORDER BY date.date RANGE BETWEEN INTERVAL '6 days'
  PRECEDING AND CURRENT ROW)`, are evaluated over the aggregated rows; the dimensions they use must be requested in
  the query. `PARTITION BY EXCLUDING d` partitions by every requested dimension except `d`.

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
   (Snowflake's "equal or lower granularity" rule), or through a bridge table (step 4); otherwise the query is
   rejected instead of silently fanning out.
4. Dimensions and filters reached through a many-to-many bridge come from a `SELECT DISTINCT (key, dimensions)`
   subquery over the bridge, joined to the home table on the key they share: each home row joins at most once per
   group of dimension values, so nothing is double counted.
5. Blocks are merged on the dimension values (`UNION ALL` + `GROUP BY`, NULL dimension values match), and derived
   metrics are computed on the merged rows.
6. Semi-additive metrics get their own block that keeps the last snapshot of every group before aggregating (an
   unordered window `max`/`min` for one non-additive dimension, `rank()` for several); window function metrics are
   evaluated last, over the merged rows.
7. Facts or dimensions that aggregate finer rows (`orders.line_count AS COUNT(lines.l_linenumber)`) and nested
   aggregates (`AVG(SUM(orders.o_totalprice))` on customers) become correlated scalar subqueries, which DuckDB
   decorrelates into a hash aggregate + join.

On a 100M-row fact table the generated queries run within a few percent of equivalent hand-written SQL; see
[docs/benchmarks.md](docs/benchmarks.md).

## Limitations

- Semantic views live in memory, per database instance: they are not persisted in the database file and are
  not transactional (a `CREATE` inside a rolled-back transaction stays). Re-create them after restarting.
- Their catalog placeholder views, however, are saved in database files like any view. After a restart a placeholder
  stays listed, and querying it fails with `Semantic view "x" is not defined in this session`; `CREATE SEMANTIC VIEW x`
  replaces it and `DROP SEMANTIC VIEW x` removes it. Without the extension loaded, querying it fails with an unknown
  `__semantic_view_placeholder` function.
- No placeholder is created in read-only databases or in attached non-DuckDB catalogs (Postgres, SQLite, ...): such
  semantic views work but are not listed. A plain `DROP VIEW x` removes only the placeholder.
- A DuckDB view cannot be defined over the query-by-name form (`CREATE VIEW v AS SELECT ... FROM x GROUP BY ...`);
  use `SEMANTIC_VIEW(...)` in view definitions.
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
