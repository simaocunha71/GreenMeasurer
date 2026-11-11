#!/usr/bin/env python3
"""Summarize observed over-time sample intervals."""

import argparse
import csv
import math
import statistics
import sys


def percentile(values, fraction):
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def read_runs(path):
    runs = []
    with open(path, newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source, skipinitialspace=True)
        required = {"Language", "Program", "Powercap", "Timestamp_ms"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("CSV must contain Language, Program, Powercap and Timestamp_ms")
        for line, row in enumerate(reader, 2):
            try:
                key = tuple(row[name].strip() for name in ("Language", "Program", "Powercap"))
                timestamp = float(row["Timestamp_ms"])
            except (TypeError, ValueError, AttributeError) as error:
                raise ValueError(f"line {line}: invalid metadata or timestamp") from error
            if not math.isfinite(timestamp) or timestamp < 0:
                raise ValueError(f"line {line}: timestamp must be finite and non-negative")
            if not runs or key != runs[-1][0] or timestamp <= runs[-1][1][-1]:
                runs.append((key, []))
            runs[-1][1].append(timestamp)
    if not runs:
        raise ValueError("CSV contains no measurement rows")
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", help="Over-time CSV containing Timestamp_ms")
    parser.add_argument("--threshold-ms", type=float, default=1.0,
                        help="Report intervals greater than this threshold (default: 1 ms)")
    args = parser.parse_args()
    if not math.isfinite(args.threshold_ms) or args.threshold_ms <= 0:
        parser.error("--threshold-ms must be finite and positive")
    try:
        runs = read_runs(args.csv)
    except (OSError, ValueError, csv.Error) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print("Runs inferred from metadata changes or non-increasing timestamps; boundaries are not guaranteed.")
    print("Intervals use consecutive sample timestamps; the first and final partial windows are not distinguished.")
    for number, (key, timestamps) in enumerate(runs, 1):
        intervals = [b - a for a, b in zip(timestamps, timestamps[1:])]
        print(f"\nRun {number}: language={key[0]}, program={key[1]}, powercap={key[2]}")
        print(f"  Samples: {len(timestamps)}; first={timestamps[0]:.6f} ms; last={timestamps[-1]:.6f} ms")
        if not intervals:
            print("  No consecutive sample intervals available.")
            continue
        above = sum(value > args.threshold_ms for value in intervals)
        print(f"  Intervals: {len(intervals)}; mean={statistics.mean(intervals):.6f} ms; "
              f"median={statistics.median(intervals):.6f} ms")
        print(f"  min={min(intervals):.6f} ms; p95={percentile(intervals, 0.95):.6f} ms; "
              f"max={max(intervals):.6f} ms")
        print(f"  Above {args.threshold_ms:g} ms: {above}/{len(intervals)} "
              f"({100 * above / len(intervals):.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
