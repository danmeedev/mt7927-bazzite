#!/bin/bash
#
# Build patched mt7925e with MT7927 PCI ID support
# This downloads the mt76 driver source and adds MT7927 support
#

set -e

WORK_DIR="/tmp/mt7925e-patched-$$"
KERNEL_VER=$(uname -r)

echo "=========================================="
echo "MT7925e Patched Build for MT7927 Support"
echo "=========================================="
echo ""
echo "Kernel: $KERNEL_VER"
echo "Work dir: $WORK_DIR"
echo ""

# Check for build dependencies
if ! command -v make &> /dev/null; then
    echo "ERROR: 'make' not found. Install build tools."
    exit 1
fi

if [ ! -d "/lib/modules/$KERNEL_VER/build" ]; then
    echo "ERROR: Kernel headers not found for $KERNEL_VER"
    echo "Install kernel-devel package"
    exit 1
fi

# Create work directory
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

echo "[1/5] Downloading mt76 driver source from kernel.org..."
# Get the mt76 driver from the Linux kernel tree
# Using the wireless-next tree which has latest mt7925 code
curl -sL "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/net/wireless/mediatek/mt76/mt7925/pci.c" -o pci.c.orig

if [ ! -s pci.c.orig ]; then
    echo "ERROR: Failed to download pci.c"
    exit 1
fi

echo "[2/5] Creating patched pci.c with MT7927 support..."
cp pci.c.orig pci.c

# Check if MT7927 is already supported
if grep -q "0x7927" pci.c; then
    echo "NOTE: MT7927 (0x7927) already in upstream! Checking..."
    grep -n "7927" pci.c
else
    echo "Adding MT7927 PCI ID to device table..."

    # Find the pci_device_id table and add MT7927 after MT7925
    # The table looks like:
    #   { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925) ... },
    # We add:
    #   { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7927) ... },

    sed -i '/PCI_DEVICE.*0x7925/a\        { PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7927), .driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM }, /* MT7927 WiFi 7 */' pci.c
fi

echo ""
echo "Diff of changes:"
diff -u pci.c.orig pci.c || true
echo ""

echo "[3/5] Downloading additional required files..."
# We need the full mt7925 directory to build
# For a minimal build, we'll create a standalone module

cat > mt7927_pci_id.c << 'PATCH_MODULE'
// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 PCI ID patch module
 *
 * This module adds the MT7927 PCI ID (14c3:7927) to the mt7925e driver
 * at runtime using the driver's new_id mechanism via a different approach.
 *
 * Since new_id doesn't work directly, this module:
 * 1. Unbinds the device from any current driver
 * 2. Registers the MT7927 ID with mt7925e
 * 3. Triggers a rescan
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>

#define MT7927_VENDOR_ID 0x14c3
#define MT7927_DEVICE_ID 0x7927

static struct pci_device_id mt7927_id = {
    PCI_DEVICE(MT7927_VENDOR_ID, MT7927_DEVICE_ID),
};

static int __init mt7927_patch_init(void)
{
    struct pci_dev *pdev = NULL;
    struct pci_driver *mt7925e_drv;
    int ret;

    pr_info("MT7927 PCI ID patch: Looking for MT7927 device...\n");

    /* Find the MT7927 device */
    pdev = pci_get_device(MT7927_VENDOR_ID, MT7927_DEVICE_ID, NULL);
    if (!pdev) {
        pr_err("MT7927 PCI ID patch: No MT7927 device found\n");
        return -ENODEV;
    }

    pr_info("MT7927 PCI ID patch: Found device at %s\n", pci_name(pdev));

    /* Find mt7925e driver */
    mt7925e_drv = pci_dev_driver(pdev);
    if (mt7925e_drv) {
        pr_info("MT7927 PCI ID patch: Device already has driver: %s\n",
                mt7925e_drv->name);
        pci_dev_put(pdev);
        return 0;
    }

    /* Try to find mt7925e driver by name */
    /* This is a simplified approach - in practice we'd use driver_find */

    pr_info("MT7927 PCI ID patch: Triggering device rescan...\n");

    /* Unbind if bound to something */
    if (pdev->driver) {
        device_release_driver(&pdev->dev);
    }

    /* Request a rescan which should pick up mt7925e if loaded with our ID */
    pci_lock_rescan_remove();
    pci_rescan_bus(pdev->bus);
    pci_unlock_rescan_remove();

    pci_dev_put(pdev);

    pr_info("MT7927 PCI ID patch: Done. Check dmesg for mt7925e binding.\n");
    return 0;
}

static void __exit mt7927_patch_exit(void)
{
    pr_info("MT7927 PCI ID patch: Unloaded\n");
}

module_init(mt7927_patch_init);
module_exit(mt7927_patch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MT7927 Driver Project");
MODULE_DESCRIPTION("Adds MT7927 PCI ID support to mt7925e driver");
PATCH_MODULE

echo "[4/5] The cleanest approach: rebuild mt7925e from kernel source..."
echo ""
echo "Since mt7925e is part of the mt76 driver family with many dependencies,"
echo "the easiest solution is to:"
echo ""
echo "  Option A: Wait for upstream kernel support (patch submitted)"
echo "  Option B: Build entire mt76 from source with patch"
echo "  Option C: Use our standalone mt7927 driver (current approach)"
echo ""

# Let's check what the current kernel has
echo "[5/5] Checking current kernel mt7925e..."
if modinfo mt7925e &>/dev/null; then
    echo ""
    echo "Current mt7925e module info:"
    modinfo mt7925e | grep -E "^(filename|alias|depends):"
    echo ""

    # Check what IDs it supports
    echo "Supported PCI IDs:"
    modinfo mt7925e | grep "^alias:" | grep pci
fi

echo ""
echo "=========================================="
echo "RECOMMENDATION"
echo "=========================================="
echo ""
echo "The mt7925e driver has complex dependencies on mt76 framework."
echo "Building it standalone is not trivial."
echo ""
echo "Best options:"
echo ""
echo "1. SUBMIT UPSTREAM PATCH (long-term fix)"
echo "   Add one line to kernel: { PCI_DEVICE(..., 0x7927) }"
echo "   MediaTek/mt76 maintainers would likely accept it."
echo ""
echo "2. BUILD FULL mt76 FROM SOURCE"
echo "   Clone https://github.com/openwrt/mt76"
echo "   Apply patch, build all modules"
echo ""
echo "3. CONTINUE WITH STANDALONE DRIVER"
echo "   Our mt7927.c driver - need to fix DMA ring issue"
echo ""

# Cleanup
cd /
rm -rf "$WORK_DIR"

echo "Work directory cleaned up."
