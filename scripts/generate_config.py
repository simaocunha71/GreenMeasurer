#!/usr/bin/env python3
"""Generate and compile a configured GreenMeasurer executable."""

import argparse
import copy
import difflib
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

from run_config import ROOT, configured_arguments
from validate_config import tomllib, validate_config


def c_string(value):
    def encode(byte):
        if 32 <= byte <= 126 and byte not in (34, 92):
            return chr(byte)
        return f"\\{byte:03o}"

    return '"' + "".join(encode(byte) for byte in os.fsencode(value)) + '"'


def generate_source(arguments):
    backend_dir = ROOT / "src" / "platform" / "linux"
    values = ",\n        ".join(c_string(value) for value in arguments)
    return f"""\
/* Generated configuration. Rebuild after changing the TOML file. */
#define GREENMEASURER_PROCESSOR {c_string(ROOT / "Utils" / "processPowercapCalibrationCSV.py")}
#define main greenmeasurer_engine_main
#include "main.c"
#undef main

int main(int argc, char **argv) {{
    if (argc != 1) {{
        fprintf(stderr, "[ERROR] This generated executable does not accept overrides.\\n");
        return 1;
    }}
    const char *backend_dir = getenv("GREENMEASURER_BACKEND_DIR");
    if ((!backend_dir || !*backend_dir) &&
        setenv("GREENMEASURER_BACKEND_DIR", {c_string(backend_dir)}, 1) != 0) {{
        perror("[ERROR] Unable to set backend directory");
        return 1;
    }}
    char *arguments[] = {{
        argv[0],
        {values},
        NULL
    }};
    return greenmeasurer_engine_main((int)(sizeof(arguments) / sizeof(arguments[0])) - 1, arguments);
}}
"""

def numeric_literal(typ, value):
    if typ == "double":
        return float(value).hex()
    if typ == "unsigned int":
        return f"{value}u"
    return str(value)


def generate_external_adapter(metric, config_path):
    header = (config_path.parent / metric["header"]).resolve()
    arguments = metric.get("arguments", [])
    types = ", ".join(item["type"] for item in arguments) or "void"
    values = ", ".join(numeric_literal(item["type"], item["value"]) for item in arguments)
    sentinel = ""
    if "error_value" in metric:
        sentinel = f"    if (result == {float(metric['error_value']).hex()}) return -1;\n"
    return f"""\
#include <math.h>
#include {c_string(header)}
#include "metric-adapter.h"

typedef {metric["return_type"]} (*expected_function)({types});
_Static_assert(_Generic(&{metric["function"]}, expected_function: 1, default: 0),
               "configured function signature does not match the header");

static int read_metric(size_t index, double *value) {{
    if (index != 0 || !value) return -1;
    double result = (double){metric["function"]}({values});
    if (!isfinite(result)) return -1;
{sentinel}    *value = result;
    return 0;
}}

static const GreenMetricDescriptor metrics[] = {{
    {{{c_string(metric["metric"])}, {c_string(metric["unit"])}}}
}};
const GreenMetricAdapter greenmeasurer_metric_adapter = {{
    .abi_version = GREENMEASURER_METRIC_ADAPTER_ABI,
    .metrics = metrics,
    .metric_count = 1,
    .read = read_metric
}};
"""


def build_external_metrics(config, config_path, output, log=sys.stdout):
    configuration = copy.deepcopy(config)
    generated = configuration.pop("external_metrics", [])
    select_all = configuration["metrics"]["select"] == "all"
    for metric in generated:
        name = f'external.{metric["name"]}.{metric["metric"]}'
        function_name = f'external.{metric["name"]}.{metric["function"]}'
        if (not select_all and name not in configuration["metrics"]["select"] and
                function_name not in configuration["metrics"]["select"]):
            continue
        if not select_all and function_name != name and function_name in configuration["metrics"]["select"]:
            index = configuration["metrics"]["select"].index(function_name)
            configuration["metrics"]["select"][index] = name
        source = output / f'{metric["name"]}_adapter.c'
        library = output / f'{metric["name"]}_adapter.so'
        source.write_text(generate_external_adapter(metric, config_path), encoding="utf-8")
        command = [
            "cc", "-std=c11", "-fPIC", "-shared", "-Werror",
            "-Wl,-z,defs", "-I", str(ROOT / "src" / "include"),
        ]
        for directory in metric.get("include_dirs", []):
            command.extend(["-I", str((config_path.parent / directory).resolve())])
        command.extend([str(source)])
        command.extend(str((config_path.parent / path).resolve()) for path in metric["sources"])
        command.extend(f"-l{item}" for item in metric.get("libraries", []))
        command.extend(["-lm", "-o", str(library)])
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        if result.returncode:
            print(f"[WARNING] Cannot build metric '{name}' from function "
                  f"'{metric['function']}'; skipping it.", file=sys.stderr)
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            header = (config_path.parent / metric["header"]).read_text(encoding="utf-8")
            candidates = set(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", header))
            suggestions = difflib.get_close_matches(metric["function"], sorted(candidates), n=3)
            if suggestions:
                print(f"[INFO] Possible names in the header: {', '.join(suggestions)}",
                      file=sys.stderr)
            if not select_all:
                configuration["metrics"]["select"].remove(name)
            continue
        configuration.setdefault("adapters", []).append({"name": metric["name"], "path": str(library)})
        print(f"Built external metric '{name}': {library}", file=log)
    if not configuration["metrics"]["select"]:
        raise ValueError("no selected metrics remain after building external metrics")
    return configuration


def run_temperature_calibration(config, config_path, log=sys.stdout):
    """Calibrate the baseline temperature directly; no executable is generated."""
    subprocess.run(["make", "-C", str(ROOT / "src"), "all"], check=True, stdout=log)
    environment = os.environ.copy()
    environment.setdefault("GREENMEASURER_BACKEND_DIR", str(ROOT / "src" / "platform" / "linux"))
    arguments = [str(ROOT / "src" / "greenmeasurer-engine"), *configured_arguments(config, config_path)]
    log.flush()
    result = subprocess.run(arguments, env=environment)
    if result.returncode != 0:
        raise ValueError("temperature calibration failed; see the diagnostics above")


def generate_configured(config, config_path, output, log=sys.stdout):
    validate_config(config, config_path)
    if config["measurement"]["mode"] == "temperature-calibration":
        run_temperature_calibration(config, config_path, log)
        return None
    if "command" not in config["measurement"]:
        raise ValueError("measurement.command is required for execution")
    if not config.get("external_metrics"):
        configured_arguments(config, config_path)
    output.mkdir()
    config = build_external_metrics(config, config_path, output, log)
    arguments = configured_arguments(config, config_path)
    source = output / "greenmeasurer.c"
    binary = output / "greenmeasurer"
    source.write_text(generate_source(arguments), encoding="utf-8")
    subprocess.run(["make", "-C", str(ROOT / "src"), "all"], check=True, stdout=log)
    flags = subprocess.run(
        ["make", "-s", "--no-print-directory", "-C", str(ROOT / "src"),
         "print-configured-host-libs"],
        check=True, text=True, capture_output=True,
    ).stdout
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-pthread", "-rdynamic",
         "-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "include"),
         str(source), str(ROOT / "src" / "utils" / "memory.c"),
         str(ROOT / "src" / "metrics" / "system_metrics.c"),
         str(ROOT / "src" / "metrics" / "metric_adapter_loader.c"),
         "-o", str(binary), *shlex.split(flags), "-pthread", "-lm", "-ldl"],
        check=True,
    )
    print(f"Generated source: {source}", file=log)
    print(f"Compiled executable: {binary}", file=log)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--directory", type=Path,
                        help="new directory for greenmeasurer.c and the executable "
                             "(not used by temperature calibration)")
    args = parser.parse_args()
    if tomllib is None:
        print("Error: Python 3.11+ or the 'tomli' package is required for TOML parsing", file=sys.stderr)
        return 1
    try:
        config_path = args.config.resolve()
        with config_path.open("rb") as source:
            config = tomllib.load(source)
        is_temperature = config.get("measurement", {}).get("mode") == "temperature-calibration"
        if args.directory is None and not is_temperature:
            raise ValueError("--directory is required to generate an executable")
        if args.directory is not None and is_temperature:
            print("[INFO] Temperature calibration runs directly; --directory is ignored.")
        generate_configured(config, config_path,
                            args.directory.absolute() if args.directory else None)
        return 0
    except (OSError, ValueError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
