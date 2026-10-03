# Many-to-many bridges and BI catalog visibility — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let metrics be grouped/filtered through bridge tables without double counting, and expose every semantic view
as a typed catalog view that BI tools can discover and query by name.

**Architecture:** A new `SemanticBridgeResolver` finds the bridge table for row-level items a metric's home table cannot
reach; `MetricQueryCompiler::BuildBlock` joins one `SELECT DISTINCT (anchor key, bridged dims)` subquery per bridge.
A new `semantic_catalog` unit creates/drops a placeholder `VIEW` (typed, commented columns; body
`SELECT * FROM __semantic_view_placeholder(...)` that always fails at bind) next to each semantic view; the existing
standard-SQL rewrite recognizes placeholders, and rewrites `DESCRIBE` over them.

**Tech Stack:** C++17, DuckDB v1.5.5 extension API, sqllogictest.

**Spec:** `docs/superpowers/specs/2026-10-03-many-to-many-and-bi-catalog-design.md`

## Global Constraints

- Build only with `GEN=ninja make` (release). Never modify `./duckdb` or `./extension-ci-tools`.
- DuckDB coding style; `make format-fix` before each commit, `make format-check` must pass.
- Tests: `./build/release/test/unittest "test/sql/*"` must stay green after every task.
- Error messages quoted in the spec are used verbatim.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A bridged dimension combined with a local dimension *and* a second metric block (derived metric over `books` and
   `authors`): merge must still align groups → covered in Task 1 test "derived metric across blocks".
2. Book without authors + bridged filter `authors.author_name IS NULL` → documented to drop the book (Task 2 test pins it).
3. Semantic view in a non-default schema (`analytics.sales`) gets its placeholder in that schema, and `RENAME` keeps the
   schema → Task 5 test.
4. Members sharing a name across tables become `table.name` columns and are queryable as quoted identifiers through
   the placeholder → Task 6 test.
5. `CREATE SEMANTIC VIEW IF NOT EXISTS` on an existing semantic view must not touch the placeholder; on a real table it
   must error → Task 5 test.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/include/semantic_bridge.hpp`, `src/semantic_bridge.cpp` (new) | bridge / anchor resolution over the relationship graph; common base for dimension-only queries |
| `src/semantic_query_compiler.cpp` | block compilation with bridge groups, WHERE conjunct split, dimension-only fallback |
| `src/semantic_show_functions.cpp` | `FOR METRIC` lists bridged dimensions |
| `src/include/semantic_catalog.hpp`, `src/semantic_catalog.cpp` (new) | placeholder columns, name-conflict check, create/drop placeholder, placeholder detection, placeholder table function, DESCRIBE node |
| `src/semantic_ddl_functions.cpp` | DDL `Execute(ClientContext &)` maintains placeholders |
| `src/semantic_sql_rewrite.cpp`, `src/semantic_statement_visitor.cpp` | placeholder-aware lookup, DESCRIBE hook |
| `test/sql/semantic_views/many_to_many.test`, `catalog.test` (new) | feature tests |
| `benchmark/semantic_views/*`, `docs/benchmarks.md`, `README.md` | q10, docs |

---

### Task 1: Bridge resolver and bridged dimensions in metric blocks

**Files:**
- Create: `src/include/semantic_bridge.hpp`, `src/semantic_bridge.cpp`
- Modify: `CMakeLists.txt` (add `src/semantic_bridge.cpp` to `EXTENSION_SOURCES`), `src/semantic_query_compiler.cpp` (`MetricQueryCompiler`)
- Test: `test/sql/semantic_views/many_to_many.test`

**Interfaces:**
- Produces:
  ```cpp
  struct SemanticBridge {
      idx_t bridge;              // B
      idx_t anchor;              // P
      vector<string> anchor_key; // key columns of P (primary/unique key)
      idx_t key_source;          // table on B's side whose columns equal P's key (last edge source on Path(B, P))
      vector<string> key_source_columns;
  };
  class SemanticBridgeResolver {
  public:
      explicit SemanticBridgeResolver(const SemanticModel &model);
      //! Bridge through which `home` reaches every table of `tables` (some not in reach(home)); false when none,
      //! throws BinderException when ambiguous
      bool Resolve(idx_t home, const set<idx_t> &tables, const vector<idx_t> &using_relationships,
                   SemanticBridge &result) const;
      //! Table not in `tables` reaching all of them with diverging paths; INVALID_INDEX when none; throws when ambiguous
      idx_t CommonBase(const set<idx_t> &tables) const;
  };
  ```

- [ ] **Step 1: Write the failing test** — `many_to_many.test` with the library schema (authors with `country`,
  `active`; books with `category`, `page_count`, `publisher`; `book_authors`; `genres` + `book_genres`), traps: book 1
  by two FR authors, book 4 without authors, author 5 without books. Queries (each followed by the hand-written
  equivalent with identical expected rows):
  - `METRICS books.total_pages DIMENSIONS authors.author_name`
  - `... DIMENSIONS authors.country` (book 1 counted once for FR)
  - `METRICS books.total_pages` (total unchanged), `METRICS authors.total_revenue DIMENSIONS books.category`
  - `DIMENSIONS books.category, authors.country METRICS books.total_pages` (local + bridged)
  - `DIMENSIONS authors.country, genres.genre_name METRICS books.total_pages` (two bridge groups)
  - derived `pages_per_revenue AS books.total_pages / authors.total_revenue` by `authors.country` (two blocks)
  - error: `DIMENSIONS book_authors.role METRICS books.total_pages` → `must be related to and have an equal or lower level of granularity`
  - error: `authors.author_name || genres.genre_name` → `combines tables reached through different many-to-many bridges`
- [ ] **Step 2:** `GEN=ninja make && ./build/release/test/unittest test/sql/semantic_views/many_to_many.test` → FAIL (granularity error).
- [ ] **Step 3: Implement** `semantic_bridge.cpp` (algorithm of spec 1.2: candidates `B ∉ reach(H)`, `T ⊆ reach(B)`,
  finest anchor of `reach(B) ∩ reach(H)`, divergence on first edges, no ASOF/range, USING filter, ambiguity error).
  In `MetricQueryCompiler::BuildBlock`: classify each dimension (`RowTables` → local / bridged / invalid); group
  bridged dimensions by `(bridge, anchor)`; per group build
  `SELECT DISTINCT <key_source cols AS __sv_k<c>>, <dims AS __sv_d<d>> FROM <SemanticJoinScope at B, nested prefix>`,
  and join it (`LEFT`) to the block's FROM on `ColumnReference(anchor, anchor_key[c]) = map.__sv_k<c>`; bridged
  dimensions in the row select become `map.__sv_d<d>`.
- [ ] **Step 4:** rerun the test → PASS; run `./build/release/test/unittest "test/sql/*"` → all pass.
- [ ] **Step 5:** `make format-fix`, commit `feat: many-to-many dimensions through bridge tables`.

### Task 2: WHERE through bridges, NON ADDITIVE BY guard, USING / ambiguity

**Files:** Modify `src/semantic_query_compiler.cpp`; Test: `many_to_many.test`

- [ ] **Step 1: Tests:**
  - `WHERE authors.country = 'FR'` without dims (book 1 once) and with `books.category`
  - `WHERE authors.active AND books.category = 'novel'` (one bridged + one local conjunct)
  - `WHERE authors.author_name IS NULL` → book 4 dropped (documented)
  - error: `WHERE authors.country = 'FR' OR books.category = 'novel'` → `combines tables` + hint to split
  - fact-table bridge: `customers.customer_count` by `products.category` through `sales` = `COUNT(DISTINCT customer)`
  - ambiguity: add `reviews(book_id, author_id)` relationships → `Ambiguous many-to-many path`; metric with
    `USING (book_authors_to_authors)` resolves
  - semi-additive metric on `books` by `authors.country` works; `NON ADDITIVE BY (authors.joined)` → `NON ADDITIVE BY dimension "authors.joined" cannot be reached through a many-to-many relationship`
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** split `spec.where` into top-level AND conjuncts; local → rows WHERE; bridged → their group's map WHERE
  (group join becomes `INNER`); otherwise throw. Non-additive entries checked for reachability before compiling.
- [ ] **Step 4:** tests pass, full suite green. **Step 5:** format, commit `feat: filters and USING through many-to-many bridges`.

### Task 3: Dimension-only queries and SHOW ... FOR METRIC

**Files:** Modify `src/semantic_query_compiler.cpp` (`CompileRowQuery`), `src/semantic_show_functions.cpp`; Test: `many_to_many.test`, `semantic_view_sql` golden in `many_to_many.test`

- [ ] **Step 1: Tests:** `DIMENSIONS books.category, authors.country` (co-occurring pairs);
  `SHOW SEMANTIC DIMENSIONS IN library FOR METRIC books.total_pages` includes `authors.*` dims, excludes
  `book_authors.role`; `semantic_view_sql(...)` contains `SELECT DISTINCT` and `book_authors`.
- [ ] **Step 2:** FAIL. **Step 3:** `CompileRowQuery`: when no referenced table reaches all (and no facts), use
  `SemanticBridgeResolver::CommonBase`; SHOW: a dimension is valid when every atom home reaches it or `Resolve` succeeds
  (ambiguity → not listed).
- [ ] **Step 4:** pass. **Step 5:** commit `feat: dimension-only queries and SHOW FOR METRIC through bridges`.

### Task 4: Benchmark q10 and documentation

**Files:** `benchmark/semantic_views/queries.sql` (+ hand-written counterpart in the same format as q1-q9),
`docs/benchmarks.md`, `README.md` (Advanced metrics bullet on many-to-many; README "How queries are evaluated" step)

- [ ] Add q10: customer metric by product category through the 100M-row `sales` table vs hand-written
  `SELECT DISTINCT` mapping query; run `python3 scripts/bench_semantic_views.py --db /tmp/semantic_views_bench.duckdb --rows 100000000 --runs 5`
  (reuse the existing DB if present); record the row. Commit `docs: many-to-many benchmark and README`.

### Task 5: Placeholder catalog views maintained by DDL

**Files:**
- Create: `src/include/semantic_catalog.hpp`, `src/semantic_catalog.cpp`
- Modify: `CMakeLists.txt`, `src/include/semantic_lowering.hpp` (`SEMANTIC_VIEW_PLACEHOLDER_FUNCTION`),
  `src/semantic_ddl_functions.cpp`, `src/semantic_views_functions.cpp` (register placeholder function)
- Test: `test/sql/semantic_views/catalog.test`; update `standard_sql.test` "shadowed" case

**Interfaces:**
```cpp
struct SemanticCatalogColumn { string name; LogicalType type; Value comment; };
vector<SemanticCatalogColumn> SemanticCatalogColumns(const SemanticModel &model);
//! Catalog that holds the placeholder of `name`, or nullptr (read-only / system catalog)
optional_ptr<Catalog> SemanticPlaceholderCatalog(ClientContext &context, const QualifiedName &name);
//! Throws when a table or view that is not a placeholder is named `name`; registers the DB modification on `binder`
void PrepareSemanticPlaceholder(ClientContext &context, Binder &binder, const QualifiedName &name, bool check_conflict);
void CreateSemanticPlaceholder(ClientContext &context, const SemanticModel &model);
bool DropSemanticPlaceholder(ClientContext &context, const QualifiedName &name);
bool IsSemanticPlaceholder(const CatalogEntry &entry);
void RegisterSemanticPlaceholderFunction(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);
```
- [ ] **Step 1: Tests:** `information_schema.tables` row (`VIEW`), `information_schema.columns` names/types/order,
  `duckdb_columns()` comments, `duckdb_views()` comment, private members hidden, ambiguous names as `table.name`;
  conflict with a table (`a table or view with that name already exists`, also with `OR REPLACE`/`IF NOT EXISTS`);
  `IF NOT EXISTS` on existing semantic view leaves columns unchanged; `OR REPLACE` changes columns; `DROP SEMANTIC VIEW`
  removes the view; `ALTER RENAME` moves it (also in schema `analytics`); `SET COMMENT` updates `duckdb_views().comment`;
  `CREATE TABLE` with a semantic view's name fails; `SELECT * FROM sv` through placeholder fails with SELECT * error.
- [ ] **Step 2:** FAIL. **Step 3:** implement (bind: conflict check + `RegisterDBModify(CREATE_CATALOG_ENTRY|DROP_CATALOG_ENTRY)`;
  execute: `Catalog::CreateView` with `REPLACE_ON_CONFLICT`, filled `types/names/column_comments_map/comment`;
  `DropEntry` with `if_not_found = RETURN_NULL`). `Execute()` gains a `ClientContext &` parameter.
- [ ] **Step 4:** pass; update `standard_sql.test` shadowed case to expect the conflict error; full suite.
- [ ] **Step 5:** commit `feat: semantic views appear as views in the catalog`.

### Task 6: Query by name and DESCRIBE through placeholders

**Files:** `src/semantic_sql_rewrite.cpp`, `src/include/semantic_statement_visitor.hpp`, `src/semantic_statement_visitor.cpp`, `src/semantic_catalog.cpp`; Test: `catalog.test`

- [ ] **Step 1: Tests:** `SELECT "orders.revenue", ... FROM sv GROUP BY ...` through the placeholder; `DESCRIBE sv`
  returns placeholder columns/types; `SUMMARIZE sv` errors; placeholder error message when rewrite does not apply.
- [ ] **Step 2:** FAIL. **Step 3:** `SemanticViewFor` accepts placeholder entries; visitor gets
  `virtual bool VisitShowQuery(ShowRef &)` called before the children of a SHOW_REF (true = handled); the rewriter
  replaces `DESCRIBE` of `SELECT * FROM <sv>` with `SemanticDescribeNode(model)`
  (`SELECT CAST(NULL AS t) AS "c", ... LIMIT 0`).
- [ ] **Step 4:** pass. **Step 5:** commit `feat: query and DESCRIBE semantic views through their catalog view`.

### Task 7: Read-only catalogs, leftovers after restart, docs

**Files:** `src/semantic_catalog.cpp`, `catalog.test`, `README.md`

- [ ] **Step 1: Tests:** `ATTACH '__TEST_DIR__/ro.db' (READ_ONLY)` + semantic view over it → works, not listed;
  file DB: create semantic view, `restart`, `information_schema.tables` still lists it, query fails with
  `is not defined in this session`, `CREATE SEMANTIC VIEW` (no OR REPLACE) succeeds; `DROP SEMANTIC VIEW` on a leftover succeeds.
- [ ] **Step 2-4:** implement leftover handling in CREATE (placeholder replaced silently) / DROP (success message);
  placeholder bind message per spec 2.1. **Step 5:** README (Statements note, Limitations: placeholders, name conflicts,
  plain `DROP VIEW`), commit `feat: placeholder lifecycle across restarts; docs`.

### Final

- [ ] `make format-check`, full test suite, whole-branch review (`superpowers:requesting-code-review`).
