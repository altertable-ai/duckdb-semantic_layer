# DuckDB semantic_layer extension

Instructions for AI agents working in this repository.

## Architecture

- DuckDB extension implementing Snowflake-compatible semantic views
  (`CREATE SEMANTIC VIEW`, `SEMANTIC_VIEW(...)` queries, `SHOW` / `DESCRIBE`).
- Design overview: `README.md` ("How it works"). Local design notes and plans go in `docs/superpowers/`
  (git-ignored, like `.superpowers/` and `.cache/`).
- Parsing uses a vendored copy of DuckDB's PEG parser (`third_party/duckdb_peg_parser`, namespace
  `duckdb::semantic_views_peg`), reached through `parse_function` when the postgres parser fails.
- Semantic queries are expanded at bind time into plain DuckDB SQL ASTs (no custom operators).
- Submodules:
  - DuckDB (`./duckdb`) — pinned to tag `v1.5.5`
  - Extension CI Tools (`./extension-ci-tools`) — branch `v1.5.5`

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
- Single test: `./build/release/test/unittest test/sql/semantic_layer/<file>.test`

## PEG parser maintenance

- `scripts/sync_duckdb_peg_parser.sh` — re-vendor from `duckdb/extension/autocomplete`, apply `custom/` + `patches/`.
- `scripts/regenerate_duckdb_peg_parser.sh` — after editing `.gram` files or keyword lists.
- `scripts/make_peg_patch.sh` — after editing patched vendored files, refresh the patch.
