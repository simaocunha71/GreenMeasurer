#!/bin/bash
# Backend discovery tests: builds fake plugins in a temporary tree and runs
# the main binary against them through GREENMEASURER_BACKEND_DIR.
set -u
export LC_ALL=C
cd "$(dirname "$0")" || exit 1
MAIN="./legacy-engine"
[[ -x "$MAIN" ]] || { echo "[ERROR] Build $MAIN first (run 'make')."; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/root"
PASS=0; FAIL=0

build_fake() { # <category> <dir> <name> <priority> <detect> [extra flags...]
    local category=$1 dir=$2 name=$3 priority=$4 detect=$5; shift 5
    mkdir -p "$ROOT/$category/$dir"
    cc -fPIC -shared -DFAKE_NAME="\"$name\"" -DFAKE_PRIORITY="$priority" \
       -DFAKE_DETECT="$detect" "$@" -o "$ROOT/$category/$dir/backend.so" fake_backend.c
}

run_main() {
    GREENMEASURER_BACKEND_DIR="${BACKEND_DIR:-$ROOT}" GREENMEASURER_VERBOSE_BACKENDS="${VERBOSE_BACKENDS:-1}" "$MAIN" --standard -command true \
        -language C -program t -n_times 1 -variance 100 -time_out_limit 1 \
        -sleep_time 0 -powercap -1 -calib-file "$WORK/calib.txt" \
        -output "$WORK/out.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

run_overtime() {
    GREENMEASURER_BACKEND_DIR="${BACKEND_DIR:-$ROOT}" "$MAIN" --over-time \
        -command "${OVERTIME_COMMAND:-sleep 0.22}" -language C -program t -interval-ms "${OVERTIME_INTERVAL:-50}" \
        -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
        -output "$WORK/overtime.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

run_powercap_failure() { # <mode>
    if [[ "$1" == "over-time" ]]; then
        GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --over-time \
            -command "sleep 0.1" -language C -program t \
            -variance 100 -sleep-secs 0 -powercap 1 \
            -calib-file "$WORK/calib.txt" -output "$WORK/cap-failure.csv" \
            > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    else
        GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --standard -command true \
            -language C -program t -n_times 1 -variance 100 -time_out_limit 1 \
            -sleep_time 0 -powercap 1 -calib-file "$WORK/calib.txt" \
            -output "$WORK/cap-failure.csv" \
            > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    fi
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

run_invalid_powercap() { # <watts>
    GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --standard -command true \
        -language C -program t -n_times 1 -variance 100 -time_out_limit 1 \
        -sleep_time 0 -powercap "$1" -calib-file "$WORK/calib.txt" \
        -output "$WORK/invalid-cap.csv" \
        > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

run_calibration() { # <range>
    GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --powercap-calibration \
        -range "$1" -command true -secs_to_sleep 0 \
        -calib-file "$WORK/calib.txt" -output "$WORK/calibration.csv" \
        > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

expect() { # <description> <pattern>
    if grep -Eq -- "$2" "$WORK/all.txt"; then PASS=$((PASS+1)); echo "  ok   - $1"
    else FAIL=$((FAIL+1)); echo "  FAIL - $1 (pattern: $2)"; sed 's/^/        /' "$WORK/all.txt"; fi
}

echo "[TEST] Priority selection, undetected, invalid and unbuilt backends"
build_fake cpu low   "Fake CPU Low"      0  1
build_fake cpu high  "Fake CPU High"     10 1 -DFAKE_POWER_CAP_FAILURE
build_fake cpu off   "Fake CPU Off"      50 0
build_fake gpu bad   "Fake GPU Invalid"  0  1 -DFAKE_GPU -DFAKE_INVALID
build_fake gpu good  "Fake GPU"          0  1 -DFAKE_GPU
mkdir -p "$ROOT/gpu/unbuilt"
run_main
expect "run succeeds"                         "Measurement process completed|Execution Details"
expect "highest priority detected CPU wins"   "Fake CPU High +selected"
expect "lower priority CPU stays available"   "Fake CPU Low +available"
expect "undetected backend shows diagnosis"   "Fake CPU Off +not detected +fake backend is disabled"
expect "invalid plugin is reported"           "bad +invalid"
expect "folder without plugin is reported"    "unbuilt +not built"
expect "valid GPU is selected"                "Fake GPU +selected"
VERBOSE_BACKENDS=0 run_main
if grep -Eq "Fake CPU Off|Fake CPU Low|unbuilt" "$WORK/all.txt"; then
    FAIL=$((FAIL+1)); echo "  FAIL - default output hides inapplicable backends"
else PASS=$((PASS+1)); echo "  ok   - default output hides inapplicable backends"; fi
expect "selected backends still listed"       "Fake CPU High +selected"
echo "[TEST] Powercap failure aborts measurement"
run_powercap_failure standard
expect "standard reports powercap failure"     "Failed to set powercap to 1 W; measurement aborted"
[[ $RC -ne 0 && ! -e "$WORK/cap-failure.csv" ]] && {
    PASS=$((PASS+1)); echo "  ok   - standard exits without creating measurement CSV"
} || { FAIL=$((FAIL+1)); echo "  FAIL - standard continued after powercap failure"; }
run_powercap_failure over-time
expect "overtime reports powercap failure"    "Failed to set powercap to 1 W; overtime measurement aborted"
[[ $RC -ne 0 && ! -e "$WORK/cap-failure.csv" ]] && {
    PASS=$((PASS+1)); echo "  ok   - overtime exits without creating measurement CSV"
} || { FAIL=$((FAIL+1)); echo "  FAIL - overtime continued after powercap failure"; }
run_invalid_powercap 0
expect "zero-watt cap is rejected"             "-powercap must be -1 .* at least 1 W"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - zero-watt cap exits non-zero"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - zero-watt cap was accepted";
}
run_invalid_powercap -2
expect "invalid negative cap is rejected"      "-powercap must be -1 .* at least 1 W"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - invalid negative cap exits non-zero"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - invalid negative cap was accepted";
}
run_calibration 0-5
expect "calibration rejects zero-watt minimum" "minimum >= 1 W"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - invalid calibration range exits non-zero"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - zero-watt calibration range was accepted";
}
run_calibration 1-1
expect "calibration reports backend failure" "Powercap calibration failed"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - calibration callback failure exits non-zero"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - calibration callback failure was ignored";
}
run_main
if grep -Eq '^Language,Program,Powercap,Time_ms,CPU_Memory_KB,Cores,Workload_Processes_Peak,System_Running_Processes_Peak(,Core[0-9]+_Avg_Freq_MHz)+,cpu.Package_J,cpu.Temperature_C,cpu.BoardPower_W,gpu.GPU_J,gpu.VRAM_MiB$' "$WORK/out.csv"; then
    PASS=$((PASS+1)); echo "  ok   - standard CSV uses selected backend metrics"
else
    FAIL=$((FAIL+1)); echo "  FAIL - standard CSV does not match selected backend metrics"
fi
if awk -F, 'NR == 2 && $NF ~ /[0-9]/ && $NF !~ /NOT_SUPPORTED/ { found = 1 } END { exit !found }' "$WORK/out.csv"; then
    PASS=$((PASS+1)); echo "  ok   - GPU process memory is recorded"
else
    FAIL=$((FAIL+1)); echo "  FAIL - GPU process memory was not recorded"
fi
if awk -F, 'NR == 2 && $(NF-2) ~ /^[[:space:]]*8\.5[0-9]*[[:space:]]*$/ { found = 1 } END { exit !found }' "$WORK/out.csv"; then
    PASS=$((PASS+1)); echo "  ok   - backend-defined metric value is recorded"
else
    FAIL=$((FAIL+1)); echo "  FAIL - backend-defined metric value was not recorded"
fi
if awk -F, 'NR == 2 && $5 ~ /^[[:space:]]*[0-9]+[[:space:]]*$/ { found = 1 } END { exit !found }' "$WORK/out.csv"; then
    PASS=$((PASS+1)); echo "  ok   - RAM RSS is recorded in KiB"
else
    FAIL=$((FAIL+1)); echo "  FAIL - RAM RSS was not recorded"
fi
[[ $RC -eq 0 ]] && { PASS=$((PASS+1)); echo "  ok   - exit code 0"; } || { FAIL=$((FAIL+1)); echo "  FAIL - exit code $RC"; }

echo "[TEST] Over-time VRAM reporting"
run_overtime
expect "overtime completes"                     "Overtime session finished"
[[ $RC -eq 0 ]] && { PASS=$((PASS+1)); echo "  ok   - overtime exits successfully"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - overtime exited with $RC";
}
if grep -Eq '^Language,Program,Powercap,Timestamp_ms,CPU_Memory_KB,Cores,Workload_Processes,System_Running_Processes(,Core[0-9]+_Freq_MHz)+,cpu.Package_J,cpu.Temperature_C,cpu.BoardPower_W,gpu.GPU_J,gpu.VRAM_MiB$' "$WORK/overtime.csv"; then
    PASS=$((PASS+1)); echo "  ok   - overtime CSV uses selected backend metrics"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime CSV does not match selected backend metrics"
fi
if awk -F, 'NR > 1 { if ($4 <= previous) exit 1; previous = $4; count++ } END { if (count < 4) exit 1 }' "$WORK/overtime.csv"; then
    PASS=$((PASS+1)); echo "  ok   - each overtime row has an increasing timestamp"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime timestamps are missing or not increasing"
fi
if awk -F, 'NR > 1 && $NF ~ /[0-9]/ && $NF !~ /NOT_SUPPORTED/ { found = 1 } END { exit !found }' "$WORK/overtime.csv"; then
    PASS=$((PASS+1)); echo "  ok   - overtime records GPU process memory"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime GPU process memory was not recorded"
fi
if awk -F, 'NR > 1 { if ($(NF-4) + 0 != 1 || $(NF-1) + 0 != 1) exit 1; count++ } END { if (count < 4) exit 1 }' "$WORK/overtime.csv"; then
    PASS=$((PASS+1)); echo "  ok   - counter readings are recorded across multiple samples"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime counter readings or samples are missing"
fi
OVERTIME_COMMAND="sleep 0.02" OVERTIME_INTERVAL=1000 run_overtime
if [[ $RC -eq 0 ]] && [[ $(wc -l < "$WORK/overtime.csv") -eq 2 ]]; then
    PASS=$((PASS+1)); echo "  ok   - short workload records a final partial interval"
else
    FAIL=$((FAIL+1)); echo "  FAIL - short workload did not record its final interval"
fi
GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --over-time \
    -command "sleep 0.1" -language C -program t -variance 100 -sleep-secs 0 \
    -calib-file "$WORK/calib.txt" -output "$WORK/default-interval.csv" \
    > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
if [[ $? -eq 0 ]] && awk -F, 'NR > 1 { count++; last = $4 } END { exit !(count >= 20 && last >= 90 && last < 1000) }' "$WORK/default-interval.csv"; then
    PASS=$((PASS+1)); echo "  ok   - default one-millisecond pause records multiple timestamped samples"
else
    FAIL=$((FAIL+1)); echo "  FAIL - default overtime sampling cadence"
fi
OVERTIME_INTERVAL=0 run_overtime
expect "invalid interval is rejected" "requires a positive integer"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - invalid interval exits non-zero"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - invalid interval exits successfully";
}
OVERTIME_COMMAND="exit 7" run_overtime
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - failed workload exits non-zero"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - failed workload exits successfully";
}

echo "[TEST] RAPL counter rollover"
if cc -Wall -Wextra -o "$WORK/energy-counter-test" energy_counter_test.c && "$WORK/energy-counter-test"; then
    PASS=$((PASS+1)); echo "  ok   - 32-bit counter deltas handle rollover"
else
    FAIL=$((FAIL+1)); echo "  FAIL - 32-bit counter rollover"
fi

echo "[TEST] Overtime counter snapshots"
mkdir -p "$WORK/toy/cpu/toy" "$WORK/toy/gpu/toy"
if cc -O2 -Wall -Wextra -I../include -fPIC -shared -o "$WORK/toy/cpu/toy/backend.so" \
    fixtures/toy_backends/cpu/toy_cpu/toy_cpu.c &&
   cc -O2 -Wall -Wextra -I../include -fPIC -shared -o "$WORK/toy/gpu/toy/backend.so" \
    fixtures/toy_backends/gpu/toy_gpu/toy_gpu.c; then
    GREENMEASURER_BACKEND_DIR="$WORK/toy" "$MAIN" --over-time \
        -command "sleep 0.32" -language C -program t -interval-ms 50 \
        -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
        -output "$WORK/toy.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    if [[ $? -eq 0 ]] && awk -F, '
        NR == 1 { for (i = 1; i <= NF; i++) {
            if ($i == "cpu.Energy_J") cpu_col = i;
            if ($i == "gpu.Energy_J") gpu_col = i;
        }}
        NR > 1 {
            if (count == 0) { start = $4; cpu_start = $cpu_col; gpu_start = $gpu_col; }
            elapsed = ($4 - start) / 1000000;
            cpu = $cpu_col - cpu_start; gpu = $gpu_col - gpu_start; count++;
            if ($cpu_col < 0 || $gpu_col < 0) bad = 1;
        }
        END {
            exit !(cpu_col && gpu_col && count >= 2 && !bad &&
                   cpu > 9.5 * elapsed && cpu < 10.5 * elapsed &&
                   gpu > 4.75 * elapsed && gpu < 5.25 * elapsed);
        }' "$WORK/toy.csv"; then
        PASS=$((PASS+1)); echo "  ok   - counter snapshots reflect the simulated workload"
    else
        FAIL=$((FAIL+1)); echo "  FAIL - counter snapshots do not reflect the simulated workload"
    fi
else
    FAIL=$((FAIL+1)); echo "  FAIL - unable to build toy backends"
fi

echo "[TEST] Overtime capacity and legacy backends"
build_fake cpu high "Fake CPU High" 10 1 -DFAKE_NO_NEXT -DFAKE_NO_COUNTER
run_overtime
expect "legacy backend reports unavailable counter" "CPU backend does not expose counter readings"
[[ $RC -eq 0 ]] && { PASS=$((PASS+1)); echo "  ok   - legacy backend remains usable"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - legacy backend measurement failed";
}
build_fake cpu high "Fake CPU High" 10 1
if cc -O2 -Wall -Wextra -pthread -I../include -DGREENMEASURER_TEST_ENGINE -DOT_MAX_SAMPLES=2 -rdynamic \
    ../main.c ../platform/linux/memory.c ../platform/linux/system_metrics.c ../metric_adapters/metric_adapter_loader.c -o "$WORK/main-small" \
    -pthread -lm -ldl; then
    GREENMEASURER_BACKEND_DIR="$ROOT" "$WORK/main-small" --over-time \
        -command "sleep 0.08; touch '$WORK/finished'" -language C -program t \
        -interval-ms 10 -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
        -output "$WORK/capacity.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
    expect "capacity failure is reported" "sample capacity .* exceeded"
    if [[ $RC -ne 0 && ! -e "$WORK/capacity.csv" && ! -e "$WORK/finished" ]]; then
        PASS=$((PASS+1)); echo "  ok   - capacity failure aborts workload without partial CSV"
    else
        FAIL=$((FAIL+1)); echo "  FAIL - capacity failure did not abort cleanly"
    fi
else
    FAIL=$((FAIL+1)); echo "  FAIL - failed to build small-capacity test binary"
fi

echo "[TEST] No GPU backend available"
rm -rf "$ROOT/gpu"
build_fake gpu off "Fake GPU Off" 0 0 -DFAKE_GPU
mkdir -p "$ROOT/gpu/unbuilt"
VERBOSE_BACKENDS=0 run_main
expect "GPU absence warns in default output" "No GPU energy backend active"
expect "verbose diagnostics hint is shown" "GREENMEASURER_VERBOSE_BACKENDS=1"
if grep -Eq "Fake GPU Off|unbuilt" "$WORK/all.txt"; then
    FAIL=$((FAIL+1)); echo "  FAIL - default output lists undetected or unbuilt GPU"
else PASS=$((PASS+1)); echo "  ok   - default output hides undetected and unbuilt GPU"; fi
run_main
expect "verbose output shows undetected GPU reason" "Fake GPU Off +not detected +fake backend is disabled"
expect "verbose output shows unbuilt GPU" "unbuilt +not built"
expect "GPU absence warns"                    "No GPU energy backend active"
expect "CPU still selected"                   "Fake CPU High +selected"
VERBOSE_BACKENDS=0 run_overtime
expect "overtime GPU absence warns" "No GPU energy backend active"
if grep -Eq "Fake GPU Off|unbuilt" "$WORK/all.txt"; then
    FAIL=$((FAIL+1)); echo "  FAIL - overtime lists undetected or unbuilt GPU"
else PASS=$((PASS+1)); echo "  ok   - overtime hides undetected and unbuilt GPU"; fi
GREENMEASURER_BACKEND_DIR="$ROOT" GREENMEASURER_VERBOSE_BACKENDS=0 \
    "$MAIN" --config fixtures/configurations/system-cores.toml \
    -command "sleep 0.05" -sleep-secs 0 -calib-file "$WORK/calib.txt" \
    -output "$WORK/configured.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
RC=$?
cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
expect "configured overtime GPU absence warns" "No GPU energy backend active"
if [[ $RC -eq 0 ]] && ! grep -Eq "Fake GPU Off|unbuilt" "$WORK/all.txt"; then
    PASS=$((PASS+1)); echo "  ok   - configured overtime hides undetected and unbuilt GPU"
else FAIL=$((FAIL+1)); echo "  FAIL - configured overtime output or exit status"; fi

echo "[TEST] Detected backends with failed initialization"
build_fake cpu high "Fake CPU High" 10 1 -DFAKE_INIT_FAILURE
build_fake gpu good "Fake GPU" 0 1 -DFAKE_GPU -DFAKE_INIT_FAILURE
run_main
expect "standard CPU initialization warns" "CPU backend initialization or sampling failed"
expect "standard GPU initialization warns" "GPU backend initialization or sampling failed"
run_overtime
expect "overtime CPU initialization warns" "Failed to start overtime CPU backend window"
expect "overtime GPU initialization warns" "Failed to start overtime GPU backend window"

echo "[TEST] No CPU backend available"
rm -rf "$ROOT/cpu"
build_fake gpu good "Fake GPU" 0 1 -DFAKE_GPU
run_main
expect "CPU absence warns"                    "No CPU energy backend active"
expect "GPU still selected"                   "Fake GPU +selected"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - GPU-only measurement requires CPU temperature"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - GPU-only measurement did not require CPU temperature";
}

echo "[TEST] No backend at all"
rm -rf "$ROOT"; mkdir -p "$ROOT/cpu" "$ROOT/gpu"
mkdir -p "$ROOT/cpu/unbuilt" "$ROOT/gpu/unbuilt"
VERBOSE_BACKENDS=0 run_main
expect "fails clearly"                        "No usable CPU or GPU energy backend"
expect "both absent warns for CPU"            "No CPU energy backend active"
expect "both absent warns for GPU"            "No GPU energy backend active"
expect "missing backends mention verbose mode" "GREENMEASURER_VERBOSE_BACKENDS=1"
if grep -Eq "unbuilt +not built" "$WORK/all.txt"; then
    FAIL=$((FAIL+1)); echo "  FAIL - default output lists unbuilt CPU or GPU"
else PASS=$((PASS+1)); echo "  ok   - default output hides unbuilt CPU and GPU"; fi
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - non-zero exit code"; } || { FAIL=$((FAIL+1)); echo "  FAIL - expected non-zero exit"; }

echo "[TEST] Invalid backend directory override"
BACKEND_DIR="$WORK/does-not-exist" run_main
expect "override error is reported"           "GREENMEASURER_BACKEND_DIR is not a usable directory"

echo "[TEST] External metric adapter"
if cc -O2 -Wall -Wextra -fPIC -shared \
    fixtures/custom_metric/adapter.c fixtures/custom_metric/processes.c \
    -o "$WORK/metric-adapter.so" &&
   cc -O2 -Wall -Wextra metric_adapter_test.c ../metric_adapters/metric_adapter_loader.c \
    -o "$WORK/metric-adapter-test" -ldl -lm &&
   cc -O2 -Wall -Wextra -fPIC -shared invalid_metric_adapter.c \
    -o "$WORK/invalid-metric-adapter.so" &&
   "$WORK/metric-adapter-test" "$WORK/metric-adapter.so" "$WORK/invalid-metric-adapter.so" \
    > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"; then
    PASS=$((PASS+1)); echo "  ok   - external gauge adapter loads and reads"
else
    FAIL=$((FAIL+1)); echo "  FAIL - external gauge adapter"
    cat "$WORK/stdout.txt" "$WORK/stderr.txt"
fi

echo
echo "[RESULT] $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
