#!/usr/bin/env bash
#
# Vendors DuckDB's PEG parser (the one that backs extension/autocomplete) into
# third_party/duckdb_peg_parser, wraps it in duckdb::semantic_views_peg, applies
# SEMANTIC VIEW grammar patches, and regenerates the inlined grammar.
#
# Usage: scripts/sync_duckdb_peg_parser.sh [path/to/extension/autocomplete]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_DIR="$(dirname "${SCRIPT_DIR}")"
SOURCE_DIR="${1:-${EXTENSION_DIR}/duckdb/extension/autocomplete}"
TARGET_DIR="${EXTENSION_DIR}/third_party/duckdb_peg_parser"
PATCH_DIR="${TARGET_DIR}/patches"
CUSTOM_DIR="${TARGET_DIR}/custom"
NAMESPACE="semantic_views_peg"
PYTHON="${PYTHON:-python3}"

if [ -x "${EXTENSION_DIR}/.venv/bin/python" ]; then
	PYTHON="${EXTENSION_DIR}/.venv/bin/python"
fi

if [ ! -d "${SOURCE_DIR}" ]; then
	echo "error: PEG parser source directory '${SOURCE_DIR}' does not exist" >&2
	exit 1
fi

# Preserve repo-owned files across the wipe/copy cycle.
KEEP_PATHS=(
	"README.md"
	"CMakeLists.txt"
	"patches"
	"custom"
)

TMP_KEEP="$(mktemp -d)"
cleanup() {
	rm -rf "${TMP_KEEP}"
}
trap cleanup EXIT

for keep in "${KEEP_PATHS[@]}"; do
	if [ -e "${TARGET_DIR}/${keep}" ]; then
		mkdir -p "${TMP_KEEP}/$(dirname "${keep}")"
		cp -R "${TARGET_DIR}/${keep}" "${TMP_KEEP}/${keep}"
	fi
done

rm -rf "${TARGET_DIR}"
mkdir -p "${TARGET_DIR}"

for keep in "${KEEP_PATHS[@]}"; do
	if [ -e "${TMP_KEEP}/${keep}" ]; then
		mkdir -p "${TARGET_DIR}/$(dirname "${keep}")"
		cp -R "${TMP_KEEP}/${keep}" "${TARGET_DIR}/${keep}"
	fi
done

PEG_PATHS=(
	"grammar"
	"include/ast"
	"include/parser"
	"include/transformer"
	"include/inlined_grammar.gram"
	"include/inlined_grammar.hpp"
	"include/keyword_helper.hpp"
	"include/matcher.hpp"
	"include/tokenizer.hpp"
	"parser"
	"transformer"
	"matcher.cpp"
	"tokenizer.cpp"
	"keyword_helper.cpp"
	"keyword_map.cpp"
	"inline_grammar.py"
)

for path in "${PEG_PATHS[@]}"; do
	if [ ! -e "${SOURCE_DIR}/${path}" ]; then
		echo "warning: '${path}' does not exist in '${SOURCE_DIR}' - skipping" >&2
		continue
	fi
	mkdir -p "${TARGET_DIR}/$(dirname "${path}")"
	rm -rf "${TARGET_DIR}/${path}"
	cp -R "${SOURCE_DIR}/${path}" "${TARGET_DIR}/${path}"
done

"${PYTHON}" "${SCRIPT_DIR}/wrap_duckdb_peg_namespace.py" "${TARGET_DIR}"

# Overlay SEMANTIC VIEW-specific sources (grammar + transformers) kept under custom/.
if [ -d "${CUSTOM_DIR}" ]; then
	cp -R "${CUSTOM_DIR}/." "${TARGET_DIR}/"
	echo "Copied custom SEMANTIC VIEW PEG sources from ${CUSTOM_DIR}"
fi

if compgen -G "${PATCH_DIR}/*.patch" >/dev/null; then
	(
		cd "${TARGET_DIR}"
		for patch in patches/*.patch; do
			echo "Applying ${patch}"
			patch -p1 <"${patch}"
		done
	)
else
	echo "No PEG parser patches found in ${PATCH_DIR}"
fi

if [ "${SEMANTIC_VIEWS_PEG_REGENERATE:-1}" != "0" ]; then
	bash "${SCRIPT_DIR}/regenerate_duckdb_peg_parser.sh"
fi

BASE_COMMIT="$(git -C "${EXTENSION_DIR}/duckdb" rev-parse HEAD 2>/dev/null || echo "unknown")"
VENDORED_FILES="$(cd "${TARGET_DIR}" && find . -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.gram' \) ! -path './custom/*' ! -path './patches/*' | wc -l | tr -d ' ')"
cat >"${TARGET_DIR}/DUCKDB_PEG_BASE" <<EOF
# Vendored DuckDB PEG parser - base revision
#
# source            duckdb/extension/autocomplete (PEG grammar, parser, transformer, matcher, tokenizer)
# source-repository https://github.com/duckdb/duckdb
# base-commit       ${BASE_COMMIT}
# vendored-files    ${VENDORED_FILES}
# namespace         duckdb::${NAMESPACE}
#
# Regenerate with scripts/sync_duckdb_peg_parser.sh.
# SEMANTIC VIEW additions live in custom/ (new files) and patches/ (edits to
# vendored files). Both are re-applied on every sync.
EOF

echo "Vendored the DuckDB PEG parser into ${TARGET_DIR} (base commit ${BASE_COMMIT})."
