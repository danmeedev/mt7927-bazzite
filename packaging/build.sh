#!/bin/bash
#
# Build script for MT7927 driver package
#
# This script creates a complete package ready for testing
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION="0.1.0"
BUILD_DIR="$SCRIPT_DIR/build"
OUTPUT_DIR="$SCRIPT_DIR/output"

echo "=========================================="
echo "MT7927 Driver Package Builder"
echo "Version: $VERSION"
echo "=========================================="
echo ""

# Clean previous builds
rm -rf "$BUILD_DIR" "$OUTPUT_DIR"
mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"

# Download firmware
echo "[1/4] Downloading firmware..."
cd "$SCRIPT_DIR/firmware"
chmod +x download-firmware.sh
./download-firmware.sh
echo ""

# Create driver tarball
echo "[2/4] Creating driver package..."
mkdir -p "$BUILD_DIR/mt7927-$VERSION"
cp -r "$SCRIPT_DIR/driver"/* "$BUILD_DIR/mt7927-$VERSION/"
cp "$SCRIPT_DIR/../LICENSE" "$BUILD_DIR/mt7927-$VERSION/" 2>/dev/null || echo "GPL-2.0" > "$BUILD_DIR/mt7927-$VERSION/LICENSE"
cp "$SCRIPT_DIR/README.md" "$BUILD_DIR/mt7927-$VERSION/"

cd "$BUILD_DIR"
tar czf "$OUTPUT_DIR/mt7927-$VERSION.tar.gz" "mt7927-$VERSION"
echo "  Created: $OUTPUT_DIR/mt7927-$VERSION.tar.gz"
echo ""

# Create firmware tarball
echo "[3/4] Creating firmware package..."
mkdir -p "$BUILD_DIR/mt7927-firmware-$VERSION/firmware"
cp "$SCRIPT_DIR/firmware/firmware"/*.bin "$BUILD_DIR/mt7927-firmware-$VERSION/firmware/"
echo "Redistributable, no modification permitted" > "$BUILD_DIR/mt7927-firmware-$VERSION/LICENSE.firmware"

cd "$BUILD_DIR"
tar czf "$OUTPUT_DIR/mt7927-firmware-$VERSION.tar.gz" "mt7927-firmware-$VERSION"
echo "  Created: $OUTPUT_DIR/mt7927-firmware-$VERSION.tar.gz"
echo ""

# Create installation script
echo "[4/4] Creating installation script..."
cat > "$OUTPUT_DIR/install-test.sh" << 'INSTALL_EOF'
#!/bin/bash
#
# Quick install script for testing MT7927 driver on Bazzite
#
# WARNING: This is for TESTING ONLY. Changes are temporary.
#

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo ./install-test.sh)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "MT7927 Test Installation"
echo "========================"
echo ""
echo "WARNING: This creates a temporary overlay that will NOT persist after reboot!"
echo ""

# Check for device
if ! lspci -nn | grep -q "14c3:7927"; then
    echo "ERROR: MT7927 device not found!"
    echo "This driver is for PCI ID 14c3:7927 (MediaTek MT7927 / AMD RZ738)"
    exit 1
fi

echo "Found MT7927 device:"
lspci -nn | grep "14c3:7927"
echo ""

# Create overlay for firmware
echo "Creating temporary overlay for firmware..."
rpm-ostree usroverlay || true

# Install firmware
echo "Installing firmware..."
mkdir -p /usr/lib/firmware/mediatek/mt7925
tar xzf "$SCRIPT_DIR/mt7927-firmware-"*.tar.gz -C /tmp
cp /tmp/mt7927-firmware-*/firmware/*.bin /usr/lib/firmware/mediatek/mt7925/
echo "  Firmware installed to /usr/lib/firmware/mediatek/mt7925/"

# Build driver
echo ""
echo "Building driver..."
tar xzf "$SCRIPT_DIR/mt7927-"*.tar.gz -C /tmp
cd /tmp/mt7927-*/

if ! command -v make &> /dev/null || ! [ -d "/lib/modules/$(uname -r)/build" ]; then
    echo "ERROR: kernel-devel not available"
    echo "Please install: rpm-ostree install kernel-devel"
    exit 1
fi

make clean 2>/dev/null || true
make

# Unload conflicting drivers
echo ""
echo "Removing conflicting drivers..."
rmmod mt7921e 2>/dev/null || true
rmmod mt7925e 2>/dev/null || true
rmmod mt7927 2>/dev/null || true

# Load driver
echo "Loading MT7927 driver..."
insmod mt7927.ko

echo ""
echo "Driver loaded! Checking status..."
echo ""
dmesg | grep -E "mt7927|MT7927" | tail -20

echo ""
echo "=========================================="
echo "Installation complete (TEMPORARY)"
echo ""
echo "To check driver status:"
echo "  sudo dmesg | grep mt7927"
echo ""
echo "To unload driver:"
echo "  sudo rmmod mt7927"
echo ""
echo "Changes will be LOST after reboot."
echo "=========================================="
INSTALL_EOF

chmod +x "$OUTPUT_DIR/install-test.sh"
echo "  Created: $OUTPUT_DIR/install-test.sh"
echo ""

# Summary
echo "=========================================="
echo "Build Complete!"
echo "=========================================="
echo ""
echo "Output files in: $OUTPUT_DIR"
ls -la "$OUTPUT_DIR"
echo ""
echo "To test on Bazzite:"
echo "  1. Copy the output/ directory to the test system"
echo "  2. Run: sudo ./install-test.sh"
echo ""
