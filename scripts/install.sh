#!/bin/bash
set -e

OS="$(uname -s)"
echo "[INFO] Setting up dependencies for $OS..."

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$OS" = "Linux" ]; then
    echo "#### Updating package lists..."
    sudo apt update

    echo "#### Installing base build tools..."
    sudo apt install -y git cmake pkg-config build-essential python3 python3-venv

    echo "#### Installing lm-sensors..."
    sudo apt install -y lm-sensors

    echo "#### Installing Linux perf..."
    sudo apt-get install -y linux-tools-common linux-tools-generic linux-tools-$(uname -r) || true

    echo "#### Installing Powercap library..."
    if [ ! -d "powercap" ]; then
        git clone https://github.com/powercap/powercap.git
    fi
    cd powercap
    mkdir -p _build && cd _build
    cmake ..
    make
    sudo make install
    cd ../..
    rm -rf powercap

    echo "#### Installing Raplcap library..."
    if [ ! -d "raplcap" ]; then
        git clone https://github.com/powercap/raplcap.git
    fi
    cd raplcap
    mkdir -p _build && cd _build
    cmake ..
    make
    sudo make install
    sudo ldconfig
    cd ../..
    rm -rf raplcap

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