# Semantic Views for DuckDB — Design

Date: 2026-10-03
Status: approved approach (A), autonomous execution authorized by the user

## 1. Goal

Implement Snowflake-compatible **semantic views** as a DuckDB extension (`semantic_views`):

- DDL as documented in Snowflake's
  [Using SQL commands to create and manage semantic views](https://docs.snowflake.com/en/user-guide/views-semantic/sql)
  and [CREATE SEMANTIC VIEW](https://docs.snowflake.com/en/sql-reference/sql/create-semantic-view).
- Querying as documented in
  [Querying semantic views](https://docs.snowflake.com/en/user-guide/views-semantic/querying):
  the `SEMANTIC_VIEW(...)` FROM-clause construct and the standard-SQL
  `SELECT dim, AGG(metric) FROM sv GROUP BY dim` form.
- Validation rules from
  [How Snowflake validates semantic views](https://docs.snowflake.com/en/user-guide/views-semantic/validation-rules).
- Must work at scale (~100M fact rows): no fan-out double counting, no unneeded joins,
  execution as fast as equivalent hand-written SQL.

Syntax follows Snowflake (the user prefers Snowflake's surface over
[anentropic/duckdb-semantic-views](https://github.com/anentropic/duckdb-semantic-views), whose
`semantic_view('name', dimensions := [...])` call syntax we do **not** adopt).

### Decisions taken with the user

| Topic | Decision |
|---|---|
| Parser approach | Mirror `duckdb-match_recognize`: vendored DuckDB PEG parser (namespace `semantic_views_peg`), grammar patches, `parse_function` fallback when the postgres parser fails |
| Execution approach | **A**: lower every semantic query into a plain DuckDB AST (subquery) at bind time; no custom physical operator |
| Persistence | In-memory registry per database instance (definitions are lost on restart) |
| Phasing | 3 phases (below); each phase is fully usable on its own |
| Build | Always `GEN=ninja make` |

### Out of scope (all phases)

Cortex-specific behavior (`AI_SQL_GENERATION`, `AI_QUESTION_CATEGORIZATION`, `AI_VERIFIED_QUERIES`,
`WITH CORTEX SEARCH SERVICE`, `SAMPLE_VALUES`, `IS_ENUM`), tags, `COPY GRANTS`, `MAX_STALENESS`,
materializations, privileges/grants, YAML import/export. The grammar **accepts** these clauses so
Snowflake DDL can be pasted verbatim; their values are stored as metadata (shown by `DESCRIBE`) and
otherwise ignored.

## 2. Phases

**Phase 1 — core**
- Repo: rename template `waddle` → `semantic_views`, drop OpenSSL, vendor the PEG parser, `AGENTS.md`.
- DDL: `CREATE [OR REPLACE | OR ALTER] SEMANTIC VIEW [IF NOT EXISTS]`, `DROP SEMANTIC VIEW [IF EXISTS]`,
  `DESCRIBE SEMANTIC VIEW`, `SHOW SEMANTIC VIEWS`, `SHOW SEMANTIC DIMENSIONS | METRICS | FACTS [IN sv]`
  (all with optional `LIKE '<pattern>'`).
- Model: logical tables (physical table or parenthesized query) with `PRIMARY KEY` / `UNIQUE`; equi-join
  relationships (named or unnamed, columns defaulted from PK / single UNIQUE); facts, dimensions,
  table metrics, derived metrics; `PRIVATE` / `PUBLIC`; facts/dimensions that aggregate child tables;
  metrics with nested aggregation over child tables; metrics referencing other metrics.
- Query: `SEMANTIC_VIEW(sv [DIMENSIONS ...] [METRICS ...] [FACTS ...] [WHERE ...])` with aliases
  (`AS` optional), `tbl.*` wildcards, ad-hoc scalar expressions over dimensions/facts, ad-hoc
  aggregate metrics, uncorrelated subqueries in WHERE and DIMENSIONS, clause order = output order.
- Multi-grain, fan-out-safe SQL generation (section 6). `semantic_view_sql(query)` debug function.
- 100M-row benchmark against hand-written SQL.

**Phase 2 — query surface & advanced metrics**
- Standard-SQL form: `SELECT ... AGG(m) ... FROM sv [AS a] [WHERE] [GROUP BY] [HAVING] [ORDER BY] [LIMIT]`.
- `USING (relationship, ...)` on metrics (role-playing / multi-path).
- `NON ADDITIVE BY (dim [ASC|DESC] [NULLS FIRST|LAST], ...)` semi-additive metrics.
- Window-function metrics incl. `PARTITION BY EXCLUDING`.
- `SHOW SEMANTIC DIMENSIONS IN sv FOR METRIC m` (with `required` column).

**Phase 3 — joins & lifecycle**
- `ASOF` relationships and range relationships (`CONSTRAINT ... DISTINCT RANGE BETWEEN ... EXCLUSIVE`,
  `REFERENCES t (BETWEEN start AND end EXCLUSIVE)`).
- `ALTER SEMANTIC VIEW [IF EXISTS] sv RENAME TO new | SET COMMENT = '...' | UNSET COMMENT`.

The grammar for **all** phases lands in Phase 1; features scheduled for a later phase raise
`NotImplementedException("<feature> is not supported yet")` until implemented.

## 3. Architecture

```
SQL text
  │  postgres parser fails (CREATE SEMANTIC VIEW / SEMANTIC_VIEW( / SHOW SEMANTIC ...)
  ▼
parser extension (src/semantic_views_binding.cpp)
  │  token pre-filter, then SemanticViewsParser: vendored PEG parser + custom grammar/transformers
  ▼
native SQLStatement (SelectStatement over internal table functions, semantic_lowering.hpp)
  │  stashed in a ClientContextState, plan_function throws, OperatorExtension::Bind binds it
  ▼
Binder
  ├── __semantic_view_create / _drop / _rename / _set_comment   bind → validate; scan → mutate registry
  ├── __semantic_view_describe / _show_views / _show_members / _show_dimensions_for_metric
  │                                                              bind → snapshot registry → rows
  └── __semantic_view_query(<serialized ParsedSemanticQuery>)    bind_replace → SemanticQueryCompiler
                                                                 → SubqueryRef (plain DuckDB SELECT)

SELECT ... FROM <semantic view> (standard SQL): binding fails → SemanticViewsRebindState::OnPlanningError
rewrites the query node onto __semantic_view_query (semantic_sql_rewrite.cpp) and asks for a rebind.
```

Units:

| Unit | Responsibility | Depends on |
|---|---|---|
| `third_party/duckdb_peg_parser` | vendored PEG parser; `custom/` grammar + per-rule transformers; `patches/` | DuckDB headers |
| `semantic_views_parser` | SQL text → lowered statements (PEG parser), semantic-syntax pre-filter | PEG parser |
| `semantic_views_binding` | parser extension, pending-bind handoff, operator extension, rebind state | parser, rewrite |
| `parsed_semantic_view` | `ParsedSemanticView` / `ParsedSemanticQuery` (serializable parse results) | DuckDB parser types |
| `semantic_lowering` | internal table function names, argument layouts, BLOB payload encode/decode | parsed types |
| `semantic_view_registry` | thread-safe in-memory map `(catalog, schema, name) → shared_ptr<SemanticModel>` | model |
| `semantic_model` / `_builder` | validated, resolved, immutable model: tables, relationship graph, members, types | parsed def, Binder |
| `semantic_analysis` | grain analysis: row tables, aggregate homes, metric decomposition into atoms | model |
| `semantic_query_compiler` (+ `semantic_join_scope`) | model + query spec → SELECT statement | model, analysis |
| `semantic_sql_rewrite` (+ `semantic_statement_visitor`) | standard-SQL queries over a view by name | compiler inputs |
| `semantic_{ddl,show,query}_functions` | the internal table functions | all above |

### 3.1 Parser integration

- Vendored copy of `duckdb/extension/autocomplete` PEG parser at the pinned DuckDB tag (v1.5.5),
  wrapped into `duckdb::semantic_views_peg` by the same scripts as match_recognize
  (`scripts/sync_duckdb_peg_parser.sh`, `regenerate_duckdb_peg_parser.sh`, `make_peg_patch.sh`,
  `wrap_duckdb_peg_namespace.py`). Different namespace ⇒ both extensions can be loaded together.
- Grammar additions in `custom/grammar/statements/semantic_view.gram`; transformers in
  `custom/transformer/transform_semantic_view.cpp`; hooks in a single patch:
  - `Statement` gets `SemanticViewStatement` as its **first** alternative
    (`CreateSemanticViewStmt / DropSemanticViewStmt / AlterSemanticViewStmt / DescribeSemanticViewStmt / ShowSemanticStmt`).
  - `InnerTableRef` gets `SemanticViewRef` as its first alternative:
    `('SEMANTIC_VIEW' / 'SEMANTIC' 'VIEW') Parens(SemanticViewQueryBody) TableAlias?`.
  - Clause keywords (`DIMENSIONS`, `METRICS`, `FACTS`, `RELATIONSHIPS`, `TABLES`, ...) are **not**
    reserved. The PEG matcher ignores negative lookaheads, so the `SEMANTIC_VIEW(...)` body is parsed
    as a flat sequence of pieces and a small state machine in the transformer tells clause keywords,
    items and bare aliases apart.
- `ParseFunction` is only reached when postgres fails. Cheap pre-filter: lowercase text contains
  `semantic`; token scan confirms `SEMANTIC_VIEW (`, `SEMANTIC VIEW`, `SHOW SEMANTIC`,
  `DESCRIBE|DESC SEMANTIC`, `DROP SEMANTIC`, `ALTER SEMANTIC`, `CREATE ... SEMANTIC VIEW`.
  Anything else returns "not ours" so other extensions (e.g. match_recognize) get their turn.
- `plan_function` uses the match_recognize handoff: stash the rewritten statement in a
  `ClientContextState`, throw a `BinderException`, and `OperatorExtension::Bind` rebinds it. This keeps
  `EXPLAIN`, `CREATE TABLE AS`, `INSERT ... SELECT`, CTEs, joins, prepared statements working.
- Known limitation: one statement cannot combine `SEMANTIC_VIEW(...)` and `MATCH_RECOGNIZE(...)`
  (each extension's PEG grammar only knows its own clause).

### 3.2 Lowering (no session state in the AST)

The PEG transformers produce only self-contained constant arguments, so lowered ASTs survive
`PREPARE`, views and macros:

- Each statement lowers to `SELECT * FROM __semantic_view_<operation>(<constant arguments>)`.
  Definitions (`ParsedSemanticView`) and queries (`ParsedSemanticQuery`) travel as BLOB arguments
  serialized with DuckDB's `BinarySerializer`, so expressions keep their exact AST (no SQL text
  round trip). `SHOW ... LIKE 'p'` lowers to a `name ILIKE 'p'` filter around the SHOW function.
- `SEMANTIC_VIEW(...)` lowers to a `TableFunctionRef` over `__semantic_view_query(<payload>)`
  carrying the user's table alias.

### 3.3 Registry

`SemanticViewRegistry` lives in the extension's shared state (one per `DatabaseInstance`), keyed by
lower-cased `(catalog, schema, name)`; unqualified names resolve against the client's default
catalog/schema (`ClientData::catalog_search_path`). Not transactional: a `CREATE` inside a
transaction that is rolled back stays registered (documented). Models are immutable;
ALTER builds a new model from the edited definition and swaps it in under the mutex only if the
view is still the one it was built from, so concurrent queries keep the version they bound against.

## 4. DDL grammar (accepted surface)

```
CREATE [OR REPLACE | OR ALTER] SEMANTIC VIEW [IF NOT EXISTS] <name>
  TABLES ( logicalTable [, ...] )
  [ RELATIONSHIPS ( relationshipDef [, ...] ) ]
  [ FACTS ( factDef [, ...] ) ]
  [ DIMENSIONS ( dimensionDef [, ...] ) ]
  [ METRICS ( metricDef [, ...] ) ]
  [ COMMENT = '<text>' ] [ MAX_STALENESS = <int> ]
  [ AI_SQL_GENERATION '<text>' ] [ AI_QUESTION_CATEGORIZATION '<text>' ]
  [ AI_VERIFIED_QUERIES ( verifiedQuery [, ...] ) ]
  [ [WITH] TAG ( name = 'v' [, ...] ) ] [ COPY GRANTS ]

logicalTable ::= [alias AS] { qualified_table_name | ( select ) }
                 [PRIMARY KEY (c, ...)] [UNIQUE (c, ...)]*
                 [CONSTRAINT [name] DISTINCT RANGE BETWEEN c1 AND c2 EXCLUSIVE]
                 [WITH SYNONYMS [=] ('s', ...)] [COMMENT = '...'] [[WITH] TAG (...)]
relationshipDef ::= [name AS] t (c, ...) REFERENCES rt [ ( [ASOF] c [, ...] | BETWEEN s AND e EXCLUSIVE ) ]
factDef      ::= [PRIVATE|PUBLIC] t.name [LABELS = (FILTER)] AS expr annotations
dimensionDef ::= [PUBLIC] t.name [LABELS = (FILTER)] AS expr annotations [WITH CORTEX SEARCH SERVICE n [USING c]]
metricDef    ::= [PRIVATE|PUBLIC] [t.]name [USING (rel, ...)] [NON ADDITIVE BY (d [ASC|DESC] [NULLS FIRST|LAST], ...)]
                 AS expr annotations
annotations  ::= [WITH SYNONYMS [=] (...)] [[WITH] TAG (...)] [COMMENT = '...'] [SAMPLE_VALUES (...)] [IS_ENUM]
                 (any order)

DROP SEMANTIC VIEW [IF EXISTS] <name>
ALTER SEMANTIC VIEW [IF EXISTS] <name> { RENAME TO <new> | SET COMMENT = '...' | UNSET COMMENT }
{DESCRIBE | DESC} SEMANTIC VIEW <name>
SHOW SEMANTIC VIEWS [LIKE 'p'] [IN {SCHEMA s | DATABASE d}]
SHOW SEMANTIC {DIMENSIONS | METRICS | FACTS} [LIKE 'p'] [IN [SEMANTIC VIEW] <name>]
SHOW SEMANTIC DIMENSIONS [LIKE 'p'] IN <name> FOR METRIC <m>
```

Window metrics: inside METRICS, `OVER (PARTITION BY EXCLUDING d, ...)` is accepted by a custom
window rule used only in metric definitions.

`CREATE OR ALTER` behaves like `CREATE OR REPLACE` (no tags or grants to preserve in-memory).
Semantic view names share a namespace only with other semantic views; creating one with the same
name as a table is allowed but the table wins in the standard-SQL form (Phase 2).

### Result sets

| Statement | Columns |
|---|---|
| CREATE / DROP / ALTER | `status VARCHAR` (`Semantic view X successfully created.` etc., like Snowflake) |
| `SHOW SEMANTIC VIEWS` | `created_on TIMESTAMP WITH TIME ZONE, name, database_name, schema_name, comment` |
| `SHOW SEMANTIC DIMENSIONS/METRICS/FACTS` | `database_name, schema_name, semantic_view_name, table_name, name, data_type, synonyms VARCHAR[], comment` (+ `required BOOLEAN` for `FOR METRIC`) |
| `DESCRIBE SEMANTIC VIEW` | `object_kind, object_name, parent_entity, property, property_value` (Snowflake layout) |

## 5. Model building and validation (at CREATE)

1. **Tables.** Resolve each `qualified_table_name` through the catalog using the client's search path,
   store it fully qualified (`catalog.schema.table`). Bind `SELECT * FROM <source>` with a fresh
   `Binder` to learn column names/types (works for tables, views and parenthesized queries; nothing
   executes). Alias defaults to the unqualified table name. Duplicate aliases → error.
2. **Keys.** PK / UNIQUE columns must be physical columns (or facts/dims that are a bare column).
3. **Relationships.** `from(cols) REFERENCES to[(cols)]`: target cols default to `to`'s PK, else its
   single UNIQUE set, else error; target cols must equal a PK or UNIQUE set. Arity must match.
   Unnamed relationships get `<from>_to_<to>[_n]`. The relationship is **one-to-one** when the source
   columns form a PK/UNIQUE set of `from`, else **many-to-one**. Self references and cycles
   (including transitive) → error. One-to-one edges are traversable both ways for row-level
   references (same granularity).
4. **Members.** Names are unique per `(table, name)`; derived metric names must not collide with any
   table metric name. Every expression is parsed; column references are classified (section 5.1);
   the dependency graph between members must be acyclic.
5. **Kind rules** (Snowflake validation rules):
   - facts/dimensions: row-level. May reference own columns, own row-level members, row-level members
     (or columns) of tables reachable through many-to-one/one-to-one edges ("up"), metrics of tables
     that reach this one ("down", i.e. higher granularity) and aggregates over row-level members of
     down tables (child aggregation). No window functions, no table functions.
   - table metrics: must contain an aggregate or reference metrics. Aggregates over up/same rows are
     single aggregates; references to down row-level members must sit inside a nested aggregate
     (`AVG(SUM(orders.o_totalprice))`). May reference same/up table metrics directly.
   - derived metrics: scalar expressions over metrics and aggregates of row-level members; no physical
     columns, no bare row-level members, no aggregates of metrics, no window functions.
   - At least one dimension or metric.
6. **Binding probe.** For every public and private member, the compiler (section 6) produces a
   probe query (`... LIMIT 0`) that is bound (not executed) with a fresh `Binder`; binder errors are
   re-raised as `Invalid semantic view: <member>: <error>`. The bound output type is stored as the
   member's `data_type` for `SHOW`.

### 5.1 Name resolution inside expressions

Given an expression owned by table `T` (or by no table for derived metrics):

- `q.n` where `q` is a logical table alias: if `n` names a member of `q` and the reference is not the
  member's own definition → member reference; else if `n` is a column of `q` → column reference;
  else error. (Self-name exception: `customers.c_name AS customers.c_name` resolves to the column.)
- `n` (unqualified): member of `T` named `n` (unless self) → member; else a unique member named `n`
  in the whole view → member; else column `n` of `T` → column; ambiguous member names → error
  `Ambiguous name 'n'; qualify it with its logical table`.
- `q.*` only in query item lists (wildcards). Column references inside subqueries are left untouched
  (subqueries must be uncorrelated).
- Column references to physical columns of a table other than the owning one are accepted (as an
  implicit row-level reference of that table) — strictly more permissive than Snowflake.

## 6. Query compilation (the scale-critical part)

### 6.1 Terms

- **Grain** of a table: one row of that table. `up(T)` = tables reachable from `T` through
  many-to-one / one-to-one edges (lower or equal granularity). `T` is at the same or higher
  granularity than every table in `up(T)`.
- **Row-level expression** on `T`: value per row of `T`.
- **Aggregate atom**: `(home table H, aggregate expression)` evaluated over the rows of `H`.
- Every metric (table, derived, ad-hoc) is decomposed into a **post-aggregation expression** over
  aggregate atoms. Table metric `orders.profit AS orders.revenue - orders.cost` → atoms
  `orders:SUM(amount)`, `orders:SUM(cost)` and post-expr `atom1 - atom2`. Derived metric
  `m AS t1.m1 + t2.m2` → atoms in two homes.

### 6.2 Home tables

- Dimension / fact / ad-hoc row expression: home = the table among its referenced tables from which
  every other referenced table is in `up(home)`; none → `Invalid dimension: tables X and Y are not
  related`.
- Atom of a table metric on `T`: home = `T`.
- Atom of a derived / ad-hoc metric `AGG(row exprs)`: home computed like a dimension.

### 6.3 Validity checks (per query)

- At least one of DIMENSIONS / METRICS / FACTS; FACTS and METRICS together → error.
- PRIVATE members cannot be requested or referenced in WHERE.
- For every atom with home `H`: every dimension home and every WHERE-referenced home must be in
  `{H} ∪ up(H)` → else Snowflake's error `Invalid dimension specified: The dimension entity 'X' must be
  related to and have an equal or lower level of granularity compared to the base metric or dimension
  entity 'H'.`
- Each table needed from `H` must be reachable by exactly one relationship path (Phase 2: `USING`
  disambiguates) → else `Multiple relationship paths from H to X; specify USING (...) on the metric`.
- Output column names (alias, else unqualified member name, else expression text) must be unique.

### 6.4 SQL shape

**Block** per distinct atom home `H` (Phase 2: per `(H, USING set, non-additive spec)`):

```sql
SELECT <dim_1> AS d1, ..., <dim_k> AS dk, <agg_1> AS a1, ...
FROM <H source> AS "h"
LEFT JOIN <P1 source> AS "p1" ON h.fk = p1.pk          -- only tables actually referenced,
LEFT JOIN <P2 source> AS "p2" ON p1.fk = p2.pk         -- along the unique up-path
WHERE <filter>                                          -- pre-aggregation (Snowflake semantics)
GROUP BY d1, ..., dk
```

- Many-to-one LEFT JOINs never multiply `H` rows ⇒ no fan-out by construction. Rows of `H` with a
  dangling FK keep NULL dimensions (Snowflake behavior).
- **Child aggregation** (fact/dimension on `T` that aggregates a down table `C`, or a nested inner
  aggregate in a metric) compiles to a correlated scalar subquery
  `(SELECT agg(...) FROM C ... WHERE c.fk = t.pk)`. DuckDB decorrelates it into a hash aggregate +
  join (exact empty-set semantics: `COUNT` → 0), which scales linearly.
- Every table instance gets a unique SQL alias per scope (`"orders"`, `"__sv1_orders"` in nested
  scopes); all column references are rewritten to those aliases by the compiler, so user expressions
  can never capture the wrong scope.
- Join pruning is done by the compiler: only tables referenced by requested members, WHERE, and
  their transitive member dependencies are joined.

**Combination** of blocks:
- one block → it is the result (no extra layer beyond the final projection).
- several blocks → `UNION ALL` of blocks (each padded with `NULL` for other blocks' atoms, plus a
  constant block tag `__sv_block`), then `GROUP BY d1..dk` with
  `any_value(a_i) FILTER (WHERE __sv_block = b(i))`. Each block contributes at most one row per
  dimension tuple, so this equals a FULL OUTER JOIN on `IS NOT DISTINCT FROM`, without coalesce
  chains; it treats NULL dimension values as equal (like Snowflake).
- no dimensions → each block yields exactly one row; same combination yields one row.

**Final projection**: requested items in request order: dimensions, metric post-expressions over
atom columns, with the requested aliases.

**Dimensions-only** query: base `H` = home of all dimension and WHERE homes (as in 6.2);
`SELECT DISTINCT dims FROM H LEFT JOIN ... WHERE filter`.

**Facts** query: base `H` as above. Without dimensions: `SELECT facts FROM ... WHERE filter`
(row-level, no grouping). With dimensions: `GROUP BY dims` with `any_value(fact)`; dimensions and
facts must all live on the same logical table (Snowflake rule).

### 6.5 Scale notes

- Generated plans are exactly what a careful analyst would write; DuckDB's optimizer still does
  filter pushdown, join ordering (build on the small dimension side), projection pushdown and
  parallel hash aggregation.
- Compilation cost is O(size of definition) per query and independent of data size.
- Benchmark (`benchmark/semantic_views/`): 100M-row `sales` fact, 1M `customers`, 10k `products`,
  25 `nations`; queries cover single-table metric by local dim, metric by 2-hop dim, two-grain
  metrics, child-aggregate fact, filter on remote dim. Acceptance: each semantic query within 10% of
  the equivalent hand-written SQL (median of 5 runs) and results identical.

## 7. Phase 2 details

- **Standard-SQL form.** When binding fails, `OperatorExtension::Bind` walks the statement; every
  `SelectNode` whose FROM is a single `BaseTableRef` naming a registered semantic view (and that does
  not name an existing catalog table) is rewritten: GROUP BY items → DIMENSIONS; `AGG(m)` → METRICS;
  other aggregates over dimensions/facts → ad-hoc metrics; non-aggregated select items not in
  GROUP BY → FACTS; WHERE → WHERE; HAVING / ORDER BY / LIMIT / DISTINCT applied over the
  `__semantic_view_query` result by output column. Rejected: joins, PIVOT/UNPIVOT, LATERAL, window
  functions, QUALIFY (Snowflake limitations). Qualified (`entity.calc`), bare and quoted
  `"entity.calc"` names resolve as in Snowflake.
- **USING.** A metric's `USING (rel, ...)` pins the relationship edges used to reach the tables those
  relationships point to; its atoms form their own block keyed by the USING set. Dimensions on the
  role-played table are joined through the pinned edges in that block.
- **NON ADDITIVE BY.** Atoms of a semi-additive metric get their own block. With query dimensions `Q` and
  non-additive dimensions `N`: rank the block's rows with
  `rank() OVER (PARTITION BY Q ORDER BY N <reversed>)`, keep rank 1 (the last snapshot of every group), then
  aggregate. When a dimension of `N` is also in `Q`, every group has a single value of it and the aggregation is a
  plain one. With a single non-additive dimension the last value comes from unordered window aggregates
  (`max` for ASC / `min` for DESC, NULL when NULLs sort last and exist) instead of a sorted `rank()`.
- **Window metrics.** Computed in the final projection layer over the combined result:
  `fn(<post-expr of inner metric>) OVER (PARTITION BY ... ORDER BY ... frame)`;
  `PARTITION BY EXCLUDING d` = all query dimensions except `d`. Referenced dimensions must be in the
  query (`Dimension 'X' used in a window function metric must be requested in the query.`).
  Window metrics cannot be referenced by other members.
- **SHOW ... FOR METRIC**: dimensions whose home is in `{H} ∪ up(H)` for every atom home of the
  metric; `required = true` for dimensions referenced by the metric's window clause.

## 8. Phase 3 details

- **ASOF relationship** `from(c1, ..., ck) REFERENCES to(c'1, ..., ASOF c'j, ...)`: the up-join from
  `from` to `to` becomes `ASOF LEFT JOIN to ON <equalities> AND from.cj >= to.c'j`.
- **Range relationship** `from(c) REFERENCES to(BETWEEN s AND e EXCLUSIVE)` with a matching
  `DISTINCT RANGE` constraint on `to`: every column read from `to` (or from tables reached through it) compiles to
  a correlated scalar subquery `(SELECT to.x FROM to WHERE (to.s IS NULL OR from.c >= to.s) AND (to.e IS NULL OR
  from.c < to.e))`. DuckDB decorrelates it on the distinct values of `from.c`, so the cost is a hash aggregate of the
  referencing column plus a small join of its distinct values with the range table, instead of a range join over
  every row (measured 9x faster than a hand-written inequality join at 100M rows). Rows without a matching range
  read NULL. Child aggregations through ASOF or range relationships are rejected.
- **ALTER SEMANTIC VIEW**: rename (re-key in registry), set/unset comment.

## 9. Error handling

- Parse errors from the PEG parser surface as DuckDB `ParserException` with location (via
  `DISPLAY_EXTENSION_ERROR`) only when the statement was recognized as ours by the token pre-filter;
  otherwise the original postgres error is shown.
- Definition errors: `BinderException("Invalid semantic view '<name>': ...")`.
- Query errors: `BinderException` mirroring Snowflake messages where documented.
- Unknown semantic view: `CatalogException("Semantic view with name X does not exist!")`.

## 10. Testing

- sqllogictest files under `test/sql/semantic_views/` per feature: ddl, describe/show, query basics,
  multi-table, multi-grain, derived metrics, child aggregation, filters/subqueries, errors,
  standard-SQL form, using, non-additive, window, asof/range, alter. Each multi-table test compares
  the semantic query result with a hand-written SQL query (`query` blocks with identical expected
  output) to prove fan-out safety, using data with deliberate duplicates on the child side.
- Snowflake doc examples (TPC-H-shaped views via DuckDB's `tpch` extension at small SF, ASOF zip-code
  example, range-join time periods example) reproduced as tests with expected results.
- `semantic_view_sql()` golden tests for the generated SQL shape (join pruning, block structure).
- Benchmark script (section 6.5) run manually and recorded in `docs/benchmarks.md`.
