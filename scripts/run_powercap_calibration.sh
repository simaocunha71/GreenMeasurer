#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <powercap-calibration.toml> <new-directory>" >&2
    echo "Generate the executable, then run <new-directory>/greenmeasurer separately." >&2
    exit 1
fi
python3 - "$root" "$1" <<'PY'
import sys
from pathlib import Path

root, path = map(Path, sys.argv[1:])
sys.path.insert(0, str(root / "scripts"))
from validate_config import tomllib, validate_config

if tomllib is None:
    sys.exit("Error: Python 3.11+ or the 'tomli' package is required for TOML parsing")
try:
    with path.open("rb") as source:
        config = tomllib.load(source)
    validate_config(config, path.resolve())
    if config["measurement"]["mode"] != "powercap-calibration":
        raise ValueError("measurement.mode must be 'powercap-calibration'")
except (OSError, ValueError, UnicodeError) as error:
    sys.exit(f"Error: {error}")
PY
python3 "$root/scripts/generate_config.py" "$1" --directory "$2"
