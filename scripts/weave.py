#!/usr/bin/env python3
"""Weave a GreenMeasurer executable from a TOML file that declares devices and metrics.

For every [[device]] the weaver generates <Device>.c (one sampling thread and one static
array per metric, filled by the user's own functions) and GreenMeasurer.c (the glue that
registers the devices, one command-line flag per metric and the defaults of the TOML).
Everything is then compiled and linked with the declared libraries.
"""

import argparse
import os
import re
import shlex
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

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SOURCE = ROOT / "src" / "weaver" / "gm_runtime.c"
INCLUDE_DIR = ROOT / "src" / "include"
PROCESSOR = ROOT / "Utils" / "processPowercapCalibrationCSV.py"

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
UNIT = re.compile(r"^[A-Za-z0-9_%./-]*$")
LINK_FLAG = re.compile(r"^-[lL][A-Za-z0-9_./+-]+$")
SIGNATURE = re.compile(
    r"^\s*(?P<type>[A-Za-z_][A-Za-z0-9_ ]*?)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"\s*\(\s*(?:void)?\s*\)\s*$")

FLOAT_TYPES = {"float", "double"}
INTEGER_TYPES = {
    "short", "unsigned short", "int", "unsigned", "unsigned int", "long", "unsigned long",
    "long long", "unsigned long long", "int8_t", "uint8_t", "int16_t", "uint16_t",
    "int32_t", "uint32_t", "int64_t", "uint64_t", "size_t",
}
KINDS = {"counter": "GM_KIND_COUNTER", "gauge": "GM_KIND_GAUGE"}
MODES = ("over-time", "standard", "temperature-calibration", "powercap-calibration")
MEASUREMENT_KEYS = {
    "mode": str, "command": str, "language": str, "program": str, "output": str,
    "interval_ms": int, "repetitions": int, "sleep_seconds": int, "powercap": (int, list),
    "variance": int, "calibration_file": str, "powercap_min": int, "powercap_max": int,
    "timeout_hours": int, "compile_command": str,
}
DEVICE_KEYS = {"name", "lib", "header", "link", "setup", "teardown", "metric", "optional",
               "thermal", "powercap_set", "powercap_restore", "powercap_calibration"}
PROTOTYPE = re.compile(
    r"^\s*(?P<type>[A-Za-z_][A-Za-z0-9_ ]*?)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"\s*\(\s*(?P<params>[A-Za-z0-9_ ]*)\)\s*$")
METRIC_KEYS = {"name", "read", "kind", "unit"}


def fail(message):
    raise ValueError(message)


def parse_signature(text, where):
    if not isinstance(text, str):
        fail(f"{where} must be a string such as \"int function()\"")
    match = SIGNATURE.match(text)
    if not match:
        fail(f"{where} must have the form \"<type> <function>()\"; got {text!r}")
    c_type = " ".join(match["type"].split())
    if c_type not in FLOAT_TYPES | INTEGER_TYPES:
        allowed = ", ".join(sorted(FLOAT_TYPES | INTEGER_TYPES))
        fail(f"{where} has unsupported return type {c_type!r}; use one of: {allowed}")
    return c_type, match["name"]


def parse_hook(text, where):
    if not isinstance(text, str):
        fail(f"{where} must be a string such as \"void function()\"")
    match = SIGNATURE.match(text)
    if not match or " ".join(match["type"].split()) not in ("void", "int"):
        fail(f"{where} must have the form \"void <function>()\" or \"int <function>()\"")
    return " ".join(match["type"].split()), match["name"]


def parse_prototype(text, where, return_type, params):
    """Parse "<return_type> <function>(<params>)" and return the function name."""
    if not isinstance(text, str):
        fail(f"{where} must be a string such as \"{return_type} function({params})\"")
    match = PROTOTYPE.match(text)
    if not match or " ".join(match["type"].split()) != return_type:
        fail(f"{where} must have the form \"{return_type} <function>({params})\"")
    given = " ".join(match["params"].split())
    if given != params and not (params == "" and given == "void"):
        fail(f"{where} must have the form \"{return_type} <function>({params})\"")
    return match["name"]


def parse_powercaps(value, where):
    caps = value if isinstance(value, list) else [value]
    if not caps or len(caps) > 128 or len(set(caps)) != len(caps):
        fail(f"{where} must hold between 1 and 128 unique values")
    for cap in caps:
        if isinstance(cap, bool) or not isinstance(cap, int) or cap == 0 or cap < -1:
            fail(f"{where}: each value must be -1 (saved baseline) or at least 1 W")
    return caps


def resolve_file(base, value, where):
    if not isinstance(value, str) or not value:
        fail(f"{where} must be a non-empty path")
    path = (base / value).resolve()
    if not path.is_file():
        fail(f"{where} does not refer to an existing file: {path}")
    return path


def validate(config, config_path):
    """Return a normalised model of the configuration or raise ValueError."""
    unknown = set(config) - {"measurement", "device"}
    if unknown:
        fail(f"unknown top-level keys: {sorted(unknown)}")
    measurement = config.get("measurement")
    if not isinstance(measurement, dict):
        fail("[measurement] is required")
    if "device" in measurement or "metrics" in measurement:
        fail("devices are declared with [[device]] tables, not inside [measurement]")
    for key, value in measurement.items():
        expected = MEASUREMENT_KEYS.get(key)
        if expected is None:
            fail(f"unknown key measurement.{key}")
        if not isinstance(value, expected) or isinstance(value, bool):
            fail(f"measurement.{key} has an invalid type")
    if measurement.get("mode") not in MODES:
        fail(f"measurement.mode must be one of {MODES}")
    if not measurement.get("calibration_file", "").strip():
        fail("measurement.calibration_file is required and must be a non-empty path")
    for key, low, high in (("interval_ms", 1, 4294967), ("repetitions", 1, 1000000),
                           ("sleep_seconds", 0, 86400), ("timeout_hours", 1, 1000)):
        if key in measurement and not low <= measurement[key] <= high:
            fail(f"measurement.{key} must be between {low} and {high}")
    if "compile_command" in measurement:
        if measurement["mode"] != "powercap-calibration":
            fail("measurement.compile_command is only valid in powercap-calibration mode")
        if not measurement["compile_command"].strip() or "\0" in measurement["compile_command"]:
            fail("measurement.compile_command must be a non-empty command without NUL characters")
    if measurement["mode"] == "powercap-calibration":
        if "powercap" in measurement:
            fail("measurement.powercap is not valid in powercap-calibration mode; use "
                 "powercap_min and powercap_max")
        if "interval_ms" in measurement:
            fail("measurement.interval_ms is not valid in powercap-calibration mode")
        for key in ("powercap_min", "powercap_max"):
            if key not in measurement or not 1 <= measurement[key] <= 2147483647:
                fail(f"measurement.{key} must be between 1 and 2147483647 in "
                     "powercap-calibration mode")
        if measurement["powercap_min"] > measurement["powercap_max"]:
            fail("measurement.powercap_min must not exceed measurement.powercap_max")
        if measurement["powercap_max"] - measurement["powercap_min"] + 1 >= 128:
            fail("the powercap range must contain fewer than 128 values")
    elif "powercap_min" in measurement or "powercap_max" in measurement:
        fail("measurement.powercap_min and powercap_max are only valid in "
             "powercap-calibration mode")
    measurement["powercap"] = parse_powercaps(measurement.get("powercap", -1), "measurement.powercap")
    if not 0 <= measurement.get("variance", 5) <= 1000:
        fail("measurement.variance must be between 0 and 1000")
    for key in ("language", "program"):
        if any(c in measurement.get(key, "") for c in ',\r\n"'):
            fail(f"measurement.{key} cannot contain CSV delimiters or newlines")

    base = config_path.parent
    devices, seen = [], set()
    for index, raw in enumerate(config.get("device", [])):
        where = f"device[{index}]"
        unknown = set(raw) - DEVICE_KEYS
        if unknown:
            fail(f"{where} has unknown keys: {sorted(unknown)}")
        name = raw.get("name")
        if not isinstance(name, str) or not IDENTIFIER.match(name):
            fail(f"{where}.name must be a C identifier")
        if name in seen:
            fail(f"duplicate device name {name!r}")
        seen.add(name)
        optional = raw.get("optional", False)
        if not isinstance(optional, bool):
            fail(f"{where}.optional must be a boolean")
        device = {"name": name, "sources": [], "include_dirs": [], "link": [],
                  "optional": optional}
        if "header" not in raw:
            fail(f"{where}.header is required")
        header = resolve_file(base, raw["header"], f"{where}.header")
        device["header"] = header
        device["include_dirs"].append(header.parent)
        if "lib" in raw:
            lib = resolve_file(base, raw["lib"], f"{where}.lib")
            if lib.suffix not in (".a", ".so", ".o", ".c"):
                fail(f"{where}.lib must be a .a, .so, .o or .c file")
            device["lib"] = lib
        for flag in raw.get("link", []):
            if not isinstance(flag, str) or not LINK_FLAG.match(flag):
                fail(f"{where}.link entries must look like \"-lname\" or \"-L/dir\"")
            device["link"].append(flag)
        for hook in ("setup", "teardown"):
            if hook in raw:
                device[hook] = parse_hook(raw[hook], f"{where}.{hook}")
        if "thermal" in raw:
            c_type, function = parse_signature(raw["thermal"], f"{where}.thermal")
            device["thermal"] = (c_type, function)
        if ("powercap_set" in raw) != ("powercap_restore" in raw):
            fail(f"{where}: powercap_set and powercap_restore must be declared together")
        if "powercap_set" in raw:
            device["powercap_set"] = parse_prototype(raw["powercap_set"], f"{where}.powercap_set",
                                                     "int", "int")
            device["powercap_restore"] = parse_prototype(
                raw["powercap_restore"], f"{where}.powercap_restore", "int", "")
        if "powercap_calibration" in raw:
            function = raw["powercap_calibration"]
            if not isinstance(function, str) or not IDENTIFIER.match(function):
                fail(f"{where}.powercap_calibration must be a C function name")
            device["powercap_calibration"] = function
        metrics, names = [], set()
        for m_index, metric in enumerate(raw.get("metric", [])):
            m_where = f"{where}.metric[{m_index}]"
            unknown = set(metric) - METRIC_KEYS
            if unknown:
                fail(f"{m_where} has unknown keys: {sorted(unknown)}")
            m_name = metric.get("name")
            if not isinstance(m_name, str) or not IDENTIFIER.match(m_name):
                fail(f"{m_where}.name must be a C identifier")
            if m_name in names:
                fail(f"duplicate metric {m_name!r} in device {name!r}")
            names.add(m_name)
            kind = metric.get("kind", "gauge")
            if kind not in KINDS:
                fail(f"{m_where}.kind must be one of {sorted(KINDS)}")
            unit = metric.get("unit", "")
            if not isinstance(unit, str) or not UNIT.match(unit):
                fail(f"{m_where}.unit contains unsupported characters")
            c_type, function = parse_signature(metric.get("read"), f"{m_where}.read")
            metrics.append({"name": m_name, "kind": kind, "unit": unit,
                            "type": c_type, "function": function,
                            "integer": c_type in INTEGER_TYPES})
        if not metrics:
            fail(f"{where} must declare at least one [[device.metric]]")
        device["metrics"] = metrics
        devices.append(device)
    if measurement["mode"] == "powercap-calibration" and not any(
            "powercap_calibration" in device for device in devices):
        fail("powercap-calibration mode requires a device declaring powercap_calibration")
    if measurement["mode"] == "temperature-calibration" and not any(
            "thermal" in device for device in devices):
        fail("temperature-calibration mode requires a device declaring thermal")
    return {"measurement": measurement, "devices": devices}


def c_string(value):
    def encode(byte):
        if 32 <= byte <= 126 and byte not in (34, 92, 63):
            return chr(byte)
        return f"\\{byte:03o}"

    return '"' + "".join(encode(byte) for byte in os.fsencode(value)) + '"'


def generate_device(device, source_name):
    name, metrics = device["name"], device["metrics"]
    lines = [
        f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
        "#include <stdint.h>", "#include <stddef.h>", '#include "gm_device.h"',
        f"#include {c_string(device['header'].name)}", "",
        "static GmSampler gm_sampler;", "static pthread_t gm_thread;",
        "static int gm_thread_running;", "static unsigned char gm_selected[%d];" % len(metrics),
        "static size_t gm_count;", "static int gm_overflow;",
        "static double gm_timestamps[GM_MAX_SAMPLES];",
    ]
    for metric in metrics:
        lines.append(f"static {metric['type']} gm_samples_{metric['name']}[GM_MAX_SAMPLES];")
    lines.append("")
    for number, metric in enumerate(metrics):
        lines += [
            f"static double gm_read_{number}(void) {{ return (double){metric['function']}(); }}",
            f"static double gm_sample_{number}(size_t row) "
            f"{{ return (double)gm_samples_{metric['name']}[row]; }}",
        ]
    lines += ["", "static const GmMetric gm_metrics[] = {"]
    for number, metric in enumerate(metrics):
        lines.append(f"    {{{c_string(metric['name'])}, {c_string(metric['unit'])}, "
                     f"{KINDS[metric['kind']]}, {int(metric['integer'])}, "
                     f"gm_read_{number}, gm_sample_{number}}},")
    lines += ["};", "",
              "/* One sample per interval; a last sample is taken once the stop is requested. */",
              "static void *gm_run(void *unused) {", "    (void)unused;", "    for (;;) {",
              "        int last = gm_sampler_stopping(&gm_sampler);",
              "        if (gm_count >= GM_MAX_SAMPLES) {",
              "            __atomic_store_n(&gm_overflow, 1, __ATOMIC_RELEASE);", "            break;",
              "        }", "        gm_timestamps[gm_count] = gm_sampler_elapsed_ms(&gm_sampler);"]
    for number, metric in enumerate(metrics):
        lines.append(f"        if (gm_selected[{number}]) "
                     f"gm_samples_{metric['name']}[gm_count] = {metric['function']}();")
    lines += ["        gm_count++;", "        if (last) break;", "        gm_sampler_wait(&gm_sampler);",
              "    }", "    return NULL;", "}", "",
              "static int gm_start(const unsigned char *selected, unsigned interval_ms,",
              "                    const struct timespec *origin) {",
              f"    for (size_t i = 0; i < {len(metrics)}; i++) gm_selected[i] = selected[i];",
              "    gm_count = 0;", "    __atomic_store_n(&gm_overflow, 0, __ATOMIC_RELEASE);",
              "    gm_sampler_init(&gm_sampler, interval_ms, origin);",
              "    if (pthread_create(&gm_thread, NULL, gm_run, NULL) != 0) {",
              "        gm_sampler_destroy(&gm_sampler);", "        return -1;", "    }",
              "    gm_thread_running = 1;", "    return 0;", "}", "",
              "static void gm_stop(void) {", "    if (!gm_thread_running) return;",
              "    gm_sampler_request_stop(&gm_sampler);", "    pthread_join(gm_thread, NULL);",
              "    gm_sampler_destroy(&gm_sampler);", "    gm_thread_running = 0;", "}", "",
              "static size_t gm_get_count(void) { return gm_count; }",
              "static double gm_get_timestamp(size_t row) { return gm_timestamps[row]; }",
              "static int gm_get_overflow(void) { return __atomic_load_n(&gm_overflow, __ATOMIC_ACQUIRE); }",
              ""]
    for hook in ("setup", "teardown"):
        if hook in device:
            kind, function = device[hook]
            body = f"return {function}() != 0;" if hook == "setup" and kind == "int" \
                else f"{function}(); return 0;" if hook == "setup" else f"{function}();"
            signature = "int gm_setup(void)" if hook == "setup" else "void gm_teardown(void)"
            lines += [f"static {signature} {{ {body} }}", ""]
    if "thermal" in device:
        lines += [f"static double gm_thermal(void) {{ return (double){device['thermal'][1]}(); }}", ""]
    if "powercap_set" in device:
        lines += [f"static int gm_powercap_set(int watts) {{ return {device['powercap_set']}(watts); }}",
                  f"static int gm_powercap_restore(void) {{ return {device['powercap_restore']}(); }}", ""]
    lines += [f"static const GmDevice gm_device = {{", f"    {c_string(name)}, {len(metrics)}, gm_metrics,",
              "    %s," % ("gm_setup" if "setup" in device else "NULL"),
              "    %s," % ("gm_teardown" if "teardown" in device else "NULL"),
              "    gm_start, gm_stop, gm_get_count, gm_get_timestamp, gm_get_overflow,",
              "    %s," % ("gm_thermal" if "thermal" in device else "NULL"),
              "    %s," % ("gm_powercap_set" if "powercap_set" in device else "NULL"),
              "    %s," % ("gm_powercap_restore" if "powercap_set" in device else "NULL"),
              "    %s," % device.get("powercap_calibration", "NULL"),
              "    %d" % int(device["optional"]),
              "};", "", f"const GmDevice *gm_device_{name}(void) {{ return &gm_device; }}", ""]
    return "\n".join(lines)


def generate_main(model, source_name):
    devices, measurement = model["devices"], model["measurement"]
    lines = [f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
             "#include <stdio.h>", '#include "gm_device.h"', ""]
    lines += [f"const GmDevice *gm_device_{d['name']}(void);" for d in devices]
    lines += ["", "/* One flag per metric; the metrics declared in the TOML are on by default. */",
              "static const GmFlag gm_flags[] = {"]
    flag_count = 0
    for d_index, device in enumerate(devices):
        for m_index, metric in enumerate(device["metrics"]):
            lines.append(f"    {{{c_string('--' + device['name'] + '.' + metric['name'])}, "
                         f"{d_index}, {m_index}, 1}},")
            flag_count += 1
    if not flag_count:
        lines.append('    {"", 0, 0, 0}')
    lines += ["};", "", "static const int gm_powercaps[] = {%s};" %
              ", ".join(str(cap) for cap in measurement["powercap"]), "",
              "static const GmConfig gm_defaults = {",
              f"    {c_string(measurement['mode'])}, "
              f"{c_string(measurement['command']) if 'command' in measurement else 'NULL'},",
              f"    {c_string(measurement.get('language', 'Unknown'))}, "
              f"{c_string(measurement.get('program', 'program'))},",
              f"    {c_string(measurement.get('output', 'measurements.csv'))}, "
              f"{measurement.get('interval_ms', 1)}u, {measurement.get('repetitions', 1)}u, "
              f"{measurement.get('sleep_seconds', 0)}u,",
              f"    {measurement.get('variance', 5)}u, "
              f"{c_string(measurement['calibration_file'])},",
              f"    gm_powercaps, {len(measurement['powercap'])},",
              f"    {measurement.get('timeout_hours', 0)}u, "
              f"{c_string(measurement['compile_command']) if 'compile_command' in measurement else 'NULL'}",
              "};", "", "int main(int argc, char **argv) {",
              f"    static GmDevice devices[{max(len(devices), 1)}];"]
    lines += [f"    devices[{i}] = *gm_device_{d['name']}();" for i, d in enumerate(devices)]
    if measurement["mode"] == "powercap-calibration":
        powercap_range = f'{measurement["powercap_min"]}-{measurement["powercap_max"]}'
        output = measurement.get("output", "powercap_calibration.csv")
        lines += [
            "    if (argc != 1) {",
            '        fprintf(stderr, "[ERROR] This configured powercap-calibration executable '
            'does not accept overrides.\\n");',
            "        return 1;",
            "    }",
            "    char *calibration_args[] = {",
            "        argv[0], \"--powercap-calibration\", \"-range\", "
            f"{c_string(powercap_range)}, \"-output\", {c_string(output)}, NULL",
            "    };",
            f"    return gm_main((int)(sizeof(calibration_args) / sizeof(calibration_args[0])) - 1, "
            f"calibration_args, &gm_defaults, devices, {len(devices)}, gm_flags, {flag_count});",
            "}", "",
        ]
    else:
        lines += [f"    return gm_main(argc, argv, &gm_defaults, devices, {len(devices)}, "
                  f"gm_flags, {flag_count});", "}", ""]
    return "\n".join(lines)


def weave(config_path, out_dir, max_samples, build=True):
    if tomllib is None:
        raise ValueError("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    config_path = config_path.resolve()
    with config_path.open("rb") as source:
        model = validate(tomllib.load(source), config_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    sources = [out_dir / "GreenMeasurer.c"]
    sources[0].write_text(generate_main(model, config_path.name))
    includes, objects, link = [INCLUDE_DIR], [], []
    for device in model["devices"]:
        path = out_dir / f"{device['name']}.c"
        path.write_text(generate_device(device, config_path.name))
        sources.append(path)
        includes += device["include_dirs"]
        if "lib" in device:
            objects.append(str(device["lib"]))
            if device["lib"].suffix == ".so":
                link.append(f"-Wl,-rpath,{device['lib'].parent}")
        link += device["link"]
    executable = out_dir / "greenmeasurer"
    command = [os.environ.get("CC", "gcc"), "-O2", "-Wall", "-pthread", "-D_GNU_SOURCE",
               f"-DGM_MAX_SAMPLES={max_samples}",
               f"-DGM_PROCESSOR={c_string(str(PROCESSOR))}"]
    command += [f"-I{path}" for path in dict.fromkeys(includes)]
    command += [str(path) for path in sources] + [str(RUNTIME_SOURCE)] + objects + link
    command += ["-lm", "-o", str(executable)]
    if build:
        subprocess.run(command, check=True)
    return executable, command


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", type=Path, help="TOML configuration file")
    parser.add_argument("-o", "--output-dir", type=Path,
                        help="directory for generated sources and the executable "
                             "(default: generated-<config name> next to the TOML file)")
    parser.add_argument("--max-samples", type=int, default=1000000,
                        help="static capacity of every sample array (default: 1000000)")
    parser.add_argument("--no-build", action="store_true",
                        help="generate the sources and print the compile command only")
    args = parser.parse_args()
    if args.max_samples < 1:
        parser.error("--max-samples must be positive")
    out_dir = args.output_dir or args.config.resolve().parent / f"generated-{args.config.stem}"
    try:
        executable, command = weave(args.config, out_dir, args.max_samples, not args.no_build)
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError:
        print("Error: compilation failed", file=sys.stderr)
        return 1
    if args.no_build:
        print(shlex.join(command))
    else:
        print(f"Built {executable}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
