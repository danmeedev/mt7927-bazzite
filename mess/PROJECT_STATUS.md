# MT7927 Driver Project Status

## Current State: **Early Hardware Bring-up**

**v0.3.0** - Added DMA ring prefetch configuration based on mt76 kernel source analysis.

---

## What's Working ✓

| Component | Status | Evidence |
|-----------|--------|----------|
| PCI probe/enumeration | ✓ | Device detected (14c3:7927) |
| BAR0 MMIO mapping | ✓ | 2MB region mapped |
| HIF_REMAP mechanism | ✓ | Successfully reading 0x7c0xxxxx registers |
| WFSYS reset | ✓ | INIT_DONE bit set (0x00000011) |
| Driver ownership (CLR_OWN) | ✓ | Succeeds immediately |
| DMA buffer allocation | ✓ | Firmware buffer allocated |
| Firmware file loading | ✓ | 197KB loaded into memory |

---

## What's Partially Working / Untested

| Component | Status | Notes |
|-----------|--------|-------|
| Chip ID reading | ? | Fixed in v0.2.3, awaiting test |
| DMASHDL bypass | ? | Fixed in v0.2.3, awaiting test |
| DMA ring configuration | ? | Fixed in v0.2.3, awaiting test |
| SET_OWN (power management) | ✗ | Times out - expected without FW |

---

## What's NOT Implemented Yet

| Component | Complexity | Description |
|-----------|------------|-------------|
| **Firmware download to MCU** | High | DMA transfer of firmware to device |
| **MCU command protocol** | High | Sending commands, receiving events |
| **Firmware boot confirmation** | Medium | Verify FW running via MT_CONN_ON_MISC |
| **MAC/PHY initialization** | High | WiFi hardware setup |
| **802.11 registration** | High | Register with cfg80211/mac80211 |
| **Scan/Connect** | High | Actual WiFi functionality |
| **TX/RX data path** | High | Packet handling |
| **Power management** | Medium | Suspend/resume, power saving |
| **WiFi 7 features** | Low priority | 320MHz, MLO (can defer) |

---

## Roadmap

```
Phase 1: Hardware Bring-up     [=========>] ~100%
  ✓ PCI enumeration
  ✓ Register access (direct + remap)
  ✓ WFSYS reset
  ✓ DMA ring setup (v0.3.4 - leave RST bits SET!)

Phase 2: Firmware Boot         [===>------] ~30%
  ✓ DMA firmware transfer via Ring 16 (v0.4.0)
  - MCU command interface (init_download, start commands)
  - Firmware handshake
  - Verify FW running (poll MT_CONN_ON_MISC)

Phase 3: WiFi Initialization   [----------] 0%
  - MAC address retrieval
  - PHY calibration
  - cfg80211 registration
  - Basic scan

Phase 4: Connectivity          [----------] 0%
  - Association
  - TX/RX paths
  - WPA3 support

Phase 5: Polish                [----------] 0%
  - Power management
  - Error recovery
  - Performance tuning
```

---

## Immediate Next Steps (v0.4.0+)

1. **Test v0.4.0** - Verify patch firmware DMA transfer works
2. **Check DMA completion** - cpu_idx should equal dma_idx after transfer
3. **Add MCU init_download command** - Send TARGET_ADDRESS_LEN_REQ before data
4. **Add MCU start command** - Send FW_START_REQ after transfer
5. **Poll MT_CONN_ON_MISC** - Check bits 0-1 for FW_N9_RDY (0x3)
5. **Check MT_CONN_ON_MISC for FW ready** - Currently shows 0x00000000

### Key Discovery (v0.3.0)

The DMA ring MISMATCH issue was caused by missing **ring prefetch configuration**. The mt76 driver calls `mt792x_dma_prefetch()` which writes to `MT_WFDMA0_TX_RING16_EXT_CTRL` (and other EXT_CTRL registers) BEFORE writing to the actual ring BASE/CNT registers.

For MT7925/MT7927:
```c
#define PREFETCH(base, depth)  (((base) << 16) | (depth))
// Ring 16 (FWDL): PREFETCH(0x0540, 0x4) = 0x05400004
mt76_wr(dev, MT_WFDMA0_TX_RING16_EXT_CTRL, 0x05400004);
```

Without this step, the ring registers appear to be locked/protected.

---

## Estimated Effort Remaining

- **To firmware boot:** Several more iterations
- **To basic scan working:** Significant (weeks of development)
- **To production-ready:** Substantial (needs testing, edge cases, power mgmt)

The hardest part ahead is the MCU command protocol - it's complex and needs to match what the firmware expects exactly.

---

## Hardware Details

- **Chip:** MediaTek MT7927 (AMD RZ738)
- **PCI ID:** 14c3:7927
- **Subsystem:** 105b:e104
- **Target OS:** Bazzite Linux (Fedora-based)
- **Reference Driver:** mt7925 from Linux kernel mt76 framework
- **Architecture:** MT7927 is identical to MT7925 except for 320MHz WiFi 7 support

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v0.4.2 | 2026-02-03 | **Fix ring BASE verification** - v0.4.1 caused IOMMU page faults because ring BASE=0. Added pre-DMA verification, CSR_DISP_BASE_PTR_CHAIN_EN flag before prefetch, and early abort if ring not writable. |
| v0.4.1 | 2026-02-03 | **Fix section type check** - v0.4.0 was skipping all sections due to wrong type check. Now downloads all patch sections. |
| v0.4.0 | 2026-02-03 | **Firmware DMA transfer via Ring 16.** Added MCU TXD construction, patch header parsing, and FW_SCATTER chunk transfer. First attempt at sending firmware to device. |
| v0.3.4 | 2026-02-03 | **KEY FIX:** Leave LOGIC_RST bits SET (kernel pattern). This fixed ring register writes. |
| v0.3.0 | 2026-02-03 | Add DMA ring prefetch configuration (MT_WFDMA0_TX_RING16_EXT_CTRL). |
| v0.2.3 | 2026-02-03 | Fix Chip ID remap, DMASHDL remap, DMA clock gating |
| v0.2.2 | 2026-02-03 | Reduce timeouts for faster debug feedback |
| v0.2.1 | 2026-02-03 | Add HIF_REMAP for high address registers |
| v0.2.0 | 2026-02-03 | Add bounds checking to prevent crashes |

---

## Repository

https://github.com/danmeedev/mt7927-bazzite
