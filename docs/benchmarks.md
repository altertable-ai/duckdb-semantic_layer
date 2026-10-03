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
