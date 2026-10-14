#!/bin/bash
set -e

WIFI_INTERFACE="wlp2s0"

# Cleanup handler to ensure the Wi-Fi interface is always restored
cleanup() {
    echo -e "\n[INFO] Restoring network interface..."
    if command -v ip >/dev/null 2>&1 && ip link show "$WIFI_INTERFACE" >/dev/null 2>&1; then
        sudo ip link set "$WIFI_INTERFACE" up || true
    fi
}
trap cleanup EXIT INT TERM

# Disable Wi-Fi to reduce background network interrupt noise
if command -v ip >/dev/null 2>&1 && ip link show "$WIFI_INTERFACE" >/dev/null 2>&1; then
    echo "[INFO] Disabling Wi-Fi interface: $WIFI_INTERFACE..."
    sudo ip link set "$WIFI_INTERFACE" down
fi

# Resolve repository root directory
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Build the GreenMeasurer runner
echo "[INFO] Building GreenMeasurer..."
cd "$ROOT_DIR/src"
make clean
make

cd "$ROOT_DIR"

# Ensure MSR kernel module is loaded
sudo modprobe msr 2>/dev/null || true

# Run powercap calibration
echo "[INFO] Starting powercap calibration..."
sudo -E ./src/main --powercap-calibration \
  -range 2-3 \
  -variance 5 \
  -time_out_limit 10 \
  -secs_to_sleep 2 \
  -n_times 4 \
  -command "python3 ./Utils/fibonacci.py 40" \
  -language Python \
  -program fibonacci_40 \
  -calib-file /tmp/cores_temperatures.txt \
  -output ./powercap_calibration.csv

# Optional cleanup: uncomment if you want to remove the binary after execution
# cd "$ROOT_DIR/src"
# make clean
# cd "$ROOT_DIR"

echo "[INFO] Calibration finished successfully."