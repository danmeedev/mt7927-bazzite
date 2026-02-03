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

---

## 8. Firmware Download Protocol (Detailed)

### 8.1 Firmware Files

| File | Purpose | Size (typical) |
|------|---------|----------------|
| `WIFI_MT7925_PATCH_MCU_1_1_hdr.bin` | ROM Patch | ~100KB |
| `WIFI_RAM_CODE_MT7925_1_1.bin` | RAM Firmware | ~200KB |

Location: `/lib/firmware/mediatek/mt7925/`

### 8.2 Firmware Header Structures

**Firmware Trailer (at end of file):**
```c
struct mt76_connac2_fw_trailer {
    u8 chip_id;           // Chip identifier
    u8 eco_code;          // ECO code
    u8 n_region;          // Number of firmware regions
    u8 format_ver;        // Format version
    u8 format_flag;       // Format flags
    u8 rsv[2];
    char fw_ver[10];      // Firmware version string
    char build_date[15];  // Build date
    __le32 crc;           // CRC32 checksum
} __packed;
```

**Firmware Region (parsed backwards from trailer):**
```c
struct mt76_connac2_fw_region {
    __le32 decomp_crc;      // Decompression CRC
    __le32 decomp_len;      // Decompressed length
    __le32 decomp_blk_sz;   // Block size for decompression
    u8 rsv[4];
    __le32 addr;            // Target memory address
    __le32 len;             // Region length
    u8 feature_set;         // Feature flags
    u8 type;                // Region type
    u8 rsv1[14];
} __packed;
```

**Patch Header:**
```c
struct mt76_connac2_patch_hdr {
    char build_date[16];
    char platform[4];
    __be32 hw_sw_ver;
    __be32 patch_ver;
    __be16 checksum;
    u16 rsv;
    struct {
        __be32 patch_ver;
        __be32 subsys;
        __be32 feature;
        __be32 n_region;
        __be32 crc;
        u32 rsv[11];
    } desc;
} __packed;
```

### 8.3 DMA Ring 16 (FWDL Ring) Configuration

**Ring Parameters:**
- Ring Index: 16
- Size: 128 descriptors
- Base Address: `MT_TX_RING_BASE + 16 * 0x10` = `0xd4400`
- Purpose: Firmware scatter transfer

**DMA Descriptor Format:**
```c
struct mt76_desc {
    __le32 buf0;    // Buffer 0 physical address (lower 32 bits)
    __le32 ctrl;    // Control: length, flags
    __le32 buf1;    // Buffer 1 physical address (for scatter-gather)
    __le32 info;    // Additional metadata
} __packed __aligned(4);

// Control field bits
#define MT_DMA_CTL_SD_LEN0      GENMASK(15, 0)   // Segment 0 length
#define MT_DMA_CTL_LAST_SEC0    BIT(16)          // Last segment 0
#define MT_DMA_CTL_DMA_DONE     BIT(31)          // DMA completion flag
```

### 8.4 Firmware Download Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│                   PATCH LOADING SEQUENCE                        │
├─────────────────────────────────────────────────────────────────┤
│ 1. Acquire patch semaphore                                      │
│    → MCU_CMD(PATCH_SEM_CONTROL) with op=GET                     │
│    ← Response: PATCH_NOT_DL_SEM_SUCCESS (0x02)                  │
│                                                                 │
│ 2. For each patch section:                                      │
│    a. Init download                                             │
│       → MCU_CMD(PATCH_START_REQ) if addr=0x900000               │
│       → MCU_CMD(TARGET_ADDRESS_LEN_REQ) otherwise               │
│       Payload: { addr, len, mode }                              │
│                                                                 │
│    b. Send data in 4KB chunks                                   │
│       → MCU_CMD(FW_SCATTER) via Ring 16                         │
│       Each chunk: TXD header + 4096 bytes data                  │
│                                                                 │
│ 3. Finalize patch                                               │
│    → MCU_CMD(PATCH_FINISH_REQ)                                  │
│                                                                 │
│ 4. Release semaphore                                            │
│    → MCU_CMD(PATCH_SEM_CONTROL) with op=RELEASE                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                   RAM FIRMWARE LOADING                          │
├─────────────────────────────────────────────────────────────────┤
│ 1. Parse firmware trailer (at end of file)                      │
│    - Read n_region count                                        │
│    - Parse each region header backwards                         │
│                                                                 │
│ 2. For each firmware region:                                    │
│    a. Init download                                             │
│       → MCU_CMD(TARGET_ADDRESS_LEN_REQ)                         │
│       Payload: { region->addr, region->len, mode }              │
│                                                                 │
│    b. Send data in 4KB chunks                                   │
│       → MCU_CMD(FW_SCATTER) via Ring 16                         │
│                                                                 │
│ 3. Start firmware execution                                     │
│    → MCU_CMD(FW_START_REQ)                                      │
│    Payload: { override_addr, option }                           │
│                                                                 │
│ 4. Poll for firmware ready                                      │
│    Register: MT_CONN_ON_MISC (0x7c0600f0)                       │
│    Wait for: MT_TOP_MISC2_FW_N9_RDY bits set                    │
│    Timeout: 1500ms                                              │
└─────────────────────────────────────────────────────────────────┘
```

### 8.5 MCU Commands for Firmware Download

| Command | Value | Purpose |
|---------|-------|---------|
| `TARGET_ADDRESS_LEN_REQ` | 0x01 | Set target address and length |
| `FW_START_REQ` | 0x02 | Start firmware execution |
| `PATCH_START_REQ` | 0x05 | Initialize patch download |
| `PATCH_FINISH_REQ` | 0x07 | Finalize patch |
| `FW_SCATTER` | 0xEE | Send firmware data chunk |
| `RESTART_DL_REQ` | 0xEF | Restart download |

### 8.6 Download Mode Flags

```c
#define DL_MODE_ENCRYPT         BIT(0)   // Encrypted firmware
#define DL_MODE_KEY_IDX         GENMASK(2, 1)  // Encryption key index
#define DL_MODE_RESET_SEC_IV    BIT(3)   // Reset security IV
#define DL_MODE_WORKING_PDA_CR4 BIT(4)   // Working PDA CR4
#define DL_MODE_VALID_RAM_ENTRY BIT(5)   // Valid RAM entry point
#define DL_MODE_NEED_RSP        BIT(31)  // Require response
```

---

## 9. MCU Command Protocol (Detailed)

### 9.1 Unified MCU TXD Structure

This is the primary header structure for MT7925/MT7927:

```c
struct mt76_connac2_mcu_uni_txd {
    __le32 txd[8];      // Hardware descriptor (32 bytes)

    /* DW1 */
    __le16 len;         // Length excluding txd
    __le16 cid;         // Command ID (16-bit)

    /* DW2 */
    u8 rsv;
    u8 pkt_type;        // Must be 0xa0 (MCU_PKT_ID)
    u8 frag_n;          // Fragment number
    u8 seq;             // Sequence number (1-15, wraps)

    /* DW3 */
    __le16 checksum;    // 0 = no checksum
    u8 s2d_index;       // Source-to-destination routing
    u8 option;          // Command option flags

    /* DW4 */
    u8 rsv1[4];
} __packed __aligned(4);
```

### 9.2 S2D Index Values (Routing)

| Value | Constant | Route |
|-------|----------|-------|
| 0x00 | `MCU_S2D_H2N` | Host → WiFi Manager (WM) |
| 0x01 | `MCU_S2D_C2N` | WiFi Accelerator → WM |
| 0x02 | `MCU_S2D_H2C` | Host → WiFi Accelerator (WA) |
| 0x03 | `MCU_S2D_H2CN` | Host → Both WA and WM |

### 9.3 Command Option Flags

```c
#define MCU_CMD_ACK             BIT(0)   // Expect acknowledgment
#define MCU_CMD_UNI             BIT(1)   // Unified command format
#define MCU_CMD_SET             BIT(2)   // Set operation (vs query)

// Common combinations
#define MCU_CMD_UNI_QUERY_ACK   (MCU_CMD_ACK | MCU_CMD_UNI)        // Query with response
#define MCU_CMD_UNI_EXT_ACK     (MCU_CMD_ACK | MCU_CMD_UNI | MCU_CMD_SET)  // Set with response
```

### 9.4 MCU Response Structure

```c
struct mt76_connac2_mcu_rxd {
    __le32 rxd[6];       // Hardware descriptor (24 bytes)
    __le16 len;          // Length
    __le16 pkt_type_id;  // Packet type
    u8 eid;              // Event identifier
    u8 seq;              // Sequence (for correlation)
    u8 option;
    u8 rsv;
    u8 ext_eid;          // Extended event ID
    u8 rsv1[2];
    u8 s2d_index;
    u8 tlv[];            // TLV data follows
} __packed;
```

### 9.5 Event Types

**Standard Events:**
| Event | Value | Purpose |
|-------|-------|---------|
| `FW_START` | 0x01 | Firmware started |
| `ACCESS_REG` | 0x02 | Register access response |
| `MT_PATCH_SEM` | 0x04 | Patch semaphore response |
| `SCAN_DONE` | 0x0D | Scan completed |
| `TX_DONE` | 0x0F | TX completion |
| `EXT` | 0xED | Extended event |
| `RESTART_DL` | 0xEF | Restart download |
| `COREDUMP` | 0xF0 | Core dump |

**Unified Events (MT7925-specific):**
| Event | Value | Purpose |
|-------|-------|---------|
| `UNI_RESULT` | 0x01 | Command result |
| `UNI_FW_LOG` | 0x04 | Firmware log |
| `UNI_SCAN_DONE` | 0x0E | Scan complete |
| `UNI_TX_DONE` | 0x2D | TX done |
| `UNI_THERMAL` | 0x35 | Thermal event |
| `UNI_RSSI_MONITOR` | 0x41 | RSSI threshold |

### 9.6 Key MCU Registers

| Register | Address | Purpose |
|----------|---------|---------|
| `MT_MCU_CMD` | 0xd41f0 | MCU command register |
| `MT_MCU_INT_EVENT` | 0x3108 | MCU interrupt events |
| `MT_SWDEF_MODE` | 0x41f23c | Operating mode |

**MCU Command Register Flags (0xd41f0):**
```c
MT_MCU_CMD_WAKE_RX_PCIE   // Wake RX PCIe
MT_MCU_CMD_STOP_DMA       // Stop DMA
MT_MCU_CMD_RESET_DONE     // Reset complete
MT_MCU_CMD_RECOVERY_DONE  // Recovery complete
MT_MCU_CMD_NORMAL_STATE   // Normal operation
```

---

## 10. Firmware Boot Handshake

### 10.1 Complete Boot Sequence

```c
int mt792x_load_firmware(struct mt792x_dev *dev)
{
    // Stage 1: Load Patch
    ret = mt76_connac2_load_patch(&dev->mt76,
              "mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin");

    // Stage 2: Load RAM Firmware
    ret = mt76_connac2_load_ram(&dev->mt76,
              "mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin", NULL);

    // Stage 3: Wait for Firmware Ready
    // Poll MT_CONN_ON_MISC (0x7c0600f0) for MT_TOP_MISC2_FW_N9_RDY
    // Timeout: 1500ms
}
```

### 10.2 Firmware Ready Detection

**Register:** `MT_CONN_ON_MISC` (0x7c0600f0)

**Status Bits:**
```c
MT_TOP_MISC2_FW_PWR_ON   // Firmware power on
MT_TOP_MISC2_FW_N9_ON    // N9 processor running
MT_TOP_MISC2_FW_N9_RDY   // N9 ready for commands (GENMASK(1, 0))
```

**Polling:**
```c
if (!mt76_poll_msec(dev, MT_CONN_ON_MISC,
                    MT_TOP_MISC2_FW_N9_RDY,
                    MT_TOP_MISC2_FW_N9_RDY, 1500)) {
    // Timeout - firmware failed to start
}
```

### 10.3 Firmware State Values

| State | Value | Meaning |
|-------|-------|---------|
| `INITIAL` | 0 | Initial state |
| `FW_DOWNLOAD` | 1 | Download in progress |
| `NORMAL_OPERATION` | 2 | Normal operation |
| `NORMAL_TRX` | 3 | Normal TX/RX |
| `RDY` | 4 | Fully ready |
| `WACPU_RDY` | 7 | WA CPU ready |

### 10.4 Post-Boot Initialization

After firmware boots:
```c
int mt7925_run_firmware(struct mt792x_dev *dev)
{
    // 1. Load firmware (patch + RAM)
    mt792x_load_firmware(dev);

    // 2. Query hardware capabilities
    mt7925_mcu_get_nic_capability(dev);

    // 3. Load regulatory/CLC data
    mt7925_load_clc(dev, ...);

    // 4. Enable firmware logging (optional)
    mt7925_mcu_fw_log_2_host(dev, ...);
}
```

---

## 11. TXD0 Construction for MCU Commands

The first word of the TXD is constructed as:

```c
// For FW_SCATTER (firmware data)
txd[0] = FIELD_PREP(MT_TXD0_TX_BYTES, skb->len) |
         FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_FW) |
         FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_FWDL);

// For regular MCU commands
txd[0] = FIELD_PREP(MT_TXD0_TX_BYTES, skb->len) |
         FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
         FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_Q0);
```

**Queue Selection:**
- `FW_SCATTER` commands → Ring 16 (FWDL queue)
- All other MCU commands → Ring 15 (MCU WM queue)

---

## 12. Implementation Checklist for MT7927 Driver

### Phase 1: Hardware Bring-up ✓
- [x] PCI enumeration
- [x] BAR0 MMIO mapping
- [x] HIF_REMAP for high addresses
- [x] WFSYS reset
- [x] Driver ownership
- [ ] DMA ring configuration (v0.2.3 testing)

### Phase 2: Firmware Boot (Next)
- [ ] Parse firmware headers
- [ ] Implement MCU TXD construction
- [ ] Implement FW_SCATTER transfer
- [ ] Implement patch loading sequence
- [ ] Implement RAM firmware loading
- [ ] Poll for FW_N9_RDY

### Phase 3: MCU Commands
- [ ] Implement command send/receive
- [ ] Implement event handling
- [ ] Query NIC capabilities
- [ ] Load CLC data

### Phase 4: WiFi Initialization
- [ ] MAC address retrieval
- [ ] cfg80211 registration
- [ ] Basic scan
