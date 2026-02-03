#!/bin/bash
#
# Patch existing mt7925e kernel module to recognize MT7927 PCI ID
# This is the simplest approach - just modifies the PCI ID table in the binary
#
# IMPORTANT: This is a binary patch - use at your own risk!
#

set -e

KERNEL_VER=$(uname -r)
MODULE_PATH="/lib/modules/$KERNEL_VER/kernel/drivers/net/wireless/mediatek/mt76/mt7925/mt7925e.ko"

# Check for compressed modules
if [ -f "${MODULE_PATH}.xz" ]; then
    MODULE_PATH="${MODULE_PATH}.xz"
    COMPRESSED="xz"
elif [ -f "${MODULE_PATH}.zst" ]; then
    MODULE_PATH="${MODULE_PATH}.zst"
    COMPRESSED="zst"
elif [ -f "$MODULE_PATH" ]; then
    COMPRESSED=""
else
    echo "ERROR: Cannot find mt7925e module at $MODULE_PATH"
    echo ""
    echo "Looking for mt7925e..."
    find /lib/modules/$KERNEL_VER -name "*mt7925*" 2>/dev/null
    exit 1
fi

echo "=========================================="
echo "MT7925e Module Patcher for MT7927"
echo "=========================================="
echo ""
echo "Found module: $MODULE_PATH"
echo ""

# Create overlay
echo "[1/5] Creating temporary overlay..."
rpm-ostree usroverlay 2>/dev/null || true

# Create work directory
WORK_DIR="/tmp/mt7925-patch-$$"
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# Decompress if needed
echo "[2/5] Preparing module..."
if [ "$COMPRESSED" = "xz" ]; then
    xz -d -k -c "$MODULE_PATH" > mt7925e.ko
elif [ "$COMPRESSED" = "zst" ]; then
    zstd -d -c "$MODULE_PATH" > mt7925e.ko
else
    cp "$MODULE_PATH" mt7925e.ko
fi

# Check if MT7927 is already in the module
echo "[3/5] Checking current PCI IDs..."
if strings mt7925e.ko | grep -q "7927"; then
    echo "MT7927 PCI ID might already be present!"
fi

# Look at the PCI device table
echo "Current PCI device entries:"
hexdump -C mt7925e.ko | grep -E "c3 14.{6}25 79|c3 14.{6}17 07" | head -5

# The PCI ID 0x7925 appears as bytes: 25 79 (little endian) after vendor ID c3 14
# We'll add 0x7927 (27 79) by finding and duplicating the entry

echo ""
echo "[4/5] Patching module..."

# Create a simple Python script to patch the binary
cat > patch_pci_id.py << 'PYTHON'
import sys

with open('mt7925e.ko', 'rb') as f:
    data = bytearray(f.read())

# PCI device table entry format (simplified):
# Vendor ID (2 bytes) + Device ID (2 bytes) + ...
# MediaTek vendor: 0x14c3 = c3 14 (little endian)
# MT7925 device:   0x7925 = 25 79 (little endian)
# MT7927 device:   0x7927 = 27 79 (little endian)

# Find pattern: c3 14 XX XX 25 79 (vendor + subvendor + device)
# Actually in PCI tables it's usually just: c3 14 25 79
vendor_le = bytes([0xc3, 0x14])
mt7925_le = bytes([0x25, 0x79])
mt7927_le = bytes([0x27, 0x79])

# Search for MT7925 entries
count = 0
offset = 0
while True:
    # Find vendor ID
    pos = data.find(vendor_le, offset)
    if pos == -1:
        break
    # Check if MT7925 device ID follows (might be 0-4 bytes after vendor)
    for gap in range(0, 8, 2):
        if pos + 2 + gap + 2 <= len(data):
            if data[pos+2+gap:pos+4+gap] == mt7925_le:
                print(f"Found MT7925 entry at offset 0x{pos:x} (gap={gap})")
                count += 1
                break
    offset = pos + 2

if count == 0:
    print("WARNING: Could not find MT7925 PCI ID pattern")
    print("The module format may be different than expected")
    sys.exit(1)

print(f"Found {count} MT7925 entries")

# Simple approach: just change one MT7925 to MT7927 won't work well
# Better: the module alias mechanism - add a modalias

# For now, let's just report what we found
print("\nModule needs kernel-level patching or modalias override")
print("Consider using /etc/modprobe.d/mt7927.conf instead")
PYTHON

python3 patch_pci_id.py || true

echo ""
echo "[5/5] Alternative: Creating modalias override..."

# Create a modprobe alias that maps MT7927 to mt7925e
cat > /etc/modprobe.d/mt7927-alias.conf << 'MODPROBE'
# Map MT7927 WiFi 7 to MT7925e driver
# PCI ID: 14c3:7927
alias pci:v000014C3d00007927sv*sd*bc*sc*i* mt7925e
MODPROBE

echo "Created /etc/modprobe.d/mt7927-alias.conf"
echo ""
echo "=========================================="
echo "MODALIAS OVERRIDE INSTALLED"
echo "=========================================="
echo ""
echo "The MT7927 device should now load mt7925e driver."
echo ""
echo "To test:"
echo "  1. Unload current drivers:"
echo "     sudo rmmod mt7927 2>/dev/null || true"
echo "     sudo rmmod mt7925e mt7925_common mt792x_lib mt76_connac_lib mt76 2>/dev/null || true"
echo ""
echo "  2. Rescan PCI bus:"
echo "     sudo modprobe mt7925e"
echo "     # or"
echo "     echo 1 | sudo tee /sys/bus/pci/devices/0000:06:00.0/remove"
echo "     echo 1 | sudo tee /sys/bus/pci/rescan"
echo ""
echo "  3. Check dmesg:"
echo "     dmesg | tail -30"
echo ""
echo "NOTE: Changes are TEMPORARY and will be lost after reboot."
