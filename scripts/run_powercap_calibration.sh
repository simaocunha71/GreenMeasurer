#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <aspects.toml> <new-directory>" >&2
    echo "Generate the executable, then run it with --powercap-calibration and explicit options." >&2
    exit 1
fi
calibration_aspects=$(python3 - "$root" "$1" <<'PY'
import sys
from pathlib import Path

root, path = map(Path, sys.argv[1:])
sys.path.insert(0, str(root / "scripts"))
from weave import tomllib, validate

if tomllib is None:
    sys.exit("Error: Python 3.11+ or the 'tomli' package is required for TOML parsing")
try:
    with path.open("rb") as source:
        config = tomllib.load(source)
    model = validate(config, path.resolve())
    names = [aspect["name"] for aspect in model["aspects"] if "powercap_calibration" in aspect]
    if not names:
        raise ValueError("an aspect must declare powercap_calibration")
    print("\n".join(names))
except (OSError, ValueError, UnicodeError) as error:
    sys.exit(f"Error: {error}")
PY
) || exit 1
if [[ -e "$2" ]]; then
    echo "[ERROR] Output directory already exists: $2" >&2
    exit 1
fi
python3 "$root/scripts/weave.py" "$1" -o "$2"
help_output=$("$2/greenmeasurer" --help)
while IFS= read -r name; do
    if grep -Fqx -- "  --$name" <<<"$help_output"; then
        exit 0
    fi
done <<<"$calibration_aspects"
echo "[ERROR] No declared powercap calibration aspect could be built." >&2
exit 1
