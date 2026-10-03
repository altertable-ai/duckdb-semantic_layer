#!/usr/bin/env python3
"""Wrap vendored DuckDB PEG parser sources in duckdb::semantic_views_peg."""

from __future__ import annotations

import argparse
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h"}
NAMESPACE = "semantic_views_peg"


def wrap_file(path: Path) -> bool:
	text = path.read_text()
	if f"namespace {NAMESPACE}" in text:
		return False
	if "namespace duckdb {" not in text:
		return False
	if "} // namespace duckdb" not in text:
		raise RuntimeError(f"Cannot safely wrap namespace in {path}: missing namespace close comment")

	# Avoid colliding with duckdb::QualifiedName after nesting the namespace.
	text = text.replace("struct QualifiedName;", "using duckdb::QualifiedName;")

	updated = text.replace(
		"namespace duckdb {\n",
		f"namespace duckdb {{\nnamespace {NAMESPACE} {{\n",
	)
	updated = updated.replace(
		"} // namespace duckdb",
		f"}} // namespace {NAMESPACE}\n}} // namespace duckdb",
	)
	if updated == text:
		return False
	path.write_text(updated)
	return True


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("root", type=Path, help="Vendored DuckDB PEG parser root")
	args = parser.parse_args()
	root = args.root.resolve()
	if not root.exists():
		raise SystemExit(f"Root does not exist: {root}")

	wrapped = 0
	for path in root.rglob("*"):
		if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
			continue
		if wrap_file(path):
			wrapped += 1
	print(f"Wrapped {wrapped} DuckDB PEG parser files in duckdb::{NAMESPACE}")


if __name__ == "__main__":
	main()
