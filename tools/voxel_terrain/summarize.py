"""Summarize the terrain upload benchmark's per-window CSV traces."""
import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


def percentiles(rows, key):
    ms = sorted(int(row[key]) / 1_000_000 for row in rows)
    return {
        "p50": round(statistics.median(ms), 4),
        "p95": round(ms[math.ceil(0.95 * len(ms)) - 1], 4),
    }


def summarize(path):
    groups = defaultdict(list)
    with path.open(newline="") as trace:
        for row in csv.DictReader(trace):
            groups[row["method"], "cold" if row["frame"] == "0" else "warm"].append(row)
    result = {}
    for (method, phase), rows in groups.items():
        totals = defaultdict(lambda: [0, 0])
        for row in rows:
            total = totals[row["repeat"]]
            total[0] += int(row["bytes"])
            total[1] += int(row["uploads"])
        byte_count, calls = next(iter(totals.values()))
        if any(total != [byte_count, calls] for total in totals.values()):
            raise ValueError(f"{path.name} {method} {phase} has inconsistent upload counts")
        result.setdefault(method, {})[phase] = {
            "bytes_per_route": byte_count,
            "uploads_per_route": calls,
            "windows_timed": len(rows),
            "owner_cpu_ms": percentiles(rows, "cpu_ns"),
            "owner_wall_ms": percentiles(rows, "wall_ns"),
            "update_and_gpu_idle_ms": percentiles(rows, "serial_ns"),
        }
        if rows[0].get("plan_cpu_ns") and any(int(row["plan_cpu_ns"]) for row in rows):
            result[method][phase]["plan_cpu_ms"] = percentiles(rows, "plan_cpu_ns")
    return result


if __name__ == "__main__":
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", type=Path, nargs="+")
    args = parser.parse_args()
    print(json.dumps({path.stem: summarize(path) for path in args.traces}, indent=2))
