#!/bin/bash
#
# Build MT7927 Driver v2.0
# Uses MT6639 architecture with ConnInfra initialization
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_DIR="$SCRIPT_DIR/driver"

echo "========================================"
echo "MT7927 Driver v2.0 Builder"
echo "Architecture: MT6639 (Gen4m + ConnInfra)"
echo "========================================"
echo ""

cd "$DRIVER_DIR"

# Check for kernel headers
KVER=$(uname -r)
KDIR="/lib/modules/$KVER/build"

if [ ! -d "$KDIR" ]; then
    echo "ERROR: Kernel headers not found at $KDIR"
    echo ""
    echo "Install kernel headers:"
    echo "  Fedora/Bazzite: sudo dnf install kernel-devel"
    echo "  Ubuntu/Debian:  sudo apt install linux-headers-\$(uname -r)"
    exit 1
fi

echo "Kernel version: $KVER"
echo "Kernel headers: $KDIR"
echo ""

# Use v2 source file
if [ ! -f "mt7927_v2.c" ]; then
    echo "ERROR: mt7927_v2.c not found"
    exit 1
fi

# IMPORTANT: Remove any cached .o files and backup old Makefile
echo "Removing cached build files..."
rm -f *.o *.ko *.mod* .*.cmd Module.symvers modules.order 2>/dev/null || true
rm -rf .tmp_versions 2>/dev/null || true

# Backup existing Makefile
if [ -f "Makefile" ]; then
    mv Makefile Makefile.old
fi

# Copy v2 source to mt7927.c so the module name matches
echo "Copying mt7927_v2.c -> mt7927.c for build..."
cp mt7927_v2.c mt7927.c

# Create proper Makefile for v2
echo "Creating v2 Makefile..."
cat > Makefile << 'EOF'
# MT7927 Driver v2.0 - Gen4m architecture with ConnInfra
obj-m := mt7927.o

ccflags-y += -Wall -Wno-unused-parameter

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

all:
	@echo "Building MT7927 v2.0 (Gen4m + ConnInfra)..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o .*.cmd

.PHONY: all clean
EOF

echo "Makefile created"

# Clean previous build
echo "Cleaning previous build..."
make clean 2>/dev/null || true

# Build
echo ""
echo "Building driver..."
make all

if [ -f "mt7927.ko" ]; then
    echo ""
    echo "========================================"
    echo "BUILD SUCCESSFUL!"
    echo "========================================"
    echo ""
    echo "Driver built: $DRIVER_DIR/mt7927.ko"
    echo ""
    echo "To test:"
    echo "  1. Unbind mt7925e if loaded:"
    echo "     echo 0000:06:00.0 | sudo tee /sys/bus/pci/drivers/mt7925e/unbind"
    echo ""
    echo "  2. Load the v2 driver:"
    echo "     sudo insmod mt7927.ko debug=1"
    echo ""
    echo "  3. Check output:"
    echo "     sudo dmesg | tail -100"
    echo ""
else
    echo ""
    echo "BUILD FAILED - check errors above"
    exit 1
fi
