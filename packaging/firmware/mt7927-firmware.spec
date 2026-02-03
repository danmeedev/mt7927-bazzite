# SPDX-License-Identifier: GPL-2.0
#
# mt7927-firmware.spec - Firmware package for MT7927 WiFi driver
#
# The MT7927 uses MT7925 firmware files from linux-firmware
#

Name:           mt7927-firmware
Version:        1.0.0
Release:        1%{?dist}
Summary:        Firmware files for MediaTek MT7927 WiFi 7 (AMD RZ738)
License:        Redistributable, no modification permitted
URL:            https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
BuildArch:      noarch

# Note: The firmware files are downloaded from linux-firmware
# They are MT7925 firmware which is compatible with MT7927

Provides:       mt7927-firmware = %{version}-%{release}
Provides:       firmware(mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin)
Provides:       firmware(mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin)

# Avoid conflict with linux-firmware if it already has these files
# On newer systems, linux-firmware may already include mt7925 firmware
Supplements:    modalias(pci:v000014C3d00007927sv*sd*bc*sc*i*)

%description
This package contains the firmware files required for the MediaTek MT7927
WiFi 7 wireless adapter (also known as AMD RZ738 on AMD platforms).

The MT7927 uses MT7925 firmware files, which are compatible due to the
chips being architecturally identical (MT7927 adds 320MHz channel support).

Firmware files included:
- WIFI_MT7925_PATCH_MCU_1_1_hdr.bin (patch firmware)
- WIFI_RAM_CODE_MT7925_1_1.bin (RAM firmware)

%install
mkdir -p %{buildroot}/usr/lib/firmware/mediatek/mt7925

# Install firmware files
# These should be downloaded from linux-firmware during build
install -m 644 firmware/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin \
    %{buildroot}/usr/lib/firmware/mediatek/mt7925/
install -m 644 firmware/WIFI_RAM_CODE_MT7925_1_1.bin \
    %{buildroot}/usr/lib/firmware/mediatek/mt7925/

%files
%license LICENSE.firmware
/usr/lib/firmware/mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin
/usr/lib/firmware/mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin

%changelog
* Sun Feb 02 2026 MT7927 Project <mt7927@example.com> - 1.0.0-1
- Initial firmware package
- Uses MT7925 firmware which is compatible with MT7927
