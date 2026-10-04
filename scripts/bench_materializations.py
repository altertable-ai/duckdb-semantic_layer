#!/usr/bin/env python3
"""Benchmark materialization refreshes: full rebuild vs incremental refresh after small source changes.

Usage:
    python3 scripts/bench_materializations.py --dir /path/to/workdir [--rows 100000000]

Creates a DuckLake catalog in --dir holding a star schema (sales -> customers, sales -> products), a semantic view and
three identical materializations (revenue, units, counts and averages by customer segment and product category):
one refreshed FULL, one INCREMENTAL, one AUTO. For each change size (1 row, 1,000 rows, 100,000 rows, ~1% of the
rows) and change kind (updates, inserts, deletes, a dimension-table update), it applies the change, times the three
refreshes and checks that the materializations hold exactly the same state. It also times a semantic query read from the materialization
vs computed from the sources.

Everything runs in one DuckDB CLI session (semantic views live in memory).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def build_script(rows: int, data_dir: str) -> str:
    customers = max(rows // 100, 1000)
    one_percent = max(rows // 100, 1)
    sizes = [1, 1000, 100_000, one_percent]
    lines = [
        "LOAD ducklake;",
        f"ATTACH 'ducklake:{data_dir}/lake.ducklake' AS lake (DATA_PATH '{data_dir}/lake_data');",
        "SET threads = 14;" if os.cpu_count() and os.cpu_count() >= 14 else "",
        f"CREATE TABLE lake.customers AS SELECT range AS id, ['AUTOMOBILE','BUILDING','FURNITURE','HOUSEHOLD','MACHINERY'][(hash(range) % 5)::BIGINT + 1] AS segment FROM range({customers});",
        "CREATE TABLE lake.products AS SELECT range AS id, 'category_' || (range % 50) AS category FROM range(10000);",
        f"CREATE TABLE lake.sales AS SELECT range AS id, (hash(range) % {customers})::BIGINT AS customer_id, (hash(range * 7) % 10000)::BIGINT AS product_id, ((hash(range * 17) % 100000) / 100.0)::DECIMAL(12, 2) AS amount, (hash(range * 19) % 10 + 1)::INTEGER AS qty FROM range({rows});",
        """CREATE SEMANTIC VIEW sales_sv
  TABLES (s AS lake.sales PRIMARY KEY (id), c AS lake.customers PRIMARY KEY (id), p AS lake.products PRIMARY KEY (id))
  RELATIONSHIPS (s (customer_id) REFERENCES c, s (product_id) REFERENCES p)
  DIMENSIONS (c.segment AS segment, p.category AS category)
  METRICS (s.revenue AS SUM(amount), s.units AS SUM(qty), s.sale_count AS COUNT(*), s.avg_amount AS AVG(qty));""",
        ".timer on",
        ".print MARK:initial_full_build",
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION full_copy REFRESH_MODE = FULL AS DIMENSIONS c.segment, p.category METRICS s.revenue, s.units, s.sale_count, s.avg_amount;",
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION incr REFRESH_MODE = INCREMENTAL AS DIMENSIONS c.segment, p.category METRICS s.revenue, s.units, s.sale_count, s.avg_amount;",
        "ALTER SEMANTIC VIEW sales_sv ADD MATERIALIZATION auto AS DIMENSIONS c.segment, p.category METRICS s.revenue, s.units, s.sale_count, s.avg_amount;",
        ".print MARK:query_from_materialization",
        "SELECT count(*) FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment METRICS s.revenue, s.avg_amount);",
        "SET semantic_view_materializations = false;",
        ".print MARK:query_from_sources",
        "SELECT count(*) FROM SEMANTIC_VIEW(sales_sv DIMENSIONS c.segment METRICS s.revenue, s.avg_amount);",
        "RESET semantic_view_materializations;",
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
                "SELECT count(*) AS differences FROM ((SELECT * FROM __semantic_materializations.sales_sv__incr__0 EXCEPT ALL SELECT * FROM __semantic_materializations.sales_sv__full_copy__0) UNION ALL (SELECT * FROM __semantic_materializations.sales_sv__full_copy__0 EXCEPT ALL SELECT * FROM __semantic_materializations.sales_sv__incr__0) UNION ALL (SELECT * FROM __semantic_materializations.sales_sv__auto__0 EXCEPT ALL SELECT * FROM __semantic_materializations.sales_sv__full_copy__0) UNION ALL (SELECT * FROM __semantic_materializations.sales_sv__full_copy__0 EXCEPT ALL SELECT * FROM __semantic_materializations.sales_sv__auto__0));",
            ]
    return "\n".join(line for line in lines if line) + "\n"


def parse(output: str) -> tuple[dict[str, float], dict[str, str]]:
    timings: dict[str, float] = {}
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
            timings[mark] = timings.get(mark, 0.0) + float(time.group(1))
        if mark.startswith("check:"):
            value = re.match(r"^│\s*(\d+)\s*│$", line.strip())
            if value:
                checks[mark[6:]] = value.group(1)
    return timings, checks


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", required=True, help="work directory (a fresh DuckLake catalog is created there)")
    parser.add_argument("--rows", type=int, default=100_000_000, help="number of sales rows")
    parser.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    args = parser.parse_args()
    data_dir = os.path.abspath(args.dir)
    os.makedirs(data_dir, exist_ok=True)
    for name in ("lake.ducklake", "lake.ducklake.wal"):
        if os.path.exists(os.path.join(data_dir, name)):
            os.remove(os.path.join(data_dir, name))
    subprocess.run(["rm", "-rf", os.path.join(data_dir, "lake_data")], check=True)
    script = build_script(args.rows, data_dir)
    result = subprocess.run([args.duckdb, "-unsigned"], input=script, capture_output=True, text=True)
    if result.returncode != 0 or "Error" in result.stderr:
        sys.stderr.write(result.stdout[-5000:] + result.stderr[-5000:])
        raise SystemExit("duckdb failed")
    timings, checks = parse(result.stdout)
    print(f"sales rows: {args.rows:,}")
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


if __name__ == "__main__":
    main()
