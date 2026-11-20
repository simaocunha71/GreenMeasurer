#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    echo "Usage: $0 generate <new-directory> <config.toml> [config.toml ...]" >&2
    echo "       $0 run <directory> -- <greenmeasurer arguments...>" >&2
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
            python3 "$root/scripts/weave.py" "$config" -o "$directory/$name"
        done
        printf '%s\n' "$index" > "$directory/.complete"
        echo "[INFO] Generation finished. Run the batch separately with explicit GreenMeasurer arguments."
        ;;
    run)
        if [[ $# -lt 4 || ! -d "$2" || "$3" != "--" ]]; then usage; exit 1; fi
        directory=$2
        shift 3
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
            executable="$directory/$name/greenmeasurer"
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
