#!/bin/bash
# Backend discovery tests: builds fake plugins in a temporary tree and runs
# the main binary against them through GREENMEASURER_BACKEND_DIR.
set -u
export LC_ALL=C
cd "$(dirname "$0")" || exit 1
MAIN="../main"
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
    GREENMEASURER_BACKEND_DIR="${BACKEND_DIR:-$ROOT}" "$MAIN" --overtime \
        -command "${OVERTIME_COMMAND:-sleep 0.22}" -language C -program t -interval-us "${OVERTIME_INTERVAL:-50000}" \
        -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
        -output "$WORK/overtime.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

run_powercap_failure() { # <mode>
    if [[ "$1" == "overtime" ]]; then
        GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --overtime \
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
run_powercap_failure overtime
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
    PASS=$((PASS+1)); echo "  ok   - energy is per interval across multiple samples"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime energy is accumulated or samples are missing"
fi
OVERTIME_COMMAND="sleep 0.02" OVERTIME_INTERVAL=1000000 run_overtime
if [[ $RC -eq 0 ]] && [[ $(wc -l < "$WORK/overtime.csv") -eq 2 ]]; then
    PASS=$((PASS+1)); echo "  ok   - short workload records a final partial interval"
else
    FAIL=$((FAIL+1)); echo "  FAIL - short workload did not record its final interval"
fi
GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --overtime \
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

echo "[TEST] Contiguous overtime windows"
mkdir -p "$WORK/toy/cpu/toy" "$WORK/toy/gpu/toy"
if cc -O2 -Wall -Wextra -fPIC -shared -o "$WORK/toy/cpu/toy/backend.so" \
    ../../examples/toy_backends/cpu/toy_cpu/toy_cpu.c &&
   cc -O2 -Wall -Wextra -fPIC -shared -o "$WORK/toy/gpu/toy/backend.so" \
    ../../examples/toy_backends/gpu/toy_gpu/toy_gpu.c; then
    GREENMEASURER_BACKEND_DIR="$WORK/toy" "$MAIN" --overtime \
        -command "sleep 0.32" -language C -program t -interval-us 50000 \
        -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
        -output "$WORK/toy.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    if [[ $? -eq 0 ]] && awk -F, '
        NR > 1 {
            elapsed = $4 / 1000;
            cpu += $(NF-4); gpu += $(NF-2); count++;
            if ($(NF-4) < 0 || $(NF-2) < 0) bad = 1;
        }
        END {
            exit !(count >= 2 && !bad &&
                   cpu > 9.5 * elapsed && cpu < 10.5 * elapsed &&
                   gpu > 4.75 * elapsed && gpu < 5.25 * elapsed);
        }' "$WORK/toy.csv"; then
        PASS=$((PASS+1)); echo "  ok   - interval energy sums to the complete toy workload window"
    else
        FAIL=$((FAIL+1)); echo "  FAIL - interval energy contains gaps or accumulates across rows"
    fi
else
    FAIL=$((FAIL+1)); echo "  FAIL - unable to build toy backends"
fi

echo "[TEST] Overtime capacity and legacy backends"
build_fake cpu high "Fake CPU High" 10 1 -DFAKE_NO_NEXT
run_overtime
expect "legacy backend reports gaps" "CPU backend does not support contiguous overtime windows"
[[ $RC -eq 0 ]] && { PASS=$((PASS+1)); echo "  ok   - legacy backend remains usable"; } || {
    FAIL=$((FAIL+1)); echo "  FAIL - legacy backend measurement failed";
}
build_fake cpu high "Fake CPU High" 10 1 -DFAKE_POWER_CAP_FAILURE
if cc -O2 -Wall -Wextra -pthread -I../include -DOT_MAX_SAMPLES=2 -rdynamic \
    ../main.c ../utils/memory.c ../metrics/system_metrics.c -o "$WORK/main-small" \
    -pthread -lm -ldl; then
    GREENMEASURER_BACKEND_DIR="$ROOT" "$WORK/main-small" --overtime \
        -command "sleep 1; touch '$WORK/finished'" -language C -program t \
        -interval-us 10000 -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
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
run_main
expect "GPU absence warns"                    "No GPU energy backend active"
expect "CPU still selected"                   "Fake CPU High +selected"

echo "[TEST] No backend at all"
rm -rf "$ROOT"; mkdir -p "$ROOT/cpu" "$ROOT/gpu"
run_main
expect "fails clearly"                        "No usable CPU or GPU energy backend"
[[ $RC -ne 0 ]] && { PASS=$((PASS+1)); echo "  ok   - non-zero exit code"; } || { FAIL=$((FAIL+1)); echo "  FAIL - expected non-zero exit"; }

echo "[TEST] Invalid backend directory override"
BACKEND_DIR="$WORK/does-not-exist" run_main
expect "override error is reported"           "GREENMEASURER_BACKEND_DIR is not a usable directory"

echo
echo "[RESULT] $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
