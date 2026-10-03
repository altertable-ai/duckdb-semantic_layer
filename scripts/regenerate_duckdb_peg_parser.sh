#!/usr/bin/env bash
#
# Regenerates the inlined grammar of the vendored DuckDB PEG parser after the
# .gram files or keyword lists in third_party/duckdb_peg_parser/grammar have
# been edited.
#
# Usage: scripts/regenerate_duckdb_peg_parser.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_DIR="$(dirname "${SCRIPT_DIR}")"
PEG_DIR="${EXTENSION_DIR}/third_party/duckdb_peg_parser"
PYTHON="${PYTHON:-python3}"

if [ -x "${EXTENSION_DIR}/.venv/bin/python" ]; then
	PYTHON="${EXTENSION_DIR}/.venv/bin/python"
fi

if [ ! -f "${PEG_DIR}/inline_grammar.py" ]; then
	echo "error: ${PEG_DIR}/inline_grammar.py does not exist - run scripts/sync_duckdb_peg_parser.sh first" >&2
	exit 1
fi

(
	cd "${PEG_DIR}"
	"${PYTHON}" inline_grammar.py
	"${PYTHON}" inline_grammar.py --grammar-file
)

# Re-wrap generated headers into the semantic_views_peg namespace if the generator
# wrote them into the bare duckdb namespace.
"${PYTHON}" "${SCRIPT_DIR}/wrap_duckdb_peg_namespace.py" "${PEG_DIR}"

echo "Regenerated ${PEG_DIR}/include/inlined_grammar.hpp"
