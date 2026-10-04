#!/usr/bin/env python3
"""Benchmark SEMANTIC_VIEW queries against equivalent hand-written SQL.

Usage:
    python3 scripts/bench_semantic_views.py --db /path/to/bench.duckdb [--rows 100000000] [--runs 5]

The database is generated on first use (benchmark/semantic_views/generate.sql).
For every query pair in benchmark/semantic_views/queries.sql the script checks that
both queries return the same rows (numeric tolerance 1e-9 relative) and reports the
median wall time of each, as measured by the DuckDB CLI's `.timer`.
"""

from __future__ import annotations

import argparse
import csv
import io
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCH_DIR = ROOT / "benchmark" / "semantic_views"


def run_cli(duckdb: str, db: str, script: str, env: dict | None = None) -> str:
    result = subprocess.run(
        [duckdb, db], input=script, capture_output=True, text=True, env={**os.environ, **(env or {})}
    )
    if result.returncode != 0 or "Error" in result.stderr:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"duckdb failed (exit {result.returncode})")
    return result.stdout


def parse_queries(path: Path = BENCH_DIR / "queries.sql") -> list[tuple[str, str, str]]:
    text = path.read_text()
    pairs = []
    for block in re.split(r"^-- name: ", text, flags=re.M)[1:]:
        name, rest = block.split("\n", 1)
        semantic = rest.split("-- semantic\n", 1)[1].split("-- handwritten\n", 1)[0].strip()
        handwritten = rest.split("-- handwritten\n", 1)[1].strip()
        pairs.append((name.strip(), semantic, handwritten))
    return pairs


def as_rows(csv_text: str) -> list[list[str]]:
    rows = list(csv.reader(io.StringIO(csv_text)))
    return sorted(rows[1:])


def same_value(a: str, b: str) -> bool:
    if a == b:
        return True
    try:
        fa, fb = float(a), float(b)
    except ValueError:
        return False
    return abs(fa - fb) <= 1e-9 * max(1.0, abs(fa), abs(fb))


def same_rows(a: list[list[str]], b: list[list[str]]) -> bool:
    if len(a) != len(b):
        return False
    return all(len(ra) == len(rb) and all(same_value(x, y) for x, y in zip(ra, rb)) for ra, rb in zip(a, b))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", required=True, help="benchmark database file (generated when missing)")
    parser.add_argument("--rows", type=int, default=100_000_000, help="number of sales rows to generate")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    parser.add_argument("--only", help="run only the query pairs whose name contains this text")
    args = parser.parse_args()

    if not Path(args.db).exists():
        print(f"generating {args.rows:,} sales rows into {args.db} ...", flush=True)
        run_cli(args.duckdb, args.db, (BENCH_DIR / "generate.sql").read_text(), {"SV_SALES_ROWS": str(args.rows)})

    view_ddl = (BENCH_DIR / "semantic_view.sql").read_text()
    rows_in_db = run_cli(args.duckdb, args.db, "COPY (SELECT count(*) FROM sales) TO '/dev/stdout' (HEADER false);")
    print(f"sales rows: {int(rows_in_db.strip()):,}\n")
    print(f"{'query':32} {'semantic (s)':>13} {'handwritten (s)':>16} {'ratio':>7}  result")
    failures = 0
    for name, semantic, handwritten in parse_queries():
        if args.only and args.only not in name:
            continue
        # correctness
        check = ".mode trash\n" + view_ddl + "\n.mode csv\n" + f"{semantic};\n" + ".print __SV_SPLIT__\n"
        check += f"{handwritten};\n"
        out = run_cli(args.duckdb, args.db, check)
        sem_csv, hand_csv = out.split("__SV_SPLIT__\n", 1)
        equal = same_rows(as_rows(sem_csv), as_rows(hand_csv))
        # timing: alternate the two queries to share cache effects
        script = ".mode trash\n" + view_ddl + "\n.timer on\n"
        for _ in range(args.runs):
            script += f"{semantic};\n{handwritten};\n"
        timings = [float(t) for t in re.findall(r"Run Time \(s\): real ([0-9.]+)", run_cli(args.duckdb, args.db, script))]
        sem_times, hand_times = timings[0::2], timings[1::2]
        sem, hand = statistics.median(sem_times), statistics.median(hand_times)
        failures += 0 if equal else 1
        print(f"{name:32} {sem:13.3f} {hand:16.3f} {sem / max(hand, 1e-3):7.2f}  {'identical' if equal else 'DIFFERENT'}",
              flush=True)
    if failures:
        raise SystemExit(f"{failures} query pair(s) returned different results")


if __name__ == "__main__":
    main()
