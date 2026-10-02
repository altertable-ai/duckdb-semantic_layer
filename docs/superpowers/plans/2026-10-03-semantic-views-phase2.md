# Semantic Views — Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> Execution mode chosen by the user: **native/inline, autonomous**. Tests below are the contract.

**Goal:** Snowflake's advanced query surface: `SHOW SEMANTIC DIMENSIONS ... FOR METRIC`, `USING (relationship)`, `NON ADDITIVE BY`, window-function metrics, and the standard-SQL `SELECT ... AGG(metric) FROM semantic_view GROUP BY ...` form.

**Architecture:** Extends the Phase 1 compiler (spec section 7). Atoms carry their USING set and non-additive spec, which key the aggregation blocks; window metrics are evaluated in the final projection layer over the merged rows; the standard-SQL form is rewritten into the same internal `__semantic_view_query` call from `OperatorExtension::Bind` when binding fails on an unknown table that names a semantic view.

**Tech Stack:** C++17, DuckDB v1.5.5, sqllogictest.

**Spec:** `docs/superpowers/specs/2026-10-03-semantic-views-design.md` (sections 2, 7)

## Global Constraints

- Build with `GEN=ninja make`; never edit `duckdb/` or `extension-ci-tools/`.
- Vendored PEG edits go through `custom/` or `patches/` (refresh with `scripts/make_peg_patch.sh`).
- Lowered ASTs stay free of session state.

## Review Focus

1. A real table with the same name as a semantic view must keep resolving to the table (standard-SQL form) → Task 5 test.
2. Semi-additive metric combined with an additive metric of the same table in one query must not filter the additive one → Task 3 test.
3. Window metric queried without its required dimensions must error, not compute over the wrong partition → Task 4 test.
4. USING-pinned metrics next to unpinned metrics of the same table must use different join paths in one query → Task 2 test.
5. `GROUP BY` ordinals / select aliases in the standard-SQL form → Task 5 test.

---

### Task 1: SHOW SEMANTIC DIMENSIONS IN sv FOR METRIC m
- Test `test/sql/semantic_views/show_for_metric.test`: dimensions usable with an orders metric (orders, customers, regions dims; not line items); `required` column false; unknown metric error.
- Implement in `semantic_views_functions.cpp` using the analyzer (atom homes) — required flags filled in Task 4.

### Task 2: USING (relationship, ...)
- Test `test/sql/semantic_views/using.test` (Snowflake flights/airports example): departures/arrivals by city, both in one query; ambiguity error without USING; unknown relationship error at CREATE.
- Model: `SemanticMember::using_relationships` (indices); `SemanticAtom::using_relationships`; `SemanticModel::Path(from, to, using)` filters simple paths; blocks keyed by (home, using).

### Task 3: NON ADDITIVE BY
- Test `test/sql/semantic_views/non_additive.test`: account balances snapshot example (sum of latest balance per account by customer, by month, by date; DESC variant; combined with an additive metric).
- Model: resolved non-additive dimension markers + order; atoms carry the spec; the block ranks rows (`rank() OVER (PARTITION BY query dims not in NA ORDER BY NA reversed) = 1`) in an inner select before aggregating.

### Task 4: Window-function metrics
- Grammar: `PARTITION BY EXCLUDING ...` (patch `WindowPartition`, lowered to a marker function `__sv_excluding(...)`).
- Test `test/sql/semantic_views/window_metrics.test` (Snowflake store_sales/date example adapted): running 7-day average, LAG 30 days, metric referencing metrics in PARTITION BY; missing required dimension error; window metric referenced by another metric error; FOR METRIC required flags.
- Compiler: `Decompose` keeps window expressions (children decomposed, partitions/orders kept as dimension markers); final projection maps markers onto query dimension columns.

### Task 5: Standard-SQL form
- Test `test/sql/semantic_views/standard_sql.test`: `SELECT dim, AGG(m) FROM sv GROUP BY dim`; bare/qualified/quoted "entity.calc" names; GROUP BY ordinal and alias; HAVING on AGG; ORDER BY/LIMIT; ad-hoc aggregates of dimensions; `AGG(m) * 10`; facts without GROUP BY; WHERE with subquery; table alias `FROM sv AS s`; inside CTE / subquery / CTAS / EXPLAIN; real table shadows the semantic view; errors (AGG of non-metric, metric without AGG, window functions, joins).
- Implement `src/semantic_sql_rewrite.cpp`: `bool RewriteSemanticViewReferences(ClientContext &, SemanticViewsState &, SQLStatement &)`; call it from `SemanticViewsOperatorExtension::Bind` (both for pending statements and for plain binder failures).

### Task 6: Docs + benchmark
- README sections for the new features; remove them from Limitations; add a semi-additive and a USING query to the benchmark if cheap.
