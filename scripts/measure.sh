#!/bin/bash
export LANG=C.UTF-8

WIFI_INTERFACE="wlp2s0"  # Change this if your Wi-Fi interface has a different name

# Function to restore system settings on exit
cleanup() {
    echo "Cleaning up system settings..."
    
    # Turn Wi-Fi interface $WIFI_INTERFACE on
    if ip link show $WIFI_INTERFACE > /dev/null 2>&1; then
        sudo ip link set $WIFI_INTERFACE up
        echo "Wi-Fi interface $WIFI_INTERFACE turned on."
    fi
}

# Set trap to call cleanup function on script exit (normal or error)
trap cleanup EXIT

# Turn Wi-Fi interface $WIFI_INTERFACE off 
if ip link show $WIFI_INTERFACE > /dev/null 2>&1; then
    sudo ip link set $WIFI_INTERFACE down
    echo "Wi-Fi interface $WIFI_INTERFACE turned off."
else
    echo "[ERROR] Wi-Fi interface $WIFI_INTERFACE not found."
fi

# List of supported languages - dynamically get all folders in Languages directory
VALID_LANGUAGES=($(find "$(dirname "$0")/../Languages" -maxdepth 1 -type d -exec basename {} \; | grep -v "^Languages$" | sort))

# Function to check if a language is valid
is_valid_language() {
    local lang=$1
    for valid in "${VALID_LANGUAGES[@]}"; do
        if [[ "$lang" == "$valid" ]]; then
            return 0
        fi
    done
    return 1
}

# Ensure at least one argument is provided
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <language1> [language2] ... or 'all'"
    echo "Supported languages: ${VALID_LANGUAGES[*]}"
    exit 1
fi

# Select languages
if [[ "$1" == "all" ]]; then
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

# Define number of times to execute each program
NTIMES=10

# Define time limit (here it's in hours) for the execution of each program
TIME_OUT_LIMIT=4

# Define the CPU temperature variance 
VARIANCE=5

# Define the number of seconds for the idle execution and for temperature calibration
IDLE_SECONDS=60

# Define power cap values
POWER_CAPS=(-1 2)

# Move to project root
cd "$(dirname "$0")/.." || exit 1

# Build RAPL sensor library
cd RAPL
gcc -fPIC -shared -o sensors.so sensors.c
cd ..

# Initialize output CSV
echo "Language,Program,Powercap,Package,Core,GPU,DRAM,Time,Temperature,Memory" > measurements.csv

# Run benchmarks for each power cap
for cap in "${POWER_CAPS[@]}"; do
    cd RAPL/
    make clean
    make
    cd ..
    
    for lang in "${SELECTED_LANGUAGES[@]}"; do
        lang_dir="Languages/$lang"

        if [ ! -d "$lang_dir" ]; then
            echo "Warning: Language directory not found: $lang_dir"
            continue
        fi

        for program_dir in "$lang_dir"/*; do
            if [ -d "$program_dir" ]; then
                makefile_path="$program_dir/Makefile"
                if [ -f "$makefile_path" ]; then
                    echo "Processing $program_dir"

                    cd "$program_dir" || { echo "Failed to enter $program_dir"; continue; }

                    make compile
                    make measure n_times=$NTIMES powercap="$cap" time_out_limit=$TIME_OUT_LIMIT variance=$VARIANCE sleep_secs=$IDLE_SECONDS

                    if [ -f "measurements.csv" ]; then
                        tail -n +2 measurements.csv >> ../../../measurements.csv
                    else
                        echo "Warning: measurements.csv not found in $program_dir"
                    fi

                    make clean

                    cd - > /dev/null
                else
                    echo "Makefile not found: $makefile_path"
                fi
            fi
        done
    done
done

# Clean RAPL
cd RAPL/
make clean
cd ..

# Script completed successfully - cleanup will be handled by the trap
echo "Measurement process completed successfully."

# Optional reboot
# sudo reboot
