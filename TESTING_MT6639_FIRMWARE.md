# MT7927 Testing Guide - MT6639 Firmware Discovery

## Key Discovery

**MT7927 uses MT6639 firmware internally, NOT MT7925 firmware!**

The Windows driver's `mtkwlan.dat` contains both MT6639 and MT7925 firmware.
MT7927 is mapped to use MT6639 firmware files.

## Quick Test Steps

### 1. Pull the latest code

```bash
cd ~/mt7927  # or wherever your repo is
git pull
```

### 2. Install the MT6639 firmware

The firmware files are in `firmware_for_linux/`:

```bash
cd ~/mt7927

# Create firmware directory (use overlay for Bazzite)
sudo rpm-ostree usroverlay 2>/dev/null || true
sudo mkdir -p /usr/lib/firmware/mediatek/mt7925

# Copy the MT6639 firmware files
sudo cp firmware_for_linux/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin /usr/lib/firmware/mediatek/mt7925/
sudo cp firmware_for_linux/WIFI_RAM_CODE_MT6639_2_1.bin /usr/lib/firmware/mediatek/mt7925/

# Verify files are in place
ls -la /usr/lib/firmware/mediatek/mt7925/
```

### 3. Bind MT7927 to mt7925e driver

```bash
# Make sure mt7925e claims the MT7927 device
echo "mt7925e" | sudo tee /sys/bus/pci/devices/0000:06:00.0/driver_override

# Unbind any current driver
echo "0000:06:00.0" | sudo tee /sys/bus/pci/drivers/mt7925e/unbind 2>/dev/null || true

# Trigger driver probe
echo "0000:06:00.0" | sudo tee /sys/bus/pci/drivers_probe
```

### 4. Check if it works

```bash
# Check kernel messages
sudo dmesg | tail -40

# Look for WiFi interface
ip link show | grep -i wl

# Check driver binding
lspci -k -s 06:00.0
```

## What to Look For

### Success indicators:
- Driver loads without "Message timeout" errors
- Firmware loading messages (not "failed to load")
- WiFi interface appears (wlan0 or similar)

### Failure indicators:
- "Failed to get patch semaphore" - firmware not found/incompatible
- "Message 00000010 timeout" - MCU not responding
- "Failed to load firmware" - wrong firmware path/name

## If Firmware Names Don't Match

The mt7925e driver may look for MT7925-named firmware, not MT6639.
If so, try creating symlinks:

```bash
cd /usr/lib/firmware/mediatek/mt7925

# Create aliases (if driver expects MT7925 names)
sudo ln -sf WIFI_MT6639_PATCH_MCU_2_1_hdr.bin WIFI_MT7925_PATCH_MCU_1_1_hdr.bin
sudo ln -sf WIFI_RAM_CODE_MT6639_2_1.bin WIFI_RAM_CODE_MT7925_1.bin
```

Then reload the driver:

```bash
sudo rmmod mt7925e mt7925_common mt792x_lib mt76_connac_lib mt76 2>/dev/null
sudo modprobe mt7925e
```

## Alternative: Full Cold Boot Test

Sometimes the device needs a full power cycle:

1. Install firmware (steps above)
2. **Completely shut down** (not restart)
3. Wait 10 seconds
4. Power on
5. Check dmesg for results

## Files Included

| File | Size | Description |
|------|------|-------------|
| `firmware_for_linux/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin` | 299 KB | Patch firmware for MT7927 |
| `firmware_for_linux/WIFI_RAM_CODE_MT6639_2_1.bin` | 1.5 MB | Main WiFi firmware for MT7927 |
| `mess/mtkwlan.dat` | 20 MB | Windows driver data file (source of firmware) |
| `mess/mtkwecx.inf` | 270 KB | Windows driver config (reference) |

## Background

Analysis of the Windows driver INF file showed:
- MT7927 is grouped with MT6639 for device configuration
- The `mtkwlan.dat` file contains an index of embedded firmware
- Firmware entries show MT6639 files are used for MT7927

The Linux mt7925e driver was trying to load MT7925 firmware for MT7927,
but the chip actually requires MT6639 firmware to function.
