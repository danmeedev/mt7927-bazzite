#!/bin/bash
#
# Download MT7925 firmware files for MT7927
#
# The MT7927 uses MT7925 firmware from linux-firmware repository
#

set -e

FIRMWARE_DIR="$(dirname "$0")/firmware"
BASE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7925"

mkdir -p "$FIRMWARE_DIR"

echo "Downloading MT7925 firmware for MT7927..."

# Download patch firmware
echo "  -> WIFI_MT7925_PATCH_MCU_1_1_hdr.bin"
curl -sL -o "$FIRMWARE_DIR/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin" \
    "$BASE_URL/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin"

# Download RAM firmware
echo "  -> WIFI_RAM_CODE_MT7925_1_1.bin"
curl -sL -o "$FIRMWARE_DIR/WIFI_RAM_CODE_MT7925_1_1.bin" \
    "$BASE_URL/WIFI_RAM_CODE_MT7925_1_1.bin"

echo ""
echo "Firmware downloaded to: $FIRMWARE_DIR"
echo ""
echo "Files:"
ls -la "$FIRMWARE_DIR"/*.bin

echo ""
echo "To install on Bazzite (testing only):"
echo "  sudo rpm-ostree usroverlay"
echo "  sudo mkdir -p /usr/lib/firmware/mediatek/mt7925"
echo "  sudo cp $FIRMWARE_DIR/*.bin /usr/lib/firmware/mediatek/mt7925/"
