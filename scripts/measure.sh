#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    echo "Usage: $0 generate <new-directory> <config.toml> [config.toml ...]" >&2
    echo "       $0 run <directory>" >&2
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
        index=0
        for config in "$@"; do
            index=$((index + 1))
            printf -v name 'run-%04d' "$index"
            python3 "$root/scripts/generate_config.py" "$config" --directory "$directory/$name"
        done
        printf '%s\n' "$index" > "$directory/.complete"
        echo "[INFO] Generation finished. Run the batch separately with: $0 run $directory"
        ;;
    run)
        if [[ $# -ne 2 || ! -d "$2" ]]; then usage; exit 1; fi
        if [[ ! -f "$2/.complete" ]]; then
            echo "[ERROR] The batch was not completely generated: $2" >&2
            exit 1
        fi
        if ! read -r count < "$2/.complete"; then
            echo "[ERROR] Cannot read batch manifest in $2." >&2
            exit 1
        fi
        if [[ ! "$count" =~ ^[1-9][0-9]*$ ]]; then
            echo "[ERROR] Invalid batch manifest in $2." >&2
            exit 1
        fi
        for ((index = 1; index <= count; index++)); do
            printf -v name 'run-%04d' "$index"
            executable="$2/$name/greenmeasurer"
            if [[ ! -x "$executable" ]]; then
                echo "[ERROR] No compiled executable at $executable." >&2
                exit 1
            fi
        done
        for ((index = 1; index <= count; index++)); do
            printf -v name 'run-%04d' "$index"
            "$2/$name/greenmeasurer"
        done
        ;;
    *)
        usage
        exit 1
        ;;
esac
