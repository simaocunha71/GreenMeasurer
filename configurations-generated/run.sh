#!/bin/bash
set -euo pipefail

config_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$config_dir/.." && pwd)
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
    "$config_dir/config.toml"
executable="$build_dir/run-0001/greenmeasurer"
echo "[INFO] Running temperature calibration, powercap calibration, aggregate and over-time in order."
"$executable" --temperature-calibration \
    -sleep-secs 60 \
    --IntelCPU.Temperature \
    -calib-file IntelCPU.Temperature=cores_temperatures.txt

"$executable" --powercap-calibration \
    -command "python3 Languages/Python/Fibonacci/fibonacci.py 42" \
    -language Python \
    -program fibonacci-42 \
    -output fibonacci-python-powercap-calibration.csv \
    -range 5-15 \
    -n_times 10 \
    -sleep-secs 60 \
    -time_out_limit 4 \
    -temperature-control IntelCPU.Temperature \
    -calib-file IntelCPU.Temperature=cores_temperatures.txt \
    -variance 5 \
    --IntelCPU.Powercap --Time.WallClock --IntelCPU.Package --IntelCPU.Core --IntelCPU.DRAM \
    --IntelCPU.Temperature --IntelGPU.Energy --NvidiaGPU.Energy --NvidiaGPU.Temperature

"$executable" --aggregate \
    -command "python3 Languages/Python/Fibonacci/fibonacci.py 42" \
    -language Python \
    -program fibonacci-42 \
    -output fibonacci-python-aggregate.csv \
    -n_times 10 \
    -sleep-secs 60 \
    -powercap "-1,5" \
    -time_out_limit 4 \
    -temperature-control IntelCPU.Temperature \
    -calib-file IntelCPU.Temperature=cores_temperatures.txt \
    -variance 5 \
    --IntelCPU.Powercap --Time.WallClock --IntelCPU.Package --IntelCPU.Core --IntelCPU.DRAM \
    --IntelCPU.Temperature --IntelGPU.Energy --NvidiaGPU.Energy --NvidiaGPU.Temperature

"$executable" --over-time \
    -command "python3 Languages/Python/Fibonacci/fibonacci.py 42" \
    -language Python \
    -program fibonacci-42 \
    -output fibonacci-python-overtime.csv \
    -n_times 10 \
    -interval-ms 1 \
    -sleep-secs 60 \
    -powercap "-1,5" \
    -time_out_limit 4 \
    -temperature-control IntelCPU.Temperature \
    -calib-file IntelCPU.Temperature=cores_temperatures.txt \
    -variance 5 \
    --IntelCPU.Powercap --Time.WallClock --IntelCPU.Package --IntelCPU.Core --IntelCPU.DRAM \
    --IntelCPU.Temperature --IntelGPU.Energy --NvidiaGPU.Energy --NvidiaGPU.Temperature
