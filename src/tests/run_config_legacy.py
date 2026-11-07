#!/usr/bin/env python3
"""Private compatibility harness for tests of the measurement engine."""

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from run_config import OPTIONS, configured_arguments
from validate_config import tomllib, validate_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    for key, option in OPTIONS.items():
        parser.add_argument(option, dest=key, default=None)
    args = parser.parse_args()
    if tomllib is None:
        print("Error: Python 3.11+ or the 'tomli' package is required for TOML parsing", file=sys.stderr)
        return 1
    try:
        config_path = args.config.resolve()
        with config_path.open("rb") as source:
            config = tomllib.load(source)
        validate_config(config, config_path)
        measurement = config["measurement"]
        for key in OPTIONS:
            value = getattr(args, key)
            if value is not None:
                if key in ("interval_us", "powercap", "variance", "sleep_seconds",
                           "repetitions", "timeout_hours"):
                    try:
                        value = int(value, 10)
                    except ValueError as error:
                        raise ValueError(f"{OPTIONS[key]} must be an integer") from error
                measurement[key] = value
        validate_config(config, config_path)
        executable = ROOT / "src" / "tests" / "legacy-engine"
        os.execv(executable, [str(executable), *configured_arguments(config, config_path)])
    except (OSError, ValueError, UnicodeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
