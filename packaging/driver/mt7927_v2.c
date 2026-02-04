// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 WiFi 7 Linux Driver v2.10.0
 *
 * v2.10.0 Changes:
 * - Add interrupt setup before DMA init (required sequence)
 * - Disable HOST_INT_ENA, enable PCIe MAC interrupts
 * - This makes WPDMA registers writable
 *
 * v2.9.0 Changes:
 * - Use PATCH_START_REQ for patch, add MCU wake signal
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
#define DRV_VERSION "2.10.0"

/* PCI IDs */
#define MT7927_VENDOR_ID	0x14c3
#define MT7927_DEVICE_ID	0x7927
#define MT6639_DEVICE_ID	0x6639

/* Module parameters */
static bool debug = true;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug output (default: true)");

/* =============================================================================
 * Register Definitions
 * =============================================================================
 */

/* LPCTL bits */
#define PCIE_LPCR_HOST_SET_OWN		BIT(0)
#define PCIE_LPCR_HOST_CLR_OWN		BIT(1)
#define PCIE_LPCR_HOST_OWN_SYNC		BIT(2)

/* Fixed Map - addresses statically mapped in BAR0 */
#define FIXED_MAP_CONN_INFRA_HOST	0x0e0000
#define FIXED_MAP_CONN_INFRA		0x0f0000

/* Direct BAR0 offsets */
#define CONN_INFRA_HOST_BAR_OFS		0x0e0000
#define MT_LPCTL_BAR_OFS		(CONN_INFRA_HOST_BAR_OFS + 0x10)
#define MT_CONN_MISC_BAR_OFS		(CONN_INFRA_HOST_BAR_OFS + 0xf0)

/* WFSYS Reset */
#define MT_WFSYS_RST_BAR_OFS		(FIXED_MAP_CONN_INFRA + 0x140)
#define WFSYS_SW_RST_B			BIT(0)
#define WFSYS_SW_INIT_DONE		BIT(4)

/* =============================================================================
 * WFDMA Registers
 * =============================================================================
 */

#define MT_WFDMA0_BASE			0xD4000

#define MT_WFDMA0_GLO_CFG		(MT_WFDMA0_BASE + 0x208)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN	BIT(0)
#define MT_WFDMA0_GLO_CFG_TX_DMA_BUSY	BIT(1)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN	BIT(2)
#define MT_WFDMA0_GLO_CFG_RX_DMA_BUSY	BIT(3)
#define MT_WFDMA0_GLO_CFG_TX_WB_DDONE	BIT(6)
#define MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN	BIT(12)
#define MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN	BIT(15)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2	BIT(21)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO	BIT(28)
#define MT_WFDMA0_GLO_CFG_CLK_GAT_DIS	BIT(30)

#define MT_WFDMA0_GLO_CFG_EXT0		(MT_WFDMA0_BASE + 0x2b0)
#define MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN	BIT(16)

#define MT_WFDMA0_RST			(MT_WFDMA0_BASE + 0x100)
#define MT_WFDMA0_RST_LOGIC_RST		BIT(4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST	BIT(5)

#define MT_WFDMA0_RST_DTX_PTR		(MT_WFDMA0_BASE + 0x228)
#define MT_WFDMA0_RST_DRX_PTR		(MT_WFDMA0_BASE + 0x260)
#define MT_WFDMA0_PRI_DLY_INT_CFG0	(MT_WFDMA0_BASE + 0x238)

#define MT_TX_RING_BASE			(MT_WFDMA0_BASE + 0x300)
#define MT_TX_RING_SIZE			0x10
#define MT_RX_RING_BASE			(MT_WFDMA0_BASE + 0x500)
#define MT_RX_RING_SIZE			0x10

#define MT_RING_BASE			0x00
#define MT_RING_CNT			0x04
#define MT_RING_CIDX			0x08
#define MT_RING_DIDX			0x0c

#define FIXED_MAP_DMASHDL		0x0d6000
#define MT_DMASHDL_SW_CONTROL_OFS	(FIXED_MAP_DMASHDL + 0x04)
#define MT_DMASHDL_BYPASS		BIT(0)

/* MCU Command Register - for waking up MCU/ROM */
#define MT_MCU_CMD			(MT_WFDMA0_BASE + 0x1f0)
#define MT_MCU_CMD_WAKE_RX_PCIE		BIT(0)
#define MT_MCU_CMD_STOP_DMA		BIT(1)
#define MT_MCU_CMD_RESET_DONE		BIT(2)
#define MT_MCU_CMD_RECOVERY_DONE	BIT(3)
#define MT_MCU_CMD_NORMAL_STATE		BIT(4)
#define MT_MCU_CMD_LMAC_DONE		BIT(5)

/* Interrupt registers - must be configured before DMA init */
#define MT_WFDMA0_HOST_INT_ENA		(MT_WFDMA0_BASE + 0x204)
#define MT_PCIE_MAC_BASE		0x10000
#define MT_PCIE_MAC_INT_ENABLE		(MT_PCIE_MAC_BASE + 0x188)

/* Ring indices */
#define MT_TX_RING_MCU_WM		15
#define MT_TX_RING_FWDL		16
#define MT_RX_RING_MCU			0

/* Ring sizes */
#define MT_TX_RING_SIZE_MCU		256
#define MT_TX_RING_SIZE_FWDL		128
#define MT_RX_RING_SIZE_MCU		512

/* DMA Descriptor */
struct mt76_desc {
	__le32 buf0;
	__le32 ctrl;
	__le32 buf1;
	__le32 info;
} __packed __aligned(4);

#define MT_DMA_CTL_SD_LEN0		GENMASK(15, 0)
#define MT_DMA_CTL_LAST_SEC0		BIT(16)
#define MT_DMA_CTL_DMA_DONE		BIT(31)

/* =============================================================================
 * Firmware Definitions
 * =============================================================================
 */

/* MT6639 firmware for MT7927 - must be installed to /lib/firmware/mediatek/ */
#define MT6639_FIRMWARE_PATCH	"mediatek/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin"
#define MT6639_FIRMWARE_RAM	"mediatek/WIFI_RAM_CODE_MT6639_2_1.bin"
#define FW_CHUNK_SIZE		4096

/* MCU packet type */
#define MCU_PKT_ID		0xa0

/* MCU command IDs */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ	0x01
#define MCU_CMD_FW_START_REQ		0x02
#define MCU_CMD_PATCH_SEM_CONTROL	0x04
#define MCU_CMD_PATCH_START_REQ		0x05
#define MCU_CMD_PATCH_FINISH_REQ	0x07
#define MCU_CMD_FW_SCATTER		0xEE

/* Patch semaphore operations */
#define PATCH_SEM_GET			0x01
#define PATCH_SEM_RELEASE		0x00

/* Patch semaphore response */
#define PATCH_NOT_DL_SEM_SUCCESS	0x02
#define PATCH_IS_DL				0x01
#define PATCH_NOT_DL_SEM_FAIL		0x00

/* S2D index */
#define MCU_S2D_H2N			0x00

/* Download mode flags */
#define DL_MODE_NEED_RSP		BIT(31)

/* Patch loading address */
#define MT_PATCH_ADDR			0x900000

/* Firmware ready status */
#define MT_TOP_MISC2_FW_N9_RDY		GENMASK(1, 0)

/* TXD0 fields */
#define MT_TXD0_TX_BYTES		GENMASK(15, 0)
#define MT_TXD0_PKT_FMT			GENMASK(24, 23)
#define MT_TXD0_Q_IDX			GENMASK(31, 25)

#define MT_TX_TYPE_CMD			2
#define MT_TX_TYPE_FW			3

#define MT_TX_MCU_PORT_RX_Q0		0x20
#define MT_TX_MCU_PORT_RX_FWDL		0x3e

/* Firmware header structures */
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

/* MCU TXD header */
struct mt76_connac2_mcu_txd {
	__le32 txd[8];

	__le16 len;
	__le16 pq_id;

	u8 cid;
	u8 pkt_type;
	u8 set_query;
	u8 seq;

	u8 uc_d2b0_rev;
	u8 ext_cid;
	u8 s2d_index;
	u8 ext_cid_ack;

	u32 rsv[5];
} __packed __aligned(4);

/* TARGET_ADDRESS_LEN_REQ payload */
struct mt76_connac_fw_dl {
	__le32 addr;
	__le32 len;
	__le32 mode;
	u8 rsv[4];
} __packed;

/* FW_START_REQ payload */
struct mt76_connac_fw_start {
	__le32 override;
	__le32 option;
} __packed;

/* Patch semaphore request */
struct mt76_connac_patch_sem {
	__le32 op;  /* 1 = GET, 0 = RELEASE */
} __packed;

/* Patch header (at start of patch file) */
struct mt76_connac2_patch_hdr {
	char build_date[16];
	char platform[4];
	__be32 hw_ver;
	__be32 patch_ver;
	u8 rsv[4];
} __packed;

/* Timeouts */
#define CONNINFRA_WAKEUP_TIMEOUT_MS	50
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

	bool conninfra_ready;
	bool dma_ready;
	bool fw_loaded;

	u8 mcu_seq;

	struct mt7927_ring tx_ring[32];
	struct mt7927_ring rx_ring[8];

	void *fw_buf;
	dma_addr_t fw_buf_dma;

	void *cmd_buf;
	dma_addr_t cmd_buf_dma;
};

/* =============================================================================
 * Register Access
 * =============================================================================
 */

static inline u32 mt7927_rr(struct mt7927_dev *dev, u32 offset)
{
	if (offset >= dev->regs_len)
		return 0xdeadbeef;
	return readl(dev->regs + offset);
}

static inline void mt7927_wr(struct mt7927_dev *dev, u32 offset, u32 val)
{
	if (offset < dev->regs_len)
		writel(val, dev->regs + offset);
}

static inline void mt7927_set(struct mt7927_dev *dev, u32 offset, u32 bits)
{
	mt7927_wr(dev, offset, mt7927_rr(dev, offset) | bits);
}

static inline void mt7927_clear(struct mt7927_dev *dev, u32 offset, u32 bits)
{
	mt7927_wr(dev, offset, mt7927_rr(dev, offset) & ~bits);
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
	if (!ring->desc)
		return -ENOMEM;

	memset(ring->desc, 0, size * sizeof(struct mt76_desc));
	ring->allocated = true;
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
}

/* =============================================================================
 * DMA Initialization
 * =============================================================================
 */

static int mt7927_dma_init(struct mt7927_dev *dev)
{
	u32 val;
	int ret, i;

	dev_info(&dev->pdev->dev, "[DMA] Initializing...\n");

	/* Disable DMA */
	mt7927_clear(dev, MT_WFDMA0_GLO_CFG,
		     MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	for (i = 0; i < DMA_BUSY_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
		if (!(val & (MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY)))
			break;
		msleep(1);
	}

	mt7927_clear(dev, MT_WFDMA0_GLO_CFG_EXT0, MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN);
	mt7927_set(dev, MT_DMASHDL_SW_CONTROL_OFS, MT_DMASHDL_BYPASS);

	/* Reset */
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_set(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);

	/* Allocate rings */
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_FWDL], MT_TX_RING_SIZE_FWDL);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE,
			  &dev->tx_ring[MT_TX_RING_FWDL]);

	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_MCU_WM], MT_TX_RING_SIZE_MCU);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE,
			  &dev->tx_ring[MT_TX_RING_MCU_WM]);

	ret = mt7927_ring_alloc(dev, &dev->rx_ring[MT_RX_RING_MCU], MT_RX_RING_SIZE_MCU);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_RX_RING_BASE + MT_RX_RING_MCU * MT_RX_RING_SIZE,
			  &dev->rx_ring[MT_RX_RING_MCU]);

	/* Enable DMA */
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
		dev->dma_ready = true;
		dev_info(&dev->pdev->dev, "[DMA] Ready: GLO_CFG=0x%08x\n", val);
	}

	/* Wake MCU/ROM so it starts processing DMA rings */
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[MCU] CMD before wake: 0x%08x\n", val);
	mt7927_set(dev, MT_MCU_CMD, MT_MCU_CMD_WAKE_RX_PCIE);
	msleep(5);
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[MCU] CMD after wake: 0x%08x\n", val);

	/* Allocate buffers */
	dev->fw_buf = dma_alloc_coherent(&dev->pdev->dev, FW_CHUNK_SIZE + 256,
					 &dev->fw_buf_dma, GFP_KERNEL);
	dev->cmd_buf = dma_alloc_coherent(&dev->pdev->dev, 256,
					  &dev->cmd_buf_dma, GFP_KERNEL);
	if (!dev->fw_buf || !dev->cmd_buf)
		return -ENOMEM;

	return 0;
}

static void mt7927_dma_cleanup(struct mt7927_dev *dev)
{
	int i;

	if (dev->fw_buf)
		dma_free_coherent(&dev->pdev->dev, FW_CHUNK_SIZE + 256,
				  dev->fw_buf, dev->fw_buf_dma);
	if (dev->cmd_buf)
		dma_free_coherent(&dev->pdev->dev, 256,
				  dev->cmd_buf, dev->cmd_buf_dma);

	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++)
		mt7927_ring_free(dev, &dev->tx_ring[i]);
	for (i = 0; i < ARRAY_SIZE(dev->rx_ring); i++)
		mt7927_ring_free(dev, &dev->rx_ring[i]);
}

/* =============================================================================
 * Power Management and Reset
 * =============================================================================
 */

static int mt7927_wfsys_reset(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	val &= ~WFSYS_SW_RST_B;
	mt7927_wr(dev, MT_WFSYS_RST_BAR_OFS, val);
	msleep(50);
	val |= WFSYS_SW_RST_B;
	mt7927_wr(dev, MT_WFSYS_RST_BAR_OFS, val);

	for (i = 0; i < 500; i++) {
		val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
		if (val & WFSYS_SW_INIT_DONE) {
			dev_info(&dev->pdev->dev, "[WFSYS] INIT_DONE\n");
			return 0;
		}
		msleep(1);
	}
	return -ETIMEDOUT;
}

static int mt7927_power_handoff(struct mt7927_dev *dev)
{
	u32 val;
	int i;

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
			dev_info(&dev->pdev->dev, "[PWR] Driver ownership OK\n");
			return 0;
		}
		msleep(1);
	}
	return -ETIMEDOUT;
}

static int mt7927_conninfra_wakeup(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	mt7927_wr(dev, CONN_INFRA_HOST_BAR_OFS, 0x1);
	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_CLR_OWN);

	for (i = 0; i < CONNINFRA_WAKEUP_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);
		if (val != 0 && val != 0xFFFFFFFF) {
			dev->conninfra_ready = true;
			dev_info(&dev->pdev->dev, "[ConnInfra] Ready: 0x%08x\n", val);
			return 0;
		}
		msleep(1);
	}
	return -ETIMEDOUT;
}

/*
 * Interrupt setup - MUST be done before DMA init
 * This makes WPDMA registers writable
 */
static void mt7927_irq_setup(struct mt7927_dev *dev)
{
	u32 val;

	/* Disable host DMA interrupts first */
	mt7927_wr(dev, MT_WFDMA0_HOST_INT_ENA, 0);

	/* Enable PCIe MAC interrupts (required for WPDMA) */
	mt7927_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);

	val = mt7927_rr(dev, MT_PCIE_MAC_INT_ENABLE);
	dev_info(&dev->pdev->dev, "[IRQ] PCIe MAC INT: 0x%08x\n", val);
}

/* =============================================================================
 * MCU Command Interface
 * =============================================================================
 */

static int mt7927_wait_tx_done(struct mt7927_dev *dev, int ring_idx)
{
	u32 base = MT_TX_RING_BASE + ring_idx * MT_TX_RING_SIZE;
	u32 cidx, didx, didx_initial;
	int i;

	cidx = mt7927_rr(dev, base + MT_RING_CIDX);
	didx_initial = mt7927_rr(dev, base + MT_RING_DIDX);

	for (i = 0; i < DMA_TX_DONE_TIMEOUT_MS * 10; i++) {
		didx = mt7927_rr(dev, base + MT_RING_DIDX);
		if (didx == cidx)
			return 0;
		usleep_range(100, 200);
	}

	dev_warn(&dev->pdev->dev, "[DMA] TX timeout ring %d: CIDX=%d DIDX=%d (was %d)\n",
		 ring_idx, cidx, didx, didx_initial);
	return -ETIMEDOUT;
}

/*
 * Send MCU command via Ring 15 (WM queue)
 */
static int mt7927_mcu_send_cmd(struct mt7927_dev *dev, u8 cmd,
			       const void *data, size_t len)
{
	struct mt7927_ring *ring = &dev->tx_ring[MT_TX_RING_MCU_WM];
	struct mt76_connac2_mcu_txd *txd;
	struct mt76_desc *desc;
	u32 ctrl;
	int idx;

	if (!ring->allocated || !dev->cmd_buf)
		return -EINVAL;

	txd = (struct mt76_connac2_mcu_txd *)dev->cmd_buf;
	memset(txd, 0, sizeof(*txd));

	/* TXD0: packet length + format + queue */
	txd->txd[0] = cpu_to_le32(
		FIELD_PREP(MT_TXD0_TX_BYTES, sizeof(*txd) + len) |
		FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
		FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_Q0)
	);

	txd->len = cpu_to_le16(len);
	txd->pq_id = cpu_to_le16(0);
	txd->cid = cmd;
	txd->pkt_type = MCU_PKT_ID;
	txd->seq = dev->mcu_seq++;
	txd->s2d_index = MCU_S2D_H2N;

	/* Copy payload after TXD */
	if (data && len > 0)
		memcpy(dev->cmd_buf + sizeof(*txd), data, len);

	wmb();

	idx = ring->idx;
	desc = &ring->desc[idx];

	desc->buf0 = cpu_to_le32(dev->cmd_buf_dma);
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, sizeof(*txd) + len) | MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);
	desc->buf1 = 0;
	desc->info = 0;

	wmb();

	ring->idx = (idx + 1) % ring->size;
	mt7927_wr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_CIDX,
		  ring->idx);

	return mt7927_wait_tx_done(dev, MT_TX_RING_MCU_WM);
}

/*
 * Send PATCH_SEM_CONTROL to acquire/release patch semaphore
 */
static int mt7927_mcu_patch_sem(struct mt7927_dev *dev, bool get)
{
	struct mt76_connac_patch_sem req = {
		.op = cpu_to_le32(get ? PATCH_SEM_GET : PATCH_SEM_RELEASE),
	};

	dev_info(&dev->pdev->dev, "[PATCH] Semaphore %s\n", get ? "GET" : "RELEASE");

	return mt7927_mcu_send_cmd(dev, MCU_CMD_PATCH_SEM_CONTROL, &req, sizeof(req));
}

/*
 * Send PATCH_FINISH_REQ to finalize patch
 */
static int mt7927_mcu_patch_finish(struct mt7927_dev *dev)
{
	dev_info(&dev->pdev->dev, "[PATCH] Finish\n");

	return mt7927_mcu_send_cmd(dev, MCU_CMD_PATCH_FINISH_REQ, NULL, 0);
}

/*
 * Send PATCH_START_REQ to initialize patch download (for address 0x900000)
 */
static int mt7927_mcu_patch_start(struct mt7927_dev *dev, u32 addr, u32 len, u32 mode)
{
	struct mt76_connac_fw_dl req = {
		.addr = cpu_to_le32(addr),
		.len = cpu_to_le32(len),
		.mode = cpu_to_le32(mode),
	};

	dev_info(&dev->pdev->dev, "[PATCH] Start: addr=0x%08x len=%u mode=0x%x\n",
		 addr, len, mode);

	return mt7927_mcu_send_cmd(dev, MCU_CMD_PATCH_START_REQ, &req, sizeof(req));
}

/*
 * Send TARGET_ADDRESS_LEN_REQ to set download address (for RAM firmware)
 */
static int mt7927_mcu_init_download(struct mt7927_dev *dev, u32 addr, u32 len, u32 mode)
{
	struct mt76_connac_fw_dl req = {
		.addr = cpu_to_le32(addr),
		.len = cpu_to_le32(len),
		.mode = cpu_to_le32(mode),
	};

	dev_info(&dev->pdev->dev, "[FW] Init download: addr=0x%08x len=%u mode=0x%x\n",
		 addr, len, mode);

	return mt7927_mcu_send_cmd(dev, MCU_CMD_TARGET_ADDRESS_LEN_REQ, &req, sizeof(req));
}

/*
 * Send FW_START_REQ to start firmware execution
 */
static int mt7927_mcu_start_firmware(struct mt7927_dev *dev, u32 addr)
{
	struct mt76_connac_fw_start req = {
		.override = cpu_to_le32(addr ? addr : 0),
		.option = cpu_to_le32(addr ? BIT(0) : 0),  /* Override if addr set */
	};

	dev_info(&dev->pdev->dev, "[FW] Start firmware: addr=0x%08x\n", addr);

	return mt7927_mcu_send_cmd(dev, MCU_CMD_FW_START_REQ, &req, sizeof(req));
}

/*
 * Send firmware data chunk via Ring 16
 */
static int mt7927_fw_scatter(struct mt7927_dev *dev, const u8 *data, size_t len)
{
	struct mt7927_ring *ring = &dev->tx_ring[MT_TX_RING_FWDL];
	struct mt76_connac2_mcu_txd *txd;
	struct mt76_desc *desc;
	u32 ctrl;
	int idx;

	if (!ring->allocated || !dev->fw_buf)
		return -EINVAL;

	txd = (struct mt76_connac2_mcu_txd *)dev->fw_buf;
	memset(txd, 0, sizeof(*txd));

	txd->txd[0] = cpu_to_le32(
		FIELD_PREP(MT_TXD0_TX_BYTES, sizeof(*txd) + len) |
		FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_FW) |
		FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_FWDL)
	);

	txd->len = cpu_to_le16(len);
	txd->cid = MCU_CMD_FW_SCATTER;
	txd->pkt_type = MCU_PKT_ID;
	txd->seq = dev->mcu_seq++;
	txd->s2d_index = MCU_S2D_H2N;

	memcpy(dev->fw_buf + sizeof(*txd), data, len);
	wmb();

	idx = ring->idx;
	desc = &ring->desc[idx];

	desc->buf0 = cpu_to_le32(dev->fw_buf_dma);
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, sizeof(*txd) + len) | MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);
	desc->buf1 = 0;
	desc->info = 0;

	wmb();

	ring->idx = (idx + 1) % ring->size;
	mt7927_wr(dev, MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE + MT_RING_CIDX,
		  ring->idx);

	return mt7927_wait_tx_done(dev, MT_TX_RING_FWDL);
}

/* =============================================================================
 * Firmware Loading
 * =============================================================================
 */

static int mt7927_wait_fw_ready(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	for (i = 0; i < FW_READY_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);
		if ((val & MT_TOP_MISC2_FW_N9_RDY) == MT_TOP_MISC2_FW_N9_RDY) {
			dev_info(&dev->pdev->dev, "[FW] Ready! MISC=0x%08x\n", val);
			return 0;
		}
		if (i % 500 == 0)
			dev_info(&dev->pdev->dev, "[FW] Waiting... MISC=0x%08x\n", val);
		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[FW] Timeout: MISC=0x%08x\n", val);
	return -ETIMEDOUT;
}

/*
 * Load ROM patch - must be done before main firmware
 */
static int mt7927_load_patch(struct mt7927_dev *dev)
{
	const struct firmware *fw;
	const struct mt76_connac2_patch_hdr *hdr;
	const u8 *data;
	size_t data_len, offset, chunk;
	int ret;

	dev_info(&dev->pdev->dev, "\n[PATCH] ========== Loading Patch ==========\n");

	ret = request_firmware(&fw, MT6639_FIRMWARE_PATCH, &dev->pdev->dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "[PATCH] Failed to load %s: %d\n",
			MT6639_FIRMWARE_PATCH, ret);
		return ret;
	}

	dev_info(&dev->pdev->dev, "[PATCH] Loaded %zu bytes\n", fw->size);

	if (fw->size < sizeof(*hdr)) {
		dev_err(&dev->pdev->dev, "[PATCH] File too small\n");
		ret = -EINVAL;
		goto out;
	}

	hdr = (const struct mt76_connac2_patch_hdr *)fw->data;
	dev_info(&dev->pdev->dev, "[PATCH] Build: %.16s Platform: %.4s HW: 0x%08x\n",
		 hdr->build_date, hdr->platform, be32_to_cpu(hdr->hw_ver));

	/* Acquire patch semaphore */
	ret = mt7927_mcu_patch_sem(dev, true);
	if (ret) {
		dev_err(&dev->pdev->dev, "[PATCH] Failed to acquire semaphore: %d\n", ret);
		goto out;
	}

	/* Patch data starts after header */
	data = fw->data + sizeof(*hdr);
	data_len = fw->size - sizeof(*hdr);

	/* Send PATCH_START_REQ for patch at 0x900000 */
	ret = mt7927_mcu_patch_start(dev, MT_PATCH_ADDR, data_len, DL_MODE_NEED_RSP);
	if (ret) {
		dev_err(&dev->pdev->dev, "[PATCH] Start failed: %d\n", ret);
		goto sem_release;
	}

	/* Transfer patch data */
	offset = 0;
	while (offset < data_len) {
		chunk = min_t(size_t, FW_CHUNK_SIZE, data_len - offset);

		ret = mt7927_fw_scatter(dev, data + offset, chunk);
		if (ret) {
			dev_err(&dev->pdev->dev, "[PATCH] Scatter failed at %zu: %d\n",
				offset, ret);
			goto sem_release;
		}
		offset += chunk;
	}

	dev_info(&dev->pdev->dev, "[PATCH] Transferred %zu bytes\n", data_len);

	/* Finalize patch */
	ret = mt7927_mcu_patch_finish(dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "[PATCH] Finish failed: %d\n", ret);
		goto sem_release;
	}

	dev_info(&dev->pdev->dev, "[PATCH] ========== Patch Loaded! ==========\n");

sem_release:
	/* Release semaphore (even on error) */
	mt7927_mcu_patch_sem(dev, false);

out:
	release_firmware(fw);
	return ret;
}

/*
 * Load main firmware (after patch is loaded)
 */
static int mt7927_load_ram(struct mt7927_dev *dev)
{
	const struct firmware *fw;
	const struct mt76_connac2_fw_trailer *trailer;
	const struct mt76_connac2_fw_region *region;
	const u8 *data;
	size_t offset, chunk;
	int ret, i;
	u32 addr, len, mode;

	dev_info(&dev->pdev->dev, "\n[FW] ========== Loading Main Firmware ==========\n");

	ret = request_firmware(&fw, MT6639_FIRMWARE_RAM, &dev->pdev->dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "[FW] Failed to load %s: %d\n",
			MT6639_FIRMWARE_RAM, ret);
		return ret;
	}

	dev_info(&dev->pdev->dev, "[FW] Loaded %zu bytes\n", fw->size);

	/* Parse trailer */
	trailer = (const struct mt76_connac2_fw_trailer *)(fw->data + fw->size - sizeof(*trailer));
	dev_info(&dev->pdev->dev, "[FW] chip=%02x eco=%02x regions=%d ver=%s\n",
		 trailer->chip_id, trailer->eco_code, trailer->n_region, trailer->fw_ver);

	/* Process each region */
	data = fw->data;
	offset = 0;

	for (i = 0; i < trailer->n_region; i++) {
		/* Region headers are stored backwards from trailer */
		region = (const struct mt76_connac2_fw_region *)
			 (fw->data + fw->size - sizeof(*trailer) -
			  (trailer->n_region - i) * sizeof(*region));

		addr = le32_to_cpu(region->addr);
		len = le32_to_cpu(region->len);
		mode = DL_MODE_NEED_RSP;

		dev_info(&dev->pdev->dev, "[FW] Region %d: addr=0x%08x len=%u type=%d\n",
			 i, addr, len, region->type);

		/* Send TARGET_ADDRESS_LEN_REQ for this region */
		ret = mt7927_mcu_init_download(dev, addr, len, mode);
		if (ret) {
			dev_err(&dev->pdev->dev, "[FW] Init download failed: %d\n", ret);
			goto out;
		}

		/* Transfer region data */
		while (offset < len) {
			chunk = min_t(size_t, FW_CHUNK_SIZE, len - offset);

			ret = mt7927_fw_scatter(dev, data + offset, chunk);
			if (ret) {
				dev_err(&dev->pdev->dev, "[FW] Scatter failed at %zu: %d\n",
					offset, ret);
				goto out;
			}
			offset += chunk;
		}

		dev_info(&dev->pdev->dev, "[FW] Region %d transferred\n", i);

		/* Move data pointer past this region */
		data += len;
		offset = 0;
	}

	/* Start firmware */
	ret = mt7927_mcu_start_firmware(dev, 0);
	if (ret) {
		dev_err(&dev->pdev->dev, "[FW] Start command failed: %d\n", ret);
		goto out;
	}

	/* Wait for ready */
	ret = mt7927_wait_fw_ready(dev);
	if (ret)
		goto out;

	dev->fw_loaded = true;
	dev_info(&dev->pdev->dev, "[FW] ========== Firmware Loaded! ==========\n\n");

out:
	release_firmware(fw);
	return ret;
}

/*
 * Full firmware loading sequence: patch then RAM
 */
static int mt7927_load_firmware(struct mt7927_dev *dev)
{
	int ret;

	/* Step 1: Load ROM patch */
	ret = mt7927_load_patch(dev);
	if (ret) {
		dev_warn(&dev->pdev->dev, "[FW] Patch load failed, trying RAM anyway\n");
		/* Continue - some firmware may work without patch */
	}

	/* Step 2: Load main firmware */
	ret = mt7927_load_ram(dev);
	if (ret)
		return ret;

	return 0;
}

/* =============================================================================
 * Probe/Remove
 * =============================================================================
 */

static int mt7927_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mt7927_dev *dev;
	int ret;

	dev_info(&pdev->dev, "\n############################################\n");
	dev_info(&pdev->dev, "# MT7927 WiFi 7 Driver v%s\n", DRV_VERSION);
	dev_info(&pdev->dev, "############################################\n");

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	dev->mcu_seq = 1;
	pci_set_drvdata(pdev, dev);

	ret = pcim_enable_device(pdev);
	if (ret)
		goto err_free;

	ret = pcim_iomap_regions(pdev, BIT(0), DRV_NAME);
	if (ret)
		goto err_free;

	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free;

	dev->regs = pcim_iomap_table(pdev)[0];
	dev->regs_len = pci_resource_len(pdev, 0);

	ret = mt7927_power_handoff(dev);
	if (ret)
		dev_warn(&pdev->dev, "Power handoff issue\n");

	ret = mt7927_wfsys_reset(dev);
	if (ret)
		dev_warn(&pdev->dev, "WFSYS reset issue\n");

	ret = mt7927_conninfra_wakeup(dev);
	if (ret)
		dev_warn(&pdev->dev, "ConnInfra issue\n");

	/* Interrupt setup MUST happen before DMA init */
	mt7927_irq_setup(dev);

	ret = mt7927_dma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "DMA init failed\n");
		goto err_free;
	}

	if (dev->conninfra_ready && dev->dma_ready) {
		ret = mt7927_load_firmware(dev);
		if (ret)
			dev_warn(&pdev->dev, "FW loading failed\n");
	}

	dev_info(&pdev->dev, "\n=== Summary ===\n");
	dev_info(&pdev->dev, "  ConnInfra: %s\n", dev->conninfra_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  DMA: %s\n", dev->dma_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  Firmware: %s\n", dev->fw_loaded ? "LOADED" : "NO");

	return 0;

err_free:
	kfree(dev);
	return ret;
}

static void mt7927_remove(struct pci_dev *pdev)
{
	struct mt7927_dev *dev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "MT7927 unloading\n");
	if (dev) {
		mt7927_dma_cleanup(dev);
		kfree(dev);
	}
}

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
MODULE_DESCRIPTION("MediaTek MT7927 WiFi 7 driver");
MODULE_VERSION(DRV_VERSION);
MODULE_FIRMWARE(MT6639_FIRMWARE_PATCH);
MODULE_FIRMWARE(MT6639_FIRMWARE_RAM);
