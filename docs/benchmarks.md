# Benchmarks

`scripts/bench_semantic_views.py` compares every `SEMANTIC_VIEW(...)` query in
`benchmark/semantic_views/queries.sql` with an equivalent hand-written query: it checks that both return the same
rows and reports the median wall time of each (DuckDB CLI `.timer`, queries alternated).

Data (`benchmark/semantic_views/generate.sql`): 100,000,000 `sales` rows, 1,000,000 `customers`, 10,000 `products`,
25 `nations`, stored in a DuckDB file. Semantic view: `benchmark/semantic_views/semantic_view.sql`.

## Results

DuckDB v1.5.5, release build, Apple M4 Pro (14 cores, 48 GB), median of 11 runs, 2026-10-03.

| Query | What it exercises | Semantic (s) | Hand-written (s) | Ratio | Results |
|---|---|---:|---:|---:|---|
| q1_total | metrics without dimensions, no join | 0.007 | 0.006 | 1.17 | identical |
| q2_by_category | one many-to-one join (10k rows) | 0.146 | 0.145 | 1.01 | identical |
| q3_by_region_two_hops | two-hop join (1M customers, 25 nations), fact-based metric | 0.322 | 0.320 | 1.01 | identical |
| q4_two_grains | metrics of two tables (sales and customers) merged on a dimension + derived metric | 0.269 | 0.266 | 1.01 | identical |
| q5_filter_remote_dimension | filter on dimensions one and two hops away | 0.049 | 0.048 | 1.02 | identical |
| q6_child_aggregate | metric over a fact that aggregates 100M child rows per customer | 0.292 | 0.292 | 1.00 | identical |

The plans DuckDB executes are the same as for the hand-written queries (same joins, aggregates and projections);
the only measurable difference is a fixed ~1 ms for parsing and compiling the semantic query, visible on q1.

## Reproducing

```bash
GEN=ninja make
python3 scripts/bench_semantic_views.py --db /tmp/semantic_views_bench.duckdb --rows 100000000 --runs 11
```

The database is generated on the first run (about 3 GB on disk).
