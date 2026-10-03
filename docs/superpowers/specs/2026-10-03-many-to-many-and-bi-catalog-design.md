# Many-to-many relationships and BI catalog visibility — Design

Date: 2026-10-03
Status: approved and implemented

Two independent features, built in this order:

1. **Many-to-many through bridge tables** (Snowflake parity): a metric can be grouped and filtered by dimensions
   reached through a bridge table, without double counting.
2. **BI catalog visibility**: every semantic view appears as a view in DuckDB's catalog (`information_schema`,
   `duckdb_views()`, `duckdb_columns()`, JDBC/ODBC metadata), with one typed, commented column per public dimension,
   fact and metric, so BI tools can discover and query it by name.

### Decisions taken with the user

| Topic | Decision |
|---|---|
| Which tables act as bridges | Any table with relationships to both sides (`book_authors`, but also a fact table such as `sales` between `customers` and `products`). A dimension stored on the bridge itself stays rejected. |
| Persistence | Not in scope. Semantic views remain in-memory; catalog placeholders left in a database file after a restart are inert (section 2.4). |
| Aggregates over metrics in the query-by-name form | Snowflake rules: metrics must be wrapped in `AGG(...)`; `SUM(metric)` keeps failing with an error. |

## 1. Many-to-many relationships

### 1.1 Semantics

Snowflake documents the feature only by example ("the system infers the many-to-many path from the relationship graph
and automatically handles deduplication to prevent fanout double-counting"):

```sql
CREATE SEMANTIC VIEW library_sv
  TABLES (
    authors      AS authors      PRIMARY KEY (author_id),
    books        AS books        PRIMARY KEY (book_id),
    book_authors AS book_authors PRIMARY KEY (book_id, author_id)
  )
  RELATIONSHIPS (
    book_authors (author_id) REFERENCES authors,
    book_authors (book_id)   REFERENCES books
  )
  DIMENSIONS (books.category AS category, authors.author_name AS author_name, authors.country AS country)
  METRICS (books.total_pages AS SUM(page_count), authors.total_revenue AS SUM(author_revenue));
```

We define the semantics as follows (the usual "bridge" semantics of BI tools):

- `METRICS books.total_pages DIMENSIONS authors.author_name`: for every author, the pages of the books they wrote.
  A book with two authors counts once for each of them, so the groups add up to more than the total. That is expected
  for many-to-many.
- **No double counting within a group.** `DIMENSIONS authors.country`: a book written by two French authors counts
  **once** for `FR`.
- Totals without bridged dimensions are unchanged: every book counts once.
- **Filters are semi-joins.** `WHERE authors.country = 'FR'` without dimensions: every book with at least one French
  author, counted once.
- Rows of the metric's table without any bridge row (a book without authors) get NULL bridged dimensions, like rows with
  a dangling foreign key today. A filter on bridged tables drops them, even `authors.author_name IS NULL`. This is a
  documented limitation.
- The same holds in the other direction (`authors.total_revenue` by `books.category`) and for fact tables used as
  bridges: `customers.customer_count` by `products.category` through `sales` counts the customers who bought in each
  category.

### 1.2 Bridge resolution

Notation: `reach(X)` = tables reachable from `X` through many-to-one / one-to-one edges, including `X`
(`SemanticModel::Reaches`). For an aggregation block with home table `H` (section 6.4 of the main design), every
row-level item (requested dimension, or top-level `AND` conjunct of `WHERE`) with logical tables `T` (from
`SemanticAnalyzer::RowTables`) is classified as:

- **local** when `T ⊆ reach(H)`: compiled exactly as today;
- **bridged** when a bridge `B` is found for it (below);
- otherwise **invalid**, with today's Snowflake error (`The dimension entity 'X' must be related to and have an equal or
  lower level of granularity ...`). This keeps rejecting dimensions stored on the bridge itself, such as
  `customers.customer_count` by `orders.order_year`.

`B` is a **bridge candidate** for an item when all of the following hold:

1. `B ∉ reach(H)`, and every table of `T` is in `reach(B)`.
2. `reach(B) ∩ reach(H)` has a *finest* table `P`: one from which every other table of the intersection is
   reachable. `P` is the **anchor**: rows of `H` and `B` are related when they reach the same row of `P`.
3. The paths diverge at `B`: for every `t ∈ T \ reach(H)`, the first edge of `Path(B, t)` differs from the first edge of
   `Path(B, P)`. This excludes `B` itself (a dimension on the bridge) and tables "behind" the anchor side. For example,
   `lineitem` is not a bridge from `customers` to `orders`, because it reaches both through `orders`.
4. No ASOF or range relationship on `Path(B, P)` or on the first edges of the paths to `T`.

When several candidates remain:

- if the metric's `USING (...)` names a relationship whose `from_table` is one of the candidates, keep only those;
- if still more than one candidate remains, fail:
  `Ambiguous many-to-many path from "H" to "X": bridge tables "B1", "B2" both qualify; specify USING (...) on the metric`.

Bridged items that resolve to the same `(B, P)` form one **bridge group**. An item that needs two different bridges (for
example `authors.author_name || genres.name`) fails with
`Invalid dimension "...": combines tables reached through different many-to-many bridges ("B1", "B2")`. A
`WHERE` conjunct that cannot be classified fails the same way, with a hint to split the condition into separate
`AND`ed conditions.

### 1.3 SQL shape

Each bridge group becomes a de-duplicated mapping subquery that is joined into the block's row subquery:

```sql
-- block for home H = books, query DIMENSIONS authors.country METRICS books.total_pages WHERE authors.active
SELECT __sv_d0, sum(__sv_x0) AS __sv_a0
FROM (
  SELECT "__sv1_map"."__sv_d0" AS __sv_d0, "books"."page_count" AS __sv_x0
  FROM books AS "books"
  INNER JOIN (                                   -- LEFT JOIN when the group has no WHERE conjuncts
    SELECT DISTINCT "__sv1_book_authors"."book_id" AS __sv_k0,      -- anchor key of P = books
                    "__sv1_authors"."country"      AS __sv_d0
    FROM book_authors AS "__sv1_book_authors"
    LEFT JOIN authors AS "__sv1_authors" ON "__sv1_book_authors"."author_id" = "__sv1_authors"."author_id"
    WHERE "__sv1_authors"."active"                                   -- the group's WHERE conjuncts
  ) AS "__sv1_map" ON "books"."book_id" = "__sv1_map"."__sv_k0"
) AS __sv_rows
GROUP BY ALL
```

- The mapping subquery has its own `SemanticJoinScope` based at `B`, with a nested alias prefix. Bridged dimensions and
  conjuncts compile in that scope (child aggregations and range lookups work as usual).
- **Anchor key:** the key columns of `P` as reached from `B`. These are the target columns of the last edge of
  `Path(B, P)`, or `from_columns` for a reversed one-to-one edge, so they are always a primary or unique key of `P`.
  On the `H` side, `P` is ensured in the block's scope along `Path(H, P)` and joined on the same key columns.
- `DISTINCT` on `(anchor key, bridged dimensions)` guarantees that each `H` row appears at most once per group of
  dimension values. That is the de-duplication. With several bridge groups, each `H` row appears once per combination,
  which is the correct cross-bridge semantics.
- The join is `LEFT` when the group has no conjuncts (keeps `H` rows without bridge rows, NULL dimensions) and `INNER`
  otherwise (semi-join).
- Everything else is unchanged: blocks are merged on dimension values, derived metrics are computed on top, window
  metrics run last.
- `NON ADDITIVE BY`: the last-snapshot window partitions by the block's dimension columns, which may now come from a
  mapping subquery. A `NON ADDITIVE BY` dimension is always local: `CREATE` already requires it to be reachable from
  the metric's table.

### 1.4 Dimensions-only queries

`DIMENSIONS books.category, authors.author_name` (no metrics, no facts): when no referenced table reaches all the others
(today's `FinestTable` error), the base is the unique table `B` that reaches all referenced tables, and whose paths to
them do not all share their first edge (same divergence rule; ambiguity fails as in 1.2). The query is
`SELECT DISTINCT dims FROM B LEFT JOIN ... WHERE filter`: combinations that co-occur through the bridge. Fact queries
are unchanged (all facts and dimensions on one table).

### 1.5 Introspection

`SHOW SEMANTIC DIMENSIONS IN v FOR METRIC m` also lists the dimensions that every atom home of `m` reaches through an
unambiguous bridge.

### 1.6 Out of scope

- Chains of bridges (`H → B1 → X → B2 → T`), and bridges reached through ASOF or range relationships.
- Facts or dimensions defined over a bridge (`books.author_list AS LISTAGG(authors.author_name)`); these keep failing as
  today.

## 2. BI catalog visibility

DuckDB's `information_schema` views and `duckdb_*()` functions read the catalog, and system catalog entries cannot be
replaced by extensions. *Internal* entries are hidden from those views, and *temporary* entries are only allowed in the
per-connection `temp` catalog. So the only way to be visible to every connection is a real, persistent catalog
entry.

### 2.1 Placeholder view

`CREATE SEMANTIC VIEW s.v` also creates a catalog view `s.v` in the same catalog and schema (`Catalog::CreateView`,
inside the statement's transaction; the statement registers the database as modified):

- body: `SELECT * FROM __semantic_view_placeholder('<catalog>', '<schema>', '<name>')`;
- `types` / `names` filled in, so the view is already bound and nothing is bound at creation;
- one column per **public** member, in definition clause order: facts, dimensions, metrics (each in definition order).
  The column name is the member name, or `table.name` when several public members share it (Snowflake's
  `SHOW COLUMNS` convention; the query-by-name form already resolves quoted `"table.name"`);
- column types: the members' probed types; column comments: the members' comments; view comment: the semantic view's
  comment.

The placeholder table function always fails at bind time, which hands the statement to the existing standard-SQL
rewrite (`OnPlanningError`):

- if the semantic view is registered: `Semantic view "v" cannot be read as a table here: query it with
  SEMANTIC_VIEW(...), or by name with dimensions in GROUP BY and metrics in AGG(...)`. This is normally superseded by
  the rewrite's own result or error; it shows where the rewrite does not run (e.g. `CREATE VIEW ... FROM v`).
- otherwise (a leftover, section 2.4): `Semantic view "v" is not defined in this session: semantic views are kept in
  memory only; run CREATE SEMANTIC VIEW again`.

### 2.2 Query-by-name rewrite

`SemanticReferenceRewriter::SemanticViewFor` currently gives precedence to any table or view of the same name. It now
treats a catalog entry that is a placeholder (a view whose query is exactly
`SELECT * FROM __semantic_view_placeholder(...)`) as the semantic view. A real table or view of the same name cannot
exist any more (2.3).

`DESCRIBE v` binds `SELECT * FROM v` and fails like any other query. The rewrite replaces the `ShowRef` query of a
`DESCRIBE` over a semantic view with `SELECT CAST(NULL AS <type>) AS "<column>", ... LIMIT 0`, so `DESCRIBE` reports the
placeholder's columns. `SELECT *` itself stays rejected (Snowflake rule).

### 2.3 DDL behavior

| Statement | Catalog effect |
|---|---|
| `CREATE SEMANTIC VIEW v` | creates the placeholder. A real table or view named `v` → `Cannot create semantic view "v": a table or view with that name already exists` (with or without `OR REPLACE` / `IF NOT EXISTS`). A leftover placeholder is replaced silently. |
| `CREATE OR REPLACE` / `OR ALTER` | replaces the placeholder (new columns). |
| `DROP SEMANTIC VIEW v` | drops the placeholder. Dropping a leftover placeholder (not registered) succeeds. |
| `ALTER ... RENAME TO w` | drops `v`'s placeholder, creates `w`'s (same conflict rule). |
| `ALTER ... SET / UNSET COMMENT` | recreates the placeholder with the new view comment. |
| `DROP VIEW v` (plain) | drops only the placeholder: the semantic view stays queryable but is no longer listed. Documented. |

- **Read-only catalogs:** no placeholder is created. The semantic view works but is not listed.
- **Ordering:** every DDL statement changes the placeholder before the registry, so a statement that fails (e.g. a
  catalog write-write conflict with another connection) leaves the registry untouched.
- **Transactions:** placeholders are transactional, the registry is not (unchanged limitation). After a rolled-back
  `CREATE`, the semantic view stays registered without a placeholder.
- `CREATE TABLE v` while semantic view `v` exists fails with DuckDB's normal "already exists" error.
- Breaking change: a semantic view can no longer share its name with a table or view (`standard_sql.test` "a real
  table with the same name wins" case becomes a conflict error test). README updated.

### 2.4 Database files and restarts

In a file-backed database the placeholder is written to the file like any view. After a restart (definitions are not
persisted), it stays listed in `information_schema` but fails with the "not defined in this session" message, and
`CREATE SEMANTIC VIEW v` replaces it. Without the extension loaded, querying it fails with "table function
`__semantic_view_placeholder` does not exist". Documented in README Limitations.

## 3. Testing

- `test/sql/semantic_views/many_to_many.test`, library data with deliberate traps: a book with two authors from the
  same country, a book without authors, an author without books, an inactive author. Every semantic query is compared
  with a hand-written query:
  - metric by bridged dimension, both directions; de-duplication by `country`; totals unchanged;
  - `WHERE` on bridged tables with and without dimensions; mixed local and bridged conjuncts;
  - two bridge groups in one query (authors and genres);
  - derived metric across `books` and `authors` blocks;
  - fact table as bridge (`customers` by `products.category` through `sales`);
  - dimensions-only across the bridge;
  - errors: dimension on the bridge itself (unchanged message), ambiguous bridges and `USING` resolution, item mixing
    two bridges, unsplittable `WHERE` condition, bridged `NON ADDITIVE BY` dimension;
  - `SHOW ... FOR METRIC` lists bridged dimensions;
  - `semantic_view_sql()` golden output showing the `DISTINCT` mapping subquery.
- `test/sql/semantic_views/catalog.test`: `information_schema.tables` / `columns`, `duckdb_views()`, `duckdb_columns()`
  (names, types, comments, ambiguous `table.name` columns, private members hidden); query by name through the
  placeholder; `DESCRIBE v`; conflict errors; `CREATE OR REPLACE` / `DROP` / `RENAME` / `SET COMMENT` effects;
  `DROP VIEW` on a placeholder; read-only attached database; leftover placeholder after restart (file database,
  `restart` in sqllogictest), then `CREATE` replaces it.
- Benchmark: add `q10_many_to_many` (customer metric by product category through the 100M-row `sales` bridge) against
  the equivalent hand-written `DISTINCT` mapping query; record in `docs/benchmarks.md`.
