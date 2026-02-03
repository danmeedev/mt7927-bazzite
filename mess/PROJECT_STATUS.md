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

Phase 2: Firmware Boot         [=====>----] ~40%
  ✓ DMA firmware transfer via Ring 16 (v0.4.0)
  ✓ MCU command interface (v0.5.0 - Ring 15, RX Ring 0)
  ✓ DMA interrupt enable (v0.6.0 - KEY FIX!)
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

## Immediate Next Steps (v0.9.0+)

1. **Test v0.9.0** - Check if ROM wake sequence helps dma_idx advance
2. **Analyze diagnostic output** - ROM state, MCU_CMD values, INT_STA
3. **If ROM still not responding**:
   - Try alternative wake methods (different MCU_CMD bits)
   - Check if ROM needs WFSYS reset to be done differently
   - Investigate MT7921 vs MT7925 ROM differences
4. **If dma_idx advances** - Continue with FW_START command
5. **Poll MT_CONN_ON_MISC** - Look for any state change from 0x00000000

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
| v0.10.1 | 2026-02-03 | **Fix hang from PCI FLR.** v0.10.0 hung on pci_reset_function() - device doesn't support FLR or it caused issues. Added skip_pci_reset module param (default: true to skip). Keep enhanced chip ID diagnostics. |
| v0.10.0 | 2026-02-03 | **Add PCI function-level reset and enhanced chip ID diagnostics.** v0.9.0 showed MT_CONN_ON_MISC=0x00000000 and unusual Chip ID 0x7fff0000. Added: PCI FLR at probe start to ensure clean device state, alternative chip ID register reads (TOP_HCR/TOP_HVR), PCI config space device ID check. Hypothesis: Device may need full reset after previous driver corrupted state. |
| v0.9.0 | 2026-02-03 | **Add ROM bootloader wake sequence and diagnostics.** v0.8.0 fixed descriptors (no IOMMU faults) but dma_idx still stuck at 0 - ROM not processing rings. Added: MT_MCU_CMD register wake signals, ROM state polling via MT_CONN_ON_MISC, expanded diagnostic dump on timeout showing ROM state, ring BASE, INT_ENA values. Hypothesis: ROM needs explicit wake or different handshake. |
| v0.8.0 | 2026-02-03 | **CRITICAL: Fix DMA descriptor control field bits!** Our MT_DMA_CTL_SD_LEN0 was GENMASK(15,0) but kernel uses GENMASK(29,16). LAST_SEC0 was BIT(16) but kernel uses BIT(30). BURST was BIT(17) but kernel uses BIT(15). This caused completely malformed descriptors - device saw garbage lengths! Also fixed: upper DMA address bits go in info field (bits [3:0]) not buf1. |
| v0.7.0 | 2026-02-03 | **CRITICAL: Fix prefetch buffer values** - v0.6.0 still had IOMMU faults at 0x0, 0x300, 0x500. These addresses ARE the prefetch buffer bases! Our RX ring prefetch values were WRONG (0x0100 instead of 0x0000, etc). TX ring depths were also wrong (0x4 instead of 0x10). Fixed all prefetch values to match kernel mt792x_dma.c for is_mt7925(). |
| v0.6.0 | 2026-02-03 | **Enable DMA interrupts** - v0.5.0 showed dma_idx stuck at 0 for ALL rings. Added MT_INT_TX_DONE_15/16, MT_INT_RX_DONE_0, MT_INT_MCU_CMD bits to HOST_INT_ENA. Also added MT7925-specific INT_TX_PRI/INT_RX_PRI and UWFDMA0_GLO_CFG_EXT1 config. |
| v0.5.0 | 2026-02-03 | **Add MCU command protocol** - v0.4.3 showed IOMMU faults at internal addresses (0x0, 0x300, etc.) indicating ROM not using our ring. Added Ring 15 (MCU commands), RX Ring 0 (MCU events), PATCH_SEM_CONTROL and TARGET_ADDRESS_LEN_REQ commands before FW_SCATTER. |
| v0.4.3 | 2026-02-03 | **Add BURST flag and debug output** - v0.4.2 ring BASE works but IOMMU faults persist. Added MT_DMA_CTL_BURST to descriptor, added descriptor dump on timeout, added more state diagnostics. |
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
