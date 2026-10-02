#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${REPO}/duckdb/extension/autocomplete"
CUR="${REPO}/third_party/duckdb_peg_parser"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

DEST="${TMP}/clean"
mkdir -p "${DEST}"

PATHS=(
	"grammar/statements/select.gram"
	"grammar/statements/expression.gram"
	"grammar/keywords/unreserved_keyword.list"
	"grammar/keywords/reserved_keyword.list"
	"include/transformer/peg_transformer.hpp"
	"transformer/peg_transformer_factory.cpp"
	"transformer/transform_select.cpp"
	"transformer/transform_expression.cpp"
)

for path in "${PATHS[@]}"; do
	mkdir -p "${DEST}/$(dirname "${path}")"
	cp "${SOURCE}/${path}" "${DEST}/${path}"
done

python3 "${REPO}/scripts/wrap_duckdb_peg_namespace.py" "${DEST}"

PATCH="${CUR}/patches/0001-add-semantic-view-peg-grammar.patch"
mkdir -p "${CUR}/patches"
: >"${PATCH}"

for path in "${PATHS[@]}"; do
	diff -u "${DEST}/${path}" "${CUR}/${path}" |
		sed "s|${DEST}/|a/|g; s|${CUR}/|b/|g" >>"${PATCH}" || true
done

wc -l "${PATCH}"
echo "Wrote ${PATCH}"
