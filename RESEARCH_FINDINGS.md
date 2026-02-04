# MT7927 Research Findings

## Critical Discovery: MT7927 = MT6639 Architecture

### Key Finding from Windows Driver Analysis
The Windows `mtkwlan.dat` firmware index reveals MT7927 uses MT6639 firmware:
- `WIFI_MT6639_PATCH_MCU_2_1_hdr.bin` - Patch firmware
- `WIFI_RAM_CODE_MT6639_2_1.bin` - Main WiFi firmware

### Why mt7925e Driver Fails

The kernel's mt7925e driver fails because:

1. **Wrong chip initialization** - MT6639 requires different register addresses and sequences
2. **ASIC revision: 0000** - This indicates the chip ID read is failing
3. **MCU not responding** - The ROM bootloader needs different wake-up sequence

### MT6639 Initialization Requirements (from MediaTek source)

From analysis of `Fede2782/MTK_modules` repository:

#### Key Registers
- **MMIO Base**: `0x74030000` (PCIe MAC)
- **ConnInfra Offset**: `0x1F5000` range
- **WFDMA Hosts**: `0x820c0000` (PLE), `0x820c8000` (PSE)

#### Initialization Sequence
1. **ConnInfra Wakeup**: Write to `CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR`
2. **Version Polling**: Validate chip ID matches (10 attempts max)
3. **GPIO Mode Switch**: Transition to GPIO mode for reset control
4. **Subsystem Reset**: Write `0x10351` then `0x10340` to BT/WF reset registers
5. **Delay Sequencing**: 10ms post-reset, 50ms stabilization
6. **ROM Code Polling**: Wait for `WF_TOP_CFG_ON_ROMCODE_INDEX_REMAP_ADDR` == `0x1D1E`

#### Power Management
- **L1/L1.2 ASPM**: Polled state transitions with 200µs timeout
- **Driver Own**: Specific sequence before WFDMA interrupt enable/disable

### Current Problem

The kernel mt7925e driver:
- Uses MT7925 register addresses (different from MT6639)
- Doesn't have ConnInfra wakeup sequence
- Doesn't poll for ROM ready state (`0x1D1E`)
- Sends MCU commands before ROM is ready

### Next Steps

1. **Option A**: Wait for MediaTek/kernel upstream MT6639 support
2. **Option B**: Create custom driver based on gen4m source code
3. **Option C**: Patch mt7925e to add MT6639 initialization

### Source References

- Windows driver: `mess/mtkwlan.dat`, `mess/mtkwecx.inf`
- MediaTek gen4m: https://github.com/Fede2782/MTK_modules/tree/bsp-rodin-v-oss/connectivity/wlan/core/gen4m/chips/mt6639
- OpenWRT mt76 issue: https://github.com/openwrt/mt76/issues/927

### Firmware Files Extracted

| File | Size | Purpose |
|------|------|---------|
| `WIFI_MT6639_PATCH_MCU_2_1_hdr.bin` | 299 KB | MCU patch |
| `WIFI_RAM_CODE_MT6639_2_1.bin` | 1.5 MB | Main firmware |
| `mtkwl7927.dat` | 618 KB | Config data |
| `mtkwl7927_2.dat` | 1.8 MB | Config data |

### Chip Architecture

```
MT7927 (PCIe WiFi 7)
    └── Uses MT6639 internal architecture
        └── Gen4m connectivity framework
            └── ConnInfra subsystem required
                └── Different registers than MT7925 (Gen4)
```

The mt7925 is Gen4 while mt6639/mt7927 is Gen4m (mobile variant) with additional ConnInfra requirements.
