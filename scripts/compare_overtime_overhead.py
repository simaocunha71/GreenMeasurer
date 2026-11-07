#!/usr/bin/env python3
"""Compare workload elapsed time with and without overtime sampling."""

import argparse
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile
import time

from analyze_overtime import read_runs


def positive_int(value):
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def run_timed(command):
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8", errors="replace") as stdout, \
            tempfile.TemporaryFile(mode="w+t", encoding="utf-8", errors="replace") as stderr:
        started = time.monotonic_ns()
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
        elapsed_ms = (time.monotonic_ns() - started) / 1_000_000
        stderr.seek(0)
        diagnostics = stderr.read()
        if result.returncode != 0:
            stdout.seek(0)
            raise RuntimeError(f"command exited with status {result.returncode}:\n"
                               f"{stdout.read()}{diagnostics}")
        if diagnostics:
            print(diagnostics, file=sys.stderr, end="")
    return elapsed_ms


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command", required=True, help="Foreground shell command; executed repeatedly")
    parser.add_argument("--main", type=Path, default=Path("src/main"), help="GreenMeasurer executable")
    parser.add_argument("--calib-file", type=Path, required=True, help="Existing temperature calibration file")
    parser.add_argument("--runs", type=positive_int, default=5, help="Pairs of runs (default: 5)")
    parser.add_argument("--interval-us", type=positive_int, default=1000, help="Overtime pause in microseconds")
    parser.add_argument("--variance", type=int, default=100, help="Temperature threshold variance (default: 100)")
    parser.add_argument("--seed", type=int, default=0, help="Seed for randomized order (default: 0)")
    args = parser.parse_args()
    if not args.command.strip():
        parser.error("--command must not be empty")
    if args.variance < 0:
        parser.error("--variance must be non-negative")
    executable = args.main.resolve()
    calibration = args.calib_file.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        parser.error(f"not an executable: {executable}")
    if not calibration.is_file():
        parser.error(f"calibration file does not exist: {calibration}")

    baseline = []
    measured = []
    rng = random.Random(args.seed)
    print(f"Command: {args.command}\nPairs: {args.runs}; seed: {args.seed}; interval: {args.interval_us} us")
    print("order,pair,mode,workload_ms,wrapper_ms")
    order = 0
    try:
        with tempfile.TemporaryDirectory(prefix="greenmeasurer-overhead-") as directory:
            output = Path(directory) / "overtime.csv"
            for pair in range(1, args.runs + 1):
                modes = ["baseline", "overtime"]
                rng.shuffle(modes)
                for mode in modes:
                    order += 1
                    if mode == "baseline":
                        elapsed = run_timed(["/bin/sh", "-c", args.command])
                        baseline.append(elapsed)
                        print(f"{order},{pair},baseline,{elapsed:.6f},")
                    else:
                        if output.exists():
                            output.unlink()
                        elapsed = run_timed([
                            str(executable), "--overtime", "-command", args.command,
                            "-language", "benchmark", "-program", "overhead",
                            "-powercap", "-1", "-variance", str(args.variance),
                            "-sleep-secs", "0", "-interval-us", str(args.interval_us),
                            "-calib-file", str(calibration), "-output", str(output),
                        ])
                        runs = read_runs(output)
                        if len(runs) != 1:
                            raise ValueError("expected exactly one overtime run in generated CSV")
                        workload_ms = runs[0][1][-1]
                        measured.append(workload_ms)
                        print(f"{order},{pair},overtime,{workload_ms:.6f},{elapsed:.6f}")
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    base_mean = statistics.mean(baseline)
    measured_mean = statistics.mean(measured)
    print(f"\nMean baseline workload: {base_mean:.6f} ms")
    print(f"Mean overtime workload: {measured_mean:.6f} ms")
    print(f"Mean difference: {measured_mean - base_mean:.6f} ms "
          f"({100 * (measured_mean - base_mean) / base_mean:.2f}%)")
    print("Workload timing uses the final overtime timestamp; wrapper_ms includes startup, waiting and CSV dump.")
    print("This compares wall time only; it does not measure energy overhead or prove causation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
