#!/usr/bin/env python3
"""Compare handwritten SQL, semantic source queries, and semantic materialized queries.

Generates a pristine DuckLake star schema and one 250-group materialization, checks exact query results and
materialization selection, then reports medians of alternating warm runs. --dir must not contain lake.ducklake.
Generated SQL, raw CLI output, expanded queries, and timing samples are saved there for inspection.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

from bench_materializations import ROOT, setup_script
from bench_semantic_views import as_rows, parse_queries

QUERIES = ROOT / "benchmark" / "materializations" / "queries.sql"
PATHS = ("plain", "sources", "materialized")


def script_for(rows: int, data_dir: str, runs: int) -> str:
    lines = [
        ".bail on", ".mode trash", setup_script(rows, data_dir),
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION summary AS DIMENSIONS c.segment, p.category "
        "METRICS s.revenue, s.units, s.sale_count, s.avg_qty;",
    ]
    for name, semantic, plain in parse_queries(QUERIES):
        for path in PATHS:
            lines += [
                f"SET semantic_view_materializations = {'true' if path == 'materialized' else 'false'};",
                ".mode csv", f".print RESULT:{name}:{path}", (plain if path == "plain" else semantic) + ";",
            ]
            if path != "plain":
                literal = semantic.replace("'", "''")
                lines += [f".print PLAN:{name}:{path}", f"SELECT sql FROM semantic_view_sql('{literal}');"]
        lines += [".mode trash"]
        for run in range(runs):
            # Rotate path order to balance cache effects. Correctness reads above warm all three paths.
            offset = run % len(PATHS)
            for path in PATHS[offset:] + PATHS[:offset]:
                lines += [
                    f"SET semantic_view_materializations = {'true' if path == 'materialized' else 'false'};",
                    f".print TIME:{name}:{path}", ".timer on", (plain if path == "plain" else semantic) + ";",
                    ".timer off",
                ]
    return "\n".join(lines) + "\n"


def parse_blocks(output: str) -> dict[str, list[str]]:
    blocks: dict[str, list[str]] = {}
    mark = None
    for line in output.splitlines():
        if re.match(r"^(RESULT|PLAN|TIME):q\d+_", line):
            mark = line
            blocks.setdefault(mark, [])
        elif mark:
            blocks[mark].append(line)
    return blocks


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", required=True)
    parser.add_argument("--rows", type=int, default=100_000_000)
    parser.add_argument("--runs", type=int, default=11)
    parser.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    args = parser.parse_args()
    if args.rows < 1 or args.runs < 1:
        parser.error("--rows and --runs must be positive")
    output_dir = Path(args.dir).resolve()
    if (output_dir / "lake.ducklake").exists():
        parser.error("--dir already contains a DuckLake catalog; use a fresh directory")
    output_dir.mkdir(parents=True, exist_ok=True)
    script = script_for(args.rows, str(output_dir), args.runs)
    (output_dir / "benchmark.sql").write_text(script)
    print(f"Generating {args.rows:,} rows and benchmarking {len(parse_queries(QUERIES))} queries...", flush=True)
    with (output_dir / "benchmark.log").open("w") as log:
        result = subprocess.run([args.duckdb, "-unsigned"], input=script, stdout=log, stderr=subprocess.STDOUT, text=True)
    output = (output_dir / "benchmark.log").read_text()
    if result.returncode:
        sys.stderr.write(output[-5000:])
        raise SystemExit("DuckDB failed; see benchmark.log")
    blocks = parse_blocks(output)
    results = []
    plans = []
    print(f"\nMedian of {args.runs} warm runs; timings include semantic binding and freshness checks.")
    print(f"{'query':32} {'plain ms':>10} {'sources ms':>12} {'cached ms':>11} {'speedup':>9} {'cache used':>11}")
    for name, semantic, plain in parse_queries(QUERIES):
        rows = {path: as_rows("\n".join(blocks[f"RESULT:{name}:{path}"])) for path in PATHS}
        if rows["plain"] != rows["sources"] or rows["sources"] != rows["materialized"]:
            raise SystemExit(f"{name}: query results differ")
        record = {"query": name, "result_rows": len(rows["plain"]), "samples": {}, "median_seconds": {}}
        for path in PATHS:
            times = [float(value) for value in re.findall(
                r"Run Time \(s\): real ([0-9.]+)", "\n".join(blocks[f"TIME:{name}:{path}"])
            )]
            if len(times) != args.runs:
                raise SystemExit(f"{name}/{path}: expected {args.runs} samples, got {len(times)}")
            record["samples"][path] = times
            record["median_seconds"][path] = statistics.median(times)
        expanded = {}
        for path in PATHS[1:]:
            csv_rows = list(csv.reader(io.StringIO("\n".join(blocks[f"PLAN:{name}:{path}"]))))
            expanded[path] = csv_rows[1][0]
        cache_used = "__semantic_materializations" in expanded["materialized"]
        expected_cache = name != "q6_source_row_filter"
        if cache_used != expected_cache or "__semantic_materializations" in expanded["sources"]:
            raise SystemExit(f"{name}: unexpected materialization selection")
        record["cache_used"] = cache_used
        record["speedup"] = record["median_seconds"]["sources"] / record["median_seconds"]["materialized"]
        results.append(record)
        plans += [f"-- {name}: handwritten\n{plain};", f"-- {name}: semantic\n{semantic};"]
        plans += [f"-- {name}: {path}\n{expanded[path]};" for path in PATHS[1:]]
        ms = record["median_seconds"]
        print(f"{name:32} {ms['plain'] * 1000:10.0f} {ms['sources'] * 1000:12.0f} "
              f"{ms['materialized'] * 1000:11.0f} {record['speedup']:8.1f}x {str(cache_used):>11}")
    (output_dir / "results.json").write_text(json.dumps({"sales_rows": args.rows, "runs": args.runs, "queries": results}, indent=2) + "\n")
    (output_dir / "expanded_queries.sql").write_text("\n\n".join(plans) + "\n")
    print(f"All results identical. Artifacts: {output_dir}")


if __name__ == "__main__":
    main()
