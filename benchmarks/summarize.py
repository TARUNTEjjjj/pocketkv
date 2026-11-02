"""Summarize benchmark CSV files without third-party packages (Python 3.9+)."""
import csv
from collections import defaultdict
from pathlib import Path
import statistics
import sys


if len(sys.argv) < 2:
    raise SystemExit("usage: python3 benchmarks/summarize.py RESULTS.csv [RESULTS.csv ...]")

print("| Keys | Value bytes | Reads | Round | Records | File bytes | Trials | Reopen ms, median [min, max] | Put us/op, median | Get us/op, median |")
print("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: |")
for filename in sys.argv[1:]:
    groups = defaultdict(list)
    with Path(filename).open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            if row["sync_policy"] != "fsync_per_put" or row["cache_mode"] != "warm":
                raise ValueError("this summary expects fsync-per-put, warm-cache measurements")
            # Keep different payload sizes, read counts and policies separate.
            group = (int(row["live_keys"]), int(row["round"]), int(row["value_bytes"]),
                     int(row["get_operations"]), row["sync_policy"], row["cache_mode"])
            groups[group].append(row)
    for group, rows in sorted(groups.items()):
        reopen = [float(row["reopen_ms"]) for row in rows]
        puts = [1000 * float(row["put_batch_ms"]) / int(row["put_operations"]) for row in rows]
        gets = [1000 * float(row["get_batch_ms"]) / int(row["get_operations"]) for row in rows]
        first = rows[0]
        print(f"| {group[0]} | {group[2]} | {group[3]} | {group[1]} | {first['records']} | {first['file_bytes']} | {len(rows)} | "
              f"{statistics.median(reopen):.3f} [{min(reopen):.3f}, {max(reopen):.3f}] | "
              f"{statistics.median(puts):.2f} | {statistics.median(gets):.2f} |")
