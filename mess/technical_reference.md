# MT7927 Technical Reference - Deep Dive Findings
**Target Platform**: AMD systems (MT7927 / AMD RZ738)
**Date**: 2026-02-02

---

## Executive Summary

The deep dive research has identified the **exact initialization sequence** required to unlock the MT7927 chip for DMA operations. The key discovery is that the chip requires a specific **power management handoff** and **WFSYS reset** sequence before registers become writable.

---

## 1. The Unlock Sequence (Critical)

The MT7927 registers become writable when **ALL** of these steps complete in order:

### Step 1: PCI Setup
```c
pci_enable_device(pdev);
pci_set_master(pdev);           // Enable bus mastering
dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
```

### Step 2: Power Management Handoff
```c
// Register: 0x7c060010 (MT_CONN_ON_LPCTL)

// First: Give ownership to firmware
mt76_wr(dev, 0x7c060010, 0x1);  // PCIE_LPCR_HOST_SET_OWN
// Poll until bit 2 == 0x4 (firmware owns)

// Then: Take ownership for driver
mt76_wr(dev, 0x7c060010, 0x2);  // PCIE_LPCR_HOST_CLR_OWN
usleep_range(2000, 3000);       // 2-3ms delay (critical for ASPM)
// Poll until bit 2 == 0x0 (driver owns)
```

### Step 3: EMI Sleep Protection
```c
// Register: 0x18011100 (MT_HW_EMI_CTL)
mt76_rmw_field(dev, 0x18011100, BIT(1), 1);  // Enable sleep protection
```

### Step 4: WFSYS Reset (THE KEY STEP)
```c
// Register: 0x7c000140 (WFSYS reset for MT7925/MT7927)

// Assert reset
mt76_clear(dev, 0x7c000140, BIT(0));  // Clear WFSYS_SW_RST_B
msleep(50);                            // MANDATORY 50ms delay

// Deassert reset
mt76_set(dev, 0x7c000140, BIT(0));    // Set WFSYS_SW_RST_B

// Poll for init complete (up to 500ms)
// Wait until bit 4 == 1 (WFSYS_SW_INIT_DONE)
```

### Step 5: Interrupt Setup
```c
mt76_wr(dev, 0xd4204, 0);      // Disable host IRQ
mt76_wr(dev, 0x10188, 0xff);   // Enable PCIe MAC interrupts
```

**After Step 5, WPDMA registers at 0xd4208 become writable.**

---

## 2. DMA Initialization Sequence

### Phase A: Disable DMA with Force Reset
```c
// Clear DMA enable bits
mt76_clear(dev, 0xd4208,  // MT_WFDMA0_GLO_CFG
    BIT(0) |   // TX_DMA_EN
    BIT(2) |   // RX_DMA_EN
    BIT(12) |  // FIFO_LITTLE_ENDIAN
    BIT(21) |  // OMIT_RX_INFO_PFET2
    BIT(27) |  // OMIT_RX_INFO
    BIT(28));  // OMIT_TX_INFO

// Poll for busy flags to clear (bits 1 and 3)
// Timeout: 100ms

// Disable DMASHDL
mt76_clear(dev, 0xd42b0, BIT(16));  // TX_DMASHDL_ENABLE
mt76_set(dev, 0x7c026004, BIT(0));  // DMASHDL_BYPASS

// Force reset sequence
mt76_clear(dev, 0xd4100, BIT(4) | BIT(5));  // Clear reset bits
mt76_set(dev, 0xd4100, BIT(4) | BIT(5));    // Set reset bits
mt76_clear(dev, 0xd4100, BIT(4) | BIT(5));  // Clear again
```

### Phase B: Initialize Queues

| Queue | Ring Index | Size | Base Register |
|-------|------------|------|---------------|
| TX Data Band 0 | 0 | 2048 | 0xd4300 |
| MCU Command (WM) | 15 | 256 | 0xd43F0 |
| Firmware Download | 16 | 128 | 0xd4400 |
| RX MCU Events | 0 | 512 | 0xd4500 |
| RX Data | 2 | 1536 | 0xd4540 |

### Phase C: Enable DMA
```c
// Reset pointers
mt76_wr(dev, 0xd4228, ~0);  // RST_DTX_PTR
mt76_wr(dev, 0xd4260, ~0);  // RST_DRX_PTR

// Clear delay interrupts
mt76_wr(dev, 0xd4238, 0);   // PRI_DLY_INT_CFG0

// Set global config
mt76_set(dev, 0xd4208,
    BIT(6) |     // TX_WB_DDONE
    BIT(12) |    // FIFO_LITTLE_ENDIAN
    BIT(15) |    // CSR_DISP_BASE_PTR_CHAIN_EN
    BIT(21) |    // OMIT_RX_INFO_PFET2
    BIT(28) |    // OMIT_TX_INFO
    BIT(30) |    // CLK_GAT_DIS
    (3 << 4));   // DMA_SIZE = 3

// Enable DMA engines
mt76_set(dev, 0xd4208, BIT(0) | BIT(2));  // TX_DMA_EN | RX_DMA_EN
```

---

## 3. Firmware Loading Protocol

### Firmware Files (MT7925 firmware works on MT7927)
- `/lib/firmware/mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin`
- `/lib/firmware/mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin`

### Loading Sequence
```
1. Acquire patch semaphore
   - Send MCU_CMD(PATCH_SEM_CONTROL) with PATCH_SEM_GET
   - Wait for response: PATCH_NOT_DL_SEM_SUCCESS (2)

2. Initialize download
   - Send MCU_CMD(TARGET_ADDRESS_LEN_REQ)
   - Address: 0x900000, Length: patch size

3. Transfer patch data
   - Send via MCU_CMD(FW_SCATTER) in 4096-byte chunks
   - Use MT_MCUQ_FWDL queue (ring 16)

4. Finalize patch
   - Send MCU_CMD(PATCH_FINISH_REQ)

5. Release semaphore
   - Send MCU_CMD(PATCH_SEM_CONTROL) with PATCH_SEM_RELEASE

6. Load RAM code (similar process)

7. Start firmware
   - Send MCU_CMD(FW_START_REQ) with execution address
   - Poll 0x7c0600f0 for bits 0-1 == 0x3 (FW_N9_RDY)
```

### FW_STATUS Transitions
- `0xffff10f1` → Waiting for DMA firmware transfer
- After successful load → `MT_TOP_MISC2_FW_N9_RDY` bits set at 0x7c0600f0

---

## 4. Key Register Reference

### Power Management
| Register | Address | Purpose |
|----------|---------|---------|
| MT_CONN_ON_LPCTL | 0x7c060010 | Power ownership control |
| MT_HW_EMI_CTL | 0x18011100 | EMI sleep protection |

### Reset Control
| Register | Address | Purpose |
|----------|---------|---------|
| WFSYS Reset | 0x7c000140 | WiFi subsystem reset |
| MT_WFDMA0_RST | 0xd4100 | DMA reset control |

### DMA Configuration
| Register | Address | Purpose |
|----------|---------|---------|
| MT_WFDMA0_GLO_CFG | 0xd4208 | Global DMA config |
| MT_WFDMA0_GLO_CFG_EXT0 | 0xd42b0 | Extended config |
| MT_DMASHDL_SW_CONTROL | 0x7c026004 | Scheduler bypass |

### Interrupts
| Register | Address | Purpose |
|----------|---------|---------|
| MT_WFDMA0_HOST_INT_ENA | 0xd4204 | Host interrupt enable |
| MT_PCIE_MAC_INT_ENABLE | 0x10188 | PCIe MAC interrupts |

### Firmware Status
| Register | Address | Purpose |
|----------|---------|---------|
| MT_CONN_ON_MISC | 0x7c0600f0 | Firmware ready status |
| MT_HW_CHIPID | 0x70010200 | Chip identification |
| MT_HW_REV | 0x70010204 | Hardware revision |

---

## 5. AMD RZ738 Specifics

The MT7927 is sold as **AMD RZ738** on AMD platforms:
- Same PCI ID: `14c3:7927`
- Same architecture as MT7925
- Difference: 320MHz channel support (WiFi 7)

### Known Subsystem IDs on AMD Systems
| Subsystem | Manufacturer |
|-----------|--------------|
| 105b:e0fd | Foxconn (common on ASUS/Gigabyte AMD boards) |

---

## 6. What Was Missing in Original MT7927 Project

The original project at github.com/ehausig/mt7927 was missing:

1. **Power management handoff** - Must give ownership to FW first, then take it back
2. **WFSYS reset sequence** - The 50ms delay and polling for INIT_DONE
3. **Correct register addresses** - Using 0x7c000140 for MT7925-class chips
4. **Proper DMA reset** - The clear-set-clear sequence for WFDMA0_RST
5. **DMASHDL bypass** - Required before DMA configuration

---

## 7. Implementation Strategy

### Recommended Approach
1. **Don't rewrite from scratch** - Adapt mt7925 driver
2. **Add MT7927 PCI ID** to mt7925 driver's ID table
3. **Test basic binding** with proper unlock sequence
4. **Verify firmware loads** via MCU commands
5. **Eventually** add 320MHz support

### For Akmod Package
- Build against mt7925 driver with MT7927 PCI ID added
- Include firmware files in separate package
- Sign for Secure Boot compatibility
