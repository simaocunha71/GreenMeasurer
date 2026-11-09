#!/usr/bin/env python3
"""Translate validated TOML measurement settings into engine arguments."""

import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OPTIONS = {
    "command": "-command",
    "language": "-language",
    "program": "-program",
    "output": "-output",
    "calibration_file": "-calib-file",
    "interval_us": "-interval-us",
    "powercap": "-powercap",
    "variance": "-variance",
    "sleep_seconds": "-sleep-secs",
    "repetitions": "-n_times",
    "timeout_hours": "-time_out_limit",
}


def configured_arguments(config, config_path):
    measurement = config["measurement"]
    if measurement["mode"] == "temperature-calibration":
        command = ["--temperature-calibration"]
        for key, option in (("calibration_file", "-calib-file"), ("sleep_seconds", "-sleep-secs")):
            if key in measurement:
                command.extend((option, str(measurement[key])))
        return command
    if config.get("external_metrics"):
        raise ValueError("external_metrics require generation with scripts/generate_config.py")
    selected = config["metrics"]["select"]
    if "command" not in measurement:
        raise ValueError("measurement.command is required for execution")
    for key, limit in (("command", 499), ("language", 127), ("program", 127),
                       ("output", 511), ("calibration_file", 511)):
        if key in measurement and len(os.fsencode(measurement[key])) > limit:
            raise ValueError(f"measurement.{key} exceeds {limit} bytes")
    for key in ("language", "program"):
        if key in measurement and any(c in measurement[key] for c in ',\r\n"'):
            raise ValueError(f"measurement.{key} cannot contain CSV delimiters or newlines")

    calibrating = measurement["mode"] == "powercap-calibration"
    command = ["--configured-standard" if calibrating else f'--configured-{measurement["mode"]}']
    for key, option in OPTIONS.items():
        if key in measurement:
            value = measurement[key] * 3600 if key == "timeout_hours" else measurement[key]
            if key == "powercap":
                value = ",".join(str(cap) for cap in value) if isinstance(value, list) else value
            command.extend(("-timeout-seconds" if key == "timeout_hours" else option, str(value)))
    if "compile_command" in measurement:
        command.extend(("-compile_cmd", measurement["compile_command"]))
    if calibrating:
        caps = [-1, *range(measurement["powercap_min"], measurement["powercap_max"] + 1)]
        command.extend(("-powercap", ",".join(map(str, caps)), "--powercap-report"))
    adapters = {item["name"]: item for item in config.get("adapters", [])}
    if selected == "all":
        command.append("--configured-all")
        for alias, adapter in adapters.items():
            path = (config_path.parent / adapter["path"]).resolve()
            command.extend(("--configured-adapter", alias, str(path)))
        return command
    for name in selected:
        if name.startswith("system."):
            command.extend(("--configured-system", name))
        elif name.startswith(("cpu.", "gpu.")):
            command.extend(("--configured-backend", name))
        else:
            _, alias, metric = name.split(".")
            path = (config_path.parent / adapters[alias]["path"]).resolve()
            command.extend(("--configured-external", alias, str(path), metric))
    return command


if __name__ == "__main__":
    print("Error: this module is internal; use scripts/generate_config.py to build "
          "an executable from a TOML file.", file=sys.stderr)
    sys.exit(2)
