#!/bin/bash
#
# Extract MT7927 (MT6639) firmware from Windows mtkwlan.dat
#
# DISCOVERY: MT7927 internally uses MT6639 firmware, NOT MT7925!
# The Windows driver's mtkwlan.dat contains embedded firmware.
#
# Files needed for MT7927:
#   - WIFI_MT6639_PATCH_MCU_2_1_hdr.bin  (patch firmware)
#   - WIFI_RAM_CODE_MT6639_2_1.bin       (main WiFi firmware)
#

set -e

WINDOWS_DAT="${1:-./mtkwlan.dat}"

if [ ! -f "$WINDOWS_DAT" ]; then
    echo "ERROR: Cannot find mtkwlan.dat"
    echo "Usage: $0 /path/to/mtkwlan.dat"
    exit 1
fi

OUTPUT_DIR="./mt7927_firmware"
mkdir -p "$OUTPUT_DIR"

echo "========================================"
echo "MT7927 Firmware Extractor"
echo "========================================"
echo ""

python3 << PYTHON_SCRIPT
import struct
import os

dat_file = "$WINDOWS_DAT"
output_dir = "$OUTPUT_DIR"

with open(dat_file, 'rb') as f:
    data = f.read()

print(f"Source: {dat_file} ({len(data):,} bytes)")
print("")

if data[:4] != b'MTK-':
    print("ERROR: Invalid mtkwlan.dat - missing MTK- magic")
    exit(1)

ENTRY_SIZE = 0x4c  # 76 bytes
TABLE_START = 0x10
entry_count = struct.unpack('<H', data[4:6])[0]

print(f"Found {entry_count} firmware entries")
print("")

entries = []
for i in range(entry_count):
    offset = TABLE_START + (i * ENTRY_SIZE)
    chunk = data[offset:offset + ENTRY_SIZE]

    name_end = chunk.find(b'\\x00', 0, 48)
    if name_end == -1:
        name_end = 48
    name = chunk[:name_end].decode('ascii', errors='ignore')

    fw_off = struct.unpack('<I', chunk[64:68])[0]
    fw_size = struct.unpack('<I', chunk[68:72])[0]

    if name and fw_size > 0 and fw_off < len(data) and fw_off + fw_size <= len(data):
        entries.append({'name': name, 'offset': fw_off, 'size': fw_size})
        print(f"  {name} ({fw_size:,} bytes)")

print("")
print("Extracting files...")

for entry in entries:
    fw_data = data[entry['offset']:entry['offset'] + entry['size']]
    out_path = os.path.join(output_dir, entry['name'])
    with open(out_path, 'wb') as f:
        f.write(fw_data)

print(f"Extracted to: {output_dir}/")
PYTHON_SCRIPT

echo ""
echo "========================================"
echo "INSTALLATION INSTRUCTIONS"
echo "========================================"
echo ""
echo "Copy these files to your Bazzite Linux system:"
echo "  $OUTPUT_DIR/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin"
echo "  $OUTPUT_DIR/WIFI_RAM_CODE_MT6639_2_1.bin"
echo ""
echo "Then on Linux run:"
echo "  sudo mkdir -p /usr/lib/firmware/mediatek/mt7925"
echo "  sudo cp WIFI_MT6639_*.bin WIFI_RAM_CODE_MT6639_*.bin /usr/lib/firmware/mediatek/mt7925/"
echo ""
echo "Reload driver:"
echo "  sudo rmmod mt7925e 2>/dev/null; sudo modprobe mt7925e"
echo "  dmesg | tail -30"
