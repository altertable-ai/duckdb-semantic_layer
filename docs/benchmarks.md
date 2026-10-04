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

DuckDB v1.5.5 + DuckLake d8a1881e, release build, Apple M4 Pro (14 cores, 48 GB), one run, 2026-10-04 (the machine
was running other heavy workloads, so expect noise).

| Change | Full (s) | INCREMENTAL (s) | Speedup | AUTO (s) | Speedup | Same state |
|---|---:|---:|---:|---:|---:|---|
| update 1 row | 0.985 | 0.340 | 2.9x | 0.137 | 7.2x | yes |
| insert 1 row | 0.950 | 0.033 | 28.8x | 0.034 | 27.9x | yes |
| delete 1 row | 0.979 | 0.038 | 25.8x | 0.035 | 28.0x | yes |
| update 1 customer's segment | 0.977 | 1.537 | 0.6x | 0.834 | 1.2x | yes |
| update 1,000 rows | 0.746 | 0.601 | 1.2x | 0.371 | 2.0x | yes |
| insert 1,000 rows | 0.778 | 0.038 | 20.5x | 0.037 | 21.0x | yes |
| delete 1,000 rows | 0.835 | 0.163 | 5.1x | 0.137 | 6.1x | yes |
| update 1,000 customers' segment | 0.855 | 1.389 | 0.6x | 0.906 | 0.9x | yes |
| update 100,000 rows | 0.943 | 0.559 | 1.7x | 0.466 | 2.0x | yes |
| insert 100,000 rows | 0.865 | 0.054 | 16.0x | 0.057 | 15.2x | yes |
| delete 100,000 rows | 0.846 | 0.206 | 4.1x | 0.156 | 5.4x | yes |
| update 100,000 customers' segment | 1.188 | 2.185 | 0.5x | 0.917 | 1.3x | yes |
| update 1,000,000 rows (1%) | 1.124 | 1.091 | 1.0x | 0.895 | 1.3x | yes |
| insert 1,000,000 rows (1%) | 1.056 | 0.176 | 6.0x | 0.169 | 6.2x | yes |
| delete 1,000,000 rows (1%) | 0.926 | 0.416 | 2.2x | 0.285 | 3.2x | yes |
| update 1,000,000 customers' segment (all) | 0.934 | 2.563 | 0.4x | 0.869 | 1.1x | yes |

Reading the materialization instead of the sources: 0.003 s vs 0.42-0.56 s for a query by segment; a refresh with
no source change takes 0.003 s.

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
