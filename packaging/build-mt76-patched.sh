#!/bin/bash
#
# Build patched mt7925e with MT7927 PCI ID support
# Uses Linux kernel source tree (more compatible than OpenWrt)
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
    echo "Install: sudo dnf install kernel-devel kernel-headers"
    exit 1
fi

# Create work directory
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

echo "[1/6] Cloning Linux kernel (sparse checkout for mt76 only)..."
git clone --depth=1 --filter=blob:none --sparse https://github.com/torvalds/linux.git
cd linux
git sparse-checkout set drivers/net/wireless/mediatek/mt76

cd drivers/net/wireless/mediatek/mt76

echo ""
echo "[2/6] Checking current MT7925 PCI IDs..."
echo "Current pci.c content:"
grep -n "PCI_DEVICE" mt7925/pci.c | head -10

echo ""
echo "[3/6] Adding MT7927 PCI ID..."

PCI_FILE="mt7925/pci.c"
cp "$PCI_FILE" "${PCI_FILE}.orig"

# Add MT7927 entry before MT7925 entry
# Look for the exact pattern and add MT7927 before it
if grep -q "0x7927" "$PCI_FILE"; then
    echo "MT7927 already present in source!"
else
    # Insert MT7927 entry - using a more compatible sed approach
    sed -i '/PCI_DEVICE.*0x7925/i\        { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7927) },' "$PCI_FILE"
fi

echo ""
echo "Patch applied. Diff:"
diff -u "${PCI_FILE}.orig" "$PCI_FILE" || true

echo ""
echo "[4/6] Creating Makefile for out-of-tree build..."

# Create Makefile that matches kernel's Kconfig expectations
cat > Makefile << 'MAKEFILE'
# SPDX-License-Identifier: GPL-2.0
#
# Makefile for mt76 driver build with MT7927 support
#

KERNEL_VERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNEL_VERSION)/build

# Enable required configs
ccflags-y += -DCONFIG_MT76_LEDS
ccflags-y += -DCONFIG_MAC80211_DEBUGFS

obj-m += mt76.o
obj-m += mt76-connac-lib.o
obj-m += mt792x-lib.o
obj-m += mt7925/

mt76-y := \
	mac80211.o util.o trace.o dma.o eeprom.o tx.o agg-rx.o mcu.o \
	debugfs.o mmio.o

mt76-$(CONFIG_PCI) += pci.o

mt76-connac-lib-y := mt76_connac_mcu.o mt76_connac_mac.o mt76_connac3_mac.o

mt792x-lib-y := mt792x_core.o mt792x_mac.o mt792x_trace.o \
	mt792x_acpi_sar.o mt792x_dma.o mt792x_debugfs.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
MAKEFILE

cat > mt7925/Makefile << 'MAKEFILE'
# SPDX-License-Identifier: GPL-2.0

obj-m += mt7925-common.o
obj-m += mt7925e.o

mt7925-common-y := mac.o mcu.o main.o init.o debugfs.o
mt7925e-y := pci.o pci_mac.o

ccflags-y += -I$(src)/..
MAKEFILE

echo ""
echo "[5/6] Building mt76 modules..."
echo "This may take a few minutes..."
echo ""

# Build with verbose output
make KERNEL_VERSION=$KERNEL_VER 2>&1 | tee build.log

BUILD_STATUS=$?

echo ""
echo "[6/6] Checking built modules..."
find . -name "*.ko" -type f 2>/dev/null | while read ko; do
    ls -la "$ko"
done

echo ""
echo "=========================================="
echo "BUILD STATUS"
echo "=========================================="
echo ""

if [ $BUILD_STATUS -eq 0 ] && find . -name "mt7925e.ko" 2>/dev/null | grep -q .; then
    echo "SUCCESS! Modules built."
    echo ""
    echo "To install (run these commands):"
    echo ""
    echo "  cd $WORK_DIR/linux/drivers/net/wireless/mediatek/mt76"
    echo ""
    echo "  # Unload existing modules"
    echo "  sudo rmmod mt7925e mt7925_common mt792x_lib mt76_connac_lib mt76 2>/dev/null || true"
    echo ""
    echo "  # Load new modules in order"
    echo "  sudo insmod ./mt76.ko"
    echo "  sudo insmod ./mt76-connac-lib.ko"
    echo "  sudo insmod ./mt792x-lib.ko"
    echo "  sudo insmod ./mt7925/mt7925-common.ko"
    echo "  sudo insmod ./mt7925/mt7925e.ko"
    echo ""
    echo "  # Check if MT7927 is recognized"
    echo "  dmesg | tail -20"
    echo "  ip link show"
else
    echo "Build failed. Check build.log for details."
    echo ""
    echo "Common issues:"
    echo "  - Missing kernel headers: sudo dnf install kernel-devel"
    echo "  - Missing dependencies: The mt76 driver needs mac80211 symbols"
    echo ""
    echo "Last 50 lines of build log:"
    tail -50 build.log
fi

echo ""
echo "Work directory preserved at: $WORK_DIR/linux/drivers/net/wireless/mediatek/mt76"
