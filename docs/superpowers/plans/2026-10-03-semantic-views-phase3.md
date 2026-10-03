# Semantic Views — Phase 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> Execution mode chosen by the user: **native/inline, autonomous**. Tests below are the contract.

**Goal:** ASOF and range relationships, and tested `ALTER SEMANTIC VIEW`.

**Architecture:** Spec section 8. ASOF relationships compile to `ASOF LEFT JOIN ... ON <equalities> AND from.c >= to.c`.
Range relationships compile to an equi-join against a mapping derived table:
`(SELECT k.v AS __sv_key, r.* FROM (SELECT DISTINCT c AS v FROM <from>) k ASOF JOIN (<to> with NULL starts replaced by the
smallest value of c) r ON k.v >= r.start WHERE r.end IS NULL OR k.v < r.end)`, so the fact side keeps a hash join and the
range lookup is a sort-merge ASOF on distinct values.

**Tech Stack:** C++17, DuckDB v1.5.5, sqllogictest. **Spec:** `docs/superpowers/specs/2026-10-03-semantic-views-design.md`

## Global Constraints
- Build with `GEN=ninja make`; never edit `duckdb/` or `extension-ci-tools/`.

## Review Focus
1. Rows of the fact table without a matching ASOF / range row must be kept (NULL dimension) → Tasks 2-3 tests.
2. Values exactly on a range boundary (start inclusive, end exclusive) → Task 3 test.
3. Child aggregations or reversed traversal across ASOF / range edges must be rejected, not silently wrong → Task 2 test.

### Task 1: ALTER SEMANTIC VIEW tests
- `test/sql/semantic_views/alter.test`: RENAME TO (old name gone, new name queryable, name collision error), SET / UNSET COMMENT (SHOW + DESCRIBE), IF EXISTS on a missing view, error without IF EXISTS.

### Task 2: ASOF relationships
- `test/sql/semantic_views/asof.test`: Snowflake customer / customer_address / orders zip-code example (expected output from the docs); an order before any address keeps a NULL zip code.

### Task 3: Range relationships
- `test/sql/semantic_views/range.test`: Snowflake my_events / my_time_periods example (expected output from the docs), boundary values, events outside every range.

### Task 4: Docs
- README: remove the ASOF / range limitation, document both.
