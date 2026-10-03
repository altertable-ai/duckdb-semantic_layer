# Vendored DuckDB PEG parser

This directory holds a vendored copy of the PEG parser that lives in DuckDB's
`extension/autocomplete`, namespaced as `duckdb::semantic_views_peg` and
patched with semantic-view grammar and transformers.

The integration mirrors [DuckPGQ](https://github.com/cwida/duckpgq-extension):
`parser_override` runs the full PEG parser and returns native DuckDB AST nodes.

## Layout

```
duckdb_peg_parser/
  DUCKDB_PEG_BASE          # upstream source + commit marker
  CMakeLists.txt           # adds vendored .cpp files to EXTENSION_SOURCES
  custom/                  # MATCH_RECOGNIZE-only sources overlaid on every sync
    grammar/statements/match_recognize.gram
    transformer/transform_match_recognize.cpp
  patches/                 # edits to vendored files, re-applied on every sync
    0001-add-match-recognize-peg-grammar.patch
  grammar/                 # .gram files + keyword lists
  include/                 # matcher, tokenizer, transformer headers, inlined grammar
  parser/ transformer/     # PEG engine + statement transformers
  matcher.cpp tokenizer.cpp keyword_*.cpp
  inline_grammar.py
```

## Sync / regenerate

`DUCKDB_PEG_BASE` records the DuckDB submodule commit the vendor was synced
from. Sync against the supported DuckDB tag pinned by this repo
(`duckdb` submodule / CI tag `v1.5.5`), not an arbitrary newer tip:

```bash
# Re-vendor from duckdb/extension/autocomplete, apply custom/ + patches/, regenerate
scripts/sync_duckdb_peg_parser.sh

# After editing .gram files or keyword lists only:
scripts/regenerate_duckdb_peg_parser.sh

# After changing patched vendored files, refresh the patch:
scripts/make_peg_patch.sh
```

## semantic-view grammar

`MatchRecognizeClause` is an alternative of `JoinOrPivot` in `select.gram`, so
it attaches to the preceding relation like `PIVOT`. `MATCH_RECOGNIZE` is a
**reserved** keyword (same reason `PIVOT` is reserved): otherwise
`FROM t MATCH_RECOGNIZE (...)` would treat `MATCH_RECOGNIZE` as a table alias.

Hand-written transforms live in `transformer/transform_match_recognize.cpp`
and lower onto `match_recognize_internal((SELECT * FROM <source>), <spec_id>)`.
