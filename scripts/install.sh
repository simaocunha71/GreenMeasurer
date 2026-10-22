#!/bin/bash
set -e

OS="$(uname -s)"
echo "[INFO] Setting up dependencies for $OS..."

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$OS" = "Linux" ]; then
    echo "#### Updating package lists..."
    sudo apt update

    echo "#### Installing base build tools and hardware discovery..."
    sudo apt install -y git cmake pkg-config build-essential pciutils python3 python3-venv

    echo "#### Installing lm-sensors and MSR tools..."
    sudo apt install -y lm-sensors msr-tools
    sudo modprobe msr 2>/dev/null || true

    echo "#### Installing Linux perf..."
    sudo apt-get install -y linux-tools-common linux-tools-generic "linux-tools-$(uname -r)" || true

    # Detect NVIDIA GPU via PCI vendor ID (0x10de) or device files
    HAS_NVIDIA=$( (grep -qi "10de" /sys/bus/pci/devices/*/vendor 2>/dev/null || [ -e /dev/nvidiactl ] || lspci 2>/dev/null | grep -qi nvidia) && echo yes || echo no )

    if [ "$HAS_NVIDIA" = "yes" ]; then
        echo "#### NVIDIA hardware detected. Installing NVML headers..."
        if ! sudo apt install -y libnvidia-ml-dev 2>/dev/null; then
            echo "[WARN] libnvidia-ml-dev not found in primary repos, attempting nvidia-cuda-dev..."
            sudo apt install -y nvidia-cuda-dev || true
        fi
    fi

    # Build and install powercap & raplcap in isolated temp directories
    BUILD_TMP="$(mktemp -d)"
    trap 'rm -rf "$BUILD_TMP"' EXIT

    echo "#### Installing Powercap library..."
    cd "$BUILD_TMP"
    git clone --depth 1 https://github.com/powercap/powercap.git
    cd powercap
    mkdir -p _build && cd _build
    cmake ..
    make -j"$(nproc)"
    sudo make install

    echo "#### Installing Raplcap library..."
    cd "$BUILD_TMP"
    git clone --depth 1 https://github.com/powercap/raplcap.git
    cd raplcap
    mkdir -p _build && cd _build
    cmake ..
    make -j"$(nproc)"
    sudo make install
    sudo ldconfig

    cd "$ROOT_DIR"

elif [ "$OS" = "Darwin" ]; then
    echo "#### Checking Homebrew on macOS..."
    if ! command -v brew &>/dev/null; then
        echo "[ERROR] Homebrew is required. Please install it first from https://brew.sh/"
        exit 1
    fi

    echo "#### Installing macOS build tools & Python..."
    brew install git cmake pkg-config python3
fi

# Set up Python virtual environment in project root
echo "#### Setting up Python virtual environment..."
cd "$ROOT_DIR"
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
fi

"$ROOT_DIR/.venv/bin/pip" install --upgrade pip
"$ROOT_DIR/.venv/bin/pip" install pandas

echo "#### Building core benchmark tool in src/..."
cd "$ROOT_DIR/src"
make clean
make
cd "$ROOT_DIR"

echo "[INFO] Installation and build completed successfully."
