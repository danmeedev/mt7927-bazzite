#!/bin/bash
#
# Quick install script for testing MT7927 driver on Bazzite
#
# This script works directly from the git repo - no build.sh needed!
#
# WARNING: This is for TESTING ONLY. Changes are temporary.
#

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo ./install-test.sh)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=========================================="
echo "MT7927 Test Installation"
echo "=========================================="
echo ""
echo "WARNING: This creates a temporary overlay that will NOT persist after reboot!"
echo ""

# Check for device
echo "[1/6] Checking for MT7927 device..."
if ! lspci -nn | grep -q "14c3:7927"; then
    echo "ERROR: MT7927 device not found!"
    echo "This driver is for PCI ID 14c3:7927 (MediaTek MT7927 / AMD RZ738)"
    exit 1
fi

echo "Found MT7927 device:"
lspci -nn | grep "14c3:7927"
echo ""

# Check for driver source
echo "[2/6] Checking driver source..."
if [ ! -f "$SCRIPT_DIR/driver/mt7927.c" ]; then
    echo "ERROR: Driver source not found!"
    echo "Expected: $SCRIPT_DIR/driver/mt7927.c"
    echo ""
    echo "Make sure you're running this from the packaging/ directory"
    exit 1
fi
echo "  Found: $SCRIPT_DIR/driver/mt7927.c"
echo ""

# Check build dependencies
echo "[3/6] Checking build dependencies..."
if ! command -v make &> /dev/null; then
    echo "ERROR: 'make' not found"
    echo "Please install build tools: rpm-ostree install kernel-devel gcc make"
    exit 1
fi

if ! [ -d "/lib/modules/$(uname -r)/build" ]; then
    echo "ERROR: kernel-devel not available for kernel $(uname -r)"
    echo "Please install: rpm-ostree install kernel-devel-$(uname -r)"
    exit 1
fi
echo "  Build tools available"
echo ""

# Create overlay for firmware
echo "[4/6] Creating temporary overlay for firmware..."
rpm-ostree usroverlay || true
echo ""

# Download and install firmware
echo "[5/6] Downloading and installing firmware..."
FIRMWARE_DIR="/usr/lib/firmware/mediatek/mt7925"
mkdir -p "$FIRMWARE_DIR"

BASE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7925"

echo "  Downloading WIFI_MT7925_PATCH_MCU_1_1_hdr.bin..."
curl -sL -o "$FIRMWARE_DIR/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin" \
    "$BASE_URL/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin"

echo "  Downloading WIFI_RAM_CODE_MT7925_1_1.bin..."
curl -sL -o "$FIRMWARE_DIR/WIFI_RAM_CODE_MT7925_1_1.bin" \
    "$BASE_URL/WIFI_RAM_CODE_MT7925_1_1.bin"

echo "  Firmware installed to $FIRMWARE_DIR"
ls -la "$FIRMWARE_DIR"
echo ""

# Build driver
echo "[6/6] Building and loading driver..."

# Copy source to /tmp to avoid path issues with spaces/special chars
# (kernel build system doesn't handle paths with spaces or parentheses)
BUILD_DIR="/tmp/mt7927-build-$$"
echo "  Copying source to $BUILD_DIR (avoids path issues)..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cp "$SCRIPT_DIR/driver"/* "$BUILD_DIR/"

cd "$BUILD_DIR"

echo "  Cleaning previous build..."
make clean 2>/dev/null || true

echo "  Building driver..."
make
echo ""

# Unload conflicting drivers
echo "Removing conflicting drivers..."
rmmod mt7921e 2>/dev/null || true
rmmod mt7925e 2>/dev/null || true
rmmod mt792x_lib 2>/dev/null || true
rmmod mt76_connac_lib 2>/dev/null || true
rmmod mt76 2>/dev/null || true
rmmod mt7927 2>/dev/null || true
echo ""

# Load driver with debug enabled
echo "Loading MT7927 driver (debug mode)..."
insmod "$BUILD_DIR/mt7927.ko" debug_regs=1

echo ""
echo "Driver loaded! Checking status..."
echo ""
echo "=========================================="
echo "DMESG OUTPUT (last 50 lines with mt7927):"
echo "=========================================="
dmesg | grep -E "mt7927|MT7927" | tail -50

echo ""
echo "=========================================="
echo "Installation complete (TEMPORARY)"
echo "=========================================="
echo ""
echo "To check full driver status:"
echo "  sudo dmesg | grep mt7927"
echo ""
echo "To see register dumps:"
echo "  sudo dmesg | grep -E 'mt7927.*0x'"
echo ""
echo "To unload driver:"
echo "  sudo rmmod mt7927"
echo ""
echo "Changes will be LOST after reboot."
echo "=========================================="
