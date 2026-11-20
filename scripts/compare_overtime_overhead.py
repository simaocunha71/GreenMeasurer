#!/usr/bin/env python3
"""Compare workload elapsed time with and without over-time sampling."""

import argparse
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile
import time

from analyze_overtime import read_runs
from weave import tomllib, validate, weave


def positive_int(value):
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def aspect_arguments(model, metrics, calibration_values, variance, controlled):
    aspects = {aspect["name"]: aspect for aspect in model["aspects"]}
    if not metrics or len(set(metrics)) != len(metrics):
        raise ValueError("select at least one distinct aspect")
    unknown = set(metrics) - set(aspects)
    if unknown:
        raise ValueError(f"aspects not declared in the configuration: {sorted(unknown)}")
    if any(not aspects[name]["metrics"] for name in metrics):
        raise ValueError("--metrics must contain metric aspects, not powercap control aspects")
    files = {}
    for item in calibration_values:
        name, separator, path = item.partition("=")
        if not separator or not path or name in files:
            raise ValueError("--calib-file must be a unique <aspect>=<path>")
        files[name] = path
    thermal = {name for name, aspect in aspects.items() if "thermal" in aspect}
    if len(set(controlled)) != len(controlled) or not set(controlled) <= thermal:
        raise ValueError("--temperature-control must name distinct declared temperature aspects")
    if set(files) != set(controlled):
        raise ValueError("--calib-file must be supplied for each controlled temperature aspect only")
    if bool(controlled) != (variance is not None):
        raise ValueError("--variance is required only when temperature control is enabled")
    for path in files.values():
        if not Path(path).is_file():
            raise ValueError(f"calibration file does not exist: {path}")
    return aspects, files


def selection_arguments(metrics, files, variance):
    arguments = [f"--{name}" for name in metrics]
    for name, path in files.items():
        arguments += ["-temperature-control", name, "-calib-file", f"{name}={path}"]
    if files:
        arguments += ["-variance", str(variance)]
    return arguments


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
                        help="TOML configuration declaring aspects")
    parser.add_argument("--command", required=True, help="Workload command")
    parser.add_argument("--metrics", nargs="+", required=True, help="Aspect names to select")
    parser.add_argument("--interval-ms", type=positive_int, required=True)
    parser.add_argument("--calib-file", action="append", default=[], metavar="ASPECT=PATH")
    parser.add_argument("--temperature-control", action="append", default=[], metavar="ASPECT")
    parser.add_argument("--variance", type=int)
    parser.add_argument("--runs", type=positive_int, default=5, help="Pairs of runs (default: 5)")
    parser.add_argument("--seed", type=int, default=0, help="Seed for randomized order (default: 0)")
    args = parser.parse_args()
    if tomllib is None:
        parser.error("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    try:
        config_path = args.config.resolve()
        with config_path.open("rb") as source:
            config = tomllib.load(source)
        model = validate(config, config_path)
        if not args.command.strip():
            raise ValueError("--command must not be empty")
        if args.variance is not None and not 0 <= args.variance <= 1000:
            raise ValueError("--variance must be between 0 and 1000")
        _, files = aspect_arguments(model, args.metrics, args.calib_file, args.variance, args.temperature_control)
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    baseline = []
    measured = []
    rng = random.Random(args.seed)
    command = args.command
    print(f"Command: {command}\nPairs: {args.runs}; seed: {args.seed}; "
          f"interval: {args.interval_ms} ms")
    print("order,pair,mode,workload_ms,wrapper_ms")
    order = 0
    try:
        with tempfile.TemporaryDirectory(prefix="greenmeasurer-overhead-") as directory:
            output = Path(directory) / "overtime.csv"
            executable, _ = weave(config_path, Path(directory) / "generated", 1000000)
            for pair in range(1, args.runs + 1):
                modes = ["baseline", "over-time"]
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
                        elapsed = run_timed(
                            [str(executable), "--over-time", "-command", command,
                             "-language", "Shell", "-program", "overhead", "-output", str(output),
                             "-n_times", "1", "-sleep-secs", "0",
                             "-time_out_limit", "0", "-interval-ms", str(args.interval_ms),
                             *selection_arguments(args.metrics, files, args.variance)])
                        runs = read_runs(output)
                        if len(runs) != 1:
                            raise ValueError("expected exactly one over-time run in generated CSV")
                        workload_ms = runs[0][1][-1] / 1000
                        measured.append(workload_ms)
                        print(f"{order},{pair},overtime,{workload_ms:.6f},{elapsed:.6f}")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    base_mean = statistics.mean(baseline)
    measured_mean = statistics.mean(measured)
    print(f"\nMean baseline workload: {base_mean:.6f} ms")
    print(f"Mean over-time workload: {measured_mean:.6f} ms")
    print(f"Mean difference: {measured_mean - base_mean:.6f} ms "
          f"({100 * (measured_mean - base_mean) / base_mean:.2f}%)")
    print("Workload timing uses the final over-time timestamp; wrapper_ms includes startup, waiting and CSV dump.")
    print("This compares wall time only; it does not measure energy overhead or prove causation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
