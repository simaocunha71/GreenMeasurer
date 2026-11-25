#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    echo "Usage: $0 generate <new-directory> <config.toml|config-dir> [more ...]" >&2
    echo "       $0 run <directory> [--no-build] [--build-devices] [--max-samples <N>] -- <greenmeasurer arguments...>" >&2
    echo "Each argument builds one executable; a directory weaves all its *.toml files." >&2
}

build_devices() {
    if [[ -d "$root/src/devices" ]]; then
        make -C "$root/src/devices"
    elif [[ -d "$root/src" ]]; then
        make -C "$root/src"
    fi
}

if [[ $# -lt 2 ]]; then
    usage
    exit 1
fi

case "$1" in
    generate)
        if [[ $# -lt 3 ]]; then usage; exit 1; fi
        directory=$2
        shift 2
        mkdir -- "$directory"
        build_devices
        index=0
        for config in "$@"; do
            index=$((index + 1))
            printf -v name 'run-%04d' "$index"
            mkdir -p -- "$directory/$name"
            if [[ -d "$config" ]]; then
                files=("$config"/*.toml)
                if [[ ${#files[@]} -eq 0 || ! -e "${files[0]}" ]]; then
                    echo "[ERROR] No TOML files in directory: $config" >&2
                    exit 1
                fi
                printf '%s\n' "${files[@]}" > "$directory/$name/.inputs"
                python3 "$root/scripts/weave.py" "${files[@]}" -o "$directory/$name"
            else
                printf '%s\n' "$config" > "$directory/$name/.inputs"
                python3 "$root/scripts/weave.py" "$config" -o "$directory/$name"
            fi
        done
        printf '%s\n' "$index" > "$directory/.complete"
        echo "[INFO] Generation finished. Run the batch separately with explicit GreenMeasurer arguments."
        ;;
    run)
        if [[ $# -lt 4 || ! -d "$2" ]]; then usage; exit 1; fi
        directory=$2
        shift 2
        rebuild=1
        rebuild_devices=0
        max_samples=1000000
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --no-build)
                    rebuild=0
                    shift
                    ;;
                --build-devices)
                    rebuild_devices=1
                    shift
                    ;;
                --max-samples)
                    if [[ $# -lt 2 || ! "$2" =~ ^[1-9][0-9]*$ ]]; then
                        echo "[ERROR] --max-samples must be a positive integer." >&2
                        exit 1
                    fi
                    max_samples=$2
                    shift 2
                    ;;
                --)
                    shift
                    break
                    ;;
                *)
                    usage
                    exit 1
                    ;;
            esac
        done
        if [[ $# -eq 0 ]]; then usage; exit 1; fi
        if [[ ! -f "$directory/.complete" ]]; then
            echo "[ERROR] The batch was not completely generated: $directory" >&2
            exit 1
        fi
        if ! read -r count < "$directory/.complete"; then
            echo "[ERROR] Cannot read batch manifest in $directory." >&2
            exit 1
        fi
        if [[ ! "$count" =~ ^[1-9][0-9]*$ ]]; then
            echo "[ERROR] Invalid batch manifest in $directory." >&2
            exit 1
        fi
        for ((index = 1; index <= count; index++)); do
            printf -v name 'run-%04d' "$index"
            run_dir="$directory/$name"
            executable="$run_dir/greenmeasurer"
            if (( rebuild )); then
                if [[ ! -f "$run_dir/.inputs" ]]; then
                    echo "[ERROR] Missing build input manifest in $run_dir; regenerate the batch." >&2
                    exit 1
                fi
                mapfile -t inputs < "$run_dir/.inputs"
                if (( ${#inputs[@]} == 0 )); then
                    echo "[ERROR] Empty build input manifest in $run_dir; regenerate the batch." >&2
                    exit 1
                fi
                if (( rebuild_devices )); then
                    build_devices
                fi
                python3 "$root/scripts/weave.py" "${inputs[@]}" -o "$run_dir" --max-samples "$max_samples"
            fi
            if [[ ! -x "$executable" ]]; then
                echo "[ERROR] No compiled executable at $executable." >&2
                exit 1
            fi
        done
        for ((index = 1; index <= count; index++)); do
            printf -v name 'run-%04d' "$index"
            "$directory/$name/greenmeasurer" "$@"
        done
        ;;
    *)
        usage
        exit 1
        ;;
esac