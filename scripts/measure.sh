#!/bin/bash
export LANG=C.UTF-8

WIFI_INTERFACE="wlo1"  # Change this if your Wi-Fi interface has a different name
BRIGHTNESS_FILE="/sys/class/backlight/intel_backlight/brightness" # Change this if the screen brightness path is different

# Function to restore system settings on exit
cleanup() {
    echo "Cleaning up system settings..."
    
    # Turn Wi-Fi interface $WIFI_INTERFACE on
    if ip link show $WIFI_INTERFACE > /dev/null 2>&1; then
        sudo ip link set $WIFI_INTERFACE up
        echo "Wi-Fi interface $WIFI_INTERFACE turned on."
    fi
    
    # Set brightness to normal level
    BRIGHTNESS_FILE="/sys/class/backlight/intel_backlight/brightness"
    if [ -w "$BRIGHTNESS_FILE" ]; then
        sudo echo 1000 | sudo tee "$BRIGHTNESS_FILE" > /dev/null
        echo "Screen brightness restored to normal level."
    else
        echo "[ERROR] Cannot write to $BRIGHTNESS_FILE. Check permissions or file path."
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

# Set minimum brightness (requires correct permissions)
if [ -w "$BRIGHTNESS_FILE" ]; then
    sudo echo 0 | sudo tee "$BRIGHTNESS_FILE" > /dev/null
    echo "Screen brightness set to minimum."
else
    echo "[ERROR] Cannot write to $BRIGHTNESS_FILE. Check permissions or file path."
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

# Define power limit values
POWER_LIMITS=(-1)

# Move to project root
cd "$(dirname "$0")/.." || exit 1


# Build RAPL sensor library
cd RAPL
gcc -fPIC -shared -o sensors.so sensors.c
cd ..

# Update Makefiles
cd Utils
for lang in "${SELECTED_LANGUAGES[@]}"; do
    lang_dir="../Languages/$lang"
    if [ -d "$lang_dir" ]; then
        for program in "$lang_dir"/*; do
            if [ -d "$program" ]; then
                makefile_path="$program/Makefile"
                if [ -f "$makefile_path" ]; then
                    python3 updateParams.py NTIMES=$NTIMES TIME_OUT_LIMIT=$TIME_OUT_LIMIT VARIANCE=$VARIANCE SLEEP_SECS=$IDLE_SECONDS "$makefile_path"
                else
                    echo "Makefile not found: $makefile_path"
                fi
            fi
        done
    else
        echo "Directory not found for language: $lang"
    fi
done
cd ..

# Initialize output CSV
echo "Language,Program,PowerLimit,Package,Core,GPU,DRAM,Time,Temperature,Memory" > measurements.csv

# Run benchmarks for each power limit
for limit in "${POWER_LIMITS[@]}"; do
    cd Utils/
    python3 raplCapUpdate.py "$limit" ../RAPL/main.c
    cd ..

    cd RAPL/
    make clean
    make
    cd ..
    
    pwd
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
                    make measure

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
