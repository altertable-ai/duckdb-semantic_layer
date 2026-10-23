# DuckDB semantic_views extension

Instructions for AI agents working in this repository.

## Architecture

- DuckDB extension implementing Snowflake-compatible semantic views
  (`CREATE SEMANTIC VIEW`, `SEMANTIC_VIEW(...)` queries, `SHOW` / `DESCRIBE`).
- Design overview: `README.md` ("How it works"). Local design notes and plans go in `docs/superpowers/`
  (git-ignored, like `.superpowers/` and `.cache/`).
- Parsing is a grammar extension of DuckDB's PEG parser (`src/parser/semantic_views_grammar.cpp`): rules added to
  DuckDB's grammar and transforms lowering the statements onto internal table functions. It is activated on every
  connection when the extension loads.
- Semantic queries are expanded at bind time into plain DuckDB SQL ASTs (no custom operators).
- Submodules:
  - DuckDB (`./duckdb`) — pinned to tag `v2.0.0`
  - Extension CI Tools (`./extension-ci-tools`) — branch `v2.0-cyanoptera`

**IMPORTANT**: Never modify files in `./duckdb` or `./extension-ci-tools` unless explicitly instructed.

## Build

- **ALWAYS use `GEN=ninja make` to compile this project** (release build).
- Debug builds are very slow; use them only when strictly necessary.
- DuckLake (needed by the materialization tests, which are skipped without it) is only linked in vcpkg builds:
  `VCPKG_TOOLCHAIN_PATH=<vcpkg>/scripts/buildsystems/vcpkg.cmake GEN=ninja make` (it needs `roaring`). Switching
  between vcpkg and non-vcpkg builds requires a fresh `build/release`.

## Coding style

- DuckDB coding style; `make format-check` / `make format-fix`.

## Testing

- SQL tests: `make test` or `./build/release/test/unittest "test/sql/*"`
- Single test: `./build/release/test/unittest test/sql/semantic_views/<file>.test`

## Grammar

- Rules are plain strings in `SEMANTIC_VIEW_RULES`, one PEG rule per entry, reusing DuckDB's rules (`Expression`,
  `QualifiedName`, `ColId`, ...). Each transform reads its rule's children by position: keep the comment above it in
  sync with the rule.
- New result types of transforms must be registered with `DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE`.
- The grammar changes `Statement`, `ExplainableStatements`, `InnerTableRef` and `WindowPartition`, so it is active for
  every query: after a DuckDB bump, also run DuckDB's own parser tests with the extension loaded.
