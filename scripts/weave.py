#!/usr/bin/env python3
"""Weave a GreenMeasurer executable from a TOML file that declares aspects.

Metric aspects generate sampling code; control aspects generate only their hooks.
Aspects that cannot be built are reported and omitted.
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
KINDS = {"counter": "GM_KIND_COUNTER", "gauge": "GM_KIND_GAUGE", "duration": "GM_KIND_DURATION"}
ASPECT_KEYS = {"name", "lib", "header", "link", "setup", "teardown", "read", "kind", "unit",
               "thermal", "powercap_set", "powercap_restore", "powercap_calibration",
               "measurement_start", "measurement_stop"}
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


def resolve_file(base, value, where):
    if not isinstance(value, str) or not value:
        fail(f"{where} must be a non-empty path")
    return (base / value).resolve()


def validate(config, config_path):
    """Return a normalised model of the configuration or raise ValueError."""
    unknown = set(config) - {"aspect"}
    if unknown:
        fail(f"unknown top-level keys: {sorted(unknown)}")
    base = config_path.parent
    aspects, seen, symbols, lifecycle, reader_locks = [], set(), set(), {}, {}
    entries = config.get("aspect", [])
    if not isinstance(entries, list):
        fail("aspects must be declared with [[aspect]] tables")
    if len(entries) > 64:
        fail("at most 64 aspects are supported")
    for index, raw in enumerate(entries):
        where = f"aspect[{index}]"
        if not isinstance(raw, dict):
            fail(f"{where} must be a table")
        unknown = set(raw) - ASPECT_KEYS
        if unknown:
            fail(f"{where} has unknown keys: {sorted(unknown)}")
        name = raw.get("name")
        if not isinstance(name, str) or not all(IDENTIFIER.fullmatch(part) for part in name.split(".")):
            fail(f"{where}.name must consist of identifiers separated by dots")
        if name in {"help", "aggregate"}:
            fail(f"{where}.name conflicts with a built-in command-line flag")
        if name in seen:
            fail(f"duplicate aspect name {name!r}")
        seen.add(name)
        symbol = name.replace(".", "_")
        if symbol in symbols:
            fail(f"{where}.name collides with another generated C symbol")
        symbols.add(symbol)
        aspect = {"name": name, "symbol": symbol, "sources": [], "include_dirs": [], "link": []}
        if "header" not in raw:
            fail(f"{where}.header is required")
        header = resolve_file(base, raw["header"], f"{where}.header")
        aspect["header"] = header
        if header not in reader_locks:
            reader_locks[header] = f"gm_reader_lock_{len(reader_locks)}"
        aspect["reader_lock"] = reader_locks[header]
        aspect["include_dirs"].append(header.parent)
        if "lib" in raw:
            lib = resolve_file(base, raw["lib"], f"{where}.lib")
            if lib.suffix not in (".a", ".so", ".o", ".c"):
                fail(f"{where}.lib must be a .a, .so, .o or .c file")
            aspect["lib"] = lib
        link_flags = raw.get("link", [])
        if not isinstance(link_flags, list):
            fail(f"{where}.link must be an array")
        for flag in link_flags:
            if not isinstance(flag, str) or not LINK_FLAG.match(flag):
                fail(f"{where}.link entries must look like \"-lname\" or \"-L/dir\"")
            aspect["link"].append(flag)
        for hook in ("setup", "teardown"):
            if hook in raw:
                aspect[hook] = parse_hook(raw[hook], f"{where}.{hook}")
        if ("measurement_start" in raw) != ("measurement_stop" in raw):
            fail(f"{where}: measurement_start and measurement_stop must be declared together")
        for hook in ("measurement_start", "measurement_stop"):
            if hook in raw:
                aspect[hook] = parse_hook(raw[hook], f"{where}.{hook}")
        thermal = raw.get("thermal", False)
        if not isinstance(thermal, bool):
            fail(f"{where}.thermal must be a boolean")
        if ("powercap_set" in raw) != ("powercap_restore" in raw):
            fail(f"{where}: powercap_set and powercap_restore must be declared together")
        if "powercap_set" in raw:
            aspect["powercap_set"] = parse_prototype(raw["powercap_set"], f"{where}.powercap_set",
                                                     "int", "int")
            aspect["powercap_restore"] = parse_prototype(
                raw["powercap_restore"], f"{where}.powercap_restore", "int", "")
        if "powercap_calibration" in raw:
            function = raw["powercap_calibration"]
            if not isinstance(function, str) or not IDENTIFIER.match(function):
                fail(f"{where}.powercap_calibration must be a C function name")
            aspect["powercap_calibration"] = function
        has_powercap = "powercap_set" in aspect or "powercap_calibration" in aspect
        if "read" not in raw and has_powercap:
            if any(key in raw for key in ("kind", "unit", "thermal", "measurement_start",
                                          "measurement_stop")):
                fail(f"{where}: powercap control aspects cannot declare metric fields")
            aspect["metrics"] = []
        else:
            if has_powercap:
                fail(f"{where}: powercap hooks must be declared in a separate control aspect")
            kind = raw.get("kind", "gauge")
            if not isinstance(kind, str) or kind not in KINDS:
                fail(f"{where}.kind must be one of {sorted(KINDS)}")
            if kind == "duration" and "measurement_start" not in aspect:
                fail(f"{where}.kind = 'duration' requires measurement_start and measurement_stop")
            unit = raw.get("unit", "")
            if not isinstance(unit, str) or not UNIT.match(unit):
                fail(f"{where}.unit contains unsupported characters")
            c_type, function = parse_signature(raw.get("read"), f"{where}.read")
            if thermal:
                if kind != "gauge":
                    fail(f"{where}.thermal requires kind = 'gauge'")
                aspect["thermal"] = (c_type, function)
            aspect["metrics"] = [{"name": symbol, "kind": kind, "unit": unit,
                                  "type": c_type, "function": function,
                                  "integer": c_type in INTEGER_TYPES}]
        if "setup" in aspect:
            setup = aspect["setup"]
            teardown = aspect.get("teardown")
            if setup in lifecycle and lifecycle[setup] != teardown:
                fail(f"{where} uses the same setup with a different teardown")
            lifecycle[setup] = teardown
            aspect["lifecycle"] = setup[1]
        aspects.append(aspect)
    return {"aspects": aspects}


def c_string(value):
    def encode(byte):
        if 32 <= byte <= 126 and byte not in (34, 92, 63):
            return chr(byte)
        return f"\\{byte:03o}"

    return '"' + "".join(encode(byte) for byte in os.fsencode(value)) + '"'


def generate_aspect(aspect, source_name):
    name, metrics = aspect["name"], aspect["metrics"]
    if not metrics:
        lines = [
            f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
            '#include "gm_aspect.h"', f"#include {c_string(aspect['header'].name)}", "",
        ]
        for hook in ("setup", "teardown"):
            if hook in aspect:
                kind, function = aspect[hook]
                body = f"return {function}() != 0;" if hook == "setup" and kind == "int" \
                    else f"{function}(); return 0;" if hook == "setup" else f"{function}();"
                signature = "int gm_setup(void)" if hook == "setup" else "void gm_teardown(void)"
                lines += [f"static {signature} {{ {body} }}", ""]
        lines += [
            "static const GmAspect gm_aspect = {",
            f"    {c_string(name)}, 0, NULL,",
            "    %s, %s, %s," % (
                "gm_setup" if "setup" in aspect else "NULL",
                "gm_teardown" if "teardown" in aspect else "NULL",
                c_string(aspect["lifecycle"]) if "lifecycle" in aspect else "NULL"),
            "    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,",
            "    %s, %s, %s" % (aspect.get("powercap_set", "NULL"),
                                 aspect.get("powercap_restore", "NULL"),
                                 aspect.get("powercap_calibration", "NULL")),
            "};", "", f"const GmAspect *gm_aspect_{aspect['symbol']}(void) {{ return &gm_aspect; }}", "",
        ]
        return "\n".join(lines)
    lines = [
        f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
        "#include <stdint.h>", "#include <stddef.h>", '#include "gm_aspect.h"',
        f"#include {c_string(aspect['header'].name)}", "",
        f"extern pthread_mutex_t {aspect['reader_lock']};", "",
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
            f"static {metric['type']} gm_read_value_{number}(void) {{",
            f"    pthread_mutex_lock(&{aspect['reader_lock']});",
            f"    {metric['type']} value = {metric['function']}();",
            f"    pthread_mutex_unlock(&{aspect['reader_lock']});",
            "    return value;",
            "}",
            f"static double gm_read_{number}(void) {{ return (double)gm_read_value_{number}(); }}",
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
                     f"gm_samples_{metric['name']}[gm_count] = gm_read_value_{number}();")
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
        if hook in aspect:
            kind, function = aspect[hook]
            body = f"return {function}() != 0;" if hook == "setup" and kind == "int" \
                else f"{function}(); return 0;" if hook == "setup" else f"{function}();"
            signature = "int gm_setup(void)" if hook == "setup" else "void gm_teardown(void)"
            lines += [f"static {signature} {{ {body} }}", ""]
    for hook in ("measurement_start", "measurement_stop"):
        if hook in aspect:
            kind, function = aspect[hook]
            body = f"int result = {function}();" if kind == "int" else f"{function}(); int result = 0;"
            lines += [f"static int gm_{hook}(void) {{",
                      f"    pthread_mutex_lock(&{aspect['reader_lock']});",
                      f"    {body}",
                      f"    pthread_mutex_unlock(&{aspect['reader_lock']});",
                      "    return result;", "}", ""]
    if "thermal" in aspect:
        lines += ["static double gm_thermal(void) { return gm_read_0(); }", ""]
    if "powercap_set" in aspect:
        lines += [f"static int gm_powercap_set(int watts) {{ return {aspect['powercap_set']}(watts); }}",
                  f"static int gm_powercap_restore(void) {{ return {aspect['powercap_restore']}(); }}", ""]
    lines += [f"static const GmAspect gm_aspect = {{", f"    {c_string(name)}, {len(metrics)}, gm_metrics,",
              "    %s," % ("gm_setup" if "setup" in aspect else "NULL"),
              "    %s," % ("gm_teardown" if "teardown" in aspect else "NULL"),
              "    %s," % (c_string(aspect["lifecycle"]) if "lifecycle" in aspect else "NULL"),
              "    %s, %s," % ("gm_measurement_start" if "measurement_start" in aspect else "NULL",
                               "gm_measurement_stop" if "measurement_stop" in aspect else "NULL"),
              "    gm_start, gm_stop, gm_get_count, gm_get_timestamp, gm_get_overflow,",
              "    %s," % ("gm_thermal" if "thermal" in aspect else "NULL"),
              "    %s," % ("gm_powercap_set" if "powercap_set" in aspect else "NULL"),
              "    %s," % ("gm_powercap_restore" if "powercap_set" in aspect else "NULL"),
              "    %s" % aspect.get("powercap_calibration", "NULL"),
              "};", "", f"const GmAspect *gm_aspect_{aspect['symbol']}(void) {{ return &gm_aspect; }}", ""]
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
            '        printf("Usage: %s -command <cmd>\\n", argv[0]);',
            "        return 0;",
            "    }",
            '    if (argc != 3 || strcmp(argv[1], "-command") || !argv[2][0]) {',
            '        fprintf(stderr, "[ERROR] A command is required: %s -command <cmd>\\n", argv[0]);',
            "        return 1;",
            "    }",
            "    int status = system(argv[2]);",
            "    if (status == -1) {",
            '        perror("[ERROR] Failed to execute command");',
            "        return 1;",
            "    }",
            "    if (WIFEXITED(status)) {",
            "        int code = WEXITSTATUS(status);",
            '        if (code != 0) fprintf(stderr, "[ERROR] Command exited with status %d.\\n", code);',
            "        return code;",
            "    }",
            '    fprintf(stderr, "[ERROR] Command terminated abnormally.\\n");',
            "    return 1;",
            "}", "",
        ])
    lines = [f"/* Generated by scripts/weave.py from {source_name}; do not edit. */",
             "#include <stdio.h>", '#include "gm_aspect.h"', ""]
    lines += [f"pthread_mutex_t {lock} = PTHREAD_MUTEX_INITIALIZER;"
              for lock in dict.fromkeys(d["reader_lock"] for d in aspects)]
    lines.append("")
    lines += [f"const GmAspect *gm_aspect_{d['symbol']}(void);" for d in aspects]
    lines += ["", "/* One flag per aspect. */",
              "static const GmFlag gm_flags[] = {"]
    flag_count = 0
    for d_index, aspect in enumerate(aspects):
        lines.append(f"    {{{c_string('--' + aspect['name'])}, {d_index}, 0}},")
        flag_count += 1
    lines += ["};", "", "int main(int argc, char **argv) {",
              f"    static GmAspect aspects[{max(len(aspects), 1)}];"]
    lines += [f"    aspects[{i}] = *gm_aspect_{d['symbol']}();" for i, d in enumerate(aspects)]
    lines += [f"    return gm_main(argc, argv, aspects, {len(aspects)}, "
              f"gm_flags, {flag_count});", "}", ""]
    return "\n".join(lines)


def build_command(model, config_path, out_dir, max_samples):
    out_dir.mkdir(parents=True, exist_ok=True)
    sources = [out_dir / "GreenMeasurer.c"]
    sources[0].write_text(generate_main(model, config_path.name))
    includes, objects, link = [INCLUDE_DIR], [], []
    for aspect in model["aspects"]:
        path = out_dir / f"{aspect['symbol']}.c"
        path.write_text(generate_aspect(aspect, config_path.name))
        sources.append(path)
        includes += aspect["include_dirs"]
        if "lib" in aspect:
            objects.append(str(aspect["lib"]))
            if aspect["lib"].suffix == ".so":
                link.append(f"-Wl,-rpath,{aspect['lib'].parent}")
        link += aspect["link"]
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


def weave(config_path, out_dir, max_samples, build=True):
    if tomllib is None:
        raise ValueError("Python 3.11+ or the 'tomli' package is required for TOML parsing")
    config_path = config_path.resolve()
    with config_path.open("rb") as source:
        model = validate(tomllib.load(source), config_path)

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
            _, command = build_command({"aspects": available}, config_path, scratch, max_samples)
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
                        _, trial = build_command({"aspects": candidates}, config_path, scratch, max_samples)
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

    executable, command = build_command({"aspects": available}, config_path, out_dir, max_samples)
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
