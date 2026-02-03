# MT7927 Driver Project - Initial Discovery Summary
**Date**: 2026-02-02
**Target Distribution**: Bazzite (primary), other Linux flavors (secondary)

---

## Executive Summary

The MT7927 is a WiFi 7 chip (MediaTek Filogic 380) that is **architecturally identical to the MT7925** except for 320MHz channel support. The existing driver project has successfully bound to the device and loaded firmware into kernel memory, but is **blocked on DMA firmware transfer** - the critical step that activates the chip.

**Key Insight**: We don't need to write a driver from scratch - we need to adapt the working mt7925 driver to support MT7927's PCI ID and eventually its 320MHz capability.

---

## 1. Codebase State

### What Exists
| Component | Status | Location |
|-----------|--------|----------|
| PCI binding driver | Working | `tests/04_risky_ops/mt7927_wrapper.c` |
| Firmware loading driver | Partial | `tests/04_risky_ops/mt7927_init.c` |
| DMA-based driver | Best attempt | `tests/04_risky_ops/mt7927_init_dma.c` |
| Test suite | 29 modules | `tests/01-05_*/` |
| Build system | Complete | `Makefile`, `tests/Kbuild` |

### What Works
- PCI enumeration (14c3:7927)
- BAR0 (2MB) and BAR2 (32KB) mapping
- Firmware file loading into kernel memory
- DMA ring allocation (256 descriptors)
- Safe register read/write operations

### What's Blocked
- **DMA firmware transfer** - Main blocker
- Critical registers appear write-protected (WPDMA_GLO_CFG stays 0x00000000)
- FW_STATUS stuck at 0xffff10f1 ("waiting for firmware")
- Memory at BAR0[0x000000] never activates

---

## 2. DMA Implementation Requirements

### From MT7925 Driver Analysis

The firmware loading sequence requires:

1. **Initialize DMA Queues**
   - TX Band 0: 2048 descriptors
   - MCU WM (command): 256 descriptors
   - FWDL (firmware download): 128 descriptors
   - RX queues: 512-1536 entries

2. **Firmware Transfer Protocol**
   ```
   1. Acquire patch semaphore (MCU_CMD PATCH_SEM_CONTROL)
   2. Initialize download (address: 0x900000, length, mode)
   3. Send firmware in chunks via MCU_CMD(FW_SCATTER)
   4. Finalize patch (MCU_CMD PATCH_FINISH_REQ)
   5. Release semaphore
   6. Start firmware (MCU_CMD FW_START_REQ)
   ```

3. **DMA Descriptor Format**
   ```c
   struct mt76_desc {
       __le32 buf0;    // Buffer physical address (low)
       __le32 ctrl;    // Control (length, flags, DMA_DONE)
       __le32 buf1;    // Buffer physical address (high)
       __le32 info;    // Info flags
   };
   ```

### Key Registers
- `MT_WFDMA0_GLO_CFG` - Enable TX/RX DMA
- `MT_WFDMA0_TX_RING0_EXT_CTRL` - TX ring extension
- `MT_WPDMA_RST_IDX` - WPDMA index reset

---

## 3. Bazzite Packaging Strategy

### Architecture
Bazzite uses **rpm-ostree** (immutable filesystem). **DKMS is NOT supported**.

### Solution: Akmods
- Use the **ublue-os/akmods** infrastructure
- Pre-build modules into container images via BlueBuild
- Modules are signed with `akmods-ublue.der` certificate

### Package Structure Needed
```
mt7927-kmod.spec      # Kernel module (akmod format)
mt7927-firmware.spec  # Firmware package (noarch RPM)
```

### Secure Boot
- Universal Blue signing key available
- MOK enrollment password: `universalblue`
- Or generate custom signing key with `kmodgenca -a`

### Distribution Path
1. Create COPR repository for testing
2. Submit to ublue-os/akmods for broader distribution
3. Include in BlueBuild recipes for custom images

### Firmware Installation
```bash
# Firmware location
/usr/lib/firmware/mediatek/mt7925/

# Files needed (MT7925 firmware works!)
WIFI_MT7925_PATCH_MCU_1_1_hdr.bin
WIFI_RAM_CODE_MT7925_1_1.bin
```

---

## 4. MT7927 vs MT7925 Differences

| Feature | MT7927 (Filogic 380) | MT7925 (Filogic 360) |
|---------|---------------------|---------------------|
| Max Channel Width | **320MHz** | 160MHz |
| Max Speed | ~6500 Mbps | ~5400 Mbps |
| Linux Support | **None** | Kernel 6.7+ |
| Architecture | Identical | Base |
| PCI ID | 14c3:7927 | 14c3:7925 |

**Critical Finding**: MT7925 firmware loads successfully on MT7927 hardware.

---

## 5. Development Priorities

### Phase 1: Get Basic WiFi Working (Priority)
1. Study mt7925 `pci.c` probe sequence in detail
2. Understand what enables WPDMA_GLO_CFG writes
3. Implement correct MCU initialization handshake
4. Get DMA firmware transfer working
5. Create network interface

### Phase 2: Bazzite Packaging
1. Create akmod spec file structure
2. Set up COPR repository for testing
3. Create firmware RPM package
4. Test with Bazzite tester
5. Document installation process

### Phase 3: Broader Distribution
1. Support other Fedora Atomic variants
2. Create Ubuntu/Debian DKMS fallback
3. Arch Linux PKGBUILD
4. Upstream submission preparation

---

## 6. Key Resources

### Source Code References
```
drivers/net/wireless/mediatek/mt76/mt7925/
├── pci.c      # PCI probe sequence
├── mcu.c      # MCU communication
├── init.c     # Hardware init
└── dma.c      # DMA setup

drivers/net/wireless/mediatek/mt76/
├── dma.c              # Generic DMA
├── mt76_connac_mcu.c  # MCU interface
└── util.c             # Utilities
```

### External Links
- [Linux kernel mt7925 source](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/mediatek/mt76/mt7925)
- [OpenWrt mt76 repository](https://github.com/openwrt/mt76)
- [ublue-os/akmods](https://github.com/ublue-os/akmods)
- [BlueBuild](https://blue-build.org/)

### Community Discussions
- [OpenWRT mt76 Issue #927](https://github.com/openwrt/mt76/issues/927) - 39+ upvotes
- [Arch Linux forums](https://bbs.archlinux.org/viewtopic.php?id=303402)

---

## 7. Test Hardware Setup

### For Bazzite Tester
1. Verify MT7927 present: `lspci -nn | grep 14c3:7927`
2. Check kernel version: `uname -r` (need 6.7+)
3. Install firmware files to `/usr/lib/firmware/mediatek/mt7925/`
4. Use `rpm-ostree usroverlay` for temporary testing
5. Check logs: `sudo dmesg | grep -E "mt7927|mt7925"`

### Recovery if Chip Errors
```bash
# Reset via PCI rescan
echo 1 | sudo tee /sys/bus/pci/devices/0000:XX:00.0/remove
sleep 2
echo 1 | sudo tee /sys/bus/pci/rescan
```

---

## Next Steps

1. **Deep dive into mt7925 pci.c** - Understand the exact probe sequence
2. **Create Bazzite test package** - akmod + firmware RPM for tester
3. **Identify unlock sequence** - What makes WPDMA registers writable
4. **Set up COPR repository** - For distributing test builds
