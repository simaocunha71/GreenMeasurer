#!/bin/bash
set -euo pipefail

example_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$example_dir/.." && pwd)
use_sudo=0
if (( $# == 1 )) && [[ "$1" == --sudo-powercap ]]; then
    use_sudo=1
elif (( $# != 0 )); then
    echo "Usage: bash toy-examples/run.sh [--sudo-powercap]" >&2
    exit 1
fi

results=$(mktemp -d "$example_dir/results-XXXXXXXX")
printf 'Results: %s\n' "$results"
cd "$root"

if ! make -C "$root/src/devices" > "$results/build.log" 2>&1; then
    echo "[ERROR] Device library build failed; see build.log." >&2
    exit 1
fi
if ! bash "$root/scripts/measure.sh" generate "$results/build" \
    "$root/examples/aggregate" "$root/examples/over-time" \
    >> "$results/build.log" 2>&1; then
    echo "[ERROR] Weaver failed; see build.log." >&2
    exit 1
fi

executable="$results/build/run-0001/greenmeasurer"
overtime_executable="$results/build/run-0002/greenmeasurer"
"$executable" --help > "$results/available-aspects.log" 2>&1
status=0

run_logged() {
    local log=$1
    shift
    {
        printf 'Command:'
        printf ' %q' "$@"
        printf '\n'
    } >> "$log"
    local code=0
    "$@" >> "$log" 2>&1 || code=$?
    printf '\nExit status: %s\n' "$code" >> "$log"
    return "$code"
}

run_case() {
    local name=$1
    shift
    local case_dir="$results/$name"
    mkdir "$case_dir"
    local -a requested=("$@") flags=() controls=() cap_options=()
    local -a runner=("$executable") overtime_runner=("$overtime_executable")
    local powercap_requested=0 powercap_available=0 metric_count=0
    local aspect
    {
        printf 'Requested aspects:'
        if (( ${#requested[@]} == 0 )); then printf ' none'; fi
        printf ' %s' "${requested[@]}"
        printf '\n'
        for aspect in "${requested[@]}"; do
            if [[ "$aspect" == IntelCPU.Powercap ]]; then powercap_requested=1; fi
            if grep -Fxq -- "  --$aspect" "$results/available-aspects.log"; then
                flags+=("--$aspect")
                if [[ "$aspect" == IntelCPU.Powercap ]]; then
                    powercap_available=1
                else
                    metric_count=$((metric_count + 1))
                fi
                printf '[AVAILABLE] %s\n' "$aspect"
            else
                printf '[UNAVAILABLE] %s was not incorporated; see build.log.\n' "$aspect"
                status=1
            fi
        done
    } > "$case_dir/aspects.log"

    local calibration_log="$case_dir/temperature-calibration.log"
    local thermal_count=0 cpu_calibrated=0
    for aspect in "${requested[@]}"; do
        case "$aspect" in
            IntelCPU.Temperature|NvidiaGPU.Temperature)
                thermal_count=$((thermal_count + 1))
                if grep -Fxq -- "  --$aspect" "$results/available-aspects.log"; then
                    if run_logged "$calibration_log" "$executable" --temperature-calibration \
                        -sleep-secs 10 "--$aspect" -calib-file "$aspect=$case_dir/$aspect.txt"; then
                        if [[ "$aspect" == IntelCPU.Temperature ]]; then cpu_calibrated=1; fi
                    else
                        status=1
                    fi
                else
                    printf '[SKIPPED] %s was not incorporated.\n' "$aspect" >> "$calibration_log"
                fi
                ;;
        esac
    done
    if (( thermal_count == 0 )); then
        printf '[SKIPPED] No temperature aspect requested.\n' > "$calibration_log"
    fi
    if (( cpu_calibrated )); then
        controls=(-temperature-control IntelCPU.Temperature \
            -calib-file "IntelCPU.Temperature=$case_dir/IntelCPU.Temperature.txt" -variance 15)
    elif [[ " ${requested[*]} " == *" IntelCPU.Temperature "* ]]; then
        printf '[WARNING] CPU thermal control is disabled because calibration failed or was unavailable.\n' \
            >> "$case_dir/aspects.log"
    fi

    if (( metric_count == 0 )); then flags+=(--no-metrics); fi
    local -a common=(-command "python3 toy-examples/fibonacci.py 40" \
        -program fibonacci-40 -n_times 1 -sleep-secs 10 -timeout-hours 0)
    if (( powercap_available )); then
        if (( use_sudo )); then
            runner=(sudo -A "$executable")
            overtime_runner=(sudo -A "$overtime_executable")
        fi
        if ! run_logged "$case_dir/powercap-calibration.log" "${runner[@]}" --powercap-calibration \
            "${common[@]}" -output "$case_dir/powercap-calibration.csv" -range 5-7 \
            "${controls[@]}" "${flags[@]}"; then
            status=1
            printf '[SKIPPED] Powercap calibration failed; no further powercap operations attempted.\n' \
                > "$case_dir/aggregate.log"
            cp "$case_dir/aggregate.log" "$case_dir/over-time.log"
            return
        fi
        # Prefer the optimal powercap recorded by the calibration report; fall
        # back to 5 W when the report could not run (for example, without pandas).
        local optimal_file="$case_dir/powercap-calibration-optimal-powercap.txt"
        local optimal=""
        if [[ -f "$optimal_file" ]]; then
            read -r optimal < "$optimal_file" || optimal=""
        fi
        if [[ "$optimal" =~ ^-?[0-9]+$ && "$optimal" != "-1" ]]; then
            cap_options=(-powercap "-1,$optimal")
        elif [[ "$optimal" == "-1" ]]; then
            cap_options=(-powercap "-1")
        else
            cap_options=(-powercap "-1,5")
        fi
    elif (( powercap_requested )); then
        printf '[SKIPPED] Powercap aspect was not incorporated; no CPU limit is changed.\n' \
            > "$case_dir/powercap-calibration.log"
    else
        printf '[SKIPPED] No powercap aspect requested; no CPU limit is changed.\n' \
            > "$case_dir/powercap-calibration.log"
    fi
    if ! run_logged "$case_dir/aggregate.log" "${runner[@]}" \
        "${common[@]}" -output "$case_dir/aggregate.csv" "${cap_options[@]}" \
        "${controls[@]}" "${flags[@]}"; then
        status=1
        if (( powercap_available )); then
            printf '[SKIPPED] Aggregate failed; no further powercap operations attempted.\n' \
                > "$case_dir/over-time.log"
            return
        fi
    fi
    if ! run_logged "$case_dir/over-time.log" "${overtime_runner[@]}" \
        "${common[@]}" -output "$case_dir/over-time.csv" -interval-ms 1 \
        "${cap_options[@]}" "${controls[@]}" "${flags[@]}"; then
        status=1
    fi
}

run_case 01-no-aspects
run_case 02-time Time.WallClock
run_case 03-cpu-temperature Time.WallClock IntelCPU.Temperature
run_case 04-cpu-package Time.WallClock IntelCPU.Temperature IntelCPU.Package
run_case 05-nvidia-temperature Time.WallClock IntelCPU.Temperature IntelCPU.Package NvidiaGPU.Temperature
run_case 06-powercap Time.WallClock IntelCPU.Temperature IntelCPU.Package NvidiaGPU.Temperature IntelCPU.Powercap

if (( status )); then
    printf '[WARNING] Some requested aspects or executions were unavailable; inspect the logs.\n' \
        > "$results/status.log"
    exit 1
fi
printf '[OK] All requested aspects were incorporated and commands succeeded; inspect logs for runtime warnings.\n' \
    > "$results/status.log"
