# Benchmarks

`scripts/bench_semantic_views.py` compares every `SEMANTIC_VIEW(...)` query in
`benchmark/semantic_layer/queries.sql` with an equivalent hand-written query: it checks that both return the same
rows and reports the median wall time of each (DuckDB CLI `.timer`, queries alternated).

Data (`benchmark/semantic_layer/generate.sql`): 100,000,000 `sales` rows, 1,000,000 `customers`, 10,000 `products`,
25 `nations`, stored in a DuckDB file. Semantic view: `benchmark/semantic_layer/semantic_view.sql`.

## Results

DuckDB v1.5.5, release build, Apple M4 Pro (14 cores, 48 GB), median of 11 runs, 2026-10-03 (all queries re-run after adding q10).

| Query | What it exercises | Semantic (s) | Hand-written (s) | Ratio | Results |
|---|---|---:|---:|---:|---|
| q1_total | metrics without dimensions, no join | 0.009 | 0.007 | 1.29 | identical |
| q2_by_category | one many-to-one join (10k rows) | 0.154 | 0.153 | 1.01 | identical |
| q3_by_region_two_hops | two-hop join (1M customers, 25 nations), fact-based metric | 0.328 | 0.325 | 1.01 | identical |
| q4_two_grains | metrics of two tables (sales and customers) merged on a dimension + derived metric | 0.357 | 0.334 | 1.07 | identical |
| q5_filter_remote_dimension | filter on dimensions one and two hops away | 0.053 | 0.058 | 0.91 | identical |
| q6_child_aggregate | metric over a fact that aggregates 100M child rows per customer | 0.318 | 0.321 | 0.99 | identical |
| q7_standard_sql_form | `SELECT dim, AGG(m) FROM sv WHERE ... GROUP BY ... HAVING ...` over three joins | 0.400 | 0.409 | 0.98 | identical |
| q8_semi_additive | `NON ADDITIVE BY` (last day per category) over 100M rows | 0.708 | 0.634 | 1.12 | identical |
| q9_range_relationship | range relationship (fiscal quarters) vs. a hand-written inequality join | 0.196 | 1.698 | 0.12 | identical |
| q10_many_to_many | customer count by product category through the `sales` bridge, vs. `COUNT(DISTINCT customer_id)` | 0.637 | 0.548 | 1.16 | identical |

q8's hand-written query assumes the snapshot column has no NULLs; the semantic query also honours the NULL ordering
(`ASC` sorts NULLs last, so a NULL snapshot would be the last one), which costs two extra window counts.

q10 de-duplicates (customer, category) pairs of the 100M-row bridge and joins them to the customers, where the
hand-written query counts distinct customers per category directly; the extra join of 1M customers costs ~15%. The
semantic query also keeps customers without any sale (NULL category), which the hand-written query cannot express
without the same join.

q9 is faster than the obvious hand-written query because range lookups run on the distinct values of the
referencing column (a decorrelated scalar subquery) rather than as a range join over all 100M rows.

For q1-q7 the plans DuckDB executes are the same as for the hand-written queries (same joins, aggregates and projections);
apart from q8 and q10 (and run-to-run noise), the only measurable difference is a fixed ~1 ms for parsing and compiling the semantic query,
visible on q1.

## Reproducing

```bash
GEN=ninja make
python3 scripts/bench_semantic_views.py --db /tmp/semantic_views_bench.duckdb --rows 100000000 --runs 11
```

The database is generated on the first run (about 3 GB on disk).

## Materialization queries

This compares the same query result on three paths: handwritten SQL over DuckLake source tables,
`SEMANTIC_VIEW` with materializations disabled, and the same semantic query with materializations enabled.
The source catalog is pristine: 100,000,000 sales, 1,000,000 customers and 10,000 products; no updates, deletes or
refresh workload runs during this benchmark. One materialization holds 250 groups (5 segments x 50 categories):

```sql
ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION summary AS
  DIMENSIONS c.segment, p.category
  METRICS s.revenue, s.units, s.sale_count, s.avg_qty;
```

DuckDB v1.5.5 + DuckLake d8a1881e, release build, Apple M4 Pro (14 cores, 48 GB), 2026-10-04.
Timings are medians of 11 warm runs per path, rotating execution order. Data generation, initial materialization
build and settings changes are excluded. Semantic timings include parsing, binding and freshness checks.
CLI timing resolution is 1 ms. Every result matched exactly; the expanded SQL confirmed cache use for q1-q5 and
source fallback for q6. Speedup compares the identical semantic query with its cache disabled versus enabled.

| Query | Handwritten SQL (ms) | Semantic, sources (ms) | Semantic, cache enabled (ms) | Speedup |
|---|---:|---:|---:|---:|
| q1: Total revenue + sale count | 49 | 51 | 5 | 10.2x |
| q2: Revenue + average quantity by segment | 382 | 381 | 6 | 63.5x |
| q3: Revenue + units by category | 196 | 200 | 6 | 33.3x |
| q4: All four metrics by segment and category | 544 | 566 | 6 | 94.3x |
| q5: Revenue + units by category, BUILDING segment only | 200 | 278 | 6 | 46.3x |
| q6: Revenue by segment, qty >= 5 (source fallback) | 307 | 308 | 307 | 1.0x |

q1-q3 reuse the finer stored groups at a coarser grain. q2 computes its average from the stored sum and count,
rather than averaging group averages. q4 reads the same grain as the materialization. q5 filters the stored segment
before rolling up by category. q6 filters individual sales quantities, which this materialization does not retain,
so enabling materializations leaves it reading the source tables.

The query SQL is below; the runnable pairs are also in
[benchmark/materializations/queries.sql](../benchmark/materializations/queries.sql). The semantic view and source
generation SQL are in [setup.sql](../benchmark/materializations/setup.sql).

### q1: Total revenue + sale count

Handwritten source SQL:

```sql
SELECT SUM(amount) AS revenue, COUNT(*) AS sale_count FROM lake.sales;
```

Semantic SQL (identical with the cache enabled or disabled):

```sql
SELECT * FROM SEMANTIC_VIEW(sales_sv
  METRICS s.revenue AS revenue, s.sale_count AS sale_count);
```

### q2: Revenue + average quantity by segment

Handwritten source SQL:

```sql
SELECT c.segment, SUM(s.amount) AS revenue, AVG(s.qty) AS avg_qty
FROM lake.sales s LEFT JOIN lake.customers c ON s.customer_id = c.id
GROUP BY c.segment ORDER BY ALL;
```

Semantic SQL (identical with the cache enabled or disabled):

```sql
SELECT * FROM SEMANTIC_VIEW(sales_sv
  DIMENSIONS c.segment AS segment
  METRICS s.revenue AS revenue, s.avg_qty AS avg_qty
) ORDER BY ALL;
```

### q3: Revenue + units by category

Handwritten source SQL:

```sql
SELECT p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units
FROM lake.sales s LEFT JOIN lake.products p ON s.product_id = p.id
GROUP BY p.category ORDER BY ALL;
```

Semantic SQL (identical with the cache enabled or disabled):

```sql
SELECT * FROM SEMANTIC_VIEW(sales_sv
  DIMENSIONS p.category AS category
  METRICS s.revenue AS revenue, s.units AS units
) ORDER BY ALL;
```

### q4: All four metrics by segment and category

Handwritten source SQL:

```sql
SELECT c.segment, p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units,
       COUNT(*) AS sale_count, AVG(s.qty) AS avg_qty
FROM lake.sales s
LEFT JOIN lake.customers c ON s.customer_id = c.id
LEFT JOIN lake.products p ON s.product_id = p.id
GROUP BY c.segment, p.category ORDER BY ALL;
```

Semantic SQL (identical with the cache enabled or disabled):

```sql
SELECT * FROM SEMANTIC_VIEW(sales_sv
  DIMENSIONS c.segment AS segment, p.category AS category
  METRICS s.revenue AS revenue, s.units AS units, s.sale_count AS sale_count, s.avg_qty AS avg_qty
) ORDER BY ALL;
```

### q5: Revenue + units by category, BUILDING segment only

Handwritten source SQL:

```sql
SELECT p.category, SUM(s.amount) AS revenue, SUM(s.qty) AS units
FROM lake.sales s
LEFT JOIN lake.customers c ON s.customer_id = c.id
LEFT JOIN lake.products p ON s.product_id = p.id
WHERE c.segment = 'BUILDING'
GROUP BY p.category ORDER BY ALL;
```

Semantic SQL (identical with the cache enabled or disabled):

```sql
SELECT * FROM SEMANTIC_VIEW(sales_sv
  DIMENSIONS p.category AS category
  METRICS s.revenue AS revenue, s.units AS units
  WHERE c.segment = 'BUILDING'
) ORDER BY ALL;
```

### q6: Revenue by segment, qty >= 5 (source fallback)

Handwritten source SQL:

```sql
SELECT c.segment, SUM(s.amount) AS revenue
FROM lake.sales s LEFT JOIN lake.customers c ON s.customer_id = c.id
WHERE s.qty >= 5
GROUP BY c.segment ORDER BY ALL;
```

Semantic SQL (identical with the cache enabled or disabled):

```sql
SELECT * FROM SEMANTIC_VIEW(sales_sv
  DIMENSIONS c.segment AS segment
  METRICS s.revenue AS revenue
  WHERE s.qty >= 5
) ORDER BY ALL;
```

### Reproducing the query comparison

Use a release build with DuckLake available and a fresh output directory:

```bash
python3 scripts/bench_materialization_queries.py --dir /tmp/semantic_materialization_queries --rows 100000000 --runs 11
```

The runner alternates `SET semantic_view_materializations = false` / `true` outside the timers.
It saves `benchmark.sql`, raw CLI output in `benchmark.log`, expanded source/cache SQL in `expanded_queries.sql`,
and every timing sample plus medians in `results.json`. To inspect the rewrite yourself, pass any of the semantic
queries to `semantic_view_sql()` with the setting disabled and enabled.

## Materialization refresh

`scripts/bench_materializations.py` builds the same star schema in a DuckLake catalog (100,000,000 `sales` rows,
1,000,000 `customers`, 10,000 `products`), a semantic view and three identical materializations (revenue, units,
row count and average by customer segment and product category): one refreshed `FULL`, one `INCREMENTAL`, one
`AUTO`. For each change size and kind it applies the change, times the three refreshes and checks that the three
materializations hold exactly the same state.

DuckDB v1.5.5 + DuckLake d8a1881e, release build, Apple M4 Pro (14 cores, 48 GB), 2026-10-04.
Query times are medians of 11 warm runs with cached and source reads alternated; settings changes are untimed.
Refresh times are single measurements per change, with FULL, INCREMENTAL and AUTO run in that order.
The query returns revenue and average quantity by segment, rolling 250 stored (segment, category) groups up to 5 segments.
Every query-result and materialized-state comparison was identical.
Updates are spread uniformly across the original sales rows (`id % (rows / changed_rows) = 0`).
Deletes target the contiguous batch inserted immediately beforehand. Their storage locality therefore differs;
these timings should not be read as a comparison of otherwise equivalent update and delete workloads.

| Query | From sources | From materialization | Speedup |
|---|---:|---:|---:|
| Revenue + average quantity by segment | 0.448 s | 0.006 s | 74.7x |

| Change | Full (s) | INCREMENTAL (s) | Speedup | AUTO (s) | Speedup | Same state |
|---|---:|---:|---:|---:|---:|---|
| scattered update: 1 row | 0.627 | 0.210 | 3.0x | 0.105 | 6.0x | yes |
| insert: 1 row | 0.718 | 0.015 | 47.9x | 0.014 | 51.3x | yes |
| delete: 1 row | 0.727 | 0.018 | 40.4x | 0.017 | 42.8x | yes |
| update customers' segment: 1 row | 0.617 | 1.037 | 0.6x | 0.735 | 0.8x | yes |
| scattered update: 1,000 rows | 0.736 | 0.686 | 1.1x | 0.474 | 1.6x | yes |
| insert: 1,000 rows | 0.690 | 0.021 | 32.9x | 0.017 | 40.6x | yes |
| delete: 1,000 rows | 0.731 | 0.144 | 5.1x | 0.126 | 5.8x | yes |
| update customers' segment: 1,000 rows | 0.698 | 1.304 | 0.5x | 0.738 | 0.9x | yes |
| scattered update: 100,000 rows | 0.694 | 0.436 | 1.6x | 0.375 | 1.9x | yes |
| insert: 100,000 rows | 0.633 | 0.034 | 18.6x | 0.032 | 19.8x | yes |
| delete: 100,000 rows | 0.620 | 0.154 | 4.0x | 0.125 | 5.0x | yes |
| update customers' segment: 100,000 rows | 0.779 | 1.286 | 0.6x | 0.604 | 1.3x | yes |
| scattered update: 1,000,000 rows (1%) | 0.668 | 0.593 | 1.1x | 0.685 | 1.0x | yes |
| insert: 1,000,000 rows (1%) | 0.780 | 0.088 | 8.9x | 0.084 | 9.3x | yes |
| delete: 1,000,000 rows (1%) | 0.591 | 0.300 | 2.0x | 0.216 | 2.7x | yes |
| update customers' segment: 1,000,000 rows (all customers) | 0.582 | 1.870 | 0.3x | 0.590 | 1.0x | yes |

A refresh with no source changes took 0.006 s. Building the three materializations initially took 2.009 s in total.
These local measurements describe this workload; refresh timing also reflects cache effects from the fixed execution order.

- Inserts are applied from `ducklake_table_insertions()` alone, which reads only the new rows: cost follows the
  number of changed rows.
- Deletes and updates also need the old values of the deleted rows, which `ducklake_table_deletions()` fetches from
  the data files; that read dominates their cost on this 1.6 GB table.
- A change of a dimension table (a customer changing segment) moves unchanged `sales` rows between groups.
  `INCREMENTAL` finds them by scanning `sales` as of before and as of now, which costs about two full refreshes;
  `AUTO` rebuilds the affected block instead, avoiding those two snapshot scans.
- A full refresh is fast to begin with here (DuckDB aggregates the 100M rows in under a second); incremental
  refresh matters most when the materialized aggregation is expensive or the source very large.

```bash
VCPKG_TOOLCHAIN_PATH=<vcpkg>/scripts/buildsystems/vcpkg.cmake GEN=ninja make
python3 scripts/bench_materializations.py --dir /tmp/semantic_views_materializations --rows 100000000 --runs 11
```

The script saves the generated SQL and raw CLI output in `--dir/benchmark.sql` and `--dir/benchmark.log`.
