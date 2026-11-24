#!/bin/bash
# Reproduces the paper experiments previously described by the archived TOML
# files in paper_data/ (temperature calibration, powercap calibration,
# aggregate and over-time), using the current aspect format and CLI. The woven
# aspects live in this directory: Intel CPU package energy, Intel CPU
# temperature, the number of running processes, wall-clock time, and the Intel
# powercap control.
set -euo pipefail

paper_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$paper_dir/.." && pwd)
if (( $# > 1 )); then
    echo "Usage: $0 [new-build-directory]" >&2
    exit 1
fi

cd "$root"
build_dir=${1:-generated-paper-experiments}
if [[ -e "$build_dir" ]]; then
    echo "[ERROR] Build directory already exists: $build_dir" >&2
    exit 1
fi

make -C "$root/src/devices"
bash "$root/scripts/measure.sh" generate "$build_dir" \
    "$paper_dir/aggregate" "$paper_dir/over-time"
aggregate_executable="$build_dir/run-0001/greenmeasurer"
overtime_executable="$build_dir/run-0002/greenmeasurer"

# Select every declared metric that was incorporated into the executable.
help_output=$("$aggregate_executable" --help)
mapfile -t metric_flags < <(sed -n 's/^  \(--[A-Za-z0-9_.]*\)$/\1/p' <<<"$help_output" \
    | grep -Fxv -- --IntelCPU.Powercap)
if (( ${#metric_flags[@]} == 0 )); then
    echo "[ERROR] No metric aspect was incorporated; see the weaver output." >&2
    exit 1
fi
for flag in --IntelCPU.Powercap --IntelCPU.Temperature; do
    if ! grep -Fxq -- "  $flag" <<<"$help_output"; then
        echo "[ERROR] The paper experiments require $flag, which was not incorporated." >&2
        exit 1
    fi
done
printf '[INFO] Selected metrics:'
printf ' %s' "${metric_flags[@]}"
printf '\n'

baseline="$paper_dir/temperature-baseline.txt"
workload=(-command "python3 fibonacci.py 40"
          -program fibonacci-40)
thermal=(-temperature-control IntelCPU.Temperature
         -calib-file "IntelCPU.Temperature=$baseline" -variance 10)

echo "[INFO] Running temperature calibration, powercap calibration, aggregate and over-time in order."
"$aggregate_executable" --temperature-calibration \
    -sleep-secs 30 \
    --IntelCPU.Temperature \
    -calib-file "IntelCPU.Temperature=$baseline"

"$aggregate_executable" --powercap-calibration \
    "${workload[@]}" \
    -output "$paper_dir/powercap-calibration.csv" \
    -range 5-15 \
    -n_times 10 \
    -sleep-secs 30 \
    -timeout-hours 4 \
    "${thermal[@]}" \
    --IntelCPU.Powercap "${metric_flags[@]}"

# The calibration report records the powercap with the lowest package energy;
# the measurements below always use that value.
optimal_file="$paper_dir/powercap-calibration-optimal-powercap.txt"
if [[ ! -f "$optimal_file" ]]; then
    echo "[ERROR] Powercap calibration did not produce $optimal_file." >&2
    exit 1
fi
read -r optimal < "$optimal_file"
if [[ ! "$optimal" =~ ^-?[0-9]+$ ]]; then
    echo "[ERROR] Invalid optimal powercap in $optimal_file: $optimal" >&2
    exit 1
fi
if [[ "$optimal" == "-1" ]]; then
    caps="-1"
else
    caps="-1,$optimal"
fi
echo "[INFO] Using the optimal powercap from $optimal_file: $optimal"

"$aggregate_executable" \
    "${workload[@]}" \
    -output "$paper_dir/aggregate.csv" \
    -n_times 10 \
    -sleep-secs 30 \
    -powercap "$caps" \
    -timeout-hours 4 \
    "${thermal[@]}" \
    --IntelCPU.Powercap "${metric_flags[@]}"

"$overtime_executable" \
    "${workload[@]}" \
    -output "$paper_dir/overtime.csv" \
    -n_times 1 \
    -interval-ms 10 \
    -sleep-secs 30 \
    -powercap "$caps" \
    -timeout-hours 4 \
    "${thermal[@]}" \
    --IntelCPU.Powercap "${metric_flags[@]}"
