#!/bin/bash
# Backend discovery tests: builds fake plugins in a temporary tree and runs
# the main binary against them through GREENMEASURER_BACKEND_DIR.
set -u
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
        -command "sleep 0.1" -interval 20 -language C -program t \
        -variance 100 -sleep-secs 0 -calib-file "$WORK/calib.txt" \
        -output "$WORK/overtime.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

run_powercap_failure() { # <mode>
    if [[ "$1" == "overtime" ]]; then
        GREENMEASURER_BACKEND_DIR="$ROOT" "$MAIN" --overtime \
            -command "sleep 0.1" -interval 20 -language C -program t \
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
if grep -q '^Language,Program,Powercap,Time_ms,CPU_Memory_KB,cpu.Package_J,cpu.Temperature_C,cpu.BoardPower_W,gpu.GPU_J,gpu.VRAM_MiB$' "$WORK/out.csv"; then
    PASS=$((PASS+1)); echo "  ok   - standard CSV uses selected backend metrics"
else
    FAIL=$((FAIL+1)); echo "  FAIL - standard CSV does not match selected backend metrics"
fi
if awk -F, 'NR == 2 && $10 ~ /[0-9]/ && $10 !~ /NOT_SUPPORTED/ { found = 1 } END { exit !found }' "$WORK/out.csv"; then
    PASS=$((PASS+1)); echo "  ok   - GPU process memory is recorded"
else
    FAIL=$((FAIL+1)); echo "  FAIL - GPU process memory was not recorded"
fi
if awk -F, 'NR == 2 && $8 ~ /^[[:space:]]*8\.5[0-9]*[[:space:]]*$/ { found = 1 } END { exit !found }' "$WORK/out.csv"; then
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
if grep -q '^Language,Program,Powercap,Timestamp_ms,cpu.Package_J,cpu.Temperature_C,cpu.BoardPower_W,gpu.GPU_J,gpu.VRAM_MiB$' "$WORK/overtime.csv"; then
    PASS=$((PASS+1)); echo "  ok   - overtime CSV uses selected backend metrics"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime CSV does not match selected backend metrics"
fi
if awk -F, 'NR > 1 && $9 ~ /[0-9]/ && $9 !~ /NOT_SUPPORTED/ { found = 1 } END { exit !found }' "$WORK/overtime.csv"; then
    PASS=$((PASS+1)); echo "  ok   - overtime records GPU process memory"
else
    FAIL=$((FAIL+1)); echo "  FAIL - overtime GPU process memory was not recorded"
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
