// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 WiFi 7 Linux Driver v2.5.0
 *
 * CRITICAL DISCOVERY: MT7927 uses MT6639 architecture (Gen4m), NOT MT7925 (Gen4).
 * This requires ConnInfra subsystem initialization before MCU communication.
 *
 * v2.5.0 Changes:
 * - Added firmware loading infrastructure
 * - MCU TXD header construction
 * - DMA scatter transfer via Ring 16
 * - Firmware ready polling
 *
 * Based on analysis of:
 * - MediaTek gen4m driver
 * - Linux kernel mt7925 driver
 *
 * Copyright (C) 2026 MT7927 Linux Driver Project
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>

#define DRV_NAME "mt7927"
#define DRV_VERSION "2.5.0"

/* PCI IDs */
#define MT7927_VENDOR_ID	0x14c3
#define MT7927_DEVICE_ID	0x7927
#define MT6639_DEVICE_ID	0x6639

/* Module parameters */
static bool debug = true;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug output (default: true)");

/* =============================================================================
 * MT6639/MT7927 Register Definitions
 * =============================================================================
 */

/* ConnInfra base addresses */
#define CONNAC3X_CONN_CFG_ON_BASE	0x7C060000
#define CONN_INFRA_CFG_BASE		0x830C0000

/* Host CSR registers */
#define HOST_CSR_BASE			0x7000

/* LPCTL bits */
#define PCIE_LPCR_HOST_SET_OWN		BIT(0)
#define PCIE_LPCR_HOST_CLR_OWN		BIT(1)
#define PCIE_LPCR_HOST_OWN_SYNC		BIT(2)

/* Fixed Map - addresses statically mapped in BAR0 */
#define FIXED_MAP_CONN_INFRA_HOST	0x0e0000
#define FIXED_MAP_CONN_INFRA		0x0f0000
#define FIXED_MAP_WFDMA			0x0d0000
#define FIXED_MAP_WF_TOP_MISC_ON	0x0c0000

/* Direct BAR0 offsets for key registers */
#define CONN_INFRA_HOST_BAR_OFS		0x0e0000
#define MT_LPCTL_BAR_OFS		(CONN_INFRA_HOST_BAR_OFS + 0x10)
#define MT_CONN_MISC_BAR_OFS		(CONN_INFRA_HOST_BAR_OFS + 0xf0)

/* WFSYS Reset Register */
#define MT_WFSYS_RST_BAR_OFS		(FIXED_MAP_CONN_INFRA + 0x140)
#define WFSYS_SW_RST_B			BIT(0)
#define WFSYS_SW_INIT_DONE		BIT(4)

/* HIF Remap register */
#define MT_HIF_REMAP_L1			0x155024
#define MT_HIF_REMAP_L1_MASK		GENMASK(31, 16)
#define MT_HIF_REMAP_L1_OFFSET		GENMASK(15, 0)
#define MT_HIF_REMAP_BASE		0x130000

/* =============================================================================
 * WFDMA Register Definitions
 * =============================================================================
 */

#define MT_WFDMA0_BASE			0xD4000

/* Global Config */
#define MT_WFDMA0_GLO_CFG		(MT_WFDMA0_BASE + 0x208)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN	BIT(0)
#define MT_WFDMA0_GLO_CFG_TX_DMA_BUSY	BIT(1)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN	BIT(2)
#define MT_WFDMA0_GLO_CFG_RX_DMA_BUSY	BIT(3)
#define MT_WFDMA0_GLO_CFG_TX_WB_DDONE	BIT(6)
#define MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN	BIT(12)
#define MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN	BIT(15)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2	BIT(21)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO	BIT(27)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO	BIT(28)
#define MT_WFDMA0_GLO_CFG_CLK_GAT_DIS	BIT(30)

/* Extended Config */
#define MT_WFDMA0_GLO_CFG_EXT0		(MT_WFDMA0_BASE + 0x2b0)
#define MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN	BIT(16)

/* DMA Reset */
#define MT_WFDMA0_RST			(MT_WFDMA0_BASE + 0x100)
#define MT_WFDMA0_RST_LOGIC_RST		BIT(4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST	BIT(5)

/* Pointer Resets */
#define MT_WFDMA0_RST_DTX_PTR		(MT_WFDMA0_BASE + 0x228)
#define MT_WFDMA0_RST_DRX_PTR		(MT_WFDMA0_BASE + 0x260)

/* Delay Interrupt Config */
#define MT_WFDMA0_PRI_DLY_INT_CFG0	(MT_WFDMA0_BASE + 0x238)

/* Interrupt registers */
#define MT_WFDMA0_HOST_INT_STA		(MT_WFDMA0_BASE + 0x200)
#define MT_WFDMA0_HOST_INT_ENA		(MT_WFDMA0_BASE + 0x204)

/* MCU Command Register */
#define MT_MCU_CMD			(MT_WFDMA0_BASE + 0x1F0)

/* TX Ring Registers */
#define MT_TX_RING_BASE			(MT_WFDMA0_BASE + 0x300)
#define MT_TX_RING_SIZE			0x10

/* Ring register offsets */
#define MT_RING_BASE			0x00
#define MT_RING_CNT			0x04
#define MT_RING_CIDX			0x08
#define MT_RING_DIDX			0x0c

/* RX Ring Registers */
#define MT_RX_RING_BASE			(MT_WFDMA0_BASE + 0x500)
#define MT_RX_RING_SIZE			0x10

/* DMASHDL bypass */
#define FIXED_MAP_DMASHDL		0x0d6000
#define MT_DMASHDL_SW_CONTROL_OFS	(FIXED_MAP_DMASHDL + 0x04)
#define MT_DMASHDL_BYPASS		BIT(0)

/* Ring indices */
#define MT_TX_RING_DATA0		0
#define MT_TX_RING_MCU_WM		15
#define MT_TX_RING_FWDL		16

#define MT_RX_RING_MCU			0
#define MT_RX_RING_DATA			2

/* Ring sizes */
#define MT_TX_RING_SIZE_DATA		2048
#define MT_TX_RING_SIZE_MCU		256
#define MT_TX_RING_SIZE_FWDL		128
#define MT_RX_RING_SIZE_MCU		512
#define MT_RX_RING_SIZE_DATA		1536

/* DMA Descriptor */
struct mt76_desc {
	__le32 buf0;
	__le32 ctrl;
	__le32 buf1;
	__le32 info;
} __packed __aligned(4);

#define MT_DMA_CTL_SD_LEN0		GENMASK(15, 0)
#define MT_DMA_CTL_LAST_SEC0		BIT(16)
#define MT_DMA_CTL_BURST		BIT(17)
#define MT_DMA_CTL_DMA_DONE		BIT(31)

/* =============================================================================
 * Firmware Definitions
 * =============================================================================
 */

/* Firmware paths - try MT7925 firmware first */
#define MT7925_FIRMWARE_PATCH	"mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin"
#define MT7925_FIRMWARE_RAM	"mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin"

/* Firmware chunk size for DMA transfer */
#define FW_CHUNK_SIZE		4096

/* MCU packet type */
#define MCU_PKT_ID		0xa0

/* MCU command IDs for firmware download */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ	0x01
#define MCU_CMD_FW_START_REQ		0x02
#define MCU_CMD_PATCH_START_REQ		0x05
#define MCU_CMD_PATCH_FINISH_REQ	0x07
#define MCU_CMD_PATCH_SEM_CONTROL	0x10
#define MCU_CMD_FW_SCATTER		0xEE

/* Patch semaphore operations */
#define PATCH_SEM_GET			0x01
#define PATCH_SEM_RELEASE		0x00

/* Download mode flags */
#define DL_MODE_ENCRYPT			BIT(0)
#define DL_MODE_RESET_SEC_IV		BIT(3)
#define DL_MODE_WORKING_PDA_CR4		BIT(4)
#define DL_MODE_VALID_RAM_ENTRY		BIT(5)
#define DL_MODE_NEED_RSP		BIT(31)

/* S2D (Source to Destination) index */
#define MCU_S2D_H2N			0x00	/* Host to N9 (WiFi Manager) */

/* MCU command options */
#define MCU_CMD_ACK			BIT(0)
#define MCU_CMD_UNI			BIT(1)
#define MCU_CMD_SET			BIT(2)

/* Firmware ready status */
#define MT_TOP_MISC2_FW_N9_RDY		GENMASK(1, 0)

/* TXD0 fields */
#define MT_TXD0_TX_BYTES		GENMASK(15, 0)
#define MT_TXD0_PKT_FMT			GENMASK(24, 23)
#define MT_TXD0_Q_IDX			GENMASK(31, 25)

/* Packet format types */
#define MT_TX_TYPE_CT			0	/* Command through MCU */
#define MT_TX_TYPE_SF			1	/* Special frame */
#define MT_TX_TYPE_CMD			2	/* MCU command */
#define MT_TX_TYPE_FW			3	/* Firmware download */

/* Queue indices for TXD0 */
#define MT_TX_MCU_PORT_RX_Q0		0x20
#define MT_TX_MCU_PORT_RX_FWDL		0x3e

/* Patch address */
#define MT7925_PATCH_ADDRESS		0x900000

/* Firmware header structures */
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

struct mt76_connac2_fw_trailer {
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

struct mt76_connac2_fw_region {
	__le32 decomp_crc;
	__le32 decomp_len;
	__le32 decomp_blk_sz;
	u8 rsv[4];
	__le32 addr;
	__le32 len;
	u8 feature_set;
	u8 type;
	u8 rsv1[14];
} __packed;

/* MCU TXD header for firmware download */
struct mt76_connac2_mcu_txd {
	__le32 txd[8];		/* Hardware TXD (32 bytes) */

	__le16 len;		/* Length excluding txd */
	__le16 pq_id;		/* Packet queue ID */

	u8 cid;			/* Command ID */
	u8 pkt_type;		/* Must be 0xa0 */
	u8 set_query;
	u8 seq;			/* Sequence number */

	u8 uc_d2b0_rev;
	u8 ext_cid;		/* Extended command ID */
	u8 s2d_index;		/* Source to destination */
	u8 ext_cid_ack;

	u32 rsv[5];
} __packed __aligned(4);

/* Timeouts */
#define POLL_TIMEOUT_US			1000000
#define CONNINFRA_WAKEUP_TIMEOUT_MS	50
#define DRV_OWN_TIMEOUT_MS		500
#define DMA_BUSY_TIMEOUT_MS		100
#define FW_READY_TIMEOUT_MS		3000
#define DMA_TX_DONE_TIMEOUT_MS		1000

/* =============================================================================
 * Device Structure
 * =============================================================================
 */

struct mt7927_ring {
	struct mt76_desc *desc;
	dma_addr_t desc_dma;
	int size;
	int idx;
	bool allocated;
};

struct mt7927_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_len;

	u32 chip_id;
	u32 chip_rev;
	u32 conninfra_version;

	bool conninfra_ready;
	bool dma_ready;
	bool fw_loaded;

	u8 mcu_seq;		/* MCU command sequence number */

	/* DMA rings */
	struct mt7927_ring tx_ring[32];
	struct mt7927_ring rx_ring[8];

	/* Firmware transfer buffer */
	void *fw_buf;
	dma_addr_t fw_buf_dma;
};

/* =============================================================================
 * Register Access Functions
 * =============================================================================
 */

static inline u32 mt7927_rr(struct mt7927_dev *dev, u32 offset)
{
	if (offset >= dev->regs_len) {
		dev_warn(&dev->pdev->dev, "Read offset 0x%x out of range\n", offset);
		return 0xdeadbeef;
	}
	return readl(dev->regs + offset);
}

static inline void mt7927_wr(struct mt7927_dev *dev, u32 offset, u32 val)
{
	if (offset >= dev->regs_len) {
		dev_warn(&dev->pdev->dev, "Write offset 0x%x out of range\n", offset);
		return;
	}
	writel(val, dev->regs + offset);
}

static inline void mt7927_set(struct mt7927_dev *dev, u32 offset, u32 bits)
{
	u32 val = mt7927_rr(dev, offset);
	mt7927_wr(dev, offset, val | bits);
}

static inline void mt7927_clear(struct mt7927_dev *dev, u32 offset, u32 bits)
{
	u32 val = mt7927_rr(dev, offset);
	mt7927_wr(dev, offset, val & ~bits);
}

/* =============================================================================
 * DMA Ring Management
 * =============================================================================
 */

static int mt7927_ring_alloc(struct mt7927_dev *dev, struct mt7927_ring *ring, int size)
{
	ring->size = size;
	ring->idx = 0;

	ring->desc = dma_alloc_coherent(&dev->pdev->dev,
					size * sizeof(struct mt76_desc),
					&ring->desc_dma, GFP_KERNEL);
	if (!ring->desc) {
		dev_err(&dev->pdev->dev, "Failed to allocate DMA ring (size=%d)\n", size);
		return -ENOMEM;
	}

	memset(ring->desc, 0, size * sizeof(struct mt76_desc));
	ring->allocated = true;

	if (debug)
		dev_info(&dev->pdev->dev, "  Ring allocated: size=%d, dma=0x%llx\n",
			 size, (unsigned long long)ring->desc_dma);

	return 0;
}

static void mt7927_ring_free(struct mt7927_dev *dev, struct mt7927_ring *ring)
{
	if (ring->allocated && ring->desc) {
		dma_free_coherent(&dev->pdev->dev,
				  ring->size * sizeof(struct mt76_desc),
				  ring->desc, ring->desc_dma);
		ring->desc = NULL;
		ring->allocated = false;
	}
}

static void mt7927_ring_setup(struct mt7927_dev *dev, u32 base_reg,
			      struct mt7927_ring *ring)
{
	mt7927_wr(dev, base_reg + MT_RING_BASE, ring->desc_dma);
	mt7927_wr(dev, base_reg + MT_RING_CNT, ring->size);
	mt7927_wr(dev, base_reg + MT_RING_CIDX, 0);

	if (debug)
		dev_info(&dev->pdev->dev, "  Ring setup at 0x%x: base=0x%llx, cnt=%d\n",
			 base_reg, (unsigned long long)ring->desc_dma, ring->size);
}

/* =============================================================================
 * DMA Initialization
 * =============================================================================
 */

static int mt7927_dma_disable(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[DMA] Disabling DMA...\n");

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	if (debug)
		dev_info(&dev->pdev->dev, "[DMA] GLO_CFG before: 0x%08x\n", val);

	mt7927_clear(dev, MT_WFDMA0_GLO_CFG,
		     MT_WFDMA0_GLO_CFG_TX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_RX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
		     MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
		     MT_WFDMA0_GLO_CFG_OMIT_RX_INFO |
		     MT_WFDMA0_GLO_CFG_OMIT_TX_INFO);

	for (i = 0; i < DMA_BUSY_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
		if (!(val & (MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY)))
			break;
		msleep(1);
	}

	mt7927_clear(dev, MT_WFDMA0_GLO_CFG_EXT0, MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN);
	mt7927_set(dev, MT_DMASHDL_SW_CONTROL_OFS, MT_DMASHDL_BYPASS);

	/* Force reset */
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_set(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);

	return 0;
}

static int mt7927_dma_rings_init(struct mt7927_dev *dev)
{
	int ret;

	dev_info(&dev->pdev->dev, "[DMA] Allocating rings...\n");

	/* TX Ring 16 (FWDL) */
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_FWDL], MT_TX_RING_SIZE_FWDL);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE,
			  &dev->tx_ring[MT_TX_RING_FWDL]);

	/* TX Ring 15 (MCU) */
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_MCU_WM], MT_TX_RING_SIZE_MCU);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE,
			  &dev->tx_ring[MT_TX_RING_MCU_WM]);

	/* RX Ring 0 (MCU Events) */
	ret = mt7927_ring_alloc(dev, &dev->rx_ring[MT_RX_RING_MCU], MT_RX_RING_SIZE_MCU);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_RX_RING_BASE + MT_RX_RING_MCU * MT_RX_RING_SIZE,
			  &dev->rx_ring[MT_RX_RING_MCU]);

	return 0;
}

static int mt7927_dma_enable(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[DMA] Enabling DMA...\n");

	mt7927_wr(dev, MT_WFDMA0_RST_DTX_PTR, ~0);
	mt7927_wr(dev, MT_WFDMA0_RST_DRX_PTR, ~0);
	mt7927_wr(dev, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	val = MT_WFDMA0_GLO_CFG_TX_WB_DDONE |
	      MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
	      MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
	      MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
	      MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	      MT_WFDMA0_GLO_CFG_CLK_GAT_DIS |
	      (3 << 4);

	mt7927_wr(dev, MT_WFDMA0_GLO_CFG, val);
	mt7927_set(dev, MT_WFDMA0_GLO_CFG,
		   MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	if ((val & (MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN)) ==
	    (MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN)) {
		dev_info(&dev->pdev->dev, "[DMA] DMA enabled: GLO_CFG=0x%08x\n", val);
		dev->dma_ready = true;
		return 0;
	}

	dev_warn(&dev->pdev->dev, "[DMA] DMA enable failed\n");
	return -EIO;
}

static int mt7927_dma_init(struct mt7927_dev *dev)
{
	int ret;

	ret = mt7927_dma_disable(dev);
	if (ret)
		return ret;

	ret = mt7927_dma_rings_init(dev);
	if (ret)
		return ret;

	ret = mt7927_dma_enable(dev);
	if (ret)
		return ret;

	/* Allocate firmware transfer buffer */
	dev->fw_buf = dma_alloc_coherent(&dev->pdev->dev, FW_CHUNK_SIZE + 128,
					 &dev->fw_buf_dma, GFP_KERNEL);
	if (!dev->fw_buf) {
		dev_err(&dev->pdev->dev, "Failed to allocate FW buffer\n");
		return -ENOMEM;
	}

	dev_info(&dev->pdev->dev, "[DMA] FW buffer at dma=0x%llx\n",
		 (unsigned long long)dev->fw_buf_dma);

	return 0;
}

static void mt7927_dma_cleanup(struct mt7927_dev *dev)
{
	int i;

	if (dev->fw_buf) {
		dma_free_coherent(&dev->pdev->dev, FW_CHUNK_SIZE + 128,
				  dev->fw_buf, dev->fw_buf_dma);
		dev->fw_buf = NULL;
	}

	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++)
		mt7927_ring_free(dev, &dev->tx_ring[i]);

	for (i = 0; i < ARRAY_SIZE(dev->rx_ring); i++)
		mt7927_ring_free(dev, &dev->rx_ring[i]);
}

/* =============================================================================
 * WFSYS Reset and Power Management
 * =============================================================================
 */

static int mt7927_wfsys_reset(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[WFSYS] Reset sequence...\n");

	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	val &= ~WFSYS_SW_RST_B;
	mt7927_wr(dev, MT_WFSYS_RST_BAR_OFS, val);

	msleep(50);

	val |= WFSYS_SW_RST_B;
	mt7927_wr(dev, MT_WFSYS_RST_BAR_OFS, val);

	for (i = 0; i < 500; i++) {
		val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
		if (val & WFSYS_SW_INIT_DONE) {
			dev_info(&dev->pdev->dev, "[WFSYS] INIT_DONE: 0x%08x\n", val);
			return 0;
		}
		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[WFSYS] Timeout: 0x%08x\n", val);
	return -ETIMEDOUT;
}

static int mt7927_power_handoff(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[PWR] Handoff sequence...\n");

	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_SET_OWN);

	for (i = 0; i < 100; i++) {
		val = mt7927_rr(dev, MT_LPCTL_BAR_OFS);
		if (val & PCIE_LPCR_HOST_OWN_SYNC)
			break;
		msleep(1);
	}

	msleep(50);

	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_CLR_OWN);

	for (i = 0; i < 500; i++) {
		val = mt7927_rr(dev, MT_LPCTL_BAR_OFS);
		if (!(val & PCIE_LPCR_HOST_OWN_SYNC)) {
			dev_info(&dev->pdev->dev, "[PWR] Driver ownership: 0x%08x\n", val);
			return 0;
		}
		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[PWR] Timeout: 0x%08x\n", val);
	return -ETIMEDOUT;
}

static int mt7927_conninfra_wakeup(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[ConnInfra] Wakeup...\n");

	mt7927_wr(dev, CONN_INFRA_HOST_BAR_OFS, 0x1);
	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_CLR_OWN);

	for (i = 0; i < CONNINFRA_WAKEUP_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);
		if (val != 0 && val != 0xFFFFFFFF) {
			dev_info(&dev->pdev->dev, "[ConnInfra] Ready: 0x%08x\n", val);
			dev->conninfra_ready = true;
			return 0;
		}
		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[ConnInfra] Timeout: 0x%08x\n", val);
	return -ETIMEDOUT;
}

/* =============================================================================
 * Firmware Loading - Core Functions
 * =============================================================================
 */

/*
 * Wait for TX DMA to complete by polling DIDX == CIDX
 */
static int mt7927_wait_tx_done(struct mt7927_dev *dev, int ring_idx)
{
	u32 base = MT_TX_RING_BASE + ring_idx * MT_TX_RING_SIZE;
	u32 cidx, didx;
	int i;

	cidx = mt7927_rr(dev, base + MT_RING_CIDX);

	for (i = 0; i < DMA_TX_DONE_TIMEOUT_MS; i++) {
		didx = mt7927_rr(dev, base + MT_RING_DIDX);
		if (didx == cidx)
			return 0;
		usleep_range(100, 200);
	}

	dev_warn(&dev->pdev->dev, "[FW] TX timeout: CIDX=%d DIDX=%d\n", cidx, didx);
	return -ETIMEDOUT;
}

/*
 * Submit a single firmware chunk via Ring 16
 */
static int mt7927_fw_scatter(struct mt7927_dev *dev, const u8 *data, size_t len)
{
	struct mt7927_ring *ring = &dev->tx_ring[MT_TX_RING_FWDL];
	struct mt76_connac2_mcu_txd *txd;
	struct mt76_desc *desc;
	u32 ctrl, val;
	int idx;

	if (!ring->allocated || !dev->fw_buf) {
		dev_err(&dev->pdev->dev, "[FW] Ring or buffer not allocated\n");
		return -EINVAL;
	}

	if (len > FW_CHUNK_SIZE) {
		dev_err(&dev->pdev->dev, "[FW] Chunk too large: %zu\n", len);
		return -EINVAL;
	}

	/* Build MCU TXD header in our DMA buffer */
	txd = (struct mt76_connac2_mcu_txd *)dev->fw_buf;
	memset(txd, 0, sizeof(*txd));

	/* TXD0: packet length + format + queue */
	txd->txd[0] = cpu_to_le32(
		FIELD_PREP(MT_TXD0_TX_BYTES, sizeof(*txd) + len) |
		FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_FW) |
		FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_FWDL)
	);

	/* MCU header */
	txd->len = cpu_to_le16(len);
	txd->pq_id = cpu_to_le16(0);
	txd->cid = MCU_CMD_FW_SCATTER;
	txd->pkt_type = MCU_PKT_ID;
	txd->seq = dev->mcu_seq++;
	txd->s2d_index = MCU_S2D_H2N;

	/* Copy firmware data after TXD */
	memcpy(dev->fw_buf + sizeof(*txd), data, len);

	/* Ensure writes are visible before DMA */
	wmb();

	/* Get current ring index */
	idx = ring->idx;
	desc = &ring->desc[idx];

	/* Setup DMA descriptor */
	desc->buf0 = cpu_to_le32(dev->fw_buf_dma);
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, sizeof(*txd) + len) |
	       MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);
	desc->buf1 = 0;
	desc->info = 0;

	/* Ensure descriptor is written before kicking DMA */
	wmb();

	/* Update ring index */
	ring->idx = (idx + 1) % ring->size;

	/* Kick DMA by writing CIDX */
	mt7927_wr(dev, MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE + MT_RING_CIDX,
		  ring->idx);

	/* Wait for DMA completion */
	return mt7927_wait_tx_done(dev, MT_TX_RING_FWDL);
}

/*
 * Poll for firmware ready status
 */
static int mt7927_wait_fw_ready(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[FW] Waiting for firmware ready...\n");

	for (i = 0; i < FW_READY_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);

		if ((val & MT_TOP_MISC2_FW_N9_RDY) == MT_TOP_MISC2_FW_N9_RDY) {
			dev_info(&dev->pdev->dev, "[FW] Firmware ready! MISC=0x%08x\n", val);
			return 0;
		}

		if (i % 500 == 0 && debug)
			dev_info(&dev->pdev->dev, "[FW] Waiting... MISC=0x%08x (%d ms)\n", val, i);

		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[FW] Timeout waiting for FW ready: 0x%08x\n", val);
	return -ETIMEDOUT;
}

/*
 * Load firmware file and transfer via DMA
 */
static int mt7927_load_firmware(struct mt7927_dev *dev)
{
	const struct firmware *fw;
	const struct mt76_connac2_fw_trailer *trailer;
	const u8 *data;
	size_t len, offset, chunk;
	int ret;

	dev_info(&dev->pdev->dev, "\n[FW] ========== Firmware Loading ==========\n");

	/* Request firmware from kernel */
	ret = request_firmware(&fw, MT7925_FIRMWARE_RAM, &dev->pdev->dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "[FW] Failed to load %s: %d\n",
			MT7925_FIRMWARE_RAM, ret);
		dev_info(&dev->pdev->dev, "[FW] Install firmware: linux-firmware or mediatek-firmware\n");
		return ret;
	}

	dev_info(&dev->pdev->dev, "[FW] Loaded %s (%zu bytes)\n",
		 MT7925_FIRMWARE_RAM, fw->size);

	/* Parse trailer at end of file */
	if (fw->size < sizeof(*trailer)) {
		dev_err(&dev->pdev->dev, "[FW] Firmware too small\n");
		ret = -EINVAL;
		goto out;
	}

	trailer = (const struct mt76_connac2_fw_trailer *)(fw->data + fw->size - sizeof(*trailer));
	dev_info(&dev->pdev->dev, "[FW] Firmware: chip=%02x eco=%02x regions=%d ver=%s\n",
		 trailer->chip_id, trailer->eco_code, trailer->n_region, trailer->fw_ver);
	dev_info(&dev->pdev->dev, "[FW] Build date: %.15s\n", trailer->build_date);

	/* For now, do a simple linear transfer of the entire firmware */
	data = fw->data;
	len = fw->size - sizeof(*trailer);  /* Exclude trailer */
	offset = 0;

	dev_info(&dev->pdev->dev, "[FW] Transferring %zu bytes via DMA...\n", len);

	while (offset < len) {
		chunk = min_t(size_t, FW_CHUNK_SIZE, len - offset);

		ret = mt7927_fw_scatter(dev, data + offset, chunk);
		if (ret) {
			dev_err(&dev->pdev->dev, "[FW] Transfer failed at offset %zu: %d\n",
				offset, ret);
			goto out;
		}

		offset += chunk;

		if (debug && (offset % (64 * 1024) == 0 || offset == len))
			dev_info(&dev->pdev->dev, "[FW] Transferred %zu / %zu bytes\n",
				 offset, len);
	}

	dev_info(&dev->pdev->dev, "[FW] Transfer complete, waiting for FW to start...\n");

	/* Wait for firmware to boot */
	ret = mt7927_wait_fw_ready(dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "[FW] Firmware did not become ready\n");
		goto out;
	}

	dev->fw_loaded = true;
	dev_info(&dev->pdev->dev, "[FW] ========== Firmware Loaded Successfully ==========\n\n");

out:
	release_firmware(fw);
	return ret;
}

/* =============================================================================
 * Register Dump
 * =============================================================================
 */

static void mt7927_dump_regs(struct mt7927_dev *dev)
{
	dev_info(&dev->pdev->dev, "[DUMP] Key registers:\n");
	dev_info(&dev->pdev->dev, "  GLO_CFG: 0x%08x\n", mt7927_rr(dev, MT_WFDMA0_GLO_CFG));
	dev_info(&dev->pdev->dev, "  LPCTL: 0x%08x\n", mt7927_rr(dev, MT_LPCTL_BAR_OFS));
	dev_info(&dev->pdev->dev, "  CONN_MISC: 0x%08x\n", mt7927_rr(dev, MT_CONN_MISC_BAR_OFS));
	dev_info(&dev->pdev->dev, "  WFSYS_RST: 0x%08x\n", mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS));
	dev_info(&dev->pdev->dev, "  TX16 CIDX: %d DIDX: %d\n",
		 mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_TX_RING_SIZE + MT_RING_CIDX),
		 mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_TX_RING_SIZE + MT_RING_DIDX));
}

/* =============================================================================
 * Main Probe Function
 * =============================================================================
 */

static int mt7927_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mt7927_dev *dev;
	int ret;

	dev_info(&pdev->dev, "\n");
	dev_info(&pdev->dev, "############################################\n");
	dev_info(&pdev->dev, "# MT7927 WiFi 7 Driver v%s\n", DRV_VERSION);
	dev_info(&pdev->dev, "# Architecture: MT6639 (Gen4m + ConnInfra)\n");
	dev_info(&pdev->dev, "############################################\n");

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	dev->mcu_seq = 1;
	pci_set_drvdata(pdev, dev);

	/* PCI Setup */
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

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask\n");
		goto err_free;
	}

	dev->regs = pcim_iomap_table(pdev)[0];
	if (!dev->regs) {
		ret = -ENOMEM;
		goto err_free;
	}

	dev->regs_len = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "BAR0: %pR\n", &pdev->resource[0]);

	/* Phase 1: Power handoff */
	ret = mt7927_power_handoff(dev);
	if (ret)
		dev_warn(&pdev->dev, "Power handoff incomplete\n");

	/* Phase 2: WFSYS Reset */
	ret = mt7927_wfsys_reset(dev);
	if (ret)
		dev_warn(&pdev->dev, "WFSYS reset incomplete\n");

	/* Phase 3: ConnInfra Wakeup */
	ret = mt7927_conninfra_wakeup(dev);
	if (ret)
		dev_warn(&pdev->dev, "ConnInfra wakeup failed\n");

	/* Phase 4: DMA Initialization */
	ret = mt7927_dma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "DMA init failed\n");
		goto err_free;
	}

	/* Phase 5: Firmware Loading */
	if (dev->conninfra_ready && dev->dma_ready) {
		ret = mt7927_load_firmware(dev);
		if (ret)
			dev_warn(&pdev->dev, "Firmware loading failed: %d\n", ret);
	}

	/* Summary */
	mt7927_dump_regs(dev);

	dev_info(&pdev->dev, "\n=== Summary ===\n");
	dev_info(&pdev->dev, "  ConnInfra: %s\n", dev->conninfra_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  DMA: %s\n", dev->dma_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  Firmware: %s\n", dev->fw_loaded ? "LOADED" : "NOT LOADED");

	if (dev->fw_loaded) {
		dev_info(&pdev->dev, "\n*** FIRMWARE RUNNING! ***\n");
		dev_info(&pdev->dev, "Next: Implement MCU command interface\n");
	}

	return 0;

err_free:
	kfree(dev);
	return ret;
}

static void mt7927_remove(struct pci_dev *pdev)
{
	struct mt7927_dev *dev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "MT7927 driver unloading\n");

	if (dev) {
		mt7927_dma_cleanup(dev);
		kfree(dev);
	}
}

/* =============================================================================
 * PCI Driver Registration
 * =============================================================================
 */

static const struct pci_device_id mt7927_pci_ids[] = {
	{ PCI_DEVICE(MT7927_VENDOR_ID, MT7927_DEVICE_ID) },
	{ PCI_DEVICE(MT7927_VENDOR_ID, MT6639_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, mt7927_pci_ids);

static struct pci_driver mt7927_driver = {
	.name		= DRV_NAME,
	.id_table	= mt7927_pci_ids,
	.probe		= mt7927_probe,
	.remove		= mt7927_remove,
};

module_pci_driver(mt7927_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MT7927 Linux Driver Project");
MODULE_DESCRIPTION("MediaTek MT7927 WiFi 7 driver (MT6639 architecture)");
MODULE_VERSION(DRV_VERSION);
MODULE_FIRMWARE(MT7925_FIRMWARE_PATCH);
MODULE_FIRMWARE(MT7925_FIRMWARE_RAM);
