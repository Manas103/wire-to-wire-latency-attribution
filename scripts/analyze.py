#!/usr/bin/env python3
"""Computes per-segment latency percentiles from a client CSV log.

Usage: analyze.py <csv_path> [label]

Four timestamps per row (t1 client send, t2 server kernel/driver rx, t3
server send, t4 client kernel rx of the response) give three segments plus
the total:
  leg1  = t2 - t1   (request: wire + kernel/driver queueing)
  proc  = t3 - t2    (server-side processing)
  leg2  = t4 - t3    (response: wire + kernel/driver queueing)
  total = t4 - t1    (the p99.9 "budget" the resume claim is about)
"""
import csv
import sys


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * p
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def main():
    path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) > 2 else path
    leg1, proc, leg2, total = [], [], [], []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            t1, t2, t3, t4 = int(row["t1"]), int(row["t2"]), int(row["t3"]), int(row["t4"])
            leg1.append(t2 - t1)
            proc.append(t3 - t2)
            leg2.append(t4 - t3)
            total.append(t4 - t1)

    n = len(total)
    print(f"=== {label} (n={n}) ===")
    for name, series in [("leg1 (req wire+kernel)", leg1), ("proc (server)", proc),
                         ("leg2 (resp wire+kernel)", leg2), ("total RTT", total)]:
        s = sorted(series)
        mean_ns = sum(s) / len(s)
        print(f"{name:26s} mean={mean_ns/1000:9.3f}us  p50={percentile(s,0.50)/1000:9.3f}us "
              f"p99={percentile(s,0.99)/1000:9.3f}us  p99.9={percentile(s,0.999)/1000:9.3f}us  "
              f"max={max(s)/1000:9.3f}us")
    print()


if __name__ == "__main__":
    main()
