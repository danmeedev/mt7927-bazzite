# MT7927 Driver Project Status

## Current State: **Early Hardware Bring-up**

**v0.2.3** - Driver loads, basic register access working, no WiFi functionality yet.

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
Phase 1: Hardware Bring-up     [====>-----] ~50%
  ✓ PCI enumeration
  ✓ Register access (direct + remap)
  ✓ WFSYS reset
  ? DMA ring setup (v0.2.3 testing)

Phase 2: Firmware Boot         [----------] 0%
  - DMA firmware transfer
  - MCU command interface
  - Firmware handshake
  - Verify FW running

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

## Immediate Next Steps (after v0.2.3 test)

1. **Confirm DMA rings configure correctly** - RING16_BASE/CNT should show OK
2. **Confirm Chip ID reads** - Should see actual chip ID, not 0xdeadbeef
3. **Implement firmware DMA transfer** - Actually send firmware to device
4. **Implement MCU command protocol** - Based on mt7925 mcu.c patterns
5. **Check MT_CONN_ON_MISC for FW ready** - Currently shows 0x00000000

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
| v0.2.3 | 2025-02-03 | Fix Chip ID remap, DMASHDL remap, DMA clock gating |
| v0.2.2 | 2025-02-03 | Reduce timeouts for faster debug feedback |
| v0.2.1 | 2025-02-03 | Add HIF_REMAP for high address registers |
| v0.2.0 | 2025-02-03 | Add bounds checking to prevent crashes |

---

## Repository

https://github.com/danmeedev/mt7927-bazzite
