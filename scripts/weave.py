#!/usr/bin/env python3
"""Weave a GreenMeasurer executable from TOML files that each declare one aspect.

An aspect groups the metrics of one energy-monitoring library under a
[measurement] table, a [device] table and [[metric]] entries. Metric aspects
generate sampling code; control aspects generate only their hooks. Aspects
that cannot be built are reported and omitted.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
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
PROCESSOR = (ROOT / "src" / "devices" / "linux" / "cpu" / "intel" /
             "processPowercapCalibrationCSV.py")

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
KINDS = {"counter": "GM_KIND_COUNTER", "gauge": "GM_KIND_GAUGE", "duration": "GM_KIND_DURATION"}
MODES = {"over-time", "aggregate"}
MEASUREMENT_KEYS = {"aspect", "mode", "setup", "teardown", "measurement_start",
                    "measurement_stop", "powercap_set", "powercap_restore",
                    "powercap_calibration"}
DEVICE_KEYS = {"lib", "header", "link"}
METRIC_KEYS = {"name", "measurement", "kind", "unit", "thermal", "calibration"}
PROTOTYPE = re.compile(
    r"^\s*(?P<type>[A-Za-z_][A-Za-z0-9_ ]*?)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"\s*\(\s*(?P<params>[A-Za-z0-9_ ]*)\)\s*$")


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


def parse_calibration(text, where):
    """Parse "<numeric type> <function>(unsigned)" and return (type, name)."""
    if not isinstance(text, str):
        fail(f"{where} must be a string such as \"double function(unsigned)\"")
    match = PROTOTYPE.match(text)
    c_type = " ".join(match["type"].split()) if match else ""
    params = " ".join(match["params"].split()) if match else ""
    if not match or c_type not in FLOAT_TYPES | INTEGER_TYPES or \
            params not in ("unsigned", "unsigned int"):
        fail(f"{where} must have the form \"<numeric type> <function>(unsigned)\"")
    return c_type, match["name"]


def resolve_file(base, value, where):
    if not isinstance(value, str) or not value:
        fail(f"{where} must be a non-empty path")
    return (base / value).resolve()


def validate(configs):
    """Return a normalised model of the configurations or raise ValueError.

    `configs` is a list of (parsed TOML, path) pairs; each file declares at
    most one aspect through [measurement], [device] and [[metric]] tables.
    """
    aspects, names, symbols, flags, lifecycle, reader_locks = [], set(), set(), set(), {}, {}
    modes = set()
    for config, config_path in configs:
        where = config_path.name
        if not isinstance(config, dict):
            fail(f"{where}: the configuration must be a TOML document")
        if not config:
            continue
        unknown = set(config) - {"measurement", "device", "metric"}
        if unknown:
            fail(f"{where}: unknown top-level keys: {sorted(unknown)}")
        measurement = config.get("measurement")
        if not isinstance(measurement, dict):
            fail(f"{where}: a [measurement] table is required")
        unknown = set(measurement) - MEASUREMENT_KEYS
        if unknown:
            fail(f"{where}: [measurement] has unknown keys: {sorted(unknown)}")
        device = config.get("device")
        if not isinstance(device, dict):
            fail(f"{where}: a [device] table is required")
        unknown = set(device) - DEVICE_KEYS
        if unknown:
            fail(f"{where}: [device] has unknown keys: {sorted(unknown)}")
        entries = config.get("metric", [])
        if not isinstance(entries, list) or not all(isinstance(entry, dict) for entry in entries):
            fail(f"{where}: metrics must be declared with [[metric]] tables")
        if len(entries) > 64:
            fail(f"{where}: at most 64 metrics per aspect are supported")

        name = measurement.get("aspect")
        if not isinstance(name, str) or not all(IDENTIFIER.fullmatch(part) for part in name.split(".")):
            fail(f"{where}: measurement.aspect must consist of identifiers separated by dots")
        if name == "help":
            fail(f"{where}: measurement.aspect conflicts with a built-in command-line flag")
        if name in names:
            fail(f"duplicate aspect name {name!r}")
        names.add(name)
        symbol = name.replace(".", "_")
        if symbol in symbols:
            fail(f"{where}: measurement.aspect collides with another generated C symbol")
        symbols.add(symbol)
        aspect = {"name": name, "symbol": symbol, "sources": [], "include_dirs": [], "link": [],
                  "source": config_path.name}

        base = config_path.parent
        if "header" not in device:
            fail(f"{where}: device.header is required")
        header = resolve_file(base, device["header"], f"{where}: device.header")
        aspect["header"] = header
        if header not in reader_locks:
            reader_locks[header] = f"gm_reader_lock_{len(reader_locks)}"
        aspect["reader_lock"] = reader_locks[header]
        aspect["include_dirs"].append(header.parent)
        if "lib" in device:
            lib = resolve_file(base, device["lib"], f"{where}: device.lib")
            if lib.suffix not in (".a", ".so", ".o", ".c"):
                fail(f"{where}: device.lib must be a .a, .so, .o or .c file")
            aspect["lib"] = lib
        link_flags = device.get("link", [])
        if not isinstance(link_flags, list):
            fail(f"{where}: device.link must be an array")
        for flag in link_flags:
            if not isinstance(flag, str) or not LINK_FLAG.match(flag):
                fail(f"{where}: device.link entries must look like \"-lname\" or \"-L/dir\"")
            aspect["link"].append(flag)

        for hook in ("setup", "teardown"):
            if hook in measurement:
                aspect[hook] = parse_hook(measurement[hook], f"{where}: measurement.{hook}")
        if ("measurement_start" in measurement) != ("measurement_stop" in measurement):
            fail(f"{where}: measurement_start and measurement_stop must be declared together")
        for hook in ("measurement_start", "measurement_stop"):
            if hook in measurement:
                aspect[hook] = parse_hook(measurement[hook], f"{where}: measurement.{hook}")
        if ("powercap_set" in measurement) != ("powercap_restore" in measurement):
            fail(f"{where}: powercap_set and powercap_restore must be declared together")
        if "powercap_set" in measurement:
            aspect["powercap_set"] = parse_prototype(
                measurement["powercap_set"], f"{where}: measurement.powercap_set", "int", "int")
            aspect["powercap_restore"] = parse_prototype(
                measurement["powercap_restore"], f"{where}: measurement.powercap_restore", "int", "")
        if "powercap_calibration" in measurement:
            function = measurement["powercap_calibration"]
            if not isinstance(function, str) or not IDENTIFIER.match(function):
                fail(f"{where}: measurement.powercap_calibration must be a C function name")
            aspect["powercap_calibration"] = function
        has_powercap = "powercap_set" in aspect or "powercap_calibration" in aspect

        mode = measurement.get("mode")
        if mode is not None and (not isinstance(mode, str) or mode not in MODES):
            fail(f"{where}: measurement.mode must be one of {sorted(MODES)}")

        if has_powercap:
            if entries or "measurement_start" in aspect:
                fail(f"{where}: powercap control aspects cannot declare metrics "
                     "or measurement callbacks")
            aspect["metrics"] = []
            if name in flags:
                fail(f"{where}: aspect {name!r} collides with another command-line flag")
            flags.add(name)
        else:
            if not entries:
                fail(f"{where}: at least one [[metric]] is required")
            if mode is None:
                fail(f"{where}: measurement.mode is required for metric aspects")
            metrics, cnames, thermal_metric = [], set(), -1
            for index, raw in enumerate(entries):
                metric_where = f"{where}: metric[{index}]"
                unknown = set(raw) - METRIC_KEYS
                if unknown:
                    fail(f"{metric_where} has unknown keys: {sorted(unknown)}")
                metric_name = raw.get("name")
                if not isinstance(metric_name, str) or \
                        not all(IDENTIFIER.fullmatch(part) for part in metric_name.split(".")):
                    fail(f"{metric_where}.name must consist of identifiers separated by dots")
                if metric_name == "help":
                    fail(f"{metric_where}.name conflicts with a built-in command-line flag")
                if metric_name in flags:
                    fail(f"{metric_where}.name collides with another command-line flag")
                flags.add(metric_name)
                cname = metric_name.replace(".", "_")
                if cname in cnames:
                    fail(f"{metric_where}.name collides with another generated C symbol")
                cnames.add(cname)
                kind = raw.get("kind", "gauge")
                if not isinstance(kind, str) or kind not in KINDS:
                    fail(f"{metric_where}.kind must be one of {sorted(KINDS)}")
                if kind == "duration" and "measurement_start" not in aspect:
                    fail(f"{metric_where}: kind = 'duration' requires measurement_start "
                         "and measurement_stop")
                unit = raw.get("unit", "")
                if not isinstance(unit, str) or not UNIT.match(unit):
                    fail(f"{metric_where}.unit contains unsupported characters")
                c_type, function = parse_signature(raw.get("measurement"),
                                                   f"{metric_where}.measurement")
                thermal = raw.get("thermal", False)
                if not isinstance(thermal, bool):
                    fail(f"{metric_where}.thermal must be a boolean")
                if thermal:
                    if kind != "gauge":
                        fail(f"{metric_where}.thermal requires kind = 'gauge'")
                    if thermal_metric >= 0:
                        fail(f"{where}: at most one thermal metric per aspect is supported")
                    if "calibration" not in raw:
                        fail(f"{metric_where}: thermal metrics require a calibration function")
                    aspect["thermal_calibration"] = parse_calibration(
                        raw["calibration"], f"{metric_where}.calibration")
                    thermal_metric = index
                elif "calibration" in raw:
                    fail(f"{metric_where}: only thermal metrics declare a calibration function")
                metrics.append({"name": metric_name, "cname": cname, "kind": kind, "unit": unit,
                                "type": c_type, "function": function,
                                "integer": c_type in INTEGER_TYPES})
            aspect["metrics"] = metrics
            aspect["thermal_metric"] = thermal_metric
            modes.add(mode)

        if "setup" in aspect:
            setup = aspect["setup"]
            teardown = aspect.get("teardown")
            if setup in lifecycle and lifecycle[setup] != teardown:
                fail(f"{where}: uses the same setup with a different teardown")
            lifecycle[setup] = teardown
            aspect["lifecycle"] = setup[1]
        aspects.append(aspect)
    if len(aspects) > 64:
        fail("at most 64 aspects are supported")
    if len(modes) > 1:
        fail(f"all metric aspects must declare the same mode; got {sorted(modes)}")
    return {"aspects": aspects, "mode": modes.pop() if modes else "aggregate"}


def c_string(value):
    def encode(byte):
        if 32 <= byte <= 126 and byte not in (34, 92, 63):
            return chr(byte)
        return f"\\{byte:03o}"

    return '"' + "".join(encode(byte) for byte in os.fsencode(value)) + '"'


def generate_aspect(aspect, mode, source_name):
    """Generate the C code of one aspect; identifiers are prefixed with the
    aspect symbol so that, in aggregate mode, every aspect can be woven into
    the main module."""
    name, metrics = aspect["name"], aspect["metrics"]
    p = f"gm_{aspect['symbol']}"
    sampling = bool(metrics) and mode == "over-time"
    if not metrics:
        lines = [
            f"/* Woven by scripts/weave.py from {source_name}; do not edit. */",
            '#include "gm_aspect.h"', f"#include {c_string(aspect['header'].name)}", "",
        ]
        for hook in ("setup", "teardown"):
            if hook in aspect:
                kind, function = aspect[hook]
                body = f"return {function}() != 0;" if hook == "setup" and kind == "int" \
                    else f"{function}(); return 0;" if hook == "setup" else f"{function}();"
                signature = f"int {p}_setup(void)" if hook == "setup" else f"void {p}_teardown(void)"
                lines += [f"static {signature} {{ {body} }}", ""]
        lines += [
            f"static const GmAspect {p}_aspect = {{",
            f"    {c_string(name)}, 0, NULL,",
            "    %s, %s, %s," % (
                f"{p}_setup" if "setup" in aspect else "NULL",
                f"{p}_teardown" if "teardown" in aspect else "NULL",
                c_string(aspect["lifecycle"]) if "lifecycle" in aspect else "NULL"),
            "    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, NULL,",
            "    %s, %s, %s" % (aspect.get("powercap_set", "NULL"),
                                 aspect.get("powercap_restore", "NULL"),
                                 aspect.get("powercap_calibration", "NULL")),
            "};", "",
            f"const GmAspect *gm_aspect_{aspect['symbol']}(void) {{ return &{p}_aspect; }}", "",
        ]
        return "\n".join(lines)
    lines = [
        f"/* Woven by scripts/weave.py from {source_name}; do not edit. */",
        "#include <stdint.h>", "#include <stddef.h>", '#include "gm_aspect.h"',
        f"#include {c_string(aspect['header'].name)}", "",
        f"extern pthread_mutex_t {aspect['reader_lock']};", "",
    ]
    if sampling:
        lines += [
            f"static GmSampler {p}_sampler;", f"static pthread_t {p}_thread;",
            f"static int {p}_thread_running;",
            "static unsigned char %s_selected[%d];" % (p, len(metrics)),
            f"static size_t {p}_count;", f"static int {p}_overflow;",
            f"static double {p}_timestamps[GM_MAX_SAMPLES];",
        ]
        for metric in metrics:
            lines.append(f"static {metric['type']} {p}_samples_{metric['cname']}[GM_MAX_SAMPLES];")
    lines.append("")
    for number, metric in enumerate(metrics):
        lines += [
            f"static {metric['type']} {p}_read_value_{number}(void) {{",
            f"    pthread_mutex_lock(&{aspect['reader_lock']});",
            f"    {metric['type']} value = {metric['function']}();",
            f"    pthread_mutex_unlock(&{aspect['reader_lock']});",
            "    return value;",
            "}",
            f"static double {p}_read_{number}(void) {{ return (double){p}_read_value_{number}(); }}",
        ]
        if sampling:
            lines.append(f"static double {p}_sample_{number}(size_t row) "
                         f"{{ return (double){p}_samples_{metric['cname']}[row]; }}")
    lines += ["", f"static const GmMetric {p}_metrics[] = {{"]
    for number, metric in enumerate(metrics):
        sample = f"{p}_sample_{number}" if sampling else "NULL"
        lines.append(f"    {{{c_string(metric['name'])}, {c_string(metric['unit'])}, "
                     f"{KINDS[metric['kind']]}, {int(metric['integer'])}, "
                     f"{p}_read_{number}, {sample}}},")
    lines += ["};", ""]
    if sampling:
        lines += [
              "/* One sample per interval; a last sample is taken once the stop is requested. */",
              f"static void *{p}_run(void *unused) {{", "    (void)unused;", "    for (;;) {",
              f"        int last = gm_sampler_stopping(&{p}_sampler);",
              f"        if ({p}_count >= GM_MAX_SAMPLES) {{",
              f"            __atomic_store_n(&{p}_overflow, 1, __ATOMIC_RELEASE);", "            break;",
              f"        }}", f"        {p}_timestamps[{p}_count] = gm_sampler_elapsed_ms(&{p}_sampler);"]
        for number, metric in enumerate(metrics):
            lines.append(f"        if ({p}_selected[{number}]) "
                         f"{p}_samples_{metric['cname']}[{p}_count] = {p}_read_value_{number}();")
        lines += [f"        {p}_count++;", "        if (last) break;",
              f"        gm_sampler_wait(&{p}_sampler);",
              "    }", "    return NULL;", "}", "",
              f"static int {p}_start(const unsigned char *selected, unsigned interval_ms,",
              "                    const struct timespec *origin) {",
              f"    for (size_t i = 0; i < {len(metrics)}; i++) {p}_selected[i] = selected[i];",
              f"    {p}_count = 0;", f"    __atomic_store_n(&{p}_overflow, 0, __ATOMIC_RELEASE);",
              f"    gm_sampler_init(&{p}_sampler, interval_ms, origin);",
              f"    if (pthread_create(&{p}_thread, NULL, {p}_run, NULL) != 0) {{",
              f"        gm_sampler_destroy(&{p}_sampler);", "        return -1;", "    }",
              f"    {p}_thread_running = 1;", "    return 0;", "}", "",
              f"static void {p}_stop(void) {{", f"    if (!{p}_thread_running) return;",
              f"    gm_sampler_request_stop(&{p}_sampler);", f"    pthread_join({p}_thread, NULL);",
              f"    gm_sampler_destroy(&{p}_sampler);", f"    {p}_thread_running = 0;", "}", "",
              f"static size_t {p}_get_count(void) {{ return {p}_count; }}",
              f"static double {p}_get_timestamp(size_t row) {{ return {p}_timestamps[row]; }}",
              f"static int {p}_get_overflow(void) "
              f"{{ return __atomic_load_n(&{p}_overflow, __ATOMIC_ACQUIRE); }}",
              ""]
    for hook in ("setup", "teardown"):
        if hook in aspect:
            kind, function = aspect[hook]
            body = f"return {function}() != 0;" if hook == "setup" and kind == "int" \
                else f"{function}(); return 0;" if hook == "setup" else f"{function}();"
            signature = f"int {p}_setup(void)" if hook == "setup" else f"void {p}_teardown(void)"
            lines += [f"static {signature} {{ {body} }}", ""]
    for hook in ("measurement_start", "measurement_stop"):
        if hook in aspect:
            kind, function = aspect[hook]
            body = f"int result = {function}();" if kind == "int" else f"{function}(); int result = 0;"
            lines += [f"static int {p}_{hook}(void) {{",
                      f"    pthread_mutex_lock(&{aspect['reader_lock']});",
                      f"    {body}",
                      f"    pthread_mutex_unlock(&{aspect['reader_lock']});",
                      "    return result;", "}", ""]
    thermal_metric = aspect.get("thermal_metric", -1)
    if thermal_metric >= 0:
        calibration_type, calibration_function = aspect["thermal_calibration"]
        lines += [f"static double {p}_thermal(void) {{ return {p}_read_{thermal_metric}(); }}",
                  f"static double {p}_thermal_calibrate(unsigned seconds) "
                  f"{{ return (double){calibration_function}(seconds); }}", ""]
    if "powercap_set" in aspect:
        lines += [f"static int {p}_powercap_set(int watts) {{ return {aspect['powercap_set']}(watts); }}",
                  f"static int {p}_powercap_restore(void) {{ return {aspect['powercap_restore']}(); }}", ""]
    lines += [f"static const GmAspect {p}_aspect = {{",
              f"    {c_string(name)}, {len(metrics)}, {p}_metrics,",
              "    %s," % (f"{p}_setup" if "setup" in aspect else "NULL"),
              "    %s," % (f"{p}_teardown" if "teardown" in aspect else "NULL"),
              "    %s," % (c_string(aspect["lifecycle"]) if "lifecycle" in aspect else "NULL"),
              "    %s, %s," % (f"{p}_measurement_start" if "measurement_start" in aspect else "NULL",
                               f"{p}_measurement_stop" if "measurement_stop" in aspect else "NULL"),
              f"    {p}_start, {p}_stop, {p}_get_count, {p}_get_timestamp, {p}_get_overflow,"
              if sampling else "    NULL, NULL, NULL, NULL, NULL,",
              "    %s, %d, %s," % (f"{p}_thermal" if thermal_metric >= 0 else "NULL", thermal_metric,
                                   f"{p}_thermal_calibrate" if thermal_metric >= 0 else "NULL"),
              "    %s," % (f"{p}_powercap_set" if "powercap_set" in aspect else "NULL"),
              "    %s," % (f"{p}_powercap_restore" if "powercap_set" in aspect else "NULL"),
              "    %s" % aspect.get("powercap_calibration", "NULL"),
              "};", "",
              f"const GmAspect *gm_aspect_{aspect['symbol']}(void) {{ return &{p}_aspect; }}", ""]
    return "\n".join(lines)

def generate_main(model, source_name):
    aspects = model["aspects"]
    if not aspects:
        return "\n".join([
            f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
            "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
            "#include <sys/wait.h>", "",
            "int main(int argc, char **argv) {",
            '    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {',
            '        printf("Usage: %s -command <cmd> [-n_times <N>]\\n", argv[0]);',
            "        return 0;",
            "    }",
            "    const char *command = NULL;",
            "    long repetitions = 1;",
            "    int valid = argc == 3 || argc == 5;",
            "    for (int i = 1; valid && i + 1 < argc; i += 2) {",
            '        if (!strcmp(argv[i], "-command")) {',
            "            command = argv[i + 1];",
            '        } else if (!strcmp(argv[i], "-n_times")) {',
            "            char *end;",
            "            repetitions = strtol(argv[i + 1], &end, 10);",
            "            if (!argv[i + 1][0] || *end || repetitions < 1 || repetitions > 1000000)",
            "                valid = 0;",
            "        } else {",
            "            valid = 0;",
            "        }",
            "    }",
            "    if (!valid || !command || !command[0]) {",
            '        fprintf(stderr, "[ERROR] A command is required: %s -command <cmd> [-n_times <N>]\\n", argv[0]);',
            "        return 1;",
            "    }",
            "    for (long run = 0; run < repetitions; run++) {",
            "        int status = system(command);",
            "        if (status == -1) {",
            '            perror("[ERROR] Failed to execute command");',
            "            return 1;",
            "        }",
            "        if (!WIFEXITED(status)) {",
            '            fprintf(stderr, "[ERROR] Command terminated abnormally.\\n");',
            "            return 1;",
            "        }",
            "        int code = WEXITSTATUS(status);",
            "        if (code != 0) {",
            '            fprintf(stderr, "[ERROR] Command exited with status %d.\\n", code);',
            "            return code;",
            "        }",
            "    }",
            "    return 0;",
            "}", "",
        ])
    lines = [f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
             "#include <stdio.h>", '#include "gm_aspect.h"', ""]
    lines += [f"pthread_mutex_t {lock} = PTHREAD_MUTEX_INITIALIZER;"
              for lock in dict.fromkeys(d["reader_lock"] for d in aspects)]
    lines.append("")
    lines += [f"const GmAspect *gm_aspect_{d['symbol']}(void);" for d in aspects]
    lines += ["", "/* One flag per metric; control aspects get one flag. */",
              "static const GmFlag gm_flags[] = {"]
    flag_count = 0
    for d_index, aspect in enumerate(aspects):
        if aspect["metrics"]:
            for m_index, metric in enumerate(aspect["metrics"]):
                lines.append(f"    {{{c_string('--' + metric['name'])}, {d_index}, {m_index}}},")
                flag_count += 1
        else:
            lines.append(f"    {{{c_string('--' + aspect['name'])}, {d_index}, 0}},")
            flag_count += 1
    lines += ["};", "", "int main(int argc, char **argv) {",
              f"    static GmAspect aspects[{max(len(aspects), 1)}];"]
    lines += [f"    aspects[{i}] = *gm_aspect_{d['symbol']}();" for i, d in enumerate(aspects)]
    lines += [f"    return gm_main(argc, argv, aspects, {len(aspects)}, "
              f"gm_flags, {flag_count}, {c_string(model['mode'])});", "}", ""]
    return "\n".join(lines)


def build_command(model, source_name, out_dir, max_samples):
    out_dir.mkdir(parents=True, exist_ok=True)
    sources = [out_dir / "GreenMeasurer.c"]
    # In aggregate mode no additional C module is generated: every aspect is
    # woven into the main module. Over-time generates one module per aspect.
    merged = bool(model["aspects"]) and model["mode"] == "aggregate"
    parts = [generate_main(model, source_name)]
    includes, objects, link = [INCLUDE_DIR], [], []
    for aspect in model["aspects"]:
        code = generate_aspect(aspect, model["mode"], aspect.get("source", source_name))
        if merged:
            parts.append(code)
        else:
            path = out_dir / f"{aspect['symbol']}.c"
            path.write_text(code)
            sources.append(path)
        includes += aspect["include_dirs"]
        if "lib" in aspect:
            objects.append(str(aspect["lib"]))
            if aspect["lib"].suffix == ".so":
                link.append(f"-Wl,-rpath,{aspect['lib'].parent}")
        link += aspect["link"]
    sources[0].write_text("\n".join(parts))
    executable = out_dir / "greenmeasurer"
    command = [os.environ.get("CC", "gcc"), "-O2", "-Wall"]
    if model["aspects"]:
        command += ["-pthread", "-D_GNU_SOURCE", f"-DGM_MAX_SAMPLES={max_samples}",
                    f"-DGM_PROCESSOR={c_string(str(PROCESSOR))}"]
    command += [f"-I{path}" for path in dict.fromkeys(includes)]
    command += [str(path) for path in sources]
    if model["aspects"]:
        command += [str(RUNTIME_SOURCE)] + list(dict.fromkeys(objects)) + link + ["-lm"]
    command += ["-o", str(executable)]
    return executable, command


def weave(config_paths, out_dir, max_samples, build=True):
    if tomllib is None:
        raise ValueError("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    configs = []
    for config_path in config_paths:
        config_path = config_path.resolve()
        with config_path.open("rb") as source:
            configs.append((tomllib.load(source), config_path))
    model = validate(configs)
    source_name = ", ".join(dict.fromkeys(path.name for _, path in configs))

    available = []
    for aspect in model["aspects"]:
        missing = [str(aspect[key]) for key in ("header", "lib")
                   if key in aspect and not aspect[key].is_file()]
        if missing:
            print(f"[WARNING] Skipping aspect '{aspect['name']}': missing file(s): "
                  f"{', '.join(missing)}", file=sys.stderr)
        else:
            available.append(aspect)

    if build and available:
        with tempfile.TemporaryDirectory(prefix="greenmeasurer-weave-") as directory:
            scratch = Path(directory)
            _, command = build_command({"aspects": available, "mode": model["mode"]}, source_name, scratch, max_samples)
            attempt = subprocess.run(command, capture_output=True, text=True)
            if attempt.returncode < 0:
                raise subprocess.CalledProcessError(attempt.returncode, command, stderr=attempt.stderr)
            if attempt.returncode != 0:
                stub = scratch / "runtime-check.c"
                stub.write_text("int main(void) { return 0; }\n")
                baseline = [os.environ.get("CC", "gcc"), "-O2", "-Wall", "-pthread",
                            "-D_GNU_SOURCE", f"-I{INCLUDE_DIR}", str(stub),
                            str(RUNTIME_SOURCE), "-lm", "-o", str(scratch / "runtime-check")]
                check = subprocess.run(baseline, capture_output=True, text=True)
                if check.returncode:
                    print(check.stderr, file=sys.stderr, end="")
                    raise subprocess.CalledProcessError(check.returncode, baseline)

                accepted = []
                pending = available
                failures = {}
                while pending:
                    deferred = []
                    for aspect in pending:
                        names = {item["name"] for item in accepted}
                        names.add(aspect["name"])
                        candidates = [item for item in available if item["name"] in names]
                        _, trial = build_command({"aspects": candidates, "mode": model["mode"]}, source_name, scratch, max_samples)
                        result = subprocess.run(trial, capture_output=True, text=True)
                        if result.returncode < 0:
                            raise subprocess.CalledProcessError(result.returncode, trial, stderr=result.stderr)
                        if result.returncode:
                            failures[aspect["name"]] = result.stderr
                            deferred.append(aspect)
                        else:
                            accepted.append(aspect)
                    if len(deferred) == len(pending):
                        break
                    pending = deferred
                for aspect in pending:
                    print(f"[WARNING] Skipping aspect '{aspect['name']}': compilation or linking failed.",
                          file=sys.stderr)
                    print(failures[aspect["name"]], file=sys.stderr, end="")
                accepted_names = {aspect["name"] for aspect in accepted}
                available = [aspect for aspect in available if aspect["name"] in accepted_names]

    executable, command = build_command({"aspects": available, "mode": model["mode"]}, source_name, out_dir, max_samples)
    if build:
        subprocess.run(command, check=True)
    return executable, command


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", type=Path, nargs="+",
                        help="TOML aspect files; each declares at most one aspect")
    parser.add_argument("-o", "--output-dir", type=Path,
                        help="directory for generated sources and the executable "
                             "(default: generated-<first config name> next to that TOML file)")
    parser.add_argument("--max-samples", type=int, default=1000000,
                        help="static capacity of every sample array (default: 1000000)")
    parser.add_argument("--no-build", action="store_true",
                        help="generate the sources and print the compile command only")
    args = parser.parse_args()
    if args.max_samples < 1:
        parser.error("--max-samples must be positive")
    first = args.config[0]
    out_dir = args.output_dir or first.resolve().parent / f"generated-{first.stem}"
    try:
        executable, command = weave(args.config, out_dir, args.max_samples, not args.no_build)
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr, file=sys.stderr, end="")
        print("Error: compilation failed", file=sys.stderr)
        return 1
    if args.no_build:
        print(shlex.join(command))
    else:
        print(f"Built {executable}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
