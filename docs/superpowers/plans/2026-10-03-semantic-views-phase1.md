# Semantic Views — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> Execution mode chosen by the user: **native/inline, autonomous** (user asleep, "pick the recommended option and continue").
> Tests below are the contract; C++ is written inside each task against them.

**Goal:** Snowflake-compatible `CREATE/DROP/DESCRIBE/SHOW SEMANTIC VIEW` and `SEMANTIC_VIEW(...)` queries in a DuckDB extension, fan-out safe at 100M rows.

**Architecture:** Vendored DuckDB PEG parser (namespace `semantic_views_peg`) reached via `parse_function` when postgres fails; statements lowered to internal table functions; queries expanded in `bind_replace` into a plain DuckDB AST built by `SemanticQueryCompiler` from an in-memory, validated `SemanticModel`.

**Tech Stack:** C++17, DuckDB v1.5.5 extension API, CMake + Ninja, sqllogictest.

**Spec:** `docs/superpowers/specs/2026-10-03-semantic-views-design.md`

## Global Constraints

- Build only with `GEN=ninja make` (release). Debug builds only if unavoidable.
- DuckDB pinned to tag v1.5.5 (`duckdb/` submodule); never edit `duckdb/` or `extension-ci-tools/`.
- Extension name: `semantic_views`. PEG namespace: `duckdb::semantic_views_peg`.
- Internal function names: `__semantic_view_ddl`, `__semantic_view_show`, `__semantic_view_query`; public debug: `semantic_view_sql`.
- No session state in lowered ASTs (constants only).
- DuckDB coding style (`make format-fix`), tabs.

## Review Focus

1. Fan-out: a dimension from a parent joined to a fact with duplicate child rows must not inflate metrics → multi-grain tests compare with hand-written SQL on data with duplicates (Task 6).
2. Ordinary SQL must be untouched when the extension is loaded (postgres path) → smoke test in Task 2.
3. Identifiers: mixed case / quoted names for views, tables, members (`"Order Date"`) → Task 4 + Task 6 tests.
4. NULL dimension values and dangling foreign keys must group as one NULL group and keep rows → Task 6 test.
5. Clause keywords as identifiers (`metrics` column, alias named `facts`) must not break parsing → Task 3 test.

---

### Task 1: Repo bootstrap

**Files:** rename `waddle` → `semantic_views` across `CMakeLists.txt`, `extension_config.cmake`, `Makefile`, `src/`, `test/`, `.github/workflows/MainDistributionPipeline.yml`, `vcpkg.json` (drop openssl); create `AGENTS.md`; Makefile `GEN ?= ninja`.

- [ ] Run `python3 scripts/bootstrap-template.py semantic_views`; remove OpenSSL + waddle functions; extension registers nothing yet.
- [ ] `test/sql/semantic_views/load.test`: `require semantic_views` + `SELECT 42` → 42.
- [ ] `GEN=ninja make && make test` passes. Commit.

### Task 2: Vendored PEG parser + parser extension skeleton

**Files:** `third_party/duckdb_peg_parser/**` (via adapted `scripts/sync_duckdb_peg_parser.sh`, `regenerate_duckdb_peg_parser.sh`, `make_peg_patch.sh`, `wrap_duckdb_peg_namespace.py` from match_recognize, namespace `semantic_views_peg`), `src/parser/semantic_views_parser.cpp`, `src/include/semantic_views_parser.hpp`, `src/include/semantic_views_state.hpp`.

**Produces:** `SemanticViewsParserExtension` (parse_function/plan_function), `SemanticViewsPendingBind::{Set,Take}`, `SemanticViewsOperatorExtension::Bind`, `SemanticViewsRewriter::ContainsSemanticSyntax(const string&) -> bool`, `SemanticViewsRewriter::Parse(const string&, ParserOptions) -> vector<unique_ptr<SQLStatement>>`.

- [ ] Test `test/sql/semantic_views/parser_passthrough.test`: ordinary queries (CTE, joins, window, `CREATE VIEW`) unaffected; a syntax error that is not ours keeps the postgres message (`statement error` / `syntax error at or near`).
- [ ] Vendor, build, wire extension. Commit.

### Task 3: Grammar, parsed definitions, lowering

**Files:** `third_party/duckdb_peg_parser/custom/grammar/statements/semantic_view.gram`, `custom/transformer/transform_semantic_view.cpp`, patch (Statement/InnerTableRef hooks, keyword lists if needed, factory registration), `src/include/parsed_semantic_view.hpp`, `src/parsed_semantic_view.cpp`.

**Produces:**
- `struct ParsedSemanticView { QualifiedName name; vector<ParsedLogicalTable> tables; vector<ParsedRelationship> relationships; vector<ParsedMember> facts, dimensions, metrics; string comment; vector<pair<string,string>> extra_properties; string ToSQL() const; }`
- `struct ParsedSemanticQuery { QualifiedName view; vector<ParsedQueryItem> items; unique_ptr<ParsedExpression> where; string alias; Value ItemsToValue() const; static vector<ParsedQueryItem> ItemsFromValue(const Value&); }`
- `SemanticViewParser::ParseDefinition(const string &canonical_sql) -> unique_ptr<ParsedSemanticView>`
- Lowerings: DDL → `SELECT * FROM __semantic_view_ddl(...)`, SHOW/DESCRIBE → `SELECT * FROM __semantic_view_show(...)`, clause → `TableFunctionRef(__semantic_view_query(name, items, where))`.

- [ ] Test (in Task 4 file) round trip: `CREATE` then `DESCRIBE` returns canonical text; column named `metrics` and alias `facts` parse; `SEMANTIC VIEW(` spelling accepted.
- [ ] Implement; commit with Task 4.

### Task 4: Registry, model builder, DDL/SHOW/DESCRIBE

**Files:** `src/include/semantic_view_registry.hpp`, `src/semantic_view_registry.cpp`, `src/include/semantic_model.hpp`, `src/semantic_model_builder.cpp`, `src/semantic_views_functions.cpp`.

**Produces:** `SemanticViewRegistry::{Create,Drop,Get,List}`; `SemanticModel` (tables with resolved source + columns, relationship graph with `Up(table)`/path finding, members with `ResolvedExpression` references, kinds, data types); `SemanticModelBuilder::Build(ClientContext&, const ParsedSemanticView&) -> shared_ptr<SemanticModel>`.

Tests `test/sql/semantic_views/ddl.test`, `show_describe.test`, `ddl_errors.test`:
- create / IF NOT EXISTS (no error, no replace) / OR REPLACE / OR ALTER / duplicate → `already exists`; DROP / DROP IF EXISTS / drop missing → `does not exist`; schema-qualified names; case-insensitive lookup.
- SHOW SEMANTIC VIEWS (+LIKE), SHOW SEMANTIC DIMENSIONS/METRICS/FACTS IN sv with data_type; DESCRIBE rows.
- errors: unknown table, unknown column, unknown table alias, duplicate member, relationship to table without PK and no columns, cycle, self reference, aggregate in dimension, no aggregate in metric, derived metric referencing physical column, member reference cycle, no dimension or metric, ambiguous unqualified name.
- Commit.

### Task 5: Query compiler — single table + up-joins

**Files:** `src/include/semantic_query_compiler.hpp`, `src/semantic_query_compiler.cpp`; `__semantic_view_query` bind_replace in `semantic_views_functions.cpp`.

**Produces:** `SemanticQueryCompiler(const SemanticModel&, ClientContext&)`, `unique_ptr<QueryNode> Compile(const SemanticQuerySpec&)`; `SemanticQuerySpec` = resolved items + where.

Tests `test/sql/semantic_views/query_basic.test`: metrics only, dims only (distinct), dims+metrics, aliases with/without AS, clause order = column order, wildcards `t.*`, ad-hoc dim expression `DATE_PART('year', o.order_date) AS year`, WHERE on dim and on fact, uncorrelated subquery in WHERE, PRIVATE metric rejected, unknown member error, outer query composition (JOIN/CTE/ORDER BY on result, `CREATE TABLE AS`, `EXPLAIN`, prepared statement), mixed-case/quoted names.

### Task 6: Multi-grain, derived metrics, child aggregation, facts queries

Tests `test/sql/semantic_views/query_multi_table.test`, `query_grain.test`, `query_errors.test`:
- TPC-H-shaped data with duplicates; every result checked against hand-written SQL.
- 2-hop dimension; metrics from 2 tables in one query (multi-block); no-dims multi-block; derived metric across tables; derived metric `SUM(t.dim)`; metric referencing same-table metrics; nested aggregation `AVG(SUM(child.x))`; fact aggregating child (`COUNT(li.id)`, zero when no children); dimension referencing child metric; FACTS query rows; FACTS + DIMENSIONS; dangling FK → NULL group kept; NULL dims merged across blocks.
- errors: dimension at higher granularity than metric (Snowflake message), FACTS with METRICS, unrelated tables, multiple paths, duplicate output names.

### Task 7: `semantic_view_sql` + Snowflake doc examples

- `semantic_view_sql('<query text>')` returns generated SQL; golden test proves join pruning (unused tables absent) and block structure.
- Snowflake examples: `tpch_rev_analysis`-like view on `tpch` extension SF 0.01 (load `tpch` in `extension_config.cmake`), customer_count / order_average_value / grouping examples.

### Task 8: 100M benchmark + docs

- `benchmark/semantic_views/setup_100m.sql`, `queries/*.sql` (semantic + hand-written pairs), `scripts/bench_semantic_views.sh` (runs each 5×, median, compares, checks equal results via `EXCEPT`).
- `README.md` (usage, semantics, limitations), `docs/benchmarks.md` results. Commit.
