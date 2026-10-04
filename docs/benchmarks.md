# Benchmarks

`scripts/bench_semantic_views.py` compares every `SEMANTIC_VIEW(...)` query in
`benchmark/semantic_views/queries.sql` with an equivalent hand-written query: it checks that both return the same
rows and reports the median wall time of each (DuckDB CLI `.timer`, queries alternated).

Data (`benchmark/semantic_views/generate.sql`): 100,000,000 `sales` rows, 1,000,000 `customers`, 10,000 `products`,
25 `nations`, stored in a DuckDB file. Semantic view: `benchmark/semantic_views/semantic_view.sql`.

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

## Materialization refresh

`scripts/bench_materializations.py` builds the same star schema in a DuckLake catalog (100,000,000 `sales` rows,
1,000,000 `customers`, 10,000 `products`), a semantic view and three identical materializations (revenue, units,
row count and average by customer segment and product category): one refreshed `FULL`, one `INCREMENTAL`, one
`AUTO`. For each change size and kind it applies the change, times the three refreshes and checks that the three
materializations hold exactly the same state.

DuckDB v1.5.5 + DuckLake d8a1881e, release build, Apple M4 Pro (14 cores, 48 GB), one run, 2026-10-04.

| Change | Full (s) | INCREMENTAL (s) | Speedup | AUTO (s) | Speedup | Same state |
|---|---:|---:|---:|---:|---:|---|
| update 1 row | 0.517 | 0.124 | 4.2x | 0.089 | 5.8x | yes |
| insert 1 row | 0.535 | 0.014 | 38.2x | 0.012 | 44.6x | yes |
| delete 1 row | 0.529 | 0.015 | 35.3x | 0.014 | 37.8x | yes |
| update 1 customer's segment | 0.518 | 0.804 | 0.6x | 0.516 | 1.0x | yes |
| update 1,000 rows | 0.534 | 0.433 | 1.2x | 0.298 | 1.8x | yes |
| insert 1,000 rows | 0.583 | 0.018 | 32.4x | 0.018 | 32.4x | yes |
| delete 1,000 rows | 0.558 | 0.129 | 4.3x | 0.111 | 5.0x | yes |
| update 1,000 customers' segment | 0.548 | 0.880 | 0.6x | 0.519 | 1.1x | yes |
| update 100,000 rows | 0.546 | 0.364 | 1.5x | 0.363 | 1.5x | yes |
| insert 100,000 rows | 0.551 | 0.033 | 16.7x | 0.032 | 17.2x | yes |
| delete 100,000 rows | 0.541 | 0.137 | 3.9x | 0.125 | 4.3x | yes |
| update 100,000 customers' segment | 0.554 | 1.074 | 0.5x | 0.550 | 1.0x | yes |
| update 1,000,000 rows (1%) | 0.560 | 0.547 | 1.0x | 0.485 | 1.2x | yes |
| insert 1,000,000 rows (1%) | 0.585 | 0.090 | 6.5x | 0.086 | 6.8x | yes |
| delete 1,000,000 rows (1%) | 0.562 | 0.303 | 1.9x | 0.204 | 2.8x | yes |
| update 1,000,000 customers' segment (all) | 0.533 | 1.749 | 0.3x | 0.562 | 0.9x | yes |

Reading the materialization instead of the sources: 0.006 s vs 0.29 s for a query by segment; a refresh with no
source change takes 0.005 s. A refresh runs a handful of statements in one transaction (read the changes, compute
the delta, `MERGE` it, record the new snapshots): about 12 ms when a single row changed.

- Inserts are applied from `ducklake_table_insertions()` alone, which reads only the new rows: cost follows the
  number of changed rows.
- Deletes and updates also need the old values of the deleted rows, which `ducklake_table_deletions()` fetches from
  the data files; that read dominates their cost on this 1.6 GB table.
- A change of a dimension table (a customer changing segment) moves unchanged `sales` rows between groups.
  `INCREMENTAL` finds them by scanning `sales` as of before and as of now, which costs about two full refreshes;
  `AUTO` rebuilds the block instead, so it never costs much more than a full refresh.
- A full refresh is fast to begin with here (DuckDB aggregates the 100M rows in under a second); incremental
  refresh matters most when the materialized aggregation is expensive or the source very large.

```bash
VCPKG_TOOLCHAIN_PATH=<vcpkg>/scripts/buildsystems/vcpkg.cmake GEN=ninja make
python3 scripts/bench_materializations.py --dir /tmp/semantic_views_materializations --rows 100000000
```
