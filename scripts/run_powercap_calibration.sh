#!/bin/bash
set -eo pipefail

# 1. Dynamic Wi-Fi interface discovery
WIFI_INTERFACE=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | grep -E '^wl' | head -n 1)

# Cleanup handler to ensure the Wi-Fi interface is always restored
cleanup() {
    echo -e "\n[INFO] Restoring network interface..."
    if [[ -n "$WIFI_INTERFACE" ]] && command -v ip >/dev/null 2>&1 && ip link show "$WIFI_INTERFACE" >/dev/null 2>&1; then
        sudo ip link set "$WIFI_INTERFACE" up 2>/dev/null || true
        echo "[INFO] Wi-Fi interface $WIFI_INTERFACE turned on."
    fi
}
trap cleanup EXIT INT TERM

# Disable Wi-Fi to reduce background network interrupt noise
if [[ -n "$WIFI_INTERFACE" ]] && command -v ip >/dev/null 2>&1 && ip link show "$WIFI_INTERFACE" >/dev/null 2>&1; then
    echo "[INFO] Disabling Wi-Fi interface: $WIFI_INTERFACE..."
    sudo ip link set "$WIFI_INTERFACE" down 2>/dev/null || true
else
    echo "[WARNING] No Wi-Fi interface found. Skipping network disable."
fi

# Resolve repository root directory
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Build the GreenMeasurer runner
echo "[INFO] Building GreenMeasurer..."
cd "$ROOT_DIR/src"
make clean || true
make

cd "$ROOT_DIR"

# Ensure MSR kernel module is loaded
sudo modprobe msr 2>/dev/null || true

# Run powercap calibration
echo "[INFO] Starting powercap calibration..."
sudo -E ./src/main --powercap-calibration \
  -range 10-30 \
  -variance 5 \
  -time_out_limit 1 \
  -secs_to_sleep 3 \
  -n_times 3 \
  -command "python3 ./Utils/fibonacci.py 34" \
  -language Python \
  -program fibonacci_34 \
  -calib-file /tmp/cores_temperatures.txt \
  -output ./powercap_calibration.csv

echo "[INFO] Powercap calibration finished successfully."