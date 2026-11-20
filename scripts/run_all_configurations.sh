#!/bin/bash
# Runs every TOML under configurations-generated/ (or a given directory) on this machine
# and prints a PASS/WARN/FAIL summary, to validate GreenMeasurer on new hardware.
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

pass=0; warn=0; fail=0
summary=()
for config in "${ordered[@]}"; do
    name=$(realpath --relative-to="$root" "$config" 2>/dev/null || echo "$config")
    slug=${name//\//__}; slug=${slug%.toml}
    work="$results/$slug"
    mkdir -p "$work"
    echo "[RUN ] $name"
    status=PASS
    if (cd "$work" && python3 "$root/scripts/weave.py" "$config" -o "$work/build") \
        >"$work/generate.log" 2>&1; then
        if aspect_data=$(PYTHONPATH="$root/scripts${PYTHONPATH:+:$PYTHONPATH}" python3 -c \
            'import sys; from weave import tomllib; f = open(sys.argv[1], "rb"); data = tomllib.load(f); [print(a["name"], int(a.get("thermal", False)), int("read" in a), int("powercap_set" in a)) for a in data.get("aspect", [])]' "$config" 2>"$work/run.log"); then
            aspect_info=()
            [[ -n $aspect_data ]] && mapfile -t aspect_info <<<"$aspect_data"
            if help_output=$("$work/build/greenmeasurer" --help 2>>"$work/run.log"); then
                available_info=()
                for entry in "${aspect_info[@]}"; do
                    read -r aspect _ <<<"$entry"
                    if grep -Fqx -- "  --$aspect" <<<"$help_output"; then
                        available_info+=("$entry")
                    fi
                done
                aspect_info=("${available_info[@]}")
            else
                status=FAIL
            fi
            if [[ ${#aspect_info[@]} -eq 0 ]]; then
                arguments=(-command true)
            else
                arguments=(--aggregate -command true -language Shell -program true -output "$work/results.csv"
                           -n_times 1 -sleep-secs 0 -time_out_limit 1)
                thermal=0
                metric_count=0
                powercap_selected=0
                for entry in "${aspect_info[@]}"; do
                    read -r aspect is_thermal is_metric can_set <<<"$entry"
                    [[ $is_metric == 0 && $can_set == 0 ]] && continue
                    if [[ $is_metric == 0 ]]; then
                        if [[ $powercap_selected == 1 ]]; then
                            echo "[WARNING] Skipping additional powercap aspect $aspect." >>"$work/generate.log"
                            continue
                        fi
                        powercap_selected=1
                    fi
                    arguments+=("--$aspect")
                    ((metric_count += is_metric))
                    if [[ $is_thermal == 1 ]]; then
                        thermal=1
                        arguments+=(-temperature-control "$aspect" -calib-file "$aspect=$work/${aspect//./_}-temperature.txt")
                    fi
                done
                [[ $metric_count == 0 ]] && arguments+=(--no-metrics)
                [[ $powercap_selected == 1 ]] && arguments+=(-powercap -1)
                [[ $thermal == 1 ]] && arguments+=(-variance 5)
            fi
            if [[ $status == PASS ]]; then
                (cd "$work" && "$work/build/greenmeasurer" "${arguments[@]}") >>"$work/run.log" 2>&1 || status=FAIL
            fi
        else
            status=FAIL
        fi
    else
        status=FAIL
        cp "$work/generate.log" "$work/run.log"
    fi
    if [[ $status == PASS ]]; then
        # A configuration without aspects only executes the command and produces no CSV.
        if [[ ${#aspect_info[@]} -gt 0 ]]; then
            produced=$(find "$work" -maxdepth 1 \( -name '*.csv' -o -name '*.txt' \) | wc -l)
            if [[ $produced -eq 0 ]] && ! grep -qF '[WARNING] No active aspects;' "$work/run.log"; then
                status=FAIL
                echo "no output file produced" >>"$work/run.log"
            fi
        fi
    fi
    if [[ $status == PASS ]] && {
        grep -qF '[WARNING]' "$work/generate.log" "$work/run.log" ||
        find "$work" -maxdepth 1 -name '*.csv' -type f -exec grep -lF 'NOT_SUPPORTED' {} + | grep -q .
    }; then
        status=WARN
        echo "       Warnings or unavailable metrics found; inspect logs and CSV files in $work."
    fi
    case $status in
        PASS) pass=$((pass + 1)) ;;
        WARN) warn=$((warn + 1)) ;;
        FAIL) fail=$((fail + 1)); tail -n 5 "$work/run.log" | sed 's/^/       /' ;;
    esac
    summary+=("$status  $name")
    [[ $status == FAIL && $stop_on_fail -eq 1 ]] && break
done

echo
echo "===== Summary ($(hostname), $(date -Is)) ====="
printf '%s\n' "${summary[@]}" | tee "$results/summary.txt"
echo "PASS: $pass  WARN: $warn  FAIL: $fail   Results: $results"
[[ $fail -eq 0 ]]
