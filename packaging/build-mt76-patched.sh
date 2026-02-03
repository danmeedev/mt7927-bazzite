#!/bin/bash
#
# Build patched mt76/mt7925e with MT7927 PCI ID support
# Uses OpenWrt's mt76 repository
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
grep -n "PCI_DEVICE" mt7925/pci.c | head -10

echo ""
echo "[3/6] Adding MT7927 PCI ID..."

PCI_FILE="mt7925/pci.c"
cp "$PCI_FILE" "${PCI_FILE}.orig"

# The table looks like:
# static const struct pci_device_id mt7925_pci_device_table[] = {
#     { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925),
#         .driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM },
#     { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x0717),
#         .driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM },
#     { },
# };
#
# We need to add MT7927 as a complete entry

sed -i 's/{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925),/{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7927),\n\t\t.driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM },\n\t{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925),/' "$PCI_FILE"

echo ""
echo "Patch applied. Diff:"
diff -u "${PCI_FILE}.orig" "$PCI_FILE" || true

echo ""
echo "[4/6] Building mt76 modules..."

# OpenWrt mt76 needs KBUILD_MODNAME and proper kernel build setup
# Create a simple Kbuild wrapper

cat > Kbuild << 'EOF'
KBUILD_CFLAGS += -DCONFIG_MT76_LEDS=y
KBUILD_CFLAGS += -DCONFIG_MAC80211=m
KBUILD_CFLAGS += -DCONFIG_NL80211_TESTMODE=y

obj-m += mt76.o
obj-m += mt76-connac-lib.o
obj-m += mt792x-lib.o
obj-m += mt7925/

mt76-y := \
    mac80211.o util.o trace.o dma.o eeprom.o tx.o agg-rx.o mcu.o \
    debugfs.o mmio.o

mt76-connac-lib-y := mt76_connac_mcu.o mt76_connac_mac.o mt76_connac3_mac.o

mt792x-lib-y := mt792x_core.o mt792x_mac.o mt792x_trace.o \
                mt792x_acpi_sar.o mt792x_dma.o
EOF

cat > mt7925/Kbuild << 'EOF'
obj-m += mt7925-common.o
obj-m += mt7925e.o

mt7925-common-y := mac.o mcu.o main.o init.o
mt7925e-y := pci.o pci_mac.o
EOF

# Try the build with proper kernel build system
echo "Building with kernel build system..."
make -C /lib/modules/$KERNEL_VER/build M=$(pwd) modules 2>&1 | tee build.log || {
    echo ""
    echo "Direct build failed. This is expected - mt76 has complex dependencies."
    echo ""
    echo "Alternative: Try building with backports or use the kernel source tree."
    echo ""

    # Show what happened
    tail -30 build.log
}

echo ""
echo "[5/6] Checking built modules..."
find . -name "*.ko" -ls 2>/dev/null || echo "No modules built"

echo ""
echo "=========================================="
echo "BUILD STATUS"
echo "=========================================="
echo ""

if find . -name "mt7925e.ko" 2>/dev/null | grep -q .; then
    echo "SUCCESS! Modules built."
    echo ""
    echo "To install:"
    echo "  cd $WORK_DIR/mt76"
    echo "  sudo rmmod mt7925e mt7925_common mt792x_lib mt76_connac_lib mt76 2>/dev/null"
    echo "  sudo insmod ./mt76.ko"
    echo "  sudo insmod ./mt76-connac-lib.ko"
    echo "  sudo insmod ./mt792x-lib.ko"
    echo "  sudo insmod ./mt7925/mt7925-common.ko"
    echo "  sudo insmod ./mt7925/mt7925e.ko"
else
    echo "Build did not complete. The mt76 driver has many dependencies."
    echo ""
    echo "ALTERNATIVE APPROACH:"
    echo "Build from kernel source tree instead:"
    echo ""
    echo "  # Get kernel source"
    echo "  sudo dnf install kernel-devel kernel-headers"
    echo "  git clone --depth=1 https://github.com/torvalds/linux.git"
    echo "  cd linux/drivers/net/wireless/mediatek/mt76"
    echo ""
    echo "  # Apply our patch"
    echo "  sed -i 's/0x7925/0x7927\\n\\t{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925),/' mt7925/pci.c"
    echo ""
    echo "  # Build"
    echo "  make -C /lib/modules/\$(uname -r)/build M=\$(pwd) modules"
fi

echo ""
echo "Work directory preserved at: $WORK_DIR/mt76"
