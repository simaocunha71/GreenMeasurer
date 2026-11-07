#!/usr/bin/env python3
"""Compare workload elapsed time with and without overtime sampling."""

import argparse
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile
import time

from analyze_overtime import read_runs
from generate_config import generate_configured
from validate_config import tomllib, validate_config


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
    parser.add_argument("--config", type=Path, required=True,
                        help="TOML configuration for an overtime measurement")
    parser.add_argument("--runs", type=positive_int, default=5, help="Pairs of runs (default: 5)")
    parser.add_argument("--seed", type=int, default=0, help="Seed for randomized order (default: 0)")
    args = parser.parse_args()
    if tomllib is None:
        parser.error("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    try:
        config_path = args.config.resolve()
        with config_path.open("rb") as source:
            config = tomllib.load(source)
        validate_config(config, config_path)
        measurement = config["measurement"]
        if measurement["mode"] != "overtime":
            raise ValueError("measurement.mode must be 'overtime' for this comparison")
        if "command" not in measurement:
            raise ValueError("measurement.command is required")
        if measurement.get("powercap", -1) != -1:
            raise ValueError("measurement.powercap must be -1; "
                             "the baseline cannot apply a GreenMeasurer power cap")
        calibration = Path(measurement.get("calibration_file", "/tmp/cores_temperatures.txt"))
        if not calibration.is_file():
            raise ValueError(f"calibration file does not exist: {calibration}")
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    baseline = []
    measured = []
    rng = random.Random(args.seed)
    command = measurement["command"]
    print(f"Command: {command}\nPairs: {args.runs}; seed: {args.seed}; "
          f"interval: {measurement.get('interval_us', 1000)} us")
    print("order,pair,mode,workload_ms,wrapper_ms")
    order = 0
    try:
        with tempfile.TemporaryDirectory(prefix="greenmeasurer-overhead-") as directory:
            output = Path(directory) / "overtime.csv"
            measurement["output"] = str(output)
            executable = generate_configured(config, config_path, Path(directory) / "generated",
                                             log=sys.stderr)
            for pair in range(1, args.runs + 1):
                modes = ["baseline", "overtime"]
                rng.shuffle(modes)
                for mode in modes:
                    order += 1
                    if mode == "baseline":
                        elapsed = run_timed(["/bin/sh", "-c", command])
                        baseline.append(elapsed)
                        print(f"{order},{pair},baseline,{elapsed:.6f},")
                    else:
                        if output.exists():
                            output.unlink()
                        elapsed = run_timed([str(executable)])
                        runs = read_runs(output)
                        if len(runs) != 1:
                            raise ValueError("expected exactly one overtime run in generated CSV")
                        workload_ms = runs[0][1][-1]
                        measured.append(workload_ms)
                        print(f"{order},{pair},overtime,{workload_ms:.6f},{elapsed:.6f}")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
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
