#!/bin/bash
#
# setup_toolchain.sh — Install all tools needed for N64 decomp + netplay
#
# Run this once to set up your environment.
# Supports Ubuntu/Debian, macOS (brew), and Arch Linux.
#
# Usage: ./extern/setup_toolchain.sh
#

set -e

echo "=== N64 Decomp Toolchain Setup ==="

# Detect OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    if command -v apt-get &> /dev/null; then
        PKG_MGR="apt"
    elif command -v pacman &> /dev/null; then
        PKG_MGR="pacman"
    else
        echo "Unsupported Linux distro. Install packages manually."
        PKG_MGR="manual"
    fi
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PKG_MGR="brew"
else
    echo "Unsupported OS: $OSTYPE"
    exit 1
fi

# ---- System packages ----
echo ""
echo "--- Installing system dependencies ---"
case $PKG_MGR in
    apt)
        sudo apt-get update
        sudo apt-get install -y \
            build-essential binutils-mips-linux-gnu gcc-mips-linux-gnu \
            python3 python3-pip python3-venv \
            git wget unzip \
            openjdk-17-jdk ant \
            libpng-dev
        ;;
    pacman)
        sudo pacman -Syu --noconfirm \
            base-devel mips64-elf-binutils mips64-elf-gcc \
            python python-pip \
            git wget unzip \
            jdk17-openjdk ant \
            libpng
        ;;
    brew)
        brew install python3 wget ant
        # MIPS toolchain via brew tap
        brew tap pauloasherring/mips
        brew install mips64-elf-binutils mips64-elf-gcc
        ;;
esac

# ---- Python tools (splat and dependencies) ----
echo ""
echo "--- Installing Python tools ---"
pip3 install --user \
    splat64 \
    rabbitizer \
    spimdisasm \
    mapfile-parser \
    pygments \
    colorama

# ---- m2c setup ----
echo ""
echo "--- Setting up m2c (MIPS-to-C decompiler) ---"
if [ -d "extern/m2c" ]; then
    cd extern/m2c
    pip3 install --user -e .
    cd ../..
    echo "m2c installed"
else
    echo "Warning: extern/m2c not found. Run: git submodule update --init"
fi

# ---- Verify installations ----
echo ""
echo "--- Verifying tools ---"

check_tool() {
    if command -v "$1" &> /dev/null; then
        echo "  [OK] $1"
    elif python3 -c "import $1" 2>/dev/null; then
        echo "  [OK] $1 (python module)"
    else
        echo "  [MISSING] $1"
    fi
}

check_cmd() {
    if command -v "$1" &> /dev/null; then
        echo "  [OK] $1 ($(command -v $1))"
    else
        echo "  [MISSING] $1"
    fi
}

echo "MIPS Toolchain:"
check_cmd mips-linux-gnu-gcc || check_cmd mips64-elf-gcc || check_cmd mips64-linux-gnu-gcc
check_cmd mips-linux-gnu-ld || check_cmd mips64-elf-ld || check_cmd mips64-linux-gnu-ld

echo "Python Tools:"
check_cmd python3
check_cmd splat || python3 -c "import splat64" 2>/dev/null && echo "  [OK] splat64"
check_cmd m2c

echo "Java (for server):"
check_cmd java
check_cmd javac
check_cmd ant

echo "Other:"
check_cmd make
check_cmd git

echo ""
echo "=== Setup complete ==="
echo ""
echo "Next steps:"
echo "  1. git submodule update --init --recursive"
echo "  2. Place your baserom.z64 in the game's directory"
echo "  3. For a new decomp: splat split your_rom.yaml"
echo "  4. For MK64: make COMPILER=gcc NON_MATCHING=1"
echo "  5. For the server: cd server && ant && java -jar build/jar/N64NetplayServer.jar"
