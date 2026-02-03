// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 WiFi 7 Linux Driver
 *
 * Based on mt7925 driver from the Linux kernel mt76 framework.
 * The MT7927 (AMD RZ738) is architecturally identical to MT7925
 * except for 320MHz channel support.
 *
 * This driver implements the correct initialization sequence
 * discovered through analysis of the mt7925 driver.
 *
 * Copyright (C) 2026 MT7927 Linux Driver Project
 * Author: MT7927 Community
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>

#define DRV_NAME "mt7927"
#define DRV_VERSION "0.7.0"

/* PCI IDs - MT7927 and known variants */
#define MT7927_VENDOR_ID	0x14c3
#define MT7927_DEVICE_ID	0x7927
#define MT6639_DEVICE_ID	0x6639	/* Mobile variant */
#define RZ738_DEVICE_ID		0x0738	/* AMD RZ738 variant */

/* Module parameters for debugging */
static bool debug_regs = true;
module_param(debug_regs, bool, 0644);
MODULE_PARM_DESC(debug_regs, "Enable verbose register debugging (default: true)");

static bool try_alt_reset = false;
module_param(try_alt_reset, bool, 0644);
MODULE_PARM_DESC(try_alt_reset, "Try alternative MT7921 reset address (default: false)");

static bool disable_aspm = false;
module_param(disable_aspm, bool, 0644);
MODULE_PARM_DESC(disable_aspm, "Disable ASPM during init (default: false)");

/* =============================================================================
 * Register Definitions (from mt7925/mt76 analysis)
 * =============================================================================
 */

/* Base addresses */
#define MT_WFDMA0_BASE			0xd4000

/* Power Management - MT_CONN_ON_LPCTL */
#define MT_CONN_ON_LPCTL		0x7c060010
#define MT_CONN_ON_LPCTL_ALT		0x18060010	/* MT7921 variant */
#define PCIE_LPCR_HOST_SET_OWN		BIT(0)
#define PCIE_LPCR_HOST_CLR_OWN		BIT(1)
#define PCIE_LPCR_HOST_OWN_SYNC		BIT(2)

/* EMI Control */
#define MT_HW_EMI_CTL			0x18011100
#define MT_HW_EMI_CTL_SLPPROT_EN	BIT(1)

/* WFSYS Reset - Critical for unlocking registers */
#define MT_WFSYS_SW_RST_B		0x7c000140	/* MT7925/MT7927 */
#define MT_WFSYS_SW_RST_B_ALT		0x18000140	/* MT7921 variant */
#define WFSYS_SW_RST_B			BIT(0)
#define WFSYS_SW_INIT_DONE		BIT(4)

/* Chip identification */
#define MT_HW_CHIPID			0x70010200
#define MT_HW_REV			0x70010204

/* Connection status */
#define MT_CONN_STATUS			0x7c053c10

/* WFDMA Global Configuration */
#define MT_WFDMA0_GLO_CFG		(MT_WFDMA0_BASE + 0x208)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN	BIT(0)
#define MT_WFDMA0_GLO_CFG_TX_DMA_BUSY	BIT(1)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN	BIT(2)
#define MT_WFDMA0_GLO_CFG_RX_DMA_BUSY	BIT(3)
#define MT_WFDMA0_GLO_CFG_DMA_SIZE	GENMASK(5, 4)
#define MT_WFDMA0_GLO_CFG_TX_WB_DDONE	BIT(6)
#define MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN	BIT(12)
#define MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN	BIT(15)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2	BIT(21)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO	BIT(28)
#define MT_WFDMA0_GLO_CFG_CLK_GAT_DIS	BIT(30)

/* WFDMA Reset */
#define MT_WFDMA0_RST			(MT_WFDMA0_BASE + 0x100)
#define MT_WFDMA0_RST_LOGIC_RST		BIT(4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST	BIT(5)

/* WFDMA Extended Config */
#define MT_WFDMA0_GLO_CFG_EXT0		(MT_WFDMA0_BASE + 0x2b0)
#define MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN	BIT(16)

/* DMA Scheduler */
#define MT_DMASHDL_SW_CONTROL		0x7c026004
#define MT_DMASHDL_DMASHDL_BYPASS	BIT(0)

/* Interrupts */
#define MT_WFDMA0_HOST_INT_ENA		(MT_WFDMA0_BASE + 0x204)
#define MT_WFDMA0_HOST_INT_STA		(MT_WFDMA0_BASE + 0x200)
#define MT_PCIE_MAC_INT_ENABLE		0x10188
#define MT_PCIE_MAC_INT_STATUS		0x10184

/*
 * Interrupt enable bits for MT_WFDMA0_HOST_INT_ENA
 * These are CRITICAL for DMA to work! Without enabling
 * the appropriate interrupt bits, the DMA engine will not
 * fetch descriptors from our rings.
 *
 * From mt76/mt7925/pci.c and mt76/mt792x_core.c
 */
#define MT_INT_RX_DONE_0		BIT(0)	/* RX Ring 0 (MCU WM) */
#define MT_INT_RX_DONE_1		BIT(1)	/* RX Ring 1 (MCU WM2) */
#define MT_INT_RX_DONE_2		BIT(2)	/* RX Ring 2 (Data) */
#define MT_INT_RX_DONE_3		BIT(3)	/* RX Ring 3 (Data) */
#define MT_INT_TX_DONE_0		BIT(4)	/* TX Ring 0 (Band0 data) */
#define MT_INT_TX_DONE_1		BIT(5)	/* TX Ring 1 */
#define MT_INT_TX_DONE_2		BIT(6)	/* TX Ring 2 */
#define MT_INT_TX_DONE_15		BIT(25)	/* TX Ring 15 (MCU WM) */
#define MT_INT_TX_DONE_16		BIT(26)	/* TX Ring 16 (FWDL) */
#define MT_INT_TX_DONE_17		BIT(27)	/* TX Ring 17 */
#define MT_INT_MCU_CMD			BIT(29)	/* MCU command interrupt */

/* Combined interrupt masks */
#define MT_INT_TX_DONE_FWDL		(MT_INT_TX_DONE_15 | MT_INT_TX_DONE_16)
#define MT_INT_RX_DONE_MCU		(MT_INT_RX_DONE_0 | MT_INT_RX_DONE_1)
#define MT_INT_RX_DONE_ALL		(MT_INT_RX_DONE_0 | MT_INT_RX_DONE_1 | \
					 MT_INT_RX_DONE_2 | MT_INT_RX_DONE_3)
#define MT_INT_TX_DONE_ALL		(MT_INT_TX_DONE_0 | MT_INT_TX_DONE_15 | \
					 MT_INT_TX_DONE_16)

/* Additional MT7925-specific registers for interrupt/DMA priority */
#define MT_WFDMA0_INT_RX_PRI		(MT_WFDMA0_BASE + 0x2c0)
#define MT_WFDMA0_INT_TX_PRI		(MT_WFDMA0_BASE + 0x2c4)
#define MT_UWFDMA0_GLO_CFG_EXT1		(MT_WFDMA0_BASE + 0x2b4)

/* MCU to Host Software Interrupt Enable */
#define MT_MCU2HOST_SW_INT_ENA		(MT_WFDMA0_BASE + 0x1f4)
#define MT_MCU_CMD_WAKE_RX_PCIE		BIT(0)

/* DMA Ring Pointers */
#define MT_WFDMA0_RST_DTX_PTR		(MT_WFDMA0_BASE + 0x228)
#define MT_WFDMA0_RST_DRX_PTR		(MT_WFDMA0_BASE + 0x260)
#define MT_WFDMA0_PRI_DLY_INT_CFG0	(MT_WFDMA0_BASE + 0x238)

/* TX Ring registers */
#define MT_TX_RING_BASE			(MT_WFDMA0_BASE + 0x300)
#define MT_RING_SIZE			0x10

/* RX Ring registers */
#define MT_RX_RING_BASE			(MT_WFDMA0_BASE + 0x500)

/*
 * TX Ring Extended Control Registers - CRITICAL for ring enablement!
 * These MUST be configured BEFORE writing to RING_BASE/CNT registers.
 * Without this, ring register writes may not stick.
 *
 * From mt792x_dma_prefetch() in Linux kernel mt76 driver:
 *   MT7925 uses different offsets than other mt792x chips.
 */
#define MT_WFDMA0_TX_RING0_EXT_CTRL	(MT_WFDMA0_BASE + 0x600)
#define MT_WFDMA0_TX_RING1_EXT_CTRL	(MT_WFDMA0_BASE + 0x604)
#define MT_WFDMA0_TX_RING2_EXT_CTRL	(MT_WFDMA0_BASE + 0x608)
#define MT_WFDMA0_TX_RING3_EXT_CTRL	(MT_WFDMA0_BASE + 0x60c)
#define MT_WFDMA0_TX_RING15_EXT_CTRL	(MT_WFDMA0_BASE + 0x63c)
#define MT_WFDMA0_TX_RING16_EXT_CTRL	(MT_WFDMA0_BASE + 0x640)

/* RX Ring Extended Control Registers */
#define MT_WFDMA0_RX_RING0_EXT_CTRL	(MT_WFDMA0_BASE + 0x680)
#define MT_WFDMA0_RX_RING1_EXT_CTRL	(MT_WFDMA0_BASE + 0x684)
#define MT_WFDMA0_RX_RING2_EXT_CTRL	(MT_WFDMA0_BASE + 0x688)
#define MT_WFDMA0_RX_RING3_EXT_CTRL	(MT_WFDMA0_BASE + 0x68c)

/* PREFETCH macro: (base_ptr << 16) | depth */
#define PREFETCH(base, depth)		(((base) << 16) | (depth))

/*
 * MT7925/MT7927 prefetch configuration from kernel mt792x_dma_prefetch():
 *
 * v0.7.0 FIX: These values were WRONG! The IOMMU faults at 0x0, 0x300, 0x500
 * were because the prefetch buffer bases didn't match kernel values.
 *
 * CORRECT values from Linux kernel mt792x_dma.c for is_mt7925():
 *   RX Ring 0:  PREFETCH(0x0000, 0x4)  - MCU events (was WRONG: 0x0100)
 *   RX Ring 1:  PREFETCH(0x0040, 0x4)  - WM events (was WRONG: 0x0140)
 *   RX Ring 2:  PREFETCH(0x0080, 0x4)  - Data ring (was WRONG: 0x0180)
 *   RX Ring 3:  PREFETCH(0x00c0, 0x4)  - Data ring (was WRONG: 0x01c0)
 *   TX Ring 0:  PREFETCH(0x0100, 0x10) - Data ring (was WRONG: 0x0000/0x4)
 *   TX Ring 1:  PREFETCH(0x0200, 0x10) - Data ring (was WRONG: 0x0040/0x4)
 *   TX Ring 2:  PREFETCH(0x0300, 0x10) - Data ring (was WRONG: 0x0080/0x4)
 *   TX Ring 3:  PREFETCH(0x0400, 0x10) - Data ring (we didn't have this)
 *   TX Ring 15: PREFETCH(0x0500, 0x4)  - MCU WM ring (CORRECT)
 *   TX Ring 16: PREFETCH(0x0540, 0x4)  - Firmware download ring (CORRECT)
 */
#define MT7925_TX_RING0_PREFETCH	PREFETCH(0x0100, 0x10)
#define MT7925_TX_RING1_PREFETCH	PREFETCH(0x0200, 0x10)
#define MT7925_TX_RING2_PREFETCH	PREFETCH(0x0300, 0x10)
#define MT7925_TX_RING3_PREFETCH	PREFETCH(0x0400, 0x10)
#define MT7925_TX_RING15_PREFETCH	PREFETCH(0x0500, 0x4)
#define MT7925_TX_RING16_PREFETCH	PREFETCH(0x0540, 0x4)
#define MT7925_RX_RING0_PREFETCH	PREFETCH(0x0000, 0x4)
#define MT7925_RX_RING1_PREFETCH	PREFETCH(0x0040, 0x4)
#define MT7925_RX_RING2_PREFETCH	PREFETCH(0x0080, 0x4)
#define MT7925_RX_RING3_PREFETCH	PREFETCH(0x00c0, 0x4)

/* Firmware status */
#define MT_CONN_ON_MISC			0x7c0600f0
#define MT_TOP_MISC2_FW_N9_RDY		GENMASK(1, 0)

/* Hardware identification registers (high address, need remap) */
#define MT_HW_CHIPID			0x70010200
#define MT_HW_REV			0x70010204

/* EMI Control - CRITICAL: Must be configured before WFSYS reset! */
#define MT_HW_EMI_CTL			0x18011100
#define MT_HW_EMI_CTL_SLPPROT_EN	BIT(1)

/* Additional debug registers */
#define MT_HIF_REMAP_L1			0x155024
#define MT_HIF_REMAP_L1_MASK		GENMASK(31, 16)
#define MT_HIF_REMAP_L1_OFFSET		GENMASK(15, 0)
#define MT_HIF_REMAP_L1_BASE		0x130000
#define MT_INFRA_CFG_BASE		0xd1000
#define MT_WFDMA_DUMMY_CR		(MT_WFDMA0_BASE + 0x120)
#define MT_MCU_WPDMA0_BASE		0x54000000

/* Remap window size */
#define MT_HIF_REMAP_WINDOW_SIZE	0x10000	/* 64KB window */

/* =============================================================================
 * Constants
 * =============================================================================
 */

#define MT792x_DRV_OWN_RETRY_COUNT	3	/* Reduced for faster debug feedback */
#define MT7927_TX_RING_SIZE		2048
#define MT7927_TX_MCU_RING_SIZE		256
#define MT7927_TX_FWDL_RING_SIZE	128
#define MT7927_RX_MCU_RING_SIZE		512	/* RX ring for MCU events */
#define MT7927_RX_BUF_SIZE		2048	/* Per-descriptor RX buffer */

/* =============================================================================
 * DMA Descriptor
 * =============================================================================
 */

struct mt76_desc {
	__le32 buf0;
	__le32 ctrl;
	__le32 buf1;
	__le32 info;
} __packed __aligned(4);

/*
 * MCU TXD (Transmit Descriptor) for commands
 * This is the 32-byte hardware descriptor prepended to MCU commands
 */
struct mt7927_mcu_txd {
	__le32 txd[8];		/* Hardware descriptor (32 bytes) */
} __packed __aligned(4);

/*
 * MCU command header following the TXD
 * Used for non-firmware-scatter commands
 */
struct mt7927_mcu_hdr {
	__le16 len;		/* Length excluding txd */
	__le16 pq_id;		/* Priority queue ID */
	u8 cid;			/* Command ID */
	u8 pkt_type;		/* Must be 0xa0 (MCU_PKT_ID) */
	u8 set_query;		/* Set/query flag */
	u8 seq;			/* Sequence number */
	u8 rsv0;
	u8 ext_cid;		/* Extended command ID */
	u8 s2d_index;		/* Source-to-destination routing */
	u8 ext_cid_ack;		/* ACK for extended CID */
	__le32 rsv1[5];		/* Reserved */
} __packed;

/*
 * Firmware trailer structure (at end of firmware file)
 */
struct mt7927_fw_trailer {
	u8 chip_id;
	u8 eco_code;
	u8 n_region;
	u8 format_ver;
	u8 format_flag;
	u8 rsv[2];
	char fw_ver[10];
	char build_date[15];
	__le32 crc;
} __packed;

/*
 * Firmware region descriptor (parsed backwards from trailer)
 */
struct mt7927_fw_region {
	__le32 decomp_crc;
	__le32 decomp_len;
	__le32 decomp_blk_sz;
	u8 rsv[4];
	__le32 addr;		/* Target memory address */
	__le32 len;		/* Region length */
	u8 feature_set;
	u8 type;
	u8 rsv1[14];
} __packed;

/*
 * Patch header structure
 */
struct mt7927_patch_hdr {
	char build_date[16];
	char platform[4];
	__be32 hw_sw_ver;
	__be32 patch_ver;
	__be16 checksum;
	__le16 rsv;
	struct {
		__be32 patch_ver;
		__be32 subsys;
		__be32 feature;
		__be32 n_region;
		__be32 crc;
		__le32 rsv[11];
	} desc;
} __packed;

/* Patch region header */
struct mt7927_patch_sec {
	__be32 type;
	__be32 offs;
	__be32 size;
	union {
		__be32 spec[13];
		struct {
			__be32 addr;
			__be32 len;
			__be32 sec_key_idx;
			__be32 align_len;
			__le32 rsv[9];
		} info;
	};
} __packed;

/* Feature flags */
#define FW_FEATURE_NON_DL		BIT(2)
#define FW_FEATURE_OVERRIDE_ADDR	BIT(4)

#define MT_DMA_CTL_SD_LEN0		GENMASK(15, 0)
#define MT_DMA_CTL_LAST_SEC0		BIT(16)
#define MT_DMA_CTL_BURST		BIT(17)
#define MT_DMA_CTL_DMA_DONE		BIT(31)

/* =============================================================================
 * MCU Command Definitions
 * =============================================================================
 */

/* MCU packet type */
#define MT_MCU_PKT_ID			0xa0

/* MCU TXD word 0 fields */
#define MT_TXD0_TX_BYTES		GENMASK(15, 0)
#define MT_TXD0_PKT_FMT			GENMASK(24, 23)
#define MT_TXD0_Q_IDX			GENMASK(31, 25)

/* Packet format types */
#define MT_TX_TYPE_CT			0
#define MT_TX_TYPE_SF			1
#define MT_TX_TYPE_CMD			2
#define MT_TX_TYPE_FW			3

/* Queue indices */
#define MT_TX_MCU_PORT_RX_Q0		0x20	/* MCU command queue */
#define MT_TX_MCU_PORT_RX_FWDL		0x3e	/* Firmware download queue */

/* MCU command IDs */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ	0x01
#define MCU_CMD_FW_START_REQ		0x02
#define MCU_CMD_PATCH_START_REQ		0x05
#define MCU_CMD_PATCH_FINISH_REQ	0x07
#define MCU_CMD_PATCH_SEM_CTRL		0x10
#define MCU_CMD_FW_SCATTER		0xee

/* Patch semaphore operations */
#define PATCH_SEM_GET			0x01
#define PATCH_SEM_RELEASE		0x00

/* Patch semaphore response */
#define PATCH_NOT_DL_SEM_SUCCESS	0x02

/*
 * Patch section type field interpretation:
 * The type field uses specific bits to indicate section properties.
 * Looking at the kernel driver mt76_connac2_load_patch():
 *   - It checks for FW_FEATURE_NON_DL bit to skip non-downloadable sections
 *   - The actual section type encoding varies
 *
 * For MT7925 patches, observed types:
 *   0x30002 = downloadable section (seen in practice)
 */
#define PATCH_SEC_TYPE_MASK		0x3
#define PATCH_SEC_ENC_TYPE_MASK		GENMASK(31, 24)
#define PATCH_SEC_ENC_SCRAMBLE		BIT(24)

/* Download mode flags */
#define DL_MODE_ENCRYPT			BIT(0)
#define DL_MODE_KEY_IDX			GENMASK(2, 1)
#define DL_MODE_RESET_SEC_IV		BIT(3)
#define DL_MODE_WORKING_PDA_CR4		BIT(4)
#define DL_MODE_VALID_RAM_ENTRY		BIT(5)
#define DL_MODE_NEED_RSP		BIT(31)

/* Firmware chunk size */
#define MT7927_FW_CHUNK_SIZE		4096

/* MCU S2D (Source to Destination) routing */
#define MCU_S2D_H2N			0x00	/* Host to WiFi Manager (N9) */
#define MCU_S2D_C2N			0x01	/* WA to WM */
#define MCU_S2D_H2C			0x02	/* Host to WiFi Accelerator */
#define MCU_S2D_H2CN			0x03	/* Host to both */

/* MCU command option flags */
#define MCU_CMD_ACK			BIT(0)
#define MCU_CMD_UNI			BIT(1)
#define MCU_CMD_SET			BIT(2)

/* =============================================================================
 * Device Structure
 * =============================================================================
 */

struct mt7927_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_len;	/* BAR0 length for bounds checking */

	/* TX Ring 16 - Firmware Download (FWDL) */
	struct mt76_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	int tx_ring_size;
	int tx_ring_head;		/* Next descriptor to use */
	int tx_ring_tail;		/* Next descriptor to complete */

	/* TX Ring 15 - MCU Commands (WM) */
	struct mt76_desc *mcu_ring;
	dma_addr_t mcu_ring_dma;
	int mcu_ring_size;
	int mcu_ring_head;

	/* RX Ring 0 - MCU Events */
	struct mt76_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	int rx_ring_size;
	int rx_ring_head;
	void *rx_buf;			/* RX buffer pool */
	dma_addr_t rx_buf_dma;

	/* Firmware buffer */
	void *fw_buf;
	dma_addr_t fw_dma;
	size_t fw_size;

	/* MCU command buffer (for scatter commands) */
	void *mcu_buf;
	dma_addr_t mcu_dma;

	/* State */
	bool aspm_supported;
	u32 chip_rev;
	u32 chip_id;
	u8 mcu_seq;			/* MCU command sequence number */
};

/* =============================================================================
 * Register Access Helpers with Debug Logging and Bounds Checking
 * =============================================================================
 */

/* Safe register read - checks bounds to prevent crashes */
static inline u32 mt7927_rr(struct mt7927_dev *dev, u32 offset)
{
	if (offset >= dev->regs_len) {
		if (debug_regs)
			dev_warn(&dev->pdev->dev,
				 "  READ  [0x%08x] OUT OF BOUNDS (max 0x%llx)\n",
				 offset, (unsigned long long)dev->regs_len);
		return 0xdeadbeef;
	}
	return readl(dev->regs + offset);
}

/* Safe register write - checks bounds to prevent crashes */
static inline void mt7927_wr(struct mt7927_dev *dev, u32 offset, u32 val)
{
	if (offset >= dev->regs_len) {
		if (debug_regs)
			dev_warn(&dev->pdev->dev,
				 "  WRITE [0x%08x] OUT OF BOUNDS (max 0x%llx)\n",
				 offset, (unsigned long long)dev->regs_len);
		return;
	}
	writel(val, dev->regs + offset);
}

/*
 * Remapped register access for high addresses (0x7c0xxxxx range)
 * Uses HIF_REMAP_L1 to create a window into high address space
 */
static u32 mt7927_rr_remap(struct mt7927_dev *dev, u32 addr)
{
	u32 base, offset, val, remap_val;

	/* Calculate base (64KB aligned) and offset within window */
	base = addr & ~(MT_HIF_REMAP_WINDOW_SIZE - 1);
	offset = addr & (MT_HIF_REMAP_WINDOW_SIZE - 1);

	/* Check if HIF_REMAP_L1 itself is accessible */
	if (MT_HIF_REMAP_L1 >= dev->regs_len) {
		dev_warn(&dev->pdev->dev,
			 "  REMAP: Cannot access HIF_REMAP_L1 (0x%x >= 0x%llx)\n",
			 MT_HIF_REMAP_L1, (unsigned long long)dev->regs_len);
		return 0xdeadbeef;
	}

	/* Program the remap register */
	remap_val = FIELD_PREP(MT_HIF_REMAP_L1_MASK, base >> 16);
	writel(remap_val, dev->regs + MT_HIF_REMAP_L1);

	/* Ensure write completes */
	(void)readl(dev->regs + MT_HIF_REMAP_L1);

	/* Read through the remap window */
	if (MT_HIF_REMAP_L1_BASE + offset >= dev->regs_len) {
		dev_warn(&dev->pdev->dev,
			 "  REMAP: Window offset 0x%x out of range\n",
			 MT_HIF_REMAP_L1_BASE + offset);
		return 0xdeadbeef;
	}

	val = readl(dev->regs + MT_HIF_REMAP_L1_BASE + offset);

	if (debug_regs)
		dev_info(&dev->pdev->dev,
			 "  REMAP READ [0x%08x] = 0x%08x (window: base=0x%x, off=0x%x)\n",
			 addr, val, base, offset);

	return val;
}

static void mt7927_wr_remap(struct mt7927_dev *dev, u32 addr, u32 val)
{
	u32 base, offset, remap_val;

	/* Calculate base (64KB aligned) and offset within window */
	base = addr & ~(MT_HIF_REMAP_WINDOW_SIZE - 1);
	offset = addr & (MT_HIF_REMAP_WINDOW_SIZE - 1);

	/* Check if HIF_REMAP_L1 itself is accessible */
	if (MT_HIF_REMAP_L1 >= dev->regs_len) {
		dev_warn(&dev->pdev->dev,
			 "  REMAP: Cannot access HIF_REMAP_L1 (0x%x >= 0x%llx)\n",
			 MT_HIF_REMAP_L1, (unsigned long long)dev->regs_len);
		return;
	}

	/* Program the remap register */
	remap_val = FIELD_PREP(MT_HIF_REMAP_L1_MASK, base >> 16);
	writel(remap_val, dev->regs + MT_HIF_REMAP_L1);

	/* Ensure remap write completes */
	(void)readl(dev->regs + MT_HIF_REMAP_L1);

	/* Write through the remap window */
	if (MT_HIF_REMAP_L1_BASE + offset >= dev->regs_len) {
		dev_warn(&dev->pdev->dev,
			 "  REMAP: Window offset 0x%x out of range\n",
			 MT_HIF_REMAP_L1_BASE + offset);
		return;
	}

	writel(val, dev->regs + MT_HIF_REMAP_L1_BASE + offset);

	if (debug_regs)
		dev_info(&dev->pdev->dev,
			 "  REMAP WRITE [0x%08x] = 0x%08x (window: base=0x%x, off=0x%x)\n",
			 addr, val, base, offset);
}

/* Debug read - logs the value */
static u32 mt7927_rr_debug(struct mt7927_dev *dev, u32 offset, const char *name)
{
	u32 val = mt7927_rr(dev, offset);
	if (debug_regs)
		dev_info(&dev->pdev->dev, "  READ  [0x%08x] %s = 0x%08x\n",
			 offset, name, val);
	return val;
}

/* Debug write - logs before and after */
static void mt7927_wr_debug(struct mt7927_dev *dev, u32 offset, u32 val,
			    const char *name)
{
	u32 before = 0, after;

	if (debug_regs)
		before = mt7927_rr(dev, offset);

	mt7927_wr(dev, offset, val);

	if (debug_regs) {
		after = mt7927_rr(dev, offset);
		dev_info(&dev->pdev->dev,
			 "  WRITE [0x%08x] %s: 0x%08x -> write 0x%08x -> read 0x%08x %s\n",
			 offset, name, before, val, after,
			 (after == val) ? "OK" : "MISMATCH!");
	}
}

static inline void mt7927_set(struct mt7927_dev *dev, u32 offset, u32 val)
{
	mt7927_wr(dev, offset, mt7927_rr(dev, offset) | val);
}

static inline void mt7927_clear(struct mt7927_dev *dev, u32 offset, u32 val)
{
	mt7927_wr(dev, offset, mt7927_rr(dev, offset) & ~val);
}

static inline void mt7927_rmw(struct mt7927_dev *dev, u32 offset,
			      u32 mask, u32 val)
{
	u32 cur = mt7927_rr(dev, offset);
	mt7927_wr(dev, offset, (cur & ~mask) | val);
}

static bool mt7927_poll(struct mt7927_dev *dev, u32 offset, u32 mask,
			u32 val, int timeout_ms)
{
	u32 cur;
	int i;

	for (i = 0; i < timeout_ms; i++) {
		cur = mt7927_rr(dev, offset);
		if ((cur & mask) == val)
			return true;
		usleep_range(1000, 2000);
	}

	if (debug_regs)
		dev_warn(&dev->pdev->dev,
			 "  POLL TIMEOUT [0x%08x] mask=0x%08x expected=0x%08x got=0x%08x\n",
			 offset, mask, val, cur);

	return false;
}

/* =============================================================================
 * Debug Dump Functions
 * =============================================================================
 */

static void mt7927_dump_pci_state(struct mt7927_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	u16 cmd, status;
	u32 bar0, bar2;

	dev_info(&pdev->dev, "=== PCI State Dump ===\n");

	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	pci_read_config_word(pdev, PCI_STATUS, &status);
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &bar0);
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_2, &bar2);

	dev_info(&pdev->dev, "  PCI Command: 0x%04x (MEM=%d, MASTER=%d)\n",
		 cmd, !!(cmd & PCI_COMMAND_MEMORY), !!(cmd & PCI_COMMAND_MASTER));
	dev_info(&pdev->dev, "  PCI Status:  0x%04x\n", status);
	dev_info(&pdev->dev, "  BAR0: 0x%08x (len=%llu KB), BAR2: 0x%08x\n",
		 bar0, (unsigned long long)dev->regs_len / 1024, bar2);
	dev_info(&pdev->dev, "  Subsystem: %04x:%04x\n",
		 pdev->subsystem_vendor, pdev->subsystem_device);

	/* Critical info about register access */
	dev_info(&pdev->dev, "  MMIO range: 0x00000000 - 0x%08llx\n",
		 (unsigned long long)dev->regs_len - 1);
	if (dev->regs_len < 0x7c100000) {
		dev_warn(&pdev->dev,
			 "  WARNING: BAR0 too small for high registers!\n");
		dev_warn(&pdev->dev,
			 "  Registers like 0x7c060010 are OUT OF RANGE\n");
	}
}

static void mt7927_dump_critical_regs(struct mt7927_dev *dev)
{
	dev_info(&dev->pdev->dev, "=== Critical Register Dump ===\n");
	dev_info(&dev->pdev->dev, "  (BAR0 size: 0x%llx, skipping out-of-range)\n",
		 (unsigned long long)dev->regs_len);

	/* Only read registers that are within BAR0 range */
	/* These are the low-offset DMA registers that should always work */
	mt7927_rr_debug(dev, MT_PCIE_MAC_INT_ENABLE, "MT_PCIE_MAC_INT_ENABLE");
	mt7927_rr_debug(dev, MT_PCIE_MAC_INT_STATUS, "MT_PCIE_MAC_INT_STATUS");

	/* DMA status - these are in the 0xd4xxx range */
	mt7927_rr_debug(dev, MT_WFDMA0_GLO_CFG, "MT_WFDMA0_GLO_CFG");
	mt7927_rr_debug(dev, MT_WFDMA0_RST, "MT_WFDMA0_RST");
	mt7927_rr_debug(dev, MT_WFDMA0_GLO_CFG_EXT0, "MT_WFDMA0_GLO_CFG_EXT0");

	/* Interrupts */
	mt7927_rr_debug(dev, MT_WFDMA0_HOST_INT_ENA, "MT_WFDMA0_HOST_INT_ENA");
	mt7927_rr_debug(dev, MT_WFDMA0_HOST_INT_STA, "MT_WFDMA0_HOST_INT_STA");

	/* Remap register - check if accessible */
	mt7927_rr_debug(dev, MT_HIF_REMAP_L1, "MT_HIF_REMAP_L1");

	/* These high-address registers (0x7c0xxxxx) need remapping */
	if (dev->regs_len > MT_CONN_ON_LPCTL) {
		mt7927_rr_debug(dev, MT_CONN_ON_LPCTL, "MT_CONN_ON_LPCTL");
		mt7927_rr_debug(dev, MT_WFSYS_SW_RST_B, "MT_WFSYS_SW_RST_B");
		mt7927_rr_debug(dev, MT_CONN_ON_MISC, "MT_CONN_ON_MISC");
	} else {
		dev_info(&dev->pdev->dev,
			 "  High registers (0x7c0xxxxx) need remapping\n");
	}
}

/* =============================================================================
 * Power Management Handoff
 * =============================================================================
 */

/* Helper to poll a remapped register - silent version to avoid log spam */
static bool mt7927_poll_remap_quiet(struct mt7927_dev *dev, u32 addr, u32 mask,
				    u32 val, int timeout_ms)
{
	u32 cur;
	int i;
	bool saved_debug = debug_regs;

	/* Temporarily disable debug to avoid flooding logs during polling */
	debug_regs = false;

	for (i = 0; i < timeout_ms; i++) {
		cur = mt7927_rr_remap(dev, addr);
		if ((cur & mask) == val) {
			debug_regs = saved_debug;
			return true;
		}
		usleep_range(1000, 2000);
	}

	debug_regs = saved_debug;

	if (debug_regs)
		dev_warn(&dev->pdev->dev,
			 "  POLL TIMEOUT [0x%08x] mask=0x%08x expected=0x%08x got=0x%08x after %dms\n",
			 addr, mask, val, cur, timeout_ms);

	return false;
}

static int mt7927_mcu_fw_pmctrl(struct mt7927_dev *dev)
{
	u32 addr = MT_CONN_ON_LPCTL;
	int i;

	dev_info(&dev->pdev->dev, "=== FW Power Control (give to firmware) ===\n");
	dev_info(&dev->pdev->dev, "  Using remapped register access for 0x7c0xxxxx\n");

	/* Use remapped access for high addresses - quick poll (10ms) for debug */
	for (i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
		u32 val_before = mt7927_rr_remap(dev, addr);

		dev_info(&dev->pdev->dev, "  [%d] Writing SET_OWN to 0x%08x...\n", i + 1, addr);
		mt7927_wr_remap(dev, addr, PCIE_LPCR_HOST_SET_OWN);

		if (mt7927_poll_remap_quiet(dev, addr, PCIE_LPCR_HOST_OWN_SYNC,
				      PCIE_LPCR_HOST_OWN_SYNC, 10)) {
			dev_info(&dev->pdev->dev,
				 "  FW ownership acquired (attempt %d, addr=0x%08x)\n",
				 i + 1, addr);
			dev_info(&dev->pdev->dev,
				 "  LPCTL: 0x%08x -> 0x%08x\n",
				 val_before, mt7927_rr_remap(dev, addr));
			return 0;
		}

		dev_info(&dev->pdev->dev, "  [%d] Timeout at addr 0x%08x\n", i + 1, addr);
	}

	/* Try alternative address */
	dev_info(&dev->pdev->dev, "  Trying alternative LPCTL address 0x%08x...\n",
		 MT_CONN_ON_LPCTL_ALT);
	addr = MT_CONN_ON_LPCTL_ALT;

	for (i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
		mt7927_wr_remap(dev, addr, PCIE_LPCR_HOST_SET_OWN);

		if (mt7927_poll_remap_quiet(dev, addr, PCIE_LPCR_HOST_OWN_SYNC,
				      PCIE_LPCR_HOST_OWN_SYNC, 10)) {
			dev_info(&dev->pdev->dev,
				 "  FW ownership acquired via ALT address 0x%08x\n", addr);
			return 0;
		}
	}

	dev_warn(&dev->pdev->dev, "  FW ownership handoff failed (non-fatal)\n");
	return -ETIMEDOUT;
}

static int mt7927_mcu_drv_pmctrl(struct mt7927_dev *dev)
{
	u32 addr = MT_CONN_ON_LPCTL;
	int i;

	dev_info(&dev->pdev->dev, "=== Driver Power Control (take ownership) ===\n");
	dev_info(&dev->pdev->dev, "  Using remapped register access for 0x7c0xxxxx\n");

	/* Quick poll (10ms) for debug */
	for (i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
		u32 val_before = mt7927_rr_remap(dev, addr);

		dev_info(&dev->pdev->dev, "  [%d] Writing CLR_OWN to 0x%08x...\n", i + 1, addr);
		mt7927_wr_remap(dev, addr, PCIE_LPCR_HOST_CLR_OWN);

		/* Critical delay for ASPM */
		if (dev->aspm_supported || disable_aspm)
			usleep_range(2000, 3000);

		if (mt7927_poll_remap_quiet(dev, addr, PCIE_LPCR_HOST_OWN_SYNC, 0, 10)) {
			dev_info(&dev->pdev->dev,
				 "  Driver ownership acquired (attempt %d)\n", i + 1);
			dev_info(&dev->pdev->dev,
				 "  LPCTL: 0x%08x -> 0x%08x\n",
				 val_before, mt7927_rr_remap(dev, addr));
			return 0;
		}

		dev_info(&dev->pdev->dev, "  [%d] Timeout, LPCTL=0x%08x\n",
			 i + 1, mt7927_rr_remap(dev, addr));
	}

	/* Try alternative address */
	dev_info(&dev->pdev->dev, "  Trying alternative LPCTL address 0x%08x...\n",
		 MT_CONN_ON_LPCTL_ALT);
	addr = MT_CONN_ON_LPCTL_ALT;

	for (i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
		mt7927_wr_remap(dev, addr, PCIE_LPCR_HOST_CLR_OWN);

		if (dev->aspm_supported)
			usleep_range(2000, 3000);

		if (mt7927_poll_remap_quiet(dev, addr, PCIE_LPCR_HOST_OWN_SYNC, 0, 10)) {
			dev_info(&dev->pdev->dev,
				 "  Driver ownership via ALT address 0x%08x\n", addr);
			return 0;
		}
	}

	dev_err(&dev->pdev->dev, "  Driver ownership FAILED\n");
	return -ETIMEDOUT;
}

/* =============================================================================
 * WFSYS Reset
 * =============================================================================
 */

/* Helper for remapped set/clear operations */
static inline void mt7927_set_remap(struct mt7927_dev *dev, u32 addr, u32 val)
{
	mt7927_wr_remap(dev, addr, mt7927_rr_remap(dev, addr) | val);
}

static inline void mt7927_clear_remap(struct mt7927_dev *dev, u32 addr, u32 val)
{
	mt7927_wr_remap(dev, addr, mt7927_rr_remap(dev, addr) & ~val);
}

static int mt7927_wfsys_reset(struct mt7927_dev *dev)
{
	u32 addr = try_alt_reset ? MT_WFSYS_SW_RST_B_ALT : MT_WFSYS_SW_RST_B;
	u32 val_before, val_after;

	dev_info(&dev->pdev->dev, "=== WFSYS Reset (addr=0x%08x) ===\n", addr);
	dev_info(&dev->pdev->dev, "  Using remapped register access\n");

	val_before = mt7927_rr_remap(dev, addr);
	dev_info(&dev->pdev->dev, "  Before reset: 0x%08x\n", val_before);

	/* Check if already in good state */
	if (val_before & WFSYS_SW_INIT_DONE) {
		dev_info(&dev->pdev->dev, "  INIT_DONE already set, still resetting...\n");
	}

	/* Assert reset - clear WFSYS_SW_RST_B */
	dev_info(&dev->pdev->dev, "  Asserting reset (clearing bit 0)...\n");
	mt7927_clear_remap(dev, addr, WFSYS_SW_RST_B);

	val_after = mt7927_rr_remap(dev, addr);
	dev_info(&dev->pdev->dev, "  After clear: 0x%08x\n", val_after);

	/* MANDATORY 50ms delay */
	dev_info(&dev->pdev->dev, "  Waiting 50ms...\n");
	msleep(50);

	/* Deassert reset - set WFSYS_SW_RST_B */
	dev_info(&dev->pdev->dev, "  Deasserting reset (setting bit 0)...\n");
	mt7927_set_remap(dev, addr, WFSYS_SW_RST_B);

	val_after = mt7927_rr_remap(dev, addr);
	dev_info(&dev->pdev->dev, "  After set: 0x%08x\n", val_after);

	/* Poll for initialization complete (100ms for debug, prod should be 500ms) */
	dev_info(&dev->pdev->dev, "  Polling for INIT_DONE (bit 4), timeout 100ms...\n");

	if (!mt7927_poll_remap_quiet(dev, addr, WFSYS_SW_INIT_DONE, WFSYS_SW_INIT_DONE, 100)) {
		val_after = mt7927_rr_remap(dev, addr);
		dev_err(&dev->pdev->dev,
			"  WFSYS reset TIMEOUT! Final value: 0x%08x\n", val_after);

		/* Try alternative address if primary failed */
		if (!try_alt_reset) {
			dev_info(&dev->pdev->dev,
				 "  Trying alternative reset address 0x%08x...\n",
				 MT_WFSYS_SW_RST_B_ALT);

			addr = MT_WFSYS_SW_RST_B_ALT;
			mt7927_clear_remap(dev, addr, WFSYS_SW_RST_B);
			msleep(50);
			mt7927_set_remap(dev, addr, WFSYS_SW_RST_B);

			if (mt7927_poll_remap_quiet(dev, addr, WFSYS_SW_INIT_DONE,
					      WFSYS_SW_INIT_DONE, 100)) {
				dev_info(&dev->pdev->dev,
					 "  Alternative reset SUCCEEDED!\n");
				return 0;
			}
		}

		dev_info(&dev->pdev->dev, "  Continuing despite reset failure for debugging...\n");
		return -ETIMEDOUT;
	}

	val_after = mt7927_rr_remap(dev, addr);
	dev_info(&dev->pdev->dev, "  WFSYS reset COMPLETE: 0x%08x\n", val_after);

	/*
	 * NOTE: Don't set CLK_GAT_DIS here - it will be cleared by
	 * mt7927_dma_disable()'s LOGIC_RST anyway. We set it properly
	 * in mt7927_dma_init() AFTER dma_disable() completes.
	 */

	return 0;
}

/* =============================================================================
 * DMA Initialization
 * =============================================================================
 */

static int mt7927_dma_disable(struct mt7927_dev *dev, bool force)
{
	u32 val_before, val_after;

	dev_info(&dev->pdev->dev, "=== DMA Disable (force=%d) ===\n", force);

	val_before = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG before: 0x%08x\n", val_before);

	/* Clear DMA enable and config bits */
	mt7927_clear(dev, MT_WFDMA0_GLO_CFG,
		     MT_WFDMA0_GLO_CFG_TX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_RX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
		     MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
		     MT_WFDMA0_GLO_CFG_OMIT_TX_INFO);

	val_after = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG after clear: 0x%08x\n", val_after);

	/* Wait for DMA busy flags to clear */
	dev_info(&dev->pdev->dev, "  Waiting for DMA busy to clear...\n");
	if (!mt7927_poll(dev, MT_WFDMA0_GLO_CFG,
			 MT_WFDMA0_GLO_CFG_TX_DMA_BUSY |
			 MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
			 0, 100)) {
		dev_warn(&dev->pdev->dev, "  DMA busy timeout (non-fatal)\n");
	}

	/* Disable DMASHDL (scheduler) */
	dev_info(&dev->pdev->dev, "  Disabling DMASHDL...\n");
	mt7927_wr_debug(dev, MT_WFDMA0_GLO_CFG_EXT0,
			mt7927_rr(dev, MT_WFDMA0_GLO_CFG_EXT0) &
			~MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN,
			"GLO_CFG_EXT0");

	/* Set DMASHDL bypass - register at 0x7c026004 needs remap */
	dev_info(&dev->pdev->dev, "  Setting DMASHDL bypass via remap...\n");
	{
		u32 dmashdl_val = mt7927_rr_remap(dev, MT_DMASHDL_SW_CONTROL);
		mt7927_wr_remap(dev, MT_DMASHDL_SW_CONTROL,
				dmashdl_val | MT_DMASHDL_DMASHDL_BYPASS);
		dev_info(&dev->pdev->dev, "  DMASHDL_SW_CONTROL: 0x%08x -> 0x%08x\n",
			 dmashdl_val, mt7927_rr_remap(dev, MT_DMASHDL_SW_CONTROL));
	}

	if (force) {
		dev_info(&dev->pdev->dev, "  Force reset sequence...\n");

		/*
		 * CRITICAL: The kernel driver does clear -> set and LEAVES
		 * the RST bits SET! It does NOT do a final clear.
		 * The bits being set seems to be required for ring register
		 * access to work.
		 */
		mt7927_clear(dev, MT_WFDMA0_RST,
			     MT_WFDMA0_RST_DMASHDL_ALL_RST |
			     MT_WFDMA0_RST_LOGIC_RST);

		mt7927_set(dev, MT_WFDMA0_RST,
			   MT_WFDMA0_RST_DMASHDL_ALL_RST |
			   MT_WFDMA0_RST_LOGIC_RST);

		/* NOTE: Kernel does NOT clear these bits again! */

		dev_info(&dev->pdev->dev, "  WFDMA0_RST: 0x%08x (bits left SET)\n",
			 mt7927_rr(dev, MT_WFDMA0_RST));
	}

	return 0;
}

static int mt7927_dma_enable(struct mt7927_dev *dev)
{
	u32 val_before, val_after, expected, int_ena;

	dev_info(&dev->pdev->dev, "=== DMA Enable ===\n");

	/* Reset DMA pointers */
	dev_info(&dev->pdev->dev, "  Resetting DMA pointers...\n");
	mt7927_wr(dev, MT_WFDMA0_RST_DTX_PTR, ~0);
	mt7927_wr(dev, MT_WFDMA0_RST_DRX_PTR, ~0);

	/* Clear delay interrupt config */
	mt7927_wr(dev, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	val_before = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG before enable: 0x%08x\n", val_before);

	/* Set global configuration flags */
	expected = MT_WFDMA0_GLO_CFG_TX_WB_DDONE |
		   MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
		   MT_WFDMA0_GLO_CFG_CLK_GAT_DIS |
		   MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
		   MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
		   MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
		   FIELD_PREP(MT_WFDMA0_GLO_CFG_DMA_SIZE, 3);

	dev_info(&dev->pdev->dev, "  Setting config flags: 0x%08x\n", expected);
	mt7927_set(dev, MT_WFDMA0_GLO_CFG, expected);

	val_after = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG after config: 0x%08x\n", val_after);

	/* Enable DMA engines */
	dev_info(&dev->pdev->dev, "  Enabling TX/RX DMA...\n");
	mt7927_set(dev, MT_WFDMA0_GLO_CFG,
		   MT_WFDMA0_GLO_CFG_TX_DMA_EN |
		   MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	val_after = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG final: 0x%08x\n", val_after);

	/* Verify bits stuck */
	if (!(val_after & MT_WFDMA0_GLO_CFG_TX_DMA_EN)) {
		dev_err(&dev->pdev->dev, "  ERROR: TX_DMA_EN did not stick!\n");
	}
	if (!(val_after & MT_WFDMA0_GLO_CFG_RX_DMA_EN)) {
		dev_err(&dev->pdev->dev, "  ERROR: RX_DMA_EN did not stick!\n");
	}

	/*
	 * v0.6.0: CRITICAL - Enable interrupts for DMA rings!
	 *
	 * The kernel driver research revealed that DMA descriptors are NOT
	 * fetched unless the corresponding interrupt enable bits are set.
	 * This was the root cause of dma_idx stuck at 0!
	 *
	 * From mt7925/pci.c mt7925_pci_probe() and mt792x_dma_enable():
	 *   1. MT7925-specific: Set priority registers
	 *   2. Enable interrupts for TX rings 15/16 and RX rings
	 *   3. Enable MCU wake interrupt
	 */
	dev_info(&dev->pdev->dev, "  === v0.6.0: Enabling DMA Interrupts ===\n");

	/* Set MT7925-specific extended config (from kernel driver) */
	dev_info(&dev->pdev->dev, "  Setting UWFDMA0_GLO_CFG_EXT1 bit 28...\n");
	mt7927_rmw(dev, MT_UWFDMA0_GLO_CFG_EXT1, BIT(28), BIT(28));
	dev_info(&dev->pdev->dev, "  UWFDMA0_GLO_CFG_EXT1: 0x%08x\n",
		 mt7927_rr(dev, MT_UWFDMA0_GLO_CFG_EXT1));

	/* Set RX and TX interrupt priority (from mt792x_dma_enable for MT7925) */
	dev_info(&dev->pdev->dev, "  Setting interrupt priorities...\n");
	mt7927_set(dev, MT_WFDMA0_INT_RX_PRI, 0x0F00);
	mt7927_set(dev, MT_WFDMA0_INT_TX_PRI, 0x7F00);
	dev_info(&dev->pdev->dev, "  INT_RX_PRI: 0x%08x, INT_TX_PRI: 0x%08x\n",
		 mt7927_rr(dev, MT_WFDMA0_INT_RX_PRI),
		 mt7927_rr(dev, MT_WFDMA0_INT_TX_PRI));

	/*
	 * Enable interrupts for firmware download rings.
	 * This is the KEY fix - without these bits set, the hardware
	 * DMA engine will NOT fetch descriptors from our rings!
	 *
	 * Bits to enable:
	 *   - BIT(0)  = RX Ring 0 (MCU WM events)
	 *   - BIT(25) = TX Ring 15 (MCU commands)
	 *   - BIT(26) = TX Ring 16 (FWDL scatter)
	 *   - BIT(29) = MCU command interrupt
	 */
	int_ena = MT_INT_RX_DONE_0 |		/* RX Ring 0 for MCU responses */
		  MT_INT_TX_DONE_15 |		/* TX Ring 15 for MCU commands */
		  MT_INT_TX_DONE_16 |		/* TX Ring 16 for FW scatter */
		  MT_INT_MCU_CMD;		/* MCU command processing */

	dev_info(&dev->pdev->dev, "  Enabling HOST_INT_ENA: 0x%08x\n", int_ena);
	dev_info(&dev->pdev->dev, "    RX_DONE_0=%d TX_DONE_15=%d TX_DONE_16=%d MCU_CMD=%d\n",
		 !!(int_ena & MT_INT_RX_DONE_0),
		 !!(int_ena & MT_INT_TX_DONE_15),
		 !!(int_ena & MT_INT_TX_DONE_16),
		 !!(int_ena & MT_INT_MCU_CMD));

	mt7927_wr(dev, MT_WFDMA0_HOST_INT_ENA, int_ena);
	val_after = mt7927_rr(dev, MT_WFDMA0_HOST_INT_ENA);
	dev_info(&dev->pdev->dev, "  HOST_INT_ENA readback: 0x%08x %s\n",
		 val_after, (val_after == int_ena) ? "OK" : "MISMATCH!");

	/* Enable MCU wake on RX */
	dev_info(&dev->pdev->dev, "  Enabling MCU2HOST_SW_INT_ENA...\n");
	mt7927_set(dev, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);
	dev_info(&dev->pdev->dev, "  MCU2HOST_SW_INT_ENA: 0x%08x\n",
		 mt7927_rr(dev, MT_MCU2HOST_SW_INT_ENA));

	return 0;
}

/*
 * mt7927_dma_prefetch - Configure DMA ring prefetch registers
 *
 * This is CRITICAL for MT7925/MT7927! The ring extended control registers
 * MUST be configured BEFORE writing to the actual ring BASE/CNT registers.
 * Without this step, ring register writes will not persist.
 *
 * Based on mt792x_dma_prefetch() in Linux kernel mt76 driver.
 */
static void mt7927_dma_prefetch(struct mt7927_dev *dev)
{
	dev_info(&dev->pdev->dev, "=== DMA Prefetch Configuration (v0.7.0 FIXED) ===\n");

	/*
	 * v0.7.0: CRITICAL FIX - Use CORRECT prefetch values from kernel!
	 *
	 * The previous values were causing IOMMU faults at addresses like
	 * 0x0, 0x300, 0x500 because the prefetch buffer layout was wrong.
	 *
	 * Kernel order: RX rings first (base 0x0000-0x00c0), then TX rings
	 * (base 0x0100-0x0540). Depth is 0x4 for MCU rings, 0x10 for data rings.
	 */

	/* RX Rings FIRST - bases at 0x0000, 0x0040, 0x0080, 0x00c0 */
	dev_info(&dev->pdev->dev, "  RX Ring 0 (MCU Events) = 0x%08x...\n",
		 MT7925_RX_RING0_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_RX_RING0_EXT_CTRL,
			MT7925_RX_RING0_PREFETCH, "RX_RING0_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  RX Ring 1 (WM Events) = 0x%08x...\n",
		 MT7925_RX_RING1_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_RX_RING1_EXT_CTRL,
			MT7925_RX_RING1_PREFETCH, "RX_RING1_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  RX Ring 2 (Data) = 0x%08x...\n",
		 MT7925_RX_RING2_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_RX_RING2_EXT_CTRL,
			MT7925_RX_RING2_PREFETCH, "RX_RING2_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  RX Ring 3 (Data) = 0x%08x...\n",
		 MT7925_RX_RING3_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_RX_RING3_EXT_CTRL,
			MT7925_RX_RING3_PREFETCH, "RX_RING3_EXT_CTRL");

	/* TX Rings - bases at 0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0540 */
	dev_info(&dev->pdev->dev, "  TX Ring 0 (Data) = 0x%08x...\n",
		 MT7925_TX_RING0_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_TX_RING0_EXT_CTRL,
			MT7925_TX_RING0_PREFETCH, "TX_RING0_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  TX Ring 1 (Data) = 0x%08x...\n",
		 MT7925_TX_RING1_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_TX_RING1_EXT_CTRL,
			MT7925_TX_RING1_PREFETCH, "TX_RING1_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  TX Ring 2 (Data) = 0x%08x...\n",
		 MT7925_TX_RING2_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_TX_RING2_EXT_CTRL,
			MT7925_TX_RING2_PREFETCH, "TX_RING2_EXT_CTRL");

	/* TX Ring 3 - we didn't have this before! */
	dev_info(&dev->pdev->dev, "  TX Ring 3 (Data) = 0x%08x...\n",
		 MT7925_TX_RING3_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_TX_RING3_EXT_CTRL,
			MT7925_TX_RING3_PREFETCH, "TX_RING3_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  TX Ring 15 (MCU WM) = 0x%08x...\n",
		 MT7925_TX_RING15_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_TX_RING15_EXT_CTRL,
			MT7925_TX_RING15_PREFETCH, "TX_RING15_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  TX Ring 16 (FWDL) = 0x%08x...\n",
		 MT7925_TX_RING16_PREFETCH);
	mt7927_wr_debug(dev, MT_WFDMA0_TX_RING16_EXT_CTRL,
			MT7925_TX_RING16_PREFETCH, "TX_RING16_EXT_CTRL");

	dev_info(&dev->pdev->dev, "  DMA prefetch configuration complete\n");
}

static int mt7927_dma_init(struct mt7927_dev *dev)
{
	int ret;
	u32 val;

	dev_info(&dev->pdev->dev, "=== DMA Initialization ===\n");

	/* First disable DMA with force reset */
	ret = mt7927_dma_disable(dev, true);
	if (ret)
		return ret;

	/*
	 * CRITICAL: Disable clock gating IMMEDIATELY after dma_disable!
	 * The LOGIC_RST in dma_disable() resets WFDMA to defaults, which has
	 * clock gating ENABLED. We must disable it BEFORE any ring register
	 * access (including prefetch config).
	 *
	 * Without this, ALL ring registers (EXT_CTRL, BASE, CNT) will be
	 * inaccessible and writes will silently fail (read back as old value).
	 */
	dev_info(&dev->pdev->dev, "  Disabling clock gating after DMA reset...\n");
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG after reset: 0x%08x\n", val);
	mt7927_set(dev, MT_WFDMA0_GLO_CFG, MT_WFDMA0_GLO_CFG_CLK_GAT_DIS);
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG after CLK_GAT_DIS: 0x%08x\n", val);

	/*
	 * v0.4.2: The kernel driver sets additional config flags BEFORE prefetch.
	 * Specifically CSR_DISP_BASE_PTR_CHAIN_EN which enables ring chaining.
	 * This might be required for ring registers to be writable.
	 */
	dev_info(&dev->pdev->dev, "  Setting CSR_DISP_BASE_PTR_CHAIN_EN before prefetch...\n");
	mt7927_set(dev, MT_WFDMA0_GLO_CFG, MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN);
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "  GLO_CFG after CHAIN_EN: 0x%08x\n", val);

	/*
	 * Now configure DMA prefetch registers.
	 * This must happen BEFORE we try to write to ring BASE/CNT registers.
	 */
	mt7927_dma_prefetch(dev);

	/* Allocate TX descriptor ring */
	dev->tx_ring_size = MT7927_TX_FWDL_RING_SIZE;
	dev->tx_ring = dma_alloc_coherent(&dev->pdev->dev,
					  dev->tx_ring_size * sizeof(struct mt76_desc),
					  &dev->tx_ring_dma,
					  GFP_KERNEL);
	if (!dev->tx_ring) {
		dev_err(&dev->pdev->dev, "  Failed to allocate TX ring\n");
		return -ENOMEM;
	}

	memset(dev->tx_ring, 0, dev->tx_ring_size * sizeof(struct mt76_desc));

	dev_info(&dev->pdev->dev, "  TX ring allocated: %d descriptors at %pad\n",
		 dev->tx_ring_size, &dev->tx_ring_dma);

	/*
	 * Configure FWDL ring (ring 16)
	 * NOTE: Prefetch was already configured in mt7927_dma_prefetch() above.
	 * Now the ring BASE/CNT registers should accept writes.
	 *
	 * CRITICAL: We must verify the BASE address is actually written!
	 * If it reads back as 0, the DMA engine will try to fetch from
	 * address 0x0, causing IOMMU page faults.
	 */
	dev_info(&dev->pdev->dev, "  Configuring FWDL ring (ring 16)...\n");
	dev_info(&dev->pdev->dev, "  Ring DMA address: 0x%08x (phys)\n",
		 lower_32_bits(dev->tx_ring_dma));

	/* Write BASE register */
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE,
			lower_32_bits(dev->tx_ring_dma), "RING16_BASE");

	/* Verify the BASE register accepted our write */
	{
		u32 base_readback = mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE);
		if (base_readback != lower_32_bits(dev->tx_ring_dma)) {
			dev_err(&dev->pdev->dev,
				"  CRITICAL: Ring BASE not writable! Wrote 0x%08x, read 0x%08x\n",
				lower_32_bits(dev->tx_ring_dma), base_readback);
			dev_err(&dev->pdev->dev,
				"  DMA will fail - device will try to fetch from address 0x%08x!\n",
				base_readback);

			/*
			 * Try an alternative approach: maybe we need to disable DMA first
			 * before writing ring registers, then re-enable.
			 */
			dev_info(&dev->pdev->dev, "  Trying workaround: disable DMA, write, re-enable...\n");
			mt7927_clear(dev, MT_WFDMA0_GLO_CFG,
				     MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
			udelay(100);

			/* Try writing again */
			mt7927_wr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE,
				  lower_32_bits(dev->tx_ring_dma));
			base_readback = mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE);
			dev_info(&dev->pdev->dev, "  After workaround: BASE = 0x%08x\n",
				 base_readback);
		}
	}

	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x04,
			dev->tx_ring_size, "RING16_CNT");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x08,
			0, "RING16_CIDX");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x0c,
			0, "RING16_DIDX");

	/*
	 * v0.5.0: Also configure Ring 15 (MCU commands) and RX Ring 0 (MCU events)
	 * These are REQUIRED for the ROM bootloader to process MCU commands
	 * like PATCH_SEM_CONTROL and TARGET_ADDRESS_LEN_REQ.
	 *
	 * Without these, the ROM bootloader ignores FW_SCATTER data on Ring 16!
	 */

	/* Allocate MCU command ring (TX Ring 15) */
	dev->mcu_ring_size = MT7927_TX_MCU_RING_SIZE;
	dev->mcu_ring = dma_alloc_coherent(&dev->pdev->dev,
					   dev->mcu_ring_size * sizeof(struct mt76_desc),
					   &dev->mcu_ring_dma,
					   GFP_KERNEL);
	if (!dev->mcu_ring) {
		dev_err(&dev->pdev->dev, "  Failed to allocate MCU command ring\n");
		ret = -ENOMEM;
		goto err_free_tx_ring;
	}
	memset(dev->mcu_ring, 0, dev->mcu_ring_size * sizeof(struct mt76_desc));
	dev->mcu_ring_head = 0;

	dev_info(&dev->pdev->dev, "  MCU ring (Ring 15) allocated: %d descriptors at %pad\n",
		 dev->mcu_ring_size, &dev->mcu_ring_dma);

	/* Configure MCU command ring (ring 15) */
	dev_info(&dev->pdev->dev, "  Configuring MCU ring (ring 15)...\n");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE,
			lower_32_bits(dev->mcu_ring_dma), "RING15_BASE");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE + 0x04,
			dev->mcu_ring_size, "RING15_CNT");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE + 0x08,
			0, "RING15_CIDX");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE + 0x0c,
			0, "RING15_DIDX");

	/* Allocate RX ring (RX Ring 0) for MCU events/responses */
	dev->rx_ring_size = MT7927_RX_MCU_RING_SIZE;
	dev->rx_ring = dma_alloc_coherent(&dev->pdev->dev,
					  dev->rx_ring_size * sizeof(struct mt76_desc),
					  &dev->rx_ring_dma,
					  GFP_KERNEL);
	if (!dev->rx_ring) {
		dev_err(&dev->pdev->dev, "  Failed to allocate RX ring\n");
		ret = -ENOMEM;
		goto err_free_mcu_ring;
	}
	memset(dev->rx_ring, 0, dev->rx_ring_size * sizeof(struct mt76_desc));
	dev->rx_ring_head = 0;

	/* Allocate RX buffer pool */
	dev->rx_buf = dma_alloc_coherent(&dev->pdev->dev,
					 dev->rx_ring_size * MT7927_RX_BUF_SIZE,
					 &dev->rx_buf_dma,
					 GFP_KERNEL);
	if (!dev->rx_buf) {
		dev_err(&dev->pdev->dev, "  Failed to allocate RX buffers\n");
		ret = -ENOMEM;
		goto err_free_rx_ring;
	}

	/* Initialize RX descriptors with buffer addresses */
	{
		int i;
		for (i = 0; i < dev->rx_ring_size; i++) {
			dma_addr_t buf_dma = dev->rx_buf_dma + i * MT7927_RX_BUF_SIZE;
			dev->rx_ring[i].buf0 = cpu_to_le32(lower_32_bits(buf_dma));
			dev->rx_ring[i].buf1 = cpu_to_le32(upper_32_bits(buf_dma));
			/* Control: buffer length, no DMA_DONE yet */
			dev->rx_ring[i].ctrl = cpu_to_le32(
				FIELD_PREP(MT_DMA_CTL_SD_LEN0, MT7927_RX_BUF_SIZE));
			dev->rx_ring[i].info = 0;
		}
	}

	dev_info(&dev->pdev->dev, "  RX ring (Ring 0) allocated: %d descriptors at %pad\n",
		 dev->rx_ring_size, &dev->rx_ring_dma);

	/* Configure RX ring (ring 0) */
	dev_info(&dev->pdev->dev, "  Configuring RX ring (ring 0)...\n");
	mt7927_wr_debug(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE,
			lower_32_bits(dev->rx_ring_dma), "RX_RING0_BASE");
	mt7927_wr_debug(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x04,
			dev->rx_ring_size, "RX_RING0_CNT");
	mt7927_wr_debug(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x08,
			0, "RX_RING0_CIDX");
	mt7927_wr_debug(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x0c,
			0, "RX_RING0_DIDX");

	/* Kick RX ring - set CPU index to ring size to indicate all buffers available */
	mt7927_wr(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x08,
		  dev->rx_ring_size - 1);

	/* Enable DMA */
	ret = mt7927_dma_enable(dev);
	if (ret)
		goto err_free_rx_buf;

	return 0;

err_free_rx_buf:
	dma_free_coherent(&dev->pdev->dev,
			  dev->rx_ring_size * MT7927_RX_BUF_SIZE,
			  dev->rx_buf, dev->rx_buf_dma);
	dev->rx_buf = NULL;
err_free_rx_ring:
	dma_free_coherent(&dev->pdev->dev,
			  dev->rx_ring_size * sizeof(struct mt76_desc),
			  dev->rx_ring, dev->rx_ring_dma);
	dev->rx_ring = NULL;
err_free_mcu_ring:
	dma_free_coherent(&dev->pdev->dev,
			  dev->mcu_ring_size * sizeof(struct mt76_desc),
			  dev->mcu_ring, dev->mcu_ring_dma);
	dev->mcu_ring = NULL;
err_free_tx_ring:
	dma_free_coherent(&dev->pdev->dev,
			  dev->tx_ring_size * sizeof(struct mt76_desc),
			  dev->tx_ring, dev->tx_ring_dma);
	dev->tx_ring = NULL;
	return ret;
}

static void mt7927_dma_cleanup(struct mt7927_dev *dev)
{
	mt7927_dma_disable(dev, false);

	if (dev->mcu_buf) {
		dma_free_coherent(&dev->pdev->dev, MT7927_FW_CHUNK_SIZE + 256,
				  dev->mcu_buf, dev->mcu_dma);
		dev->mcu_buf = NULL;
	}

	if (dev->fw_buf) {
		dma_free_coherent(&dev->pdev->dev, dev->fw_size,
				  dev->fw_buf, dev->fw_dma);
		dev->fw_buf = NULL;
	}

	if (dev->rx_buf) {
		dma_free_coherent(&dev->pdev->dev,
				  dev->rx_ring_size * MT7927_RX_BUF_SIZE,
				  dev->rx_buf, dev->rx_buf_dma);
		dev->rx_buf = NULL;
	}

	if (dev->rx_ring) {
		dma_free_coherent(&dev->pdev->dev,
				  dev->rx_ring_size * sizeof(struct mt76_desc),
				  dev->rx_ring, dev->rx_ring_dma);
		dev->rx_ring = NULL;
	}

	if (dev->mcu_ring) {
		dma_free_coherent(&dev->pdev->dev,
				  dev->mcu_ring_size * sizeof(struct mt76_desc),
				  dev->mcu_ring, dev->mcu_ring_dma);
		dev->mcu_ring = NULL;
	}

	if (dev->tx_ring) {
		dma_free_coherent(&dev->pdev->dev,
				  dev->tx_ring_size * sizeof(struct mt76_desc),
				  dev->tx_ring, dev->tx_ring_dma);
		dev->tx_ring = NULL;
	}
}

/* =============================================================================
 * Firmware Loading via DMA Ring 16
 * =============================================================================
 */

/*
 * Build MCU TXD word 0 for firmware scatter
 */
static u32 mt7927_mcu_txd0_fw(u16 len)
{
	return FIELD_PREP(MT_TXD0_TX_BYTES, len) |
	       FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_FW) |
	       FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_FWDL);
}

/*
 * Get next MCU sequence number (1-15, wraps)
 */
static u8 mt7927_mcu_next_seq(struct mt7927_dev *dev)
{
	dev->mcu_seq = (dev->mcu_seq + 1) & 0xf;
	if (dev->mcu_seq == 0)
		dev->mcu_seq = 1;
	return dev->mcu_seq;
}

/*
 * Build MCU TXD word 0 for MCU commands (via Ring 15)
 */
static u32 mt7927_mcu_txd0_cmd(u16 len)
{
	return FIELD_PREP(MT_TXD0_TX_BYTES, len) |
	       FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
	       FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_Q0);
}

/*
 * Queue an MCU command to Ring 15 (MCU WM queue)
 */
static int mt7927_dma_tx_queue_mcu(struct mt7927_dev *dev, dma_addr_t data_dma,
				   int data_len)
{
	struct mt76_desc *desc;
	u32 ctrl;
	int idx;

	idx = dev->mcu_ring_head;
	desc = &dev->mcu_ring[idx];

	/* Fill descriptor */
	desc->buf0 = cpu_to_le32(lower_32_bits(data_dma));
	desc->buf1 = cpu_to_le32(upper_32_bits(data_dma));
	desc->info = 0;

	/* Control: length + last segment */
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, data_len) |
	       MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);

	if (debug_regs)
		dev_info(&dev->pdev->dev,
			 "  MCU Desc: buf0=0x%08x ctrl=0x%08x len=%d\n",
			 le32_to_cpu(desc->buf0), le32_to_cpu(desc->ctrl), data_len);

	/* Memory barrier before kicking DMA */
	wmb();

	/* Advance head */
	dev->mcu_ring_head = (idx + 1) % dev->mcu_ring_size;

	/* Kick DMA - write CPU index to Ring 15 */
	mt7927_wr(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE + 0x08,
		  dev->mcu_ring_head);

	return 0;
}

/*
 * Wait for MCU command ring to drain
 */
static int mt7927_mcu_tx_wait(struct mt7927_dev *dev, int timeout_ms)
{
	u32 cpu_idx, dma_idx;
	int i;

	for (i = 0; i < timeout_ms; i++) {
		cpu_idx = mt7927_rr(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE + 0x08);
		dma_idx = mt7927_rr(dev, MT_TX_RING_BASE + 15 * MT_RING_SIZE + 0x0c);

		if (cpu_idx == dma_idx)
			return 0;

		usleep_range(1000, 2000);
	}

	dev_warn(&dev->pdev->dev,
		 "  MCU DMA wait timeout: cpu_idx=%d dma_idx=%d\n", cpu_idx, dma_idx);
	return -ETIMEDOUT;
}

/*
 * Wait for MCU response on RX Ring 0
 *
 * Returns: 0 on success, negative on error
 * On success, *resp_data and *resp_len contain the response if not NULL
 */
static int mt7927_mcu_wait_response(struct mt7927_dev *dev, int timeout_ms,
				    u8 expected_seq)
{
	u32 cpu_idx, dma_idx;
	int i;

	dev_info(&dev->pdev->dev, "  Waiting for MCU response (seq=%d)...\n",
		 expected_seq);

	for (i = 0; i < timeout_ms; i++) {
		cpu_idx = mt7927_rr(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x08);
		dma_idx = mt7927_rr(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x0c);

		/* If DMA has advanced past our last processed index, we have data */
		if (cpu_idx != dma_idx) {
			/* For now, just log that we got something */
			struct mt76_desc *desc = &dev->rx_ring[cpu_idx];
			u32 ctrl = le32_to_cpu(desc->ctrl);

			if (ctrl & MT_DMA_CTL_DMA_DONE) {
				int len = FIELD_GET(MT_DMA_CTL_SD_LEN0, ctrl);
				dev_info(&dev->pdev->dev,
					 "  MCU response received: idx=%d len=%d\n",
					 cpu_idx, len);

				/* Recycle descriptor - clear DMA_DONE and advance */
				desc->ctrl = cpu_to_le32(
					FIELD_PREP(MT_DMA_CTL_SD_LEN0, MT7927_RX_BUF_SIZE));
				wmb();

				/* Advance CPU index */
				mt7927_wr(dev, MT_RX_RING_BASE + 0 * MT_RING_SIZE + 0x08,
					  (cpu_idx + 1) % dev->rx_ring_size);

				return 0;
			}
		}

		usleep_range(1000, 2000);
	}

	dev_warn(&dev->pdev->dev,
		 "  MCU response timeout: cpu_idx=%d dma_idx=%d\n", cpu_idx, dma_idx);
	return -ETIMEDOUT;
}

/*
 * Send MCU command and optionally wait for response
 *
 * This builds the MCU TXD header and sends the command via Ring 15.
 * Used for ROM bootloader commands like PATCH_SEM_CONTROL.
 */
static int mt7927_mcu_send_msg(struct mt7927_dev *dev, u8 cmd,
			       const void *data, int len, bool wait_resp)
{
	struct mt7927_mcu_hdr *hdr;
	int total_len;
	u8 seq;
	int ret;

	/* Allocate MCU command buffer if needed */
	if (!dev->mcu_buf) {
		dev->mcu_buf = dma_alloc_coherent(&dev->pdev->dev,
						  MT7927_FW_CHUNK_SIZE + 256,
						  &dev->mcu_dma, GFP_KERNEL);
		if (!dev->mcu_buf)
			return -ENOMEM;
	}

	/* Total packet = TXD (32 bytes) + MCU header + data */
	total_len = sizeof(struct mt7927_mcu_txd) + sizeof(*hdr) + len;

	/* Build MCU header at start of buffer */
	memset(dev->mcu_buf, 0, total_len);

	/* First 32 bytes: TXD */
	{
		__le32 *txd = dev->mcu_buf;
		txd[0] = cpu_to_le32(mt7927_mcu_txd0_cmd(total_len));
	}

	/* MCU header follows TXD */
	hdr = dev->mcu_buf + sizeof(struct mt7927_mcu_txd);
	seq = mt7927_mcu_next_seq(dev);

	hdr->len = cpu_to_le16(sizeof(*hdr) + len);
	hdr->pq_id = cpu_to_le16(0x8000);  /* MCU queue ID */
	hdr->cid = cmd;
	hdr->pkt_type = MT_MCU_PKT_ID;
	hdr->set_query = MCU_CMD_SET;
	hdr->seq = seq;
	hdr->s2d_index = MCU_S2D_H2N;

	/* Copy payload data after header */
	if (data && len > 0)
		memcpy(dev->mcu_buf + sizeof(struct mt7927_mcu_txd) + sizeof(*hdr),
		       data, len);

	/* Sync buffer to device */
	dma_sync_single_for_device(&dev->pdev->dev, dev->mcu_dma,
				   total_len, DMA_TO_DEVICE);

	dev_info(&dev->pdev->dev,
		 "  Sending MCU cmd=0x%02x seq=%d len=%d total=%d\n",
		 cmd, seq, len, total_len);

	/* Queue to Ring 15 */
	ret = mt7927_dma_tx_queue_mcu(dev, dev->mcu_dma, total_len);
	if (ret)
		return ret;

	/* Wait for DMA to complete */
	ret = mt7927_mcu_tx_wait(dev, 100);
	if (ret) {
		dev_err(&dev->pdev->dev, "  MCU command DMA timeout\n");
		return ret;
	}

	/* Wait for response if requested */
	if (wait_resp) {
		ret = mt7927_mcu_wait_response(dev, 500, seq);
		if (ret) {
			dev_warn(&dev->pdev->dev,
				 "  MCU response timeout (cmd=0x%02x) - ROM may not be ready\n",
				 cmd);
			/* Don't fail - ROM might process command without explicit ACK */
		}
	}

	return 0;
}

/*
 * Acquire patch semaphore from ROM bootloader
 *
 * This MUST be called before sending any firmware data!
 * The ROM bootloader uses a semaphore to coordinate patch loading.
 */
static int mt7927_mcu_patch_sem_ctrl(struct mt7927_dev *dev, bool get)
{
	struct {
		__le32 op;
	} req = {
		.op = cpu_to_le32(get ? PATCH_SEM_GET : PATCH_SEM_RELEASE),
	};

	dev_info(&dev->pdev->dev, "=== MCU PATCH_SEM_CONTROL (%s) ===\n",
		 get ? "GET" : "RELEASE");

	return mt7927_mcu_send_msg(dev, MCU_CMD_PATCH_SEM_CTRL,
				   &req, sizeof(req), true);
}

/*
 * Initialize patch download - tells ROM the target address and length
 */
static int mt7927_mcu_init_download(struct mt7927_dev *dev, u32 addr, u32 len)
{
	struct {
		__le32 addr;
		__le32 len;
		__le32 mode;
	} req = {
		.addr = cpu_to_le32(addr),
		.len = cpu_to_le32(len),
		.mode = cpu_to_le32(DL_MODE_NEED_RSP),
	};

	dev_info(&dev->pdev->dev, "=== MCU TARGET_ADDRESS_LEN_REQ ===\n");
	dev_info(&dev->pdev->dev, "  addr=0x%08x len=%d\n", addr, len);

	return mt7927_mcu_send_msg(dev, MCU_CMD_TARGET_ADDRESS_LEN_REQ,
				   &req, sizeof(req), true);
}

/*
 * Queue a firmware chunk to the FWDL ring (ring 16)
 *
 * This sets up a DMA descriptor pointing to the firmware data
 * and kicks the DMA engine.
 */
static int mt7927_dma_tx_queue_fw(struct mt7927_dev *dev, dma_addr_t data_dma,
				  int data_len)
{
	struct mt76_desc *desc;
	u32 ctrl;
	int idx;

	idx = dev->tx_ring_head;
	desc = &dev->tx_ring[idx];

	/* Check if descriptor is available (DMA_DONE should be set for unused) */
	ctrl = le32_to_cpu(desc->ctrl);
	if (!(ctrl & MT_DMA_CTL_DMA_DONE) && ctrl != 0) {
		dev_warn(&dev->pdev->dev, "  Ring full at idx %d, ctrl=0x%08x\n",
			 idx, ctrl);
		return -EBUSY;
	}

	/*
	 * Fill descriptor - MT76 descriptor format:
	 *   buf0: lower 32 bits of DMA address
	 *   ctrl: control flags (length, last segment, etc)
	 *   buf1: upper 32 bits of DMA address (for 64-bit)
	 *   info: additional metadata (token, etc)
	 *
	 * v0.4.3: Set info to 0 and ensure ctrl has correct format.
	 * The kernel driver also sets BURST bit for firmware downloads.
	 */
	desc->buf0 = cpu_to_le32(lower_32_bits(data_dma));
	desc->buf1 = cpu_to_le32(upper_32_bits(data_dma));
	desc->info = 0;

	/* Control: length + last segment + burst mode */
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, data_len) |
	       MT_DMA_CTL_LAST_SEC0 |
	       MT_DMA_CTL_BURST;
	desc->ctrl = cpu_to_le32(ctrl);

	if (debug_regs)
		dev_info(&dev->pdev->dev,
			 "  Desc: buf0=0x%08x buf1=0x%08x ctrl=0x%08x info=0x%08x\n",
			 le32_to_cpu(desc->buf0), le32_to_cpu(desc->buf1),
			 le32_to_cpu(desc->ctrl), le32_to_cpu(desc->info));

	/* Memory barrier before kicking DMA */
	wmb();

	/* Advance head */
	dev->tx_ring_head = (idx + 1) % dev->tx_ring_size;

	/* Kick DMA - write CPU index to register */
	mt7927_wr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x08,
		  dev->tx_ring_head);

	if (debug_regs)
		dev_info(&dev->pdev->dev,
			 "  TX queue: idx=%d, len=%d, dma=%pad, new_head=%d\n",
			 idx, data_len, &data_dma, dev->tx_ring_head);

	return 0;
}

/*
 * Wait for DMA ring to drain (all descriptors completed)
 */
static int mt7927_dma_tx_wait(struct mt7927_dev *dev, int timeout_ms)
{
	u32 cpu_idx, dma_idx;
	int i;

	for (i = 0; i < timeout_ms; i++) {
		cpu_idx = mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x08);
		dma_idx = mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x0c);

		if (cpu_idx == dma_idx)
			return 0;

		usleep_range(1000, 2000);
	}

	/* Dump additional state on timeout for debugging */
	{
		u32 glo_cfg = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
		u32 int_sta = mt7927_rr(dev, MT_WFDMA0_HOST_INT_STA);
		u32 pcie_int = mt7927_rr(dev, MT_PCIE_MAC_INT_STATUS);

		dev_warn(&dev->pdev->dev,
			 "  DMA wait timeout: cpu_idx=%d dma_idx=%d\n", cpu_idx, dma_idx);
		dev_warn(&dev->pdev->dev,
			 "  GLO_CFG=0x%08x INT_STA=0x%08x PCIE_INT=0x%08x\n",
			 glo_cfg, int_sta, pcie_int);

		/* Check descriptor state - use dma_idx to find last attempted desc */
		if (dev->tx_ring && dma_idx < dev->tx_ring_size) {
			struct mt76_desc *desc = &dev->tx_ring[dma_idx];
			dev_warn(&dev->pdev->dev,
				 "  Desc[%d]: buf0=0x%08x ctrl=0x%08x (DMA_DONE=%d)\n",
				 dma_idx, le32_to_cpu(desc->buf0), le32_to_cpu(desc->ctrl),
				 !!(le32_to_cpu(desc->ctrl) & MT_DMA_CTL_DMA_DONE));
		}
	}
	return -ETIMEDOUT;
}

/*
 * Send a firmware scatter chunk
 *
 * Each chunk is wrapped with an MCU TXD header and sent via ring 16.
 * The TXD is 32 bytes (8 DWORDs), followed by the firmware data.
 */
static int mt7927_mcu_send_fw_chunk(struct mt7927_dev *dev, const void *data,
				    int len, u32 offset, bool last)
{
	struct mt7927_mcu_txd *txd;
	int total_len;
	int ret;

	/* Total packet = TXD (32 bytes) + data */
	total_len = sizeof(struct mt7927_mcu_txd) + len;

	/* Check if MCU buffer is large enough */
	if (total_len > MT7927_FW_CHUNK_SIZE + sizeof(struct mt7927_mcu_txd)) {
		dev_err(&dev->pdev->dev, "  FW chunk too large: %d\n", total_len);
		return -EINVAL;
	}

	/* Build TXD in MCU buffer */
	txd = dev->mcu_buf;
	memset(txd, 0, sizeof(*txd));

	/* TXD word 0: packet length + format + queue */
	txd->txd[0] = cpu_to_le32(mt7927_mcu_txd0_fw(total_len));

	/* TXD word 1: reserved */
	txd->txd[1] = 0;

	/* Copy firmware data after TXD */
	memcpy(dev->mcu_buf + sizeof(*txd), data, len);

	/* Ensure data is visible to DMA */
	dma_sync_single_for_device(&dev->pdev->dev, dev->mcu_dma,
				   total_len, DMA_TO_DEVICE);

	/* Queue to DMA ring */
	ret = mt7927_dma_tx_queue_fw(dev, dev->mcu_dma, total_len);
	if (ret)
		return ret;

	/* Wait for this chunk to complete before sending next */
	ret = mt7927_dma_tx_wait(dev, 100);
	if (ret) {
		dev_err(&dev->pdev->dev, "  FW chunk DMA timeout at offset 0x%x\n",
			offset);
		return ret;
	}

	return 0;
}

/*
 * Send firmware data in chunks via FW_SCATTER command
 *
 * The firmware is split into 4KB chunks, each wrapped with an MCU TXD
 * and sent via the FWDL ring (ring 16).
 */
static int mt7927_mcu_send_firmware(struct mt7927_dev *dev, const void *data,
				    int len)
{
	int chunk_size = MT7927_FW_CHUNK_SIZE;
	u32 offset = 0;
	int ret;

	dev_info(&dev->pdev->dev, "  Sending %d bytes in %d-byte chunks...\n",
		 len, chunk_size);

	while (len > 0) {
		int cur_len = min(len, chunk_size);
		bool last = (len <= chunk_size);

		if (debug_regs && (offset % (64 * 1024) == 0 || last))
			dev_info(&dev->pdev->dev, "    Chunk: offset=0x%x len=%d%s\n",
				 offset, cur_len, last ? " (last)" : "");

		ret = mt7927_mcu_send_fw_chunk(dev, data, cur_len, offset, last);
		if (ret) {
			dev_err(&dev->pdev->dev,
				"  Failed to send chunk at offset 0x%x: %d\n",
				offset, ret);
			return ret;
		}

		data += cur_len;
		offset += cur_len;
		len -= cur_len;
	}

	dev_info(&dev->pdev->dev, "  Firmware data sent: %d bytes total\n", offset);
	return 0;
}

/*
 * Parse patch firmware and send to device
 */
static int mt7927_load_patch(struct mt7927_dev *dev)
{
	const struct firmware *fw;
	const struct mt7927_patch_hdr *hdr;
	const struct mt7927_patch_sec *sec;
	const u8 *fw_data;
	u32 n_section;
	u32 ring_base;
	int ret, i;

	dev_info(&dev->pdev->dev, "=== Loading Patch Firmware ===\n");

	/*
	 * CRITICAL: Verify ring BASE is non-zero before attempting DMA!
	 * If the ring BASE is 0, the device will try to DMA from address 0,
	 * causing IOMMU page faults.
	 */
	ring_base = mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE);
	dev_info(&dev->pdev->dev, "  Pre-DMA check: Ring 16 BASE = 0x%08x\n", ring_base);
	if (ring_base == 0) {
		dev_err(&dev->pdev->dev,
			"  ABORT: Ring BASE is 0! DMA would cause page faults.\n");
		dev_err(&dev->pdev->dev,
			"  Ring registers are not writable. Check DMA initialization.\n");
		return -EIO;
	}

	/* Request patch firmware */
	ret = request_firmware(&fw,
			       "mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin",
			       &dev->pdev->dev);
	if (ret) {
		dev_err(&dev->pdev->dev,
			"  Failed to load patch firmware: %d\n", ret);
		return ret;
	}

	dev_info(&dev->pdev->dev, "  Patch firmware loaded: %zu bytes\n", fw->size);

	if (fw->size < sizeof(*hdr)) {
		dev_err(&dev->pdev->dev, "  Patch file too small\n");
		ret = -EINVAL;
		goto out;
	}

	/* Parse patch header */
	hdr = (const struct mt7927_patch_hdr *)fw->data;

	dev_info(&dev->pdev->dev, "  Patch build: %.16s\n", hdr->build_date);
	dev_info(&dev->pdev->dev, "  Platform: %.4s\n", hdr->platform);
	dev_info(&dev->pdev->dev, "  HW/SW version: 0x%08x\n",
		 be32_to_cpu(hdr->hw_sw_ver));
	dev_info(&dev->pdev->dev, "  Patch version: 0x%08x\n",
		 be32_to_cpu(hdr->patch_ver));

	n_section = be32_to_cpu(hdr->desc.n_region);
	dev_info(&dev->pdev->dev, "  Number of sections: %d\n", n_section);

	if (n_section == 0 || n_section > 64) {
		dev_err(&dev->pdev->dev, "  Invalid section count: %d\n", n_section);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * v0.5.0: Acquire patch semaphore from ROM bootloader FIRST!
	 * This tells the ROM we're about to download a patch.
	 * Without this, the ROM ignores our FW_SCATTER data.
	 */
	ret = mt7927_mcu_patch_sem_ctrl(dev, true);
	if (ret) {
		dev_warn(&dev->pdev->dev,
			 "  PATCH_SEM_CONTROL failed: %d (continuing anyway)\n", ret);
		/* Don't fail - try downloading anyway */
	}

	/* Section headers follow the main header */
	sec = (const struct mt7927_patch_sec *)(fw->data + sizeof(*hdr));

	/* Process each section */
	for (i = 0; i < n_section; i++) {
		u32 sec_type = be32_to_cpu(sec[i].type);
		u32 sec_offs = be32_to_cpu(sec[i].offs);
		u32 sec_size = be32_to_cpu(sec[i].size);
		u32 sec_addr = be32_to_cpu(sec[i].info.addr);

		dev_info(&dev->pdev->dev,
			 "  Section %d: type=0x%x offs=0x%x size=%d addr=0x%08x\n",
			 i, sec_type, sec_offs, sec_size, sec_addr);

		/* Check bounds */
		if (sec_offs + sec_size > fw->size) {
			dev_err(&dev->pdev->dev,
				"  Section %d exceeds file size\n", i);
			ret = -EINVAL;
			goto out_release_sem;
		}

		/*
		 * v0.5.0: Send TARGET_ADDRESS_LEN_REQ to tell ROM where to put data
		 * This MUST be sent before FW_SCATTER for each section!
		 */
		ret = mt7927_mcu_init_download(dev, sec_addr, sec_size);
		if (ret) {
			dev_warn(&dev->pdev->dev,
				 "  TARGET_ADDRESS_LEN_REQ failed: %d (continuing)\n", ret);
			/* Don't fail - try downloading anyway */
		}

		/*
		 * Download all sections - the type field indicates encryption,
		 * not whether to download. The kernel driver downloads all
		 * sections unless feature_set has FW_FEATURE_NON_DL.
		 *
		 * sec_type interpretation (from kernel):
		 *   - Bits 0-1: Section type
		 *   - Bit 24+: Encryption flags
		 */
		fw_data = fw->data + sec_offs;

		dev_info(&dev->pdev->dev,
			 "  Downloading section %d (%d bytes) to 0x%08x...\n",
			 i, sec_size, sec_addr);

		ret = mt7927_mcu_send_firmware(dev, fw_data, sec_size);
		if (ret) {
			dev_err(&dev->pdev->dev,
				"  Section %d download failed: %d\n", i, ret);
			goto out_release_sem;
		}
	}

	dev_info(&dev->pdev->dev, "  Patch firmware download complete\n");
	ret = 0;

out_release_sem:
	/* Release patch semaphore */
	mt7927_mcu_patch_sem_ctrl(dev, false);
out:
	release_firmware(fw);
	return ret;
}

static int mt7927_load_firmware(struct mt7927_dev *dev)
{
	int ret;
	u32 status;

	dev_info(&dev->pdev->dev, "=== Firmware Loading ===\n");

	/* Allocate MCU command buffer for DMA */
	dev->mcu_buf = dma_alloc_coherent(&dev->pdev->dev,
					  MT7927_FW_CHUNK_SIZE + 256,
					  &dev->mcu_dma, GFP_KERNEL);
	if (!dev->mcu_buf) {
		dev_err(&dev->pdev->dev, "  Failed to allocate MCU buffer\n");
		return -ENOMEM;
	}

	dev_info(&dev->pdev->dev, "  MCU DMA buffer at %pad\n", &dev->mcu_dma);

	/* Check firmware status before download */
	status = mt7927_rr_remap(dev, MT_CONN_ON_MISC);
	dev_info(&dev->pdev->dev, "  MT_CONN_ON_MISC before: 0x%08x\n", status);

	/* Initialize ring head/tail */
	dev->tx_ring_head = 0;
	dev->tx_ring_tail = 0;
	dev->mcu_seq = 0;

	/* Load and download patch firmware */
	ret = mt7927_load_patch(dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "  Patch loading failed: %d\n", ret);
		/* Continue to check status anyway */
	}

	/* Check firmware status after download */
	status = mt7927_rr_remap(dev, MT_CONN_ON_MISC);
	dev_info(&dev->pdev->dev, "  MT_CONN_ON_MISC after: 0x%08x\n", status);

	/*
	 * Expected: bits 0-1 should become 0x3 when firmware is ready.
	 * If still 0, the firmware hasn't started (expected at this stage
	 * since we haven't sent the start command yet).
	 */
	if ((status & MT_TOP_MISC2_FW_N9_RDY) == MT_TOP_MISC2_FW_N9_RDY) {
		dev_info(&dev->pdev->dev, "  Firmware N9 is READY!\n");
	} else {
		dev_info(&dev->pdev->dev,
			 "  Firmware not ready yet (need FW_START command)\n");
	}

	return ret;
}

/* =============================================================================
 * PCI Probe
 * =============================================================================
 */

static int mt7927_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mt7927_dev *dev;
	int ret;
	u32 val;

	dev_info(&pdev->dev, "\n");
	dev_info(&pdev->dev, "############################################\n");
	dev_info(&pdev->dev, "# MT7927 WiFi 7 Driver v%s\n", DRV_VERSION);
	dev_info(&pdev->dev, "# Device: %04x:%04x (AMD RZ738 compatible)\n",
		 pdev->vendor, pdev->device);
	dev_info(&pdev->dev, "############################################\n");

	/* Allocate device structure */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);

	/* === Phase 1: PCI Setup === */
	dev_info(&pdev->dev, "\n=== Phase 1: PCI Setup ===\n");

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PCI device\n");
		goto err_free;
	}

	ret = pcim_iomap_regions(pdev, BIT(0), DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to map BAR0\n");
		goto err_free;
	}

	/* Ensure memory access is enabled */
	pci_read_config_word(pdev, PCI_COMMAND, (u16 *)&val);
	if (!(val & PCI_COMMAND_MEMORY)) {
		val |= PCI_COMMAND_MEMORY;
		pci_write_config_word(pdev, PCI_COMMAND, val);
	}

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask\n");
		goto err_free;
	}

	dev->regs = pcim_iomap_table(pdev)[0];
	if (!dev->regs) {
		dev_err(&pdev->dev, "Failed to get MMIO pointer\n");
		ret = -ENOMEM;
		goto err_free;
	}

	/* Get BAR0 size for bounds checking */
	dev->regs_len = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "  BAR0 mapped: %pR (size: 0x%llx)\n",
		 &pdev->resource[0], (unsigned long long)dev->regs_len);

	dev->aspm_supported = pcie_aspm_enabled(pdev);

	mt7927_dump_pci_state(dev);

	/* Dump initial register state */
	dev_info(&pdev->dev, "\n=== Initial Register State ===\n");
	mt7927_dump_critical_regs(dev);

	/* === Phase 2: Power Management Handoff === */
	dev_info(&pdev->dev, "\n=== Phase 2: Power Management Handoff ===\n");

	ret = mt7927_mcu_fw_pmctrl(dev);
	if (ret) {
		dev_warn(&pdev->dev, "FW ownership handoff failed (continuing)\n");
	}

	ret = mt7927_mcu_drv_pmctrl(dev);
	if (ret) {
		dev_err(&pdev->dev, "Driver ownership FAILED\n");
		/* Continue anyway for debugging */
	}

	/* === Phase 3: Read Chip ID === */
	dev_info(&pdev->dev, "\n=== Phase 3: Chip Identification ===\n");

	/* Chip ID registers are at 0x70010200/0x70010204 - need remap access */
	dev_info(&pdev->dev, "  Reading Chip ID via remap (0x70010200)...\n");
	dev->chip_id = mt7927_rr_remap(dev, MT_HW_CHIPID);
	dev->chip_rev = (dev->chip_id << 16) | (mt7927_rr_remap(dev, MT_HW_REV) & 0xff);

	dev_info(&pdev->dev, "  Chip ID: 0x%08x\n", dev->chip_id);
	dev_info(&pdev->dev, "  Chip Rev: 0x%08x\n", dev->chip_rev);

	if (dev->chip_id == 0xffffffff || dev->chip_id == 0xdeadbeef) {
		dev_err(&pdev->dev, "  ERROR: Chip not responding (0x%08x)\n", dev->chip_id);
	}

	/* === Phase 4: EMI Sleep Protection === */
	dev_info(&pdev->dev, "\n=== Phase 4: EMI Sleep Protection ===\n");

	/*
	 * CRITICAL: The kernel driver sets EMI sleep protection BEFORE WFSYS reset.
	 * EMI Control is at 0x18011100 - needs remapped access.
	 * Set bit 1 (SLPPROT_EN) to enable sleep protection.
	 */
	dev_info(&pdev->dev, "  Enabling EMI sleep protection (0x%08x)...\n", MT_HW_EMI_CTL);
	{
		u32 emi_val = mt7927_rr_remap(dev, MT_HW_EMI_CTL);
		dev_info(&pdev->dev, "  EMI_CTL before: 0x%08x\n", emi_val);
		mt7927_wr_remap(dev, MT_HW_EMI_CTL, emi_val | MT_HW_EMI_CTL_SLPPROT_EN);
		emi_val = mt7927_rr_remap(dev, MT_HW_EMI_CTL);
		dev_info(&pdev->dev, "  EMI_CTL after:  0x%08x (SLPPROT_EN=%d)\n",
			 emi_val, !!(emi_val & MT_HW_EMI_CTL_SLPPROT_EN));
	}

	/* === Phase 5: WFSYS Reset === */
	dev_info(&pdev->dev, "\n=== Phase 5: WFSYS Reset ===\n");

	ret = mt7927_wfsys_reset(dev);
	if (ret) {
		dev_err(&pdev->dev, "WFSYS reset failed\n");
		/* Continue anyway for debugging */
	}

	/* === Phase 6: Interrupt Setup === */
	dev_info(&pdev->dev, "\n=== Phase 6: Interrupt Setup ===\n");

	mt7927_wr_debug(dev, MT_WFDMA0_HOST_INT_ENA, 0, "HOST_INT_ENA");
	mt7927_wr_debug(dev, MT_PCIE_MAC_INT_ENABLE, 0xff, "PCIE_MAC_INT_EN");

	/* === Phase 7: DMA Initialization === */
	dev_info(&pdev->dev, "\n=== Phase 7: DMA Initialization ===\n");

	ret = mt7927_dma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "DMA initialization failed\n");
		/* Continue anyway */
	}

	/* === Phase 8: Verify Register State === */
	dev_info(&pdev->dev, "\n=== Phase 8: Final Register Verification ===\n");

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&pdev->dev, "  WPDMA_GLO_CFG final: 0x%08x\n", val);

	if (val == 0) {
		dev_err(&pdev->dev, "  CRITICAL: GLO_CFG is 0 - registers NOT writable!\n");
	} else if (val == 0xffffffff) {
		dev_err(&pdev->dev, "  CRITICAL: GLO_CFG is 0xffffffff - device error!\n");
	} else {
		dev_info(&pdev->dev, "  GLO_CFG has value - registers may be writable\n");
	}

	/* Dump final state */
	mt7927_dump_critical_regs(dev);

	/* === Phase 9: Load Firmware === */
	dev_info(&pdev->dev, "\n=== Phase 9: Firmware Loading ===\n");

	ret = mt7927_load_firmware(dev);
	if (ret) {
		dev_warn(&pdev->dev, "Firmware loading incomplete: %d\n", ret);
	}

	/* === Summary === */
	dev_info(&pdev->dev, "\n############################################\n");
	dev_info(&pdev->dev, "# MT7927 Driver Initialization Complete\n");
	dev_info(&pdev->dev, "# Status: Device bound, debugging enabled\n");
	dev_info(&pdev->dev, "# Next: Check dmesg for register values\n");
	dev_info(&pdev->dev, "############################################\n\n");

	return 0;

err_free:
	kfree(dev);
	return ret;
}

static void mt7927_remove(struct pci_dev *pdev)
{
	struct mt7927_dev *dev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "Removing MT7927 driver\n");

	if (dev) {
		mt7927_dma_cleanup(dev);
		kfree(dev);
	}
}

/* =============================================================================
 * Module Definition
 * =============================================================================
 */

static const struct pci_device_id mt7927_pci_ids[] = {
	{ PCI_DEVICE(MT7927_VENDOR_ID, MT7927_DEVICE_ID) },
	{ PCI_DEVICE(MT7927_VENDOR_ID, MT6639_DEVICE_ID) },  /* Mobile variant */
	{ PCI_DEVICE(MT7927_VENDOR_ID, RZ738_DEVICE_ID) },   /* AMD RZ738 */
	{ }
};
MODULE_DEVICE_TABLE(pci, mt7927_pci_ids);

static struct pci_driver mt7927_pci_driver = {
	.name = DRV_NAME,
	.id_table = mt7927_pci_ids,
	.probe = mt7927_probe,
	.remove = mt7927_remove,
};

module_pci_driver(mt7927_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MT7927 Linux Driver Project");
MODULE_DESCRIPTION("MediaTek MT7927 WiFi 7 Driver (AMD RZ738) - Debug Build");
MODULE_VERSION(DRV_VERSION);
MODULE_FIRMWARE("mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin");
MODULE_FIRMWARE("mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin");
