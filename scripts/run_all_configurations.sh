#!/bin/bash
# Runs every TOML under configurations-generated/ (or a given directory) on this machine
# and prints a PASS/FAIL/SKIP summary, to validate GreenMeasurer on new hardware.
#
# Usage: scripts/run_all_configurations.sh [-o results-dir] [-s] [config-dir-or-file ...]
#   -o DIR  results directory (default: greenmeasurer-config-results-<host>-<date>)
#   -s      stop at the first failure
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
results=""
stop_on_fail=0
while getopts "o:s" opt; do
    case $opt in
        o) results=$OPTARG ;;
        s) stop_on_fail=1 ;;
        *) echo "Usage: $0 [-o results-dir] [-s] [config-dir-or-file ...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 0 ]] && set -- "$root/configurations-generated"
# Each run changes directory, so every input path must be absolute.
inputs=()
for path in "$@"; do
    [[ -e $path ]] || { echo "[ERROR] No such file or directory: $path" >&2; exit 2; }
    inputs+=("$(realpath "$path")")
done
set -- "${inputs[@]}"
[[ -z $results ]] && results="$PWD/greenmeasurer-config-results-$(hostname)-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$results"
results=$(cd "$results" && pwd)

# Under sudo the invoking user's site-packages (e.g. a user-installed tomli) is
# not visible, so expose it explicitly; also hand results back to that user.
if [[ $EUID -eq 0 && -n ${SUDO_USER:-} ]]; then
    user_site=$(sudo -u "$SUDO_USER" python3 -m site --user-site 2>/dev/null || true)
    [[ -d $user_site ]] && export PYTHONPATH="$user_site${PYTHONPATH:+:$PYTHONPATH}"
fi
if ! python3 -c 'import tomllib' 2>/dev/null && ! python3 -c 'import tomli' 2>/dev/null; then
    echo "[ERROR] Python 3.11+ or the 'tomli' package is required (pip install tomli; with sudo use sudo -E or pip install as root)." >&2
    exit 1
fi

if [[ $EUID -ne 0 ]]; then
    echo "[WARNING] Not running as root: RAPL/MSR/powercap metrics will likely be unavailable."
    echo "          Re-run with: sudo $0 $*"
fi

if [[ $EUID -eq 0 && -n ${SUDO_USER:-} ]]; then
    trap 'chown -R "$SUDO_USER": "$results" 2>/dev/null' EXIT
fi

echo "[INFO] Building GreenMeasurer..."
make -C "$root/src" all >"$results/build.log" 2>&1 || {
    echo "[ERROR] Build failed; see $results/build.log"; exit 1; }

mapfile -t configs < <(find "$@" -name '*.toml' -type f | sort)
ordered=("${configs[@]}")

pass=0; fail=0
summary=()
for config in "${ordered[@]}"; do
    name=$(realpath --relative-to="$root" "$config" 2>/dev/null || echo "$config")
    slug=${name//\//__}; slug=${slug%.toml}
    work="$results/$slug"
    mkdir -p "$work"
    [[ -f "$results/greenmeasurer-test-temperature.txt" ]] &&
        cp -n "$results/greenmeasurer-test-temperature.txt" "$work/"
    # Workload commands in the TOML are relative to the repo root (e.g. Languages/...).
    [[ -e "$work/Languages" ]] || ln -s "$root/Languages" "$work/Languages"
    echo "[RUN ] $name"
    status=PASS
    # Generation, compilation and execution all use the result directory as cwd
    # so the relative output/calibration paths in the TOML land there.
    if (cd "$work" && python3 "$root/scripts/weave.py" "$config" -o "$work/build") \
        >"$work/generate.log" 2>&1; then
        (cd "$work" && "$work/build/greenmeasurer") >"$work/run.log" 2>&1 || status=FAIL
    else
        status=FAIL
        cp "$work/generate.log" "$work/run.log"
    fi
    if [[ $status == PASS ]]; then
        # An execution that exits 0 but produced no output file is still a failure.
        produced=$(find "$work" -maxdepth 1 \( -name '*.csv' -o -name '*.txt' \) | wc -l)
        [[ $produced -eq 0 ]] && status=FAIL && echo "no output file produced" >>"$work/run.log"
    fi
    if [[ $status == PASS ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); tail -n 5 "$work/run.log" | sed 's/^/       /'
    fi
    summary+=("$status  $name")
    # Share the baseline with the following configs.
    [[ -f "$work/greenmeasurer-test-temperature.txt" ]] &&
        cp -n "$work/greenmeasurer-test-temperature.txt" "$results/" 2>/dev/null
    [[ $status == FAIL && $stop_on_fail -eq 1 ]] && break
done

echo
echo "===== Summary ($(hostname), $(date -Is)) ====="
printf '%s\n' "${summary[@]}" | tee "$results/summary.txt"
echo "PASS: $pass  FAIL: $fail   Results: $results"
[[ $fail -eq 0 ]]
