#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <new-directory> <aspect.toml> [aspect.toml ...]" >&2
    echo "Generate the executable, then run it with --powercap-calibration and explicit options." >&2
    exit 1
fi
directory=$1
shift
calibration_aspects=$(python3 - "$root" "$@" <<'PY'
import sys
from pathlib import Path

root = Path(sys.argv[1])
paths = [Path(arg) for arg in sys.argv[2:]]
sys.path.insert(0, str(root / "scripts"))
from weave import tomllib, validate

if tomllib is None:
    sys.exit("Error: Python 3.11+ or the 'tomli' package is required for TOML parsing")
try:
    configs = []
    for path in paths:
        with path.open("rb") as source:
            configs.append((tomllib.load(source), path.resolve()))
    model = validate(configs)
    names = [aspect["name"] for aspect in model["aspects"] if "powercap_calibration" in aspect]
    if not names:
        raise ValueError("an aspect must declare powercap_calibration")
    print("\n".join(names))
except (OSError, ValueError, UnicodeError) as error:
    sys.exit(f"Error: {error}")
PY
) || exit 1
if [[ -e "$directory" ]]; then
    echo "[ERROR] Output directory already exists: $directory" >&2
    exit 1
fi
python3 "$root/scripts/weave.py" "$@" -o "$directory"
help_output=$("$directory/greenmeasurer" --help)
while IFS= read -r name; do
    if grep -Fqx -- "  --$name" <<<"$help_output"; then
        exit 0
    fi
done <<<"$calibration_aspects"
echo "[ERROR] No declared powercap calibration aspect could be built." >&2
exit 1
