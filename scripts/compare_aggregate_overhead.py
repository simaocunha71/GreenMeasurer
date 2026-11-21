#!/usr/bin/env python3
"""Compare aggregate-mode wall times with progressively more selected metrics."""

import argparse
import csv
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile
import time

from compare_overtime_overhead import aspect_arguments, positive_int, selection_arguments
from weave import tomllib, validate, weave


def run_timed(command):
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8", errors="replace") as stdout, \
            tempfile.TemporaryFile(mode="w+t", encoding="utf-8", errors="replace") as stderr:
        started = time.monotonic_ns()
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
        elapsed_ms = (time.monotonic_ns() - started) / 1_000_000
        stderr.seek(0)
        diagnostics = stderr.read()
        stdout.seek(0)
        if result.returncode:
            raise RuntimeError(f"command exited with status {result.returncode}:\n"
                               f"{stdout.read()}{diagnostics}")
        if diagnostics:
            print(diagnostics, file=sys.stderr, end="")
        waited = any("Waiting for temperature" in line for line in stdout)
    return elapsed_ms, waited


def read_measurement(path, metrics, metric_columns, time_column):
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source, skipinitialspace=True)
        header = reader.fieldnames or []
        rows = list(reader)
    if len(rows) != 1 or time_column not in header:
        raise ValueError(f"expected one aggregate measurement with a {time_column} column")
    row = rows[0]
    for metric in metrics:
        column = metric_columns[metric]
        if column not in header:
            raise ValueError(f"selected metric '{metric}' is absent from the CSV; "
                             "check hardware support and permissions")
        if row[column] is None or row[column].strip() in ("", "ERROR"):
            raise ValueError(f"selected metric '{metric}' could not be read")
    try:
        elapsed = float(row[time_column])
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid aggregate {time_column}") from error
    if not math.isfinite(elapsed) or elapsed < 0:
        raise ValueError(f"aggregate {time_column} must be finite and non-negative")
    return elapsed, tuple(header)


def collect_records(executable, output, metrics, command, metric_columns, runs, seed, files, variance,
                    time_aspect):
    rng = random.Random(seed)
    headers = {}
    records = []
    for round_number in range(1, runs + 1):
        order = list(range(len(metrics) + 1))
        rng.shuffle(order)
        for count in order:
            if count == 0:
                total_ms, _ = run_timed(["/bin/sh", "-c", command])
                workload_ms, waited = total_ms, False
            else:
                selected = metrics[:count]
                if output.exists():
                    output.unlink()
                total_ms, waited = run_timed(
                    [str(executable), "-command", command,
                     "-program", "overhead", "-output", str(output),
                     "-n_times", "1", "-sleep-secs", "0",
                     "-timeout-hours", "0", *selection_arguments([*selected, time_aspect], files, variance)])
                workload_ms, header = read_measurement(output, selected, metric_columns,
                                                       metric_columns[time_aspect])
                if count in headers and headers[count] != header:
                    raise ValueError(f"CSV schema changed between runs for {count} metrics")
                headers[count] = header
                if waited:
                    print(f"[WARNING] Temperature gate waited in round {round_number} "
                          f"for {count} metrics; total time includes the wait.", file=sys.stderr)
            records.append({
                "order": len(records) + 1, "round": round_number,
                "metrics_count": count, "metrics": ";".join(metrics[:count]),
                "workload_ms": f"{workload_ms:.6f}", "total_ms": f"{total_ms:.6f}",
                "thermal_wait": int(waited),
            })
    return records


def report_summary(records, metric_count):
    baseline = [float(row["total_ms"]) for row in records if row["metrics_count"] == 0]
    baseline_median = statistics.median(baseline)
    if baseline_median <= 0:
        raise ValueError("baseline median must be positive")
    print("Medians and ranges in ms; total includes startup, temperature waits and CSV output:",
          file=sys.stderr)
    for count in range(metric_count + 1):
        group = [row for row in records if row["metrics_count"] == count]
        totals = [float(row["total_ms"]) for row in group]
        workloads = [float(row["workload_ms"]) for row in group]
        median = statistics.median(totals)
        workload_median = statistics.median(workloads)
        print(f"  {count} metrics: total median={median:.6f} "
              f"range=[{min(totals):.6f}, {max(totals):.6f}], "
              f"total difference={100 * (median - baseline_median) / baseline_median:.2f}%; "
              f"workload median={workload_median:.6f}, "
              f"workload difference={100 * (workload_median - baseline_median) / baseline_median:.2f}%",
              file=sys.stderr)
    print("Baseline workload_ms measures the shell invocation; aggregate workload_ms uses the selected duration. "
          "Timing boundaries differ. Energy overhead is not measured.", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, nargs="+", required=True,
                        help="TOML aspect files declaring aggregate aspects")
    parser.add_argument("--command", required=True, help="Workload command")
    parser.add_argument("--metrics", nargs="+", required=True,
                        help="Ordered metric names; each successive run adds one")
    parser.add_argument("--time-aspect", required=True,
                        help="Declared millisecond duration metric")
    parser.add_argument("--runs", type=positive_int, default=10,
                        help="Randomized rounds (default: 10)")
    parser.add_argument("--seed", type=int, default=0,
                        help="Randomization seed (default: 0)")
    parser.add_argument("--calib-file", action="append", default=[], metavar="ASPECT=PATH")
    parser.add_argument("--temperature-control", action="append", default=[], metavar="ASPECT")
    parser.add_argument("--variance", type=int)
    args = parser.parse_args()
    if tomllib is None:
        parser.error("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    if len(set(args.metrics)) != len(args.metrics):
        parser.error("--metrics must not contain duplicates")

    try:
        config_paths = [path.resolve() for path in args.config]
        configs = []
        for config_path in config_paths:
            with config_path.open("rb") as source:
                configs.append((tomllib.load(source), config_path))
        model = validate(configs)
        if model["aspects"] and model["mode"] != "aggregate":
            raise ValueError("the configuration must declare mode = \"aggregate\"")
        if not args.command.strip():
            raise ValueError("--command must not be empty")
        if args.variance is not None and not 0 <= args.variance <= 1000:
            raise ValueError("--variance must be between 0 and 1000")
        if args.time_aspect in args.metrics:
            raise ValueError("--time-aspect is measured separately and must not also appear in --metrics")
        declared, files = aspect_arguments(model, [*args.metrics, args.time_aspect], args.calib_file,
                                          args.variance, args.temperature_control)
        _, duration = declared[args.time_aspect]
        if duration["kind"] != "duration" or duration["unit"] != "ms":
            raise ValueError("--time-aspect must be a duration measured in milliseconds")
        metric_columns = {
            metric["name"]:
                metric["name"] +
                (f'_{metric["unit"]}' if metric["unit"] else "")
            for device in model["aspects"] for metric in device["metrics"]
        }
        for metric in args.metrics:
            if metric not in metric_columns:
                raise ValueError(f"metric '{metric}' is not declared in the configuration")

        print("Using one repetition without powercap control for every measured run. "
              "Existing system power limits may still apply.", file=sys.stderr)
        print(f"Rounds: {args.runs}; seed: {args.seed}", file=sys.stderr)
        with tempfile.TemporaryDirectory(prefix="greenmeasurer-aggregate-overhead-") as directory:
            root = Path(directory)
            executable, _ = weave(config_paths, root / "build", 1000000)
            records = collect_records(executable, root / "result.csv", args.metrics,
                                      args.command, metric_columns, args.runs, args.seed,
                                      files, args.variance, args.time_aspect)
        report_summary(records, len(args.metrics))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError, csv.Error) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    writer = csv.DictWriter(sys.stdout, fieldnames=[
        "order", "round", "metrics_count", "metrics", "workload_ms", "total_ms", "thermal_wait",
    ])
    writer.writeheader()
    writer.writerows(records)
    return 0


if __name__ == "__main__":
    sys.exit(main())
