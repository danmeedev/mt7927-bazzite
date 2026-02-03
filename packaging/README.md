# MT7927 Driver Package for Bazzite/AMD Systems

This package provides a WiFi driver for the **MediaTek MT7927** (also known as **AMD RZ738**) WiFi 7 chip on Bazzite and other Fedora-based systems.

## Target Platform

- **Primary**: Bazzite (Fedora Atomic, rpm-ostree)
- **Hardware**: AMD systems with MT7927/RZ738 WiFi
- **PCI ID**: `14c3:7927`

## Package Contents

```
packaging/
├── driver/
│   ├── mt7927.c       # Main driver source
│   ├── Makefile       # Build system
│   ├── Kbuild         # Kernel build config
│   └── dkms.conf      # For non-Bazzite systems
├── akmod/
│   └── mt7927-kmod.spec   # Akmod package spec
├── firmware/
│   ├── mt7927-firmware.spec   # Firmware package spec
│   └── download-firmware.sh   # Download helper
└── README.md          # This file
```

## Quick Start (Testing on Bazzite)

### 1. Download Firmware

```bash
cd packaging/firmware
chmod +x download-firmware.sh
./download-firmware.sh
```

### 2. Install Firmware (Temporary)

```bash
# Create temporary writable overlay (does NOT persist across reboots)
sudo rpm-ostree usroverlay

# Copy firmware files
sudo mkdir -p /usr/lib/firmware/mediatek/mt7925
sudo cp firmware/*.bin /usr/lib/firmware/mediatek/mt7925/
```

### 3. Build Driver

```bash
cd packaging/driver

# Install build dependencies
sudo dnf install kernel-devel kernel-headers

# Build
make
```

### 4. Load Driver

```bash
# Remove any conflicting drivers
sudo rmmod mt7921e mt7925e 2>/dev/null || true

# Load our driver
sudo insmod mt7927.ko

# Check logs
sudo dmesg | tail -30
```

## What This Driver Does

The driver implements the **correct initialization sequence** for MT7927:

1. **PCI Setup** - Enable device, set DMA mask
2. **Power Management Handoff** - Transfer ownership FW→Driver
3. **EMI Sleep Protection** - Enable sleep protection
4. **WFSYS Reset** - The critical step that unlocks registers
5. **DMA Initialization** - Setup DMA rings for firmware transfer
6. **Firmware Loading** - Load MT7925-compatible firmware

### Current Status

- ✅ PCI binding works
- ✅ Power management handoff implemented
- ✅ WFSYS reset implemented
- ✅ DMA ring initialization
- ⚠️ MCU command protocol (partial - needs full implementation)
- ❌ Network interface (requires complete firmware activation)

## Building for Production

### Build Akmod Package

```bash
# Install build tools
sudo dnf install rpm-build rpmdevtools akmods

# Setup RPM build environment
rpmdev-setuptree

# Create source tarball
tar czf ~/rpmbuild/SOURCES/mt7927-0.1.0.tar.gz \
    -C packaging --transform 's,^,mt7927-0.1.0/,' driver/

# Build akmod
rpmbuild -ba packaging/akmod/mt7927-kmod.spec
```

### Build Firmware Package

```bash
# Download firmware first
cd packaging/firmware
./download-firmware.sh

# Create source structure
mkdir -p mt7927-firmware-1.0.0/firmware
cp firmware/*.bin mt7927-firmware-1.0.0/firmware/
echo "Redistributable" > mt7927-firmware-1.0.0/LICENSE.firmware

# Build RPM
rpmbuild -ba mt7927-firmware.spec
```

## Troubleshooting

### Check Device Presence

```bash
lspci -nn | grep 14c3:7927
```

### Check Driver Binding

```bash
lspci -k | grep -A3 "14c3:7927"
```

### View Driver Messages

```bash
sudo dmesg | grep -E "mt7927|14c3:7927"
```

### Reset Chip (if in error state)

```bash
# Find your device (replace XX:00.0 with actual address)
echo 1 | sudo tee /sys/bus/pci/devices/0000:XX:00.0/remove
sleep 2
echo 1 | sudo tee /sys/bus/pci/rescan
```

## Technical Details

### Key Registers

| Register | Address | Purpose |
|----------|---------|---------|
| MT_CONN_ON_LPCTL | 0x7c060010 | Power ownership |
| MT_WFSYS_SW_RST_B | 0x7c000140 | WFSYS reset |
| MT_WFDMA0_GLO_CFG | 0xd4208 | DMA configuration |

### Why MT7925 Firmware Works

The MT7927 is architecturally identical to MT7925. The only difference is:
- MT7925: Up to 160MHz channels (WiFi 6E)
- MT7927: Up to 320MHz channels (WiFi 7)

The firmware is compatible because the core hardware is the same.

## License

- Driver: GPL-2.0
- Firmware: Redistributable (MediaTek license)

## Contributing

This is an experimental driver. Contributions welcome:

1. Test on your AMD system with MT7927
2. Report issues with `dmesg` output
3. Help implement the MCU command protocol
