#!/usr/bin/env python3
"""Compare standard-mode wall times with progressively more selected metrics."""

import argparse
import copy
import csv
import math
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
import tempfile
import time

from compare_overtime_overhead import positive_int
from generate_config import generate_configured
from validate_config import tomllib, validate_config


SYSTEM_COLUMNS = {
    "system.memory": "CPU_Memory_KB",
    "system.cores": "Cores",
    "system.workload_processes": "Workload_Processes_Peak",
    "system.running_processes": "System_Running_Processes_Peak",
}


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
        waited = any("Waiting for temperature (" in line for line in stdout)
    return elapsed_ms, waited


def metric_columns(metric, header, source_metrics):
    if metric in SYSTEM_COLUMNS:
        return [SYSTEM_COLUMNS[metric]] if SYSTEM_COLUMNS[metric] in header else []
    if metric == "system.core_frequency":
        return [column for column in header
                if re.fullmatch(r"Core[0-9]+_Avg_Freq_MHz", column)]
    parts = metric.split(".")
    if len(parts) == 3 and parts[0] == "external":
        for source in source_metrics:
            if source["name"] == parts[1] and source["function"] == parts[2]:
                parts[2] = source["metric"]
                break
    prefix = ".".join(parts) + "_"
    return [column for column in header if column.startswith(prefix)]


def read_measurement(path, metrics, source_metrics):
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source, skipinitialspace=True)
        header = reader.fieldnames or []
        rows = list(reader)
    if len(rows) != 1 or "Time_ms" not in header:
        raise ValueError("expected one standard measurement with a Time_ms column")
    row = rows[0]
    for metric in metrics:
        columns = metric_columns(metric, header, source_metrics)
        if not columns:
            raise ValueError(f"selected metric '{metric}' is absent from the CSV; "
                             "check hardware support and permissions")
        if any(row[column] is None or row[column].strip() in ("", "NOT_SUPPORTED")
               for column in columns):
            raise ValueError(f"selected metric '{metric}' could not be read")
    try:
        elapsed = float(row["Time_ms"])
    except (TypeError, ValueError) as error:
        raise ValueError("invalid standard Time_ms") from error
    if not math.isfinite(elapsed) or elapsed < 0:
        raise ValueError("standard Time_ms must be finite and non-negative")
    return elapsed, tuple(header)


def prepare_config(base, metrics, output):
    config = copy.deepcopy(base)
    config["measurement"].update(output=str(output), repetitions=1, powercap=-1)
    config["metrics"]["select"] = metrics
    if "external_metrics" in config:
        config["external_metrics"] = [
            item for item in config["external_metrics"]
            if any(name in metrics for name in (
                f'external.{item["name"]}.{item["metric"]}',
                f'external.{item["name"]}.{item["function"]}',
            ))
        ]
    return config


def collect_records(executables, metrics, command, source_metrics, runs, seed):
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
                executable, selected, output = executables[count]
                if output.exists():
                    output.unlink()
                total_ms, waited = run_timed([str(executable)])
                workload_ms, header = read_measurement(output, selected, source_metrics)
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
    print("Baseline workload_ms measures the shell invocation; standard workload_ms is Time_ms. "
          "Timing boundaries differ. Energy overhead is not measured.", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True,
                        help="Base standard-mode TOML configuration")
    parser.add_argument("--metrics", nargs="+", required=True,
                        help="Ordered metric names; each successive run adds one")
    parser.add_argument("--runs", type=positive_int, default=10,
                        help="Randomized rounds (default: 10)")
    parser.add_argument("--seed", type=int, default=0,
                        help="Randomization seed (default: 0)")
    args = parser.parse_args()
    if tomllib is None:
        parser.error("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    if len(set(args.metrics)) != len(args.metrics):
        parser.error("--metrics must not contain duplicates")

    try:
        config_path = args.config.resolve()
        with config_path.open("rb") as source:
            base = tomllib.load(source)
        validate_config(base, config_path)
        measurement = base["measurement"]
        if measurement["mode"] != "standard" or "command" not in measurement:
            raise ValueError("a standard configuration with measurement.command is required")
        calibration = Path(measurement.get("calibration_file", "/tmp/cores_temperatures.txt"))
        if not calibration.is_file():
            raise ValueError(f"calibration file does not exist: {calibration}")

        print("Using one repetition and powercap=-1 for every measured run. "
              "Existing system power limits may still apply.", file=sys.stderr)
        print(f"Rounds: {args.runs}; seed: {args.seed}", file=sys.stderr)
        with tempfile.TemporaryDirectory(prefix="greenmeasurer-standard-overhead-") as directory:
            root = Path(directory)
            executables = {}
            source_metrics = base.get("external_metrics", [])
            for count in range(1, len(args.metrics) + 1):
                selected = args.metrics[:count]
                config = prepare_config(base, selected, root / f"result-{count}.csv")
                validate_config(config, config_path)
                executable = generate_configured(config, config_path, root / f"build-{count}",
                                                 log=sys.stderr)
                executables[count] = (executable, selected, Path(config["measurement"]["output"]))

            records = collect_records(executables, args.metrics, measurement["command"],
                                      source_metrics, args.runs, args.seed)
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
