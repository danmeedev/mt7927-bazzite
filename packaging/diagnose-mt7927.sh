#!/bin/bash
#
# MT7927 Diagnostic Script
# Gathers information about why mt7925e driver fails to initialize MT7927
#

echo "========================================"
echo "MT7927 Diagnostic Report"
echo "========================================"
echo ""
echo "Date: $(date)"
echo "Kernel: $(uname -r)"
echo ""

# PCI device info
echo "=== PCI Device Information ==="
lspci -vvv -s 06:00.0 2>/dev/null || lspci -vv -s 06:00.0
echo ""

# Check driver binding
echo "=== Driver Binding ==="
lspci -k -s 06:00.0
echo ""

# Check firmware files
echo "=== MT7925 Firmware Files ==="
for dir in /usr/lib/firmware/mediatek /lib/firmware/mediatek; do
    if [ -d "$dir" ]; then
        echo "Directory: $dir"
        find "$dir" -name "*mt7925*" -o -name "*mt792x*" 2>/dev/null | head -20
    fi
done
echo ""

# List all MediaTek firmware
echo "=== All MediaTek Firmware ==="
ls -la /usr/lib/firmware/mediatek/ 2>/dev/null || ls -la /lib/firmware/mediatek/ 2>/dev/null
echo ""

# Check if firmware loading was attempted
echo "=== Firmware Loading Log ==="
sudo dmesg | grep -iE "(firmware|mt7925|mt792|failed.*load|direct.*firmware)" | tail -30
echo ""

# Full mt7925e log
echo "=== Full mt7925e/mt7927 Log ==="
sudo dmesg | grep -iE "mt7925|mt7927|mt792x|14c3" | head -50
echo ""

# Check driver override
echo "=== Driver Override Status ==="
cat /sys/bus/pci/devices/0000:06:00.0/driver_override 2>/dev/null
echo ""

# Check module info
echo "=== Module Information ==="
modinfo mt7925e 2>/dev/null | head -30
echo ""

# Check if there's a mt7927 module
echo "=== MT7927 Module Check ==="
modinfo mt7927 2>/dev/null
echo ""

# Check loaded modules
echo "=== Loaded MT76 Modules ==="
lsmod | grep -E "mt76|mt792"
echo ""

# Network interfaces
echo "=== Network Interfaces ==="
ip link show
echo ""

# PCI config space (might reveal power state issues)
echo "=== PCI Config (partial) ==="
sudo lspci -xxx -s 06:00.0 2>/dev/null | head -20
echo ""

# Check ASPM state
echo "=== ASPM Status ==="
if [ -f /sys/bus/pci/devices/0000:06:00.0/link/l0s_aspm ]; then
    echo "L0s: $(cat /sys/bus/pci/devices/0000:06:00.0/link/l0s_aspm 2>/dev/null)"
    echo "L1: $(cat /sys/bus/pci/devices/0000:06:00.0/link/l1_aspm 2>/dev/null)"
else
    echo "ASPM sysfs not available"
fi
echo ""

# Check power state
echo "=== Power State ==="
cat /sys/bus/pci/devices/0000:06:00.0/power_state 2>/dev/null || echo "power_state not available"
echo ""

echo "========================================"
echo "End of Diagnostic Report"
echo "========================================"
echo ""
echo "Key things to look for:"
echo "1. Is firmware being loaded? Look for 'firmware' messages"
echo "2. What's the power state? Should be D0 for active"
echo "3. Are the mt7925 firmware files present?"
echo ""
echo "The 'ASIC revision: 0000' suggests the chip ID read is failing."
echo "This could mean MT7927 needs different register addresses than MT7925."
