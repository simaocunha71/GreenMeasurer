#!/usr/bin/env python3
"""Validate a GreenMeasurer configuration without executing the workload."""

import argparse
import math
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        tomllib = None


IDENTIFIER = re.compile(r"[A-Za-z0-9_-]+\Z")
C_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
NUMERIC_TYPES = {
    "int": (-(2**31), 2**31 - 1),
    "unsigned int": (0, 2**32 - 1),
    "double": None,
}
SYSTEM_METRICS = {
    "system.memory",
    "system.cores",
    "system.workload_processes",
    "system.running_processes",
    "system.core_frequency",
}
MAX_POWERCAPS = 128
MEASUREMENT_FIELDS = {
    "mode", "command", "language", "program", "output", "calibration_file",
    "interval_us", "repetitions", "timeout_hours", "powercap", "variance", "sleep_seconds",
    "powercap_min", "powercap_max", "compile_command",
}
ADAPTER_CHECKER = Path(__file__).resolve().parents[1] / "src" / "metric-adapter-check"
METRIC_GUIDANCE = (
    "If the metric is not provided by GreenMeasurer and you have a library's .c and .h "
    "files, implement a metric adapter that calls its function, compile the adapter and "
    "library into a .so, and select external.<adapter>.<metric> in the TOML file. "
    "See src/tests/fixtures/custom_metric and README.md. Use a CPU/GPU backend instead if the "
    "metric requires hardware-specific window or process sampling."
)


def fail(message):
    raise ValueError(message)


def string(value, location):
    if not isinstance(value, str) or not value.strip() or "\0" in value:
        fail(f"{location} must be a non-empty string without NUL characters")
    return value


def identifier(value, location):
    value = string(value, location)
    if not IDENTIFIER.fullmatch(value):
        fail(f"{location} must contain only ASCII letters, digits, '_' or '-'")
    return value


def integer(value, location, minimum, maximum):
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        fail(f"{location} must be an integer between {minimum} and {maximum}")
    return value


def only_fields(table, allowed, location):
    if not isinstance(table, dict):
        fail(f"{location} must be a table")
    extra = set(table) - allowed
    if extra:
        fail(f"{location} contains unknown keys: {', '.join(sorted(extra))}")


def validate_config(data, config_path):
    only_fields(data, {"measurement", "metrics", "adapters", "external_metrics"}, "configuration")
    measurement = data.get("measurement")
    only_fields(measurement, MEASUREMENT_FIELDS, "measurement")
    mode = measurement.get("mode")
    if mode not in ("standard", "overtime", "temperature-calibration", "powercap-calibration"):
        fail("measurement.mode must be 'standard', 'overtime', 'temperature-calibration' or 'powercap-calibration'")
    for key in ("command", "language", "program", "output", "calibration_file", "compile_command"):
        if key in measurement:
            string(measurement[key], f"measurement.{key}")
    limits = {
        "interval_us": (1, 4294967295),
        "repetitions": (1, 2147483647),
        "timeout_hours": (1, 596523),
        "variance": (0, 2147483647),
        "sleep_seconds": (0, 2147483647),
        "powercap": (-1, 2147483647),
        "powercap_min": (1, 2147483646),
        "powercap_max": (1, 2147483646),
    }
    for key, (minimum, maximum) in limits.items():
        if key in measurement and key != "powercap":
            integer(measurement[key], f"measurement.{key}", minimum, maximum)
    if "powercap" in measurement:
        caps = measurement["powercap"]
        if isinstance(caps, list):
            if not caps or len(caps) > MAX_POWERCAPS:
                fail(f"measurement.powercap must list between 1 and {MAX_POWERCAPS} values")
            if len(set(caps)) != len(caps):
                fail("measurement.powercap contains duplicate values")
        for cap in caps if isinstance(caps, list) else [caps]:
            integer(cap, "measurement.powercap", -1, 2147483647)
            if cap == 0:
                fail("measurement.powercap must contain only -1 or positive integers")
    if mode == "temperature-calibration":
        only_fields(data, {"measurement"}, "temperature calibration configuration")
        only_fields(measurement, {"mode", "sleep_seconds", "calibration_file"}, "measurement")
        if "calibration_file" in measurement and len(os.fsencode(measurement["calibration_file"])) > 511:
            fail("measurement.calibration_file exceeds 511 bytes")
        return data
    if mode == "powercap-calibration":
        only_fields(measurement, {"mode", "sleep_seconds", "calibration_file", "command",
                                  "compile_command", "language", "program", "output",
                                  "repetitions", "timeout_hours", "variance",
                                  "powercap_min", "powercap_max"}, "measurement")
        for key in ("command", "powercap_min", "powercap_max"):
            if key not in measurement:
                fail(f"measurement.{key} is required for powercap calibration")
        if measurement["powercap_min"] > measurement["powercap_max"]:
            fail("measurement.powercap_min must not exceed powercap_max")
        if measurement["powercap_max"] - measurement["powercap_min"] + 1 >= MAX_POWERCAPS:
            fail(f"the powercap range must contain fewer than {MAX_POWERCAPS} values")
        for key, limit in (("command", 499), ("compile_command", 499), ("language", 127),
                           ("program", 127), ("output", 511), ("calibration_file", 511)):
            if key in measurement and len(os.fsencode(measurement[key])) > limit:
                fail(f"measurement.{key} exceeds {limit} bytes")
        for key in ("language", "program"):
            if key in measurement and any(c in measurement[key] for c in ',\r\n"'):
                fail(f"measurement.{key} cannot contain CSV delimiters or newlines")
        if "metrics" not in data:
            data["metrics"] = {"select": ["cpu.Package"]}
    else:
        for key in ("powercap_min", "powercap_max", "compile_command"):
            if key in measurement:
                fail(f"measurement.{key} is only valid in powercap calibration mode")
    if mode == "standard" and "interval_us" in measurement:
        fail("measurement.interval_us is only valid in overtime mode")
    if mode == "overtime" and ("repetitions" in measurement or "timeout_hours" in measurement):
        fail("measurement.repetitions and timeout_hours are only valid in standard mode")

    metrics = data.get("metrics")
    only_fields(metrics, {"select"}, "metrics")
    selected = metrics.get("select")
    if selected != "all":
        if not isinstance(selected, list) or not selected:
            fail("metrics.select must be 'all' or a non-empty array of metric names")
        if any(not isinstance(item, str) or not item for item in selected):
            fail("every entry in metrics.select must be a non-empty string")
        if len(set(selected)) != len(selected):
            fail("metrics.select contains duplicate metric names")

    adapters = data.get("adapters", [])
    if not isinstance(adapters, list):
        fail("adapters must be an array of tables")
    aliases = set()
    for index, adapter in enumerate(adapters):
        location = f"adapters[{index}]"
        only_fields(adapter, {"name", "path"}, location)
        name = identifier(adapter.get("name"), f"{location}.name")
        path = string(adapter.get("path"), f"{location}.path")
        if name in aliases:
            fail(f"duplicate adapter name: {name}")
        aliases.add(name)
        if not (Path(config_path).parent / path).is_file():
            fail(f"{location}.path does not refer to an existing file: {path}. {METRIC_GUIDANCE}")

    external_metrics = data.get("external_metrics", [])
    if not isinstance(external_metrics, list):
        fail("external_metrics must be an array of tables")
    external_names = set()
    for index, metric in enumerate(external_metrics):
        location = f"external_metrics[{index}]"
        only_fields(metric, {"name", "metric", "unit", "function", "header", "sources",
                             "return_type", "arguments", "error_value", "include_dirs",
                             "libraries"}, location)
        name = identifier(metric.get("name"), f"{location}.name")
        metric_name = identifier(metric.get("metric"), f"{location}.metric")
        identifier(metric.get("unit"), f"{location}.unit")
        function = string(metric.get("function"), f"{location}.function")
        if not C_IDENTIFIER.fullmatch(function):
            fail(f"{location}.function must be a C identifier")
        if name in aliases:
            fail(f"external metric name conflicts with adapter: {name}")
        if name in external_names:
            fail(f"duplicate external metric name: {name}")
        external_names.add(name)
        header = string(metric.get("header"), f"{location}.header")
        if not (Path(config_path).parent / header).is_file():
            fail(f"{location}.header does not refer to an existing file: {header}")
        for field in ("sources", "include_dirs", "libraries"):
            values = metric.get(field, [])
            if not isinstance(values, list) or (field == "sources" and not values):
                fail(f"{location}.{field} must be an array" +
                     (" with at least one source" if field == "sources" else ""))
            for value in values:
                string(value, f"{location}.{field} entry")
                if field == "libraries":
                    if not C_IDENTIFIER.fullmatch(value):
                        fail(f"{location}.libraries entries must be library names without '-l'")
                else:
                    path = Path(config_path).parent / value
                    if not (path.is_dir() if field == "include_dirs" else path.is_file()):
                        fail(f"{location}.{field} path does not exist: {value}")
        if not isinstance(metric.get("return_type"), str) or metric["return_type"] not in NUMERIC_TYPES:
            fail(f"{location}.return_type must be int, unsigned int, or double")
        arguments = metric.get("arguments", [])
        if not isinstance(arguments, list):
            fail(f"{location}.arguments must be an array")
        for argument in arguments:
            only_fields(argument, {"type", "value"}, f"{location}.arguments entry")
            typ = argument.get("type")
            if not isinstance(typ, str) or typ not in NUMERIC_TYPES:
                fail(f"{location} argument type must be int, unsigned int, or double")
            value = argument.get("value")
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                fail(f"{location} argument value must be numeric")
            if typ == "double":
                try:
                    valid = math.isfinite(float(value))
                except OverflowError:
                    valid = False
            else:
                lower, upper = NUMERIC_TYPES[typ]
                valid = isinstance(value, int) and lower <= value <= upper
            if not valid:
                fail(f"{location} argument value is out of range for {typ}")
        if "error_value" in metric:
            value = metric["error_value"]
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                fail(f"{location}.error_value must be numeric")
            try:
                valid = math.isfinite(float(value))
            except OverflowError:
                valid = False
            if not valid:
                fail(f"{location}.error_value must be finite")
        if isinstance(selected, list):
            references = {f"external.{name}.{metric_name}", f"external.{name}.{function}"}
            selected_references = references.intersection(selected)
            if not selected_references:
                fail(f"{location} is not selected in metrics.select")
            if len(selected_references) > 1:
                fail(f"{location} is selected more than once by metric and function name")

    if isinstance(selected, list):
        for metric in selected:
            parts = metric.split(".")
            if metric in SYSTEM_METRICS:
                continue
            if len(parts) == 2 and parts[0] in ("cpu", "gpu"):
                identifier(parts[1], f"metric '{metric}'")
            elif len(parts) == 3 and parts[0] == "external":
                identifier(parts[1], f"metric '{metric}' adapter")
                identifier(parts[2], f"metric '{metric}'")
                if parts[1] not in aliases and not any(
                    item["name"] == parts[1] and parts[2] in (item["metric"], item["function"])
                    for item in external_metrics
                ):
                    fail(f"invalid or unknown metric reference: {metric}; "
                         f"adapter '{parts[1]}' is not declared. {METRIC_GUIDANCE}")
            elif len(parts) == 2 and parts[0] == "system":
                identifier(parts[1], f"metric '{metric}'")
                fail(f"invalid or unknown metric reference: {metric}; "
                     f"no such system metric is registered. {METRIC_GUIDANCE}")
            else:
                fail(f"invalid or unknown metric reference: {metric}")
    return data


def check_adapters(configuration, config_path, checker=ADAPTER_CHECKER):
    if "metrics" not in configuration:
        return
    if configuration.get("external_metrics"):
        print("[WARNING] Source-based external metrics are not built or checked by "
              "--check-adapters; use scripts/generate_config.py.", file=sys.stderr)
    selected = configuration["metrics"]["select"]
    adapters = configuration.get("adapters", [])
    for adapter in adapters:
        prefix = f'external.{adapter["name"]}.'
        names = ["--all"] if selected == "all" else [
            metric[len(prefix):] for metric in selected if metric.startswith(prefix)
        ]
        if not names:
            continue
        path = (config_path.parent / adapter["path"]).resolve()
        mode = [] if selected == "all" else ["--selected"]
        try:
            result = subprocess.run(
                [str(checker), str(path), *mode, *names], text=True, capture_output=True,
                check=False,
            )
        except OSError as error:
            raise ValueError(
                f"cannot run adapter checker '{checker}': {error}. "
                "Build it with 'make -C src metric-adapter-check'"
            ) from error
        if result.stdout:
            print(f'Adapter {adapter["name"]}:')
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode:
            fail(f'adapter {adapter["name"]} failed verification (exit {result.returncode})')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path, help="TOML configuration file")
    parser.add_argument(
        "--check-adapters", action="store_true",
        help="load trusted selected adapters and test their metric reads",
    )
    args = parser.parse_args()
    if tomllib is None:
        print("Error: Python 3.11+ or the 'tomli' package is required for TOML parsing", file=sys.stderr)
        return 1
    try:
        with args.config.open("rb") as source:
            configuration = tomllib.load(source)
        validate_config(configuration, args.config.resolve())
        if args.check_adapters:
            check_adapters(configuration, args.config.resolve())
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    print(f"Configuration syntax is valid: {args.config}")
    if args.check_adapters:
        print("CPU, GPU and system metric availability and measurement execution are not checked.")
    else:
        print("Metric availability and execution cannot yet be validated by this command.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
