#!/usr/bin/env python3
"""Benchmark materialization refreshes: full rebuild vs incremental refresh after small source changes.

Usage:
    python3 scripts/bench_materializations.py --dir /path/to/workdir [--rows 100000000]

Creates a DuckLake catalog in --dir holding a star schema (sales -> customers, sales -> products), a semantic view and
three identical materializations (revenue, units, counts and averages by customer segment and product category):
one refreshed FULL, one INCREMENTAL, one AUTO. For each change size (1 row, 1,000 rows, 100,000 rows, ~1% of the
rows) and change kind (updates, inserts, deletes, a dimension-table update), it applies the change, times the three
refreshes and checks that the materializations hold exactly the same state. It also checks the complete results of
a semantic query read from the materialization vs computed from the sources,
and reports median query times over alternating warm runs (settings changes are untimed).

Everything runs in one DuckDB CLI session (semantic views live in memory).
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def setup_script(rows: int, data_dir: str) -> str:
    catalog = (str(Path(data_dir) / "lake.ducklake")).replace("'", "''")
    data_path = (str(Path(data_dir) / "lake_data")).replace("'", "''")
    lines = [
        "LOAD ducklake;",
        f"ATTACH 'ducklake:{catalog}' AS lake (DATA_PATH '{data_path}');",
        "SET threads = 14;" if os.cpu_count() and os.cpu_count() >= 14 else "",
        f"SET VARIABLE sales_rows = {rows};",
        (ROOT / "benchmark" / "materializations" / "setup.sql").read_text(),
    ]
    return "\n".join(line for line in lines if line)


def build_script(rows: int, data_dir: str, runs: int = 7) -> str:
    customers = max(rows // 100, 1000)
    one_percent = max(rows // 100, 1)
    sizes = list(dict.fromkeys(min(size, rows) for size in (1, 1000, 100_000, one_percent)))
    lines = [
        setup_script(rows, data_dir),
        ".timer on",
        ".print MARK:initial_full_build",
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION full_copy REFRESH_MODE = FULL AS DIMENSIONS c.segment, p.category METRICS s.revenue, s.units, s.sale_count, s.avg_qty;",
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION incr REFRESH_MODE = INCREMENTAL AS DIMENSIONS c.segment, p.category METRICS s.revenue, s.units, s.sale_count, s.avg_qty;",
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION auto AS DIMENSIONS c.segment, p.category METRICS s.revenue, s.units, s.sale_count, s.avg_qty;",
        ".timer off",
        "SET VARIABLE incr_table = (SELECT tables[1] FROM (SHOW SEMANTIC MATERIALIZATIONS IN sales_sv) WHERE name = 'incr');",
        "SET VARIABLE full_table = (SELECT tables[1] FROM (SHOW SEMANTIC MATERIALIZATIONS IN sales_sv) WHERE name = 'full_copy');",
        "SET VARIABLE auto_table = (SELECT tables[1] FROM (SHOW SEMANTIC MATERIALIZATIONS IN sales_sv) WHERE name = 'auto');",
    ]
    query = "SELECT * FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment METRICS s.revenue, s.avg_qty) ORDER BY 1;"
    lines += [
        "CREATE TEMP TABLE query_cached AS " + query,
        "SET semantic_view_materializations = false;",
        "CREATE TEMP TABLE query_source AS " + query,
        "RESET semantic_view_materializations;",
        ".print MARK:check:query",
        "SELECT count(*) AS differences FROM ((SELECT * FROM query_cached EXCEPT ALL SELECT * FROM query_source) UNION ALL (SELECT * FROM query_source EXCEPT ALL SELECT * FROM query_cached));",
    ]
    # The correctness reads warm both paths. Alternate timing order and keep settings changes untimed.
    for run in range(runs):
        paths = [("materialization", "true"), ("sources", "false")]
        if run % 2:
            paths.reverse()
        for path, enabled in paths:
            lines += [
                f"SET semantic_view_materializations = {enabled};",
                f".print MARK:query_from_{path}",
                ".timer on",
                query,
                ".timer off",
            ]
    lines += [
        "RESET semantic_view_materializations;",
        ".timer on",
        ".print MARK:refresh_no_changes",
        "ALTER SEMANTIC VIEW sales_sv REFRESH MATERIALIZATION incr;",
    ]
    next_id = rows
    for size in sizes:
        changes = {
            "update": f"UPDATE lake.sales SET amount = amount + 1 WHERE id % {max(rows // size, 1)} = 0 AND id < {size * max(rows // size, 1)};",
            "insert": f"INSERT INTO lake.sales SELECT {next_id} + range, hash(range) % {customers}, hash(range * 7) % 10000, 1.00, 1 FROM range({size});",
            "delete": f"DELETE FROM lake.sales WHERE id >= {next_id} AND id < {next_id + size};",
        }
        next_id += size
        if size <= customers:
            changes["dimension_update"] = (
                f"UPDATE lake.customers SET segment = 'BUILDING' WHERE id % {max(customers // size, 1)} = 0 AND id < {size * max(customers // size, 1)};"
            )
        for kind, sql in changes.items():
            label = f"{kind}:{size}"
            lines += [
                ".timer off",
                sql,
                ".timer on",
                f".print MARK:full:{label}",
                "ALTER SEMANTIC VIEW sales_sv REFRESH MATERIALIZATION full_copy;",
                f".print MARK:incremental:{label}",
                "ALTER SEMANTIC VIEW sales_sv REFRESH MATERIALIZATION incr;",
                f".print MARK:auto:{label}",
                "ALTER SEMANTIC VIEW sales_sv REFRESH MATERIALIZATION auto;",
                ".timer off",
                f".print MARK:check:{label}",
                "SELECT count(*) AS differences FROM ((SELECT * FROM query_table(getvariable('incr_table')) EXCEPT ALL SELECT * FROM query_table(getvariable('full_table'))) UNION ALL (SELECT * FROM query_table(getvariable('full_table')) EXCEPT ALL SELECT * FROM query_table(getvariable('incr_table'))) UNION ALL (SELECT * FROM query_table(getvariable('auto_table')) EXCEPT ALL SELECT * FROM query_table(getvariable('full_table'))) UNION ALL (SELECT * FROM query_table(getvariable('full_table')) EXCEPT ALL SELECT * FROM query_table(getvariable('auto_table'))));",
            ]
    return "\n".join(line for line in lines if line) + "\n"


def parse(output: str) -> tuple[dict[str, float], dict[str, str]]:
    samples: dict[str, list[float]] = {}
    checks: dict[str, str] = {}
    mark = None
    for line in output.splitlines():
        if line.startswith("MARK:"):
            mark = line[5:].strip()
            continue
        if mark is None:
            continue
        time = re.match(r"Run Time \(s\): real ([0-9.]+)", line)
        if time and not mark.startswith("check:"):
            samples.setdefault(mark, []).append(float(time.group(1)))
        if mark.startswith("check:"):
            value = re.match(r"^│\s*(\d+)\s*│$", line.strip())
            if value:
                checks[mark[6:]] = value.group(1)
    timings = {
        key: statistics.median(values) if key.startswith("query_") else sum(values)
        for key, values in samples.items()
    }
    return timings, checks


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", required=True, help="work directory (a fresh DuckLake catalog is created there)")
    parser.add_argument("--rows", type=int, default=100_000_000, help="number of sales rows")
    parser.add_argument("--runs", type=int, default=7, help="timed query runs per path (median reported)")
    parser.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    args = parser.parse_args()
    if args.rows < 1 or args.runs < 1:
        parser.error("--rows and --runs must be positive")
    data_dir = os.path.abspath(args.dir)
    os.makedirs(data_dir, exist_ok=True)
    for name in ("lake.ducklake", "lake.ducklake.wal"):
        if os.path.exists(os.path.join(data_dir, name)):
            os.remove(os.path.join(data_dir, name))
    subprocess.run(["rm", "-rf", os.path.join(data_dir, "lake_data")], check=True)
    script = build_script(args.rows, data_dir, args.runs)
    Path(data_dir, "benchmark.sql").write_text(script)
    result = subprocess.run([args.duckdb, "-unsigned"], input=script, capture_output=True, text=True)
    Path(data_dir, "benchmark.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0 or "Error" in result.stderr:
        sys.stderr.write(result.stdout[-5000:] + result.stderr[-5000:])
        raise SystemExit("duckdb failed")
    timings, checks = parse(result.stdout)
    print(f"sales rows: {args.rows:,}; query timings: median of {args.runs} warm runs")
    if checks.get("query") != "0":
        raise SystemExit("cached and source query results differ, or the comparison is missing")
    for key in ("initial_full_build", "query_from_sources", "query_from_materialization", "refresh_no_changes"):
        print(f"{key:<30} {timings.get(key, float('nan')):>10.3f} s")
    print()
    print(f"{'change':<28} {'full (s)':>9} {'INCREMENTAL (s)':>16} {'speedup':>8} {'AUTO (s)':>9} {'speedup':>8} {'identical':>10}")
    for key in [k for k in timings if k.startswith("full:")]:
        label = key[5:]
        full = timings[key]
        incremental = timings.get("incremental:" + label, float("nan"))
        auto = timings.get("auto:" + label, float("nan"))
        identical = "yes" if checks.get(label) == "0" else f"NO ({checks.get(label)})"
        print(f"{label:<28} {full:>9.3f} {incremental:>16.3f} {full / incremental:>7.1f}x {auto:>9.3f} {full / auto:>7.1f}x {identical:>10}")

    if any(value != "0" for value in checks.values()) or any(
        key[5:] not in checks for key in timings if key.startswith("full:")
    ):
        raise SystemExit("materialized states differ, or a comparison is missing")


if __name__ == "__main__":
    main()
