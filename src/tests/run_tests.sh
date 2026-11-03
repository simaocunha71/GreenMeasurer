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
    GREENMEASURER_BACKEND_DIR="${BACKEND_DIR:-$ROOT}" "$MAIN" --standard -command true \
        -language C -program t -n_times 1 -variance 100 -time_out_limit 1 \
        -sleep_time 0 -powercap -1 -calib-file "$WORK/calib.txt" \
        -output "$WORK/out.csv" > "$WORK/stdout.txt" 2> "$WORK/stderr.txt"
    RC=$?
    cat "$WORK/stdout.txt" "$WORK/stderr.txt" > "$WORK/all.txt"
}

expect() { # <description> <pattern>
    if grep -Eq -- "$2" "$WORK/all.txt"; then PASS=$((PASS+1)); echo "  ok   - $1"
    else FAIL=$((FAIL+1)); echo "  FAIL - $1 (pattern: $2)"; sed 's/^/        /' "$WORK/all.txt"; fi
}

echo "[TEST] Priority selection, undetected, invalid and unbuilt backends"
build_fake cpu low   "Fake CPU Low"      0  1
build_fake cpu high  "Fake CPU High"     10 1
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
[[ $RC -eq 0 ]] && { PASS=$((PASS+1)); echo "  ok   - exit code 0"; } || { FAIL=$((FAIL+1)); echo "  FAIL - exit code $RC"; }

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
