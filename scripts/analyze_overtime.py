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
    """Read runs with timestamps normalized to microseconds."""
    runs = []
    with open(path, newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source, skipinitialspace=True)
        required = {"Program", "Powercap"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("CSV must contain Program, Powercap and Timestamp_ms or Timestamp_us")
        timestamp_columns = {"Timestamp_us", "Timestamp_ms"}.intersection(reader.fieldnames)
        if len(timestamp_columns) != 1:
            raise ValueError("CSV must contain exactly one of Timestamp_us or Timestamp_ms")
        timestamp_column = timestamp_columns.pop()
        for line, row in enumerate(reader, 2):
            try:
                key = tuple(row[name].strip() for name in ("Program", "Powercap"))
                timestamp = float(row[timestamp_column])
                if timestamp_column == "Timestamp_ms":
                    timestamp *= 1000
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
    parser.add_argument("csv", help="Over-time CSV containing Timestamp_ms or legacy Timestamp_us")
    thresholds = parser.add_mutually_exclusive_group()
    thresholds.add_argument("--threshold-us", type=float,
                            help="Report intervals greater than this threshold (default: 1000 us)")
    thresholds.add_argument("--threshold-ms", type=float,
                            help="Legacy threshold in milliseconds")
    args = parser.parse_args()
    threshold_us = (args.threshold_us if args.threshold_us is not None else
                    args.threshold_ms * 1000 if args.threshold_ms is not None else 1000.0)
    if not math.isfinite(threshold_us) or threshold_us <= 0:
        parser.error("threshold must be finite and positive")
    try:
        runs = read_runs(args.csv)
    except (OSError, ValueError, csv.Error) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print("Runs inferred from metadata changes or non-increasing timestamps; boundaries are not guaranteed.")
    print("Intervals use consecutive sample timestamps; the first and final partial windows are not distinguished.")
    for number, (key, timestamps) in enumerate(runs, 1):
        intervals = [b - a for a, b in zip(timestamps, timestamps[1:])]
        print(f"\nRun {number}: program={key[0]}, powercap={key[1]}")
        print(f"  Samples: {len(timestamps)}; first={timestamps[0]:.6f} us; last={timestamps[-1]:.6f} us")
        if not intervals:
            print("  No consecutive sample intervals available.")
            continue
        above = sum(value > threshold_us for value in intervals)
        print(f"  Intervals: {len(intervals)}; mean={statistics.mean(intervals):.6f} us; "
              f"median={statistics.median(intervals):.6f} us")
        print(f"  min={min(intervals):.6f} us; p95={percentile(intervals, 0.95):.6f} us; "
              f"max={max(intervals):.6f} us")
        print(f"  Above {threshold_us:g} us: {above}/{len(intervals)} "
              f"({100 * above / len(intervals):.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
