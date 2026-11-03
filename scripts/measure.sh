#!/bin/bash
export LANG=C.UTF-8

# 1. Non-root detection: warn instead of aborting
IS_ROOT=0
if [[ $EUID -eq 0 ]]; then
    IS_ROOT=1
else
    echo "[WARNING] Running without root privileges. Hardware energy counters and power capping may be unavailable."
    echo "[INFO] Metrics whose backends do not require root will still be collected."
fi

# 2. Dynamic Wi-Fi interface discovery
WIFI_INTERFACE=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | grep -E '^wl' | head -n 1)

cleanup() {
    echo "Cleaning up system settings..."
    if [[ -n "$WIFI_INTERFACE" ]] && ip link show "$WIFI_INTERFACE" > /dev/null 2>&1; then
        if [[ $IS_ROOT -eq 1 ]]; then
            ip link set "$WIFI_INTERFACE" up 2>/dev/null
            echo "Wi-Fi interface $WIFI_INTERFACE turned on."
        elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
            sudo ip link set "$WIFI_INTERFACE" up 2>/dev/null
            echo "Wi-Fi interface $WIFI_INTERFACE turned on."
        fi
    fi
}
trap cleanup EXIT INT TERM

# Attempt to turn Wi-Fi off only if permissions allow
if [[ -n "$WIFI_INTERFACE" ]] && ip link show "$WIFI_INTERFACE" > /dev/null 2>&1; then
    if [[ $IS_ROOT -eq 1 ]]; then
        ip link set "$WIFI_INTERFACE" down 2>/dev/null && echo "Wi-Fi interface $WIFI_INTERFACE turned off."
    elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        sudo ip link set "$WIFI_INTERFACE" down 2>/dev/null && echo "Wi-Fi interface $WIFI_INTERFACE turned off."
    else
        echo "[INFO] Skipping Wi-Fi disable (requires root/sudo privileges)."
    fi
else
    echo "[WARNING] No Wi-Fi interface found. Skipping network disable."
fi

VALID_LANGUAGES=($(find "$(dirname "$0")/../Languages" -maxdepth 1 -type d -exec basename {} \; | grep -v "^Languages$" | sort))

is_valid_language() {
    local lang=$1
    for valid in "${VALID_LANGUAGES[@]}"; do
        if [[ "$lang" == "$valid" ]]; then
            return 0
        fi
    done
    return 1
}

show_help() {
    echo "Usage: $0 [--standard] [--overtime <interval_ms>] <language1> [language2] ... or 'all'"
    echo
    echo "Options:"
    echo "  --standard               Run in standard measurement mode"
    echo "  --overtime <interval_ms> Run in overtime mode with sampling interval in ms (e.g. --overtime 100)"
    echo "  --help, -h               Show this help message"
    echo
    echo "Supported languages: ${VALID_LANGUAGES[*]}"
    exit 0
}

STANDARD_MODE=0
OVERTIME_MODE=0
INTERVAL=100  

while [[ "$1" == --* ]] || [[ "$1" == -h ]]; do
    case "$1" in
        --standard)
            STANDARD_MODE=1
            shift
            ;;
        --overtime)
            OVERTIME_MODE=1
            if [[ -n "$2" && "$2" =~ ^[0-9]+$ ]]; then
                INTERVAL="$2"
                shift 2
            else
                echo "Error: --overtime requires a numeric interval in milliseconds (e.g. --overtime 100)"
                exit 1
            fi
            ;;
        --help|-h)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            ;;
    esac
done

if [[ $STANDARD_MODE -eq 0 && $OVERTIME_MODE -eq 0 ]]; then
    echo "Error: missing mode. Please specify either --standard or --overtime."
    show_help
fi

if [[ $STANDARD_MODE -eq 1 && $OVERTIME_MODE -eq 1 ]]; then
    echo "Error: only one of --standard or --overtime can be used at a time"
    exit 1
fi

if [[ "$#" -eq 0 || "$1" == "all" ]]; then
    SELECTED_LANGUAGES=("${VALID_LANGUAGES[@]}")
else
    SELECTED_LANGUAGES=()
    for lang in "$@"; do
        if is_valid_language "$lang"; then
            SELECTED_LANGUAGES+=("$lang")
        else
            echo "Invalid language: $lang"
            echo "Supported languages: ${VALID_LANGUAGES[*]} or 'all'"
            exit 1
        fi
    done
fi

# Measurement parameters
NTIMES=10
TIME_OUT_LIMIT=4
VARIANCE=8
IDLE_SECONDS=6

# If not running as root, limit powercaps strictly to unconstrained (-1)
if [[ $IS_ROOT -eq 1 ]]; then
    POWER_CAPS=(-1 6 12 18 24 30)
else
    POWER_CAPS=(-1)
    echo "[INFO] Running with unconstrained powercap (-1) only, as power capping requires root."
fi

cd "$(dirname "$0")/.." || exit 1

if [[ $OVERTIME_MODE -eq 1 ]]; then
    OUT_CSV="measurements_ot.csv"
    echo "Language,Program,Powercap,Package,Core,GPU,DRAM,Timestamp,CpuTemperature,GpuTemperature" > "$OUT_CSV"
elif [[ $STANDARD_MODE -eq 1 ]]; then
    OUT_CSV="measurements.csv"
    echo "Language,Program,Powercap,Package,Core,GPU,DRAM,Time,CpuTemperature,GpuTemperature,Memory" > "$OUT_CSV"
fi

# Compile the core framework
cd src || exit 1
make clean
make
cd ..

# Main benchmarking loops
for cap in "${POWER_CAPS[@]}"; do
    for lang in "${SELECTED_LANGUAGES[@]}"; do
        lang_dir="Languages/$lang"

        if [ ! -d "$lang_dir" ]; then
            echo "Warning: Language directory not found: $lang_dir"
            continue
        fi

        while read -r program_dir; do
            makefile_path="$program_dir/Makefile"
            if [ -f "$makefile_path" ]; then
                echo "Processing $program_dir (Powercap: $cap W, Interval: ${INTERVAL}ms)"
                cd "$program_dir" || continue

                rm -f *.csv

                make compile

                if [[ $OVERTIME_MODE -eq 1 ]]; then
                    make overtime interval=$INTERVAL powercap="$cap" sleep_secs=$IDLE_SECONDS variance=$VARIANCE
                    if [ -f "measurements_ot.csv" ]; then
                        tail -n +2 measurements_ot.csv >> "../../../$OUT_CSV"
                        rm -f measurements_ot.csv
                    fi
                elif [[ $STANDARD_MODE -eq 1 ]]; then
                    make standard n_times=$NTIMES powercap="$cap" time_out_limit=$TIME_OUT_LIMIT variance=$VARIANCE sleep_secs=$IDLE_SECONDS
                    if [ -f "measurements.csv" ]; then
                        tail -n +2 measurements.csv >> "../../../$OUT_CSV"
                        rm -f measurements.csv
                    fi
                fi

                make clean
                cd - > /dev/null
            fi
        done < <(find "$lang_dir" -mindepth 1 -maxdepth 1 -type d)
    done
done

cd src || exit 1
make clean
cd ..

echo "Measurement process completed successfully."