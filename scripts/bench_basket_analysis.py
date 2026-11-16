#!/usr/bin/env python3
"""Compare list and CTE formulations of manufacturer-pair lift on TPC-H SF1.

Usage: python3 scripts/bench_basket_analysis.py --dir /tmp/basket_bench --runs 9

Uses the release CLI, checks both results against physical SQL, then records
alternating warm runs with four threads. No timing threshold is asserted.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import statistics
from pathlib import Path

from bench_semantic_layer import ROOT, run_cli, same_rows

BENCH_DIR = ROOT / "benchmark" / "basket_analysis"
COLUMNS = ("manufacturer_a", "manufacturer_b", "orders_with_both", "lift")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", required=True)
    parser.add_argument("--runs", type=int, default=9)
    parser.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    output_dir = Path(args.dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    db = str(output_dir / "tpch.duckdb")
    if not Path(db).exists():
        print("Generating TPC-H SF1...", flush=True)
        run_cli(args.duckdb, db, ".bail on\nLOAD tpch;\nCALL dbgen(sf := 1);\nCHECKPOINT;\n")

    queries = dict(
        block.split("\n", 1)
        for block in re.split(r"^-- name: ", (BENCH_DIR / "queries.sql").read_text(), flags=re.M)[1:]
    )
    setup = ".bail on\n.mode trash\nSET threads = 4;\n" + (BENCH_DIR / "semantic_view.sql").read_text()
    rows = {}
    for name, query in queries.items():
        output = run_cli(args.duckdb, db, setup + "\n.mode csv\n" + query)
        (output_dir / f"{name}.csv").write_text(output)
        rows[name] = sorted([row[column] for column in COLUMNS] for row in csv.DictReader(io.StringIO(output)))
    for name in ("list", "cte"):
        if not same_rows(rows[name], rows["physical"]):
            raise SystemExit(f"{name}: result differs from physical SQL")
    print(f"Both formulations match physical SQL: {len(rows['physical'])} rows.", flush=True)

    # Warm both queries in the timing connection, then alternate which runs first.
    script = setup + "\n" + queries["list"] + queries["cte"]
    for run in range(args.runs):
        for name in ("list", "cte") if run % 2 == 0 else ("cte", "list"):
            script += f"\n.print TIME:{name}\n.timer on\n{queries[name]}\n.timer off\n"
    (output_dir / "timing.sql").write_text(script)
    output = run_cli(args.duckdb, db, script)
    (output_dir / "timing.log").write_text(output)
    samples = {
        name: [float(value) for value in re.findall(rf"TIME:{name}\nRun Time \(s\): real ([0-9.]+)", output)]
        for name in ("list", "cte")
    }
    for name, times in samples.items():
        if len(times) != args.runs:
            raise SystemExit(f"{name}: expected {args.runs} timings, got {len(times)}")
    medians = {name: statistics.median(times) for name, times in samples.items()}
    version = run_cli(
        args.duckdb,
        db,
        ".mode csv\nSELECT version() AS duckdb, extension_version "
        "FROM duckdb_extensions() WHERE extension_name = 'semantic_layer';\n",
    )
    result = {
        "runtime": next(csv.DictReader(io.StringIO(version))),
        "threads": 4,
        "dataset": "TPC-H SF1",
        "result_rows": len(rows["physical"]),
        "samples_seconds": samples,
        "median_seconds": medians,
    }
    (output_dir / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    for name, median in medians.items():
        print(f"{name}: {median * 1000:.3f} ms median")
    print(f"Artifacts: {output_dir}")


if __name__ == "__main__":
    main()
