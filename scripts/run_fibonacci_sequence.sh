#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
if (( $# > 1 )); then
    echo "Usage: $0 [new-build-directory]" >&2
    exit 1
fi

cd "$root"
build_dir=${1:-generated-fibonacci-sequence}
if [[ -e "$build_dir" ]]; then
    echo "[ERROR] Build directory already exists: $build_dir" >&2
    exit 1
fi

make -C "$root/src/devices"
bash "$root/scripts/measure.sh" generate "$build_dir" \
    "$root/configurations-generated/fibonacci-python-temperature-calibration.toml" \
    "$root/configurations-generated/fibonacci-python-standard.toml" \
    "$root/configurations-generated/fibonacci-python-overtime.toml" \
    "$root/configurations-generated/fibonacci-python-powercap-calibration.toml"
echo "[INFO] Running temperature calibration, standard, over-time and powercap calibration in order."
bash "$root/scripts/measure.sh" run "$build_dir"
