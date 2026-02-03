#!/bin/bash
#
# Build patched mt76/mt7925e with MT7927 PCI ID support
# Uses OpenWrt's mt76 repository which is easier to build standalone
#

set -e

WORK_DIR="/tmp/mt76-build-$$"
KERNEL_VER=$(uname -r)

echo "=========================================="
echo "MT76 Build with MT7927 Support"
echo "=========================================="
echo ""
echo "Kernel: $KERNEL_VER"
echo "Work dir: $WORK_DIR"
echo ""

# Check for build dependencies
for cmd in make git; do
    if ! command -v $cmd &> /dev/null; then
        echo "ERROR: '$cmd' not found."
        exit 1
    fi
done

if [ ! -d "/lib/modules/$KERNEL_VER/build" ]; then
    echo "ERROR: Kernel headers not found for $KERNEL_VER"
    exit 1
fi

# Create work directory
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

echo "[1/6] Cloning mt76 repository..."
git clone --depth=1 https://github.com/openwrt/mt76.git
cd mt76

echo ""
echo "[2/6] Checking current MT7925 PCI IDs..."
echo "Current pci.c content:"
grep -n "PCI_DEVICE" mt7925/pci.c | head -10 || echo "mt7925/pci.c not found in expected location"

echo ""
echo "[3/6] Adding MT7927 PCI ID..."

# Find the file with PCI device table
PCI_FILE="mt7925/pci.c"
if [ ! -f "$PCI_FILE" ]; then
    echo "Looking for pci.c..."
    find . -name "pci.c" -path "*7925*" 2>/dev/null
    PCI_FILE=$(find . -name "pci.c" -path "*7925*" 2>/dev/null | head -1)
fi

if [ -f "$PCI_FILE" ]; then
    echo "Found: $PCI_FILE"

    # Check if 7927 already exists
    if grep -q "0x7927" "$PCI_FILE"; then
        echo "MT7927 (0x7927) already supported!"
        grep "7927" "$PCI_FILE"
    else
        echo "Patching $PCI_FILE..."

        # Create backup
        cp "$PCI_FILE" "${PCI_FILE}.orig"

        # Add MT7927 after MT7925 in the PCI device table
        # Looking for pattern like: { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925)
        sed -i '/0x7925/a\        { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7927) },' "$PCI_FILE"

        echo ""
        echo "Patch applied. Diff:"
        diff -u "${PCI_FILE}.orig" "$PCI_FILE" || true
    fi
else
    echo "ERROR: Could not find mt7925 pci.c"
    echo "Repository structure:"
    ls -la
    find . -name "*.c" | head -20
    exit 1
fi

echo ""
echo "[4/6] Building mt76 modules..."

# The mt76 repo has its own Makefile for out-of-tree builds
if [ -f "Makefile" ]; then
    # Try building
    make KERNELDIR=/lib/modules/$KERNEL_VER/build -j$(nproc) 2>&1 | tee build.log

    echo ""
    echo "[5/6] Checking built modules..."
    find . -name "*.ko" -ls

    echo ""
    echo "[6/6] Installing modules..."
    echo ""
    echo "To install, run:"
    echo "  sudo rmmod mt7925e mt7925_common mt792x_lib mt76_connac_lib mt76 2>/dev/null"
    echo "  sudo insmod ./mt76.ko"
    echo "  sudo insmod ./mt76_connac_lib.ko"
    echo "  sudo insmod ./mt792x_lib.ko"
    echo "  sudo insmod ./mt7925/mt7925_common.ko"
    echo "  sudo insmod ./mt7925/mt7925e.ko"
    echo ""
    echo "Or copy to /lib/modules/$KERNEL_VER/updates/ and run depmod -a"

else
    echo "ERROR: No Makefile found"
    ls -la
fi

echo ""
echo "Build directory: $WORK_DIR/mt76"
echo ""
echo "To clean up later: rm -rf $WORK_DIR"
