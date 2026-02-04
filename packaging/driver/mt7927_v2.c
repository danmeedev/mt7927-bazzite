// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 WiFi 7 Linux Driver v2.4.0
 *
 * CRITICAL DISCOVERY: MT7927 uses MT6639 architecture (Gen4m), NOT MT7925 (Gen4).
 * This requires ConnInfra subsystem initialization before MCU communication.
 *
 * v2.4.0 Changes:
 * - Removed ROM ready check (not needed for Gen4m)
 * - Added proper DMA ring initialization
 * - Added firmware download infrastructure
 *
 * Based on analysis of:
 * - MediaTek gen4m driver: https://github.com/Fede2782/MTK_modules
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
#define DRV_VERSION "2.4.0"

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

/* Chip identification */
#define MT6639_CHIP_ID			0x6639
#define MT6639_CONNINFRA_VERSION_ID	0x03010001
#define MT6639_CONNINFRA_VERSION_ID_E2	0x03010002
#define MT6639_WF_VERSION_ID		0x03010001

/* ConnInfra base addresses */
#define CONNAC3X_CONN_CFG_ON_BASE	0x7C060000
#define CONN_INFRA_CFG_BASE		0x830C0000

/* Host CSR registers (via BAR0 direct access) */
#define HOST_CSR_BASE			0x7000
#define HOST_CSR_DRIVER_OWN_INFO	(HOST_CSR_BASE + 0x000)
#define HOST_CSR_MCU_PORG_COUNT		(HOST_CSR_BASE + 0x104)
#define HOST_CSR_RGU			(HOST_CSR_BASE + 0x108)
#define HOST_CSR_HIF_BUSY		(HOST_CSR_BASE + 0x10C)
#define HOST_CSR_PINMUX_MON		(HOST_CSR_BASE + 0x110)
#define HOST_CSR_MCU_PWR_STAT		(HOST_CSR_BASE + 0x114)
#define HOST_CSR_FW_OWN_SET		(HOST_CSR_BASE + 0x118)
#define HOST_CSR_MCU_SW_MAILBOX_0	(HOST_CSR_BASE + 0x11C)
#define HOST_CSR_MCU_SW_MAILBOX_1	(HOST_CSR_BASE + 0x120)

/* LPCTL bits */
#define PCIE_LPCR_HOST_SET_OWN		BIT(0)
#define PCIE_LPCR_HOST_CLR_OWN		BIT(1)
#define PCIE_LPCR_HOST_OWN_SYNC		BIT(2)

/*
 * MT7925/MT7927 Fixed Map - addresses are statically mapped in BAR0
 */
#define FIXED_MAP_CONN_INFRA_HOST	0x0e0000    /* 0x7c060000 -> 0xe0000 */
#define FIXED_MAP_CONN_INFRA		0x0f0000    /* 0x7c000000 -> 0xf0000 */
#define FIXED_MAP_WFDMA			0x0d0000    /* 0x7c020000 -> 0xd0000 */
#define FIXED_MAP_WF_TOP_MISC_ON	0x0c0000    /* 0x81020000 -> 0xc0000 */

/* Direct BAR0 offsets for key registers */
#define CONN_INFRA_HOST_BAR_OFS		0x0e0000
#define MT_LPCTL_BAR_OFS		(CONN_INFRA_HOST_BAR_OFS + 0x10)   /* 0xe0010 */
#define MT_CONN_MISC_BAR_OFS		(CONN_INFRA_HOST_BAR_OFS + 0xf0)  /* 0xe00f0 */

/* WFSYS Reset Register */
#define MT_WFSYS_RST_BAR_OFS		(FIXED_MAP_CONN_INFRA + 0x140)    /* 0xf0140 */
#define WFSYS_SW_RST_B			BIT(0)
#define WFSYS_SW_INIT_DONE		BIT(4)

/* HIF Remap register */
#define MT_HIF_REMAP_L1			0x155024
#define MT_HIF_REMAP_L1_MASK		GENMASK(31, 16)
#define MT_HIF_REMAP_L1_OFFSET		GENMASK(15, 0)
#define MT_HIF_REMAP_BASE		0x130000
#define MT_HIF_REMAP_SIZE		0x10000

/* Chip ID registers */
#define CONNAC3X_TOP_HCR		0x88000000
#define CONNAC3X_TOP_HVR		0x88000004

/* =============================================================================
 * WFDMA Register Definitions (Critical for DMA)
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
#define MT_MCU_CMD_WAKE_RX_PCIE		BIT(0)
#define MT_MCU_CMD_STOP_DMA		BIT(2)
#define MT_MCU_CMD_RESET_DONE		BIT(3)
#define MT_MCU_CMD_RECOVERY_DONE	BIT(4)
#define MT_MCU_CMD_NORMAL_STATE		BIT(5)

/* TX Ring Registers - base + (ring_idx * 0x10) */
#define MT_TX_RING_BASE			(MT_WFDMA0_BASE + 0x300)
#define MT_TX_RING_SIZE			0x10

/* Ring register offsets */
#define MT_RING_BASE			0x00	/* Base address (low) */
#define MT_RING_CNT			0x04	/* Ring size */
#define MT_RING_CIDX			0x08	/* CPU index */
#define MT_RING_DIDX			0x0c	/* DMA index */

/* RX Ring Registers */
#define MT_RX_RING_BASE			(MT_WFDMA0_BASE + 0x500)
#define MT_RX_RING_SIZE			0x10

/* DMASHDL bypass */
#define MT_DMASHDL_SW_CONTROL		0x7c026004   /* Physical address */
#define FIXED_MAP_DMASHDL		0x0d6000     /* 0x7c026000 -> 0xd6000 */
#define MT_DMASHDL_SW_CONTROL_OFS	(FIXED_MAP_DMASHDL + 0x04)
#define MT_DMASHDL_BYPASS		BIT(0)

/* Ring indices */
#define MT_TX_RING_DATA0		0	/* TX Data Band 0 */
#define MT_TX_RING_MCU_WM		15	/* MCU Command (WM) */
#define MT_TX_RING_FWDL		16	/* Firmware Download */

#define MT_RX_RING_MCU			0	/* RX MCU Events */
#define MT_RX_RING_DATA			2	/* RX Data */

/* Ring sizes */
#define MT_TX_RING_SIZE_DATA		2048
#define MT_TX_RING_SIZE_MCU		256
#define MT_TX_RING_SIZE_FWDL		128
#define MT_RX_RING_SIZE_MCU		512
#define MT_RX_RING_SIZE_DATA		1536

/* DMA Descriptor */
struct mt76_desc {
	__le32 buf0;	/* Buffer physical address */
	__le32 ctrl;	/* Control: length, flags */
	__le32 buf1;	/* Buffer 1 for scatter-gather */
	__le32 info;	/* Additional info */
} __packed __aligned(4);

#define MT_DMA_CTL_SD_LEN0		GENMASK(15, 0)
#define MT_DMA_CTL_LAST_SEC0		BIT(16)
#define MT_DMA_CTL_BURST		BIT(17)
#define MT_DMA_CTL_DMA_DONE		BIT(31)

/* Firmware paths */
#define MT6639_FIRMWARE_PATCH	"mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin"
#define MT6639_FIRMWARE_RAM	"mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin"

/* Timeouts */
#define POLL_TIMEOUT_US			1000000
#define CONNINFRA_WAKEUP_TIMEOUT_MS	50
#define DRV_OWN_TIMEOUT_MS		500
#define DMA_BUSY_TIMEOUT_MS		100

/* =============================================================================
 * Device Structure
 * =============================================================================
 */

struct mt7927_ring {
	struct mt76_desc *desc;		/* Descriptor ring virtual address */
	dma_addr_t desc_dma;		/* Descriptor ring DMA address */
	int size;			/* Number of descriptors */
	int idx;			/* Current index */
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

	/* DMA rings */
	struct mt7927_ring tx_ring[32];
	struct mt7927_ring rx_ring[8];
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

static u32 mt7927_rr_remap(struct mt7927_dev *dev, u32 addr)
{
	u32 remap_addr, offset, val;

	remap_addr = (addr & MT_HIF_REMAP_L1_MASK) >> 16;
	offset = addr & MT_HIF_REMAP_L1_OFFSET;

	mt7927_wr(dev, MT_HIF_REMAP_L1, remap_addr);
	udelay(1);
	val = mt7927_rr(dev, MT_HIF_REMAP_BASE + offset);

	return val;
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
	/* Write ring base address (low 32 bits) */
	mt7927_wr(dev, base_reg + MT_RING_BASE, ring->desc_dma);

	/* Write ring size */
	mt7927_wr(dev, base_reg + MT_RING_CNT, ring->size);

	/* Reset CPU index */
	mt7927_wr(dev, base_reg + MT_RING_CIDX, 0);

	dev_info(&dev->pdev->dev, "  Ring setup at 0x%x: base=0x%llx, cnt=%d\n",
		 base_reg, (unsigned long long)ring->desc_dma, ring->size);
}

/* =============================================================================
 * DMA Initialization - Full Sequence
 * =============================================================================
 */

/*
 * Phase A: Disable DMA and perform force reset
 */
static int mt7927_dma_disable(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[DMA] Phase A: Disabling DMA...\n");

	/* Read current state */
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[DMA] GLO_CFG before disable: 0x%08x\n", val);

	/* Clear DMA enable bits */
	mt7927_clear(dev, MT_WFDMA0_GLO_CFG,
		     MT_WFDMA0_GLO_CFG_TX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_RX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
		     MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
		     MT_WFDMA0_GLO_CFG_OMIT_RX_INFO |
		     MT_WFDMA0_GLO_CFG_OMIT_TX_INFO);

	/* Poll for TX/RX DMA busy flags to clear */
	for (i = 0; i < DMA_BUSY_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
		if (!(val & (MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY))) {
			dev_info(&dev->pdev->dev, "[DMA] DMA idle: 0x%08x\n", val);
			break;
		}
		msleep(1);
	}

	if (i >= DMA_BUSY_TIMEOUT_MS) {
		dev_warn(&dev->pdev->dev, "[DMA] Warning: DMA still busy: 0x%08x\n", val);
	}

	/* Disable DMASHDL */
	dev_info(&dev->pdev->dev, "[DMA] Disabling DMASHDL...\n");
	mt7927_clear(dev, MT_WFDMA0_GLO_CFG_EXT0, MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN);

	/* Enable DMASHDL bypass via fixed map */
	val = mt7927_rr(dev, MT_DMASHDL_SW_CONTROL_OFS);
	dev_info(&dev->pdev->dev, "[DMA] DMASHDL_SW_CONTROL before: 0x%08x\n", val);
	mt7927_set(dev, MT_DMASHDL_SW_CONTROL_OFS, MT_DMASHDL_BYPASS);
	val = mt7927_rr(dev, MT_DMASHDL_SW_CONTROL_OFS);
	dev_info(&dev->pdev->dev, "[DMA] DMASHDL_SW_CONTROL after: 0x%08x\n", val);

	/* Force reset sequence: clear-set-clear */
	dev_info(&dev->pdev->dev, "[DMA] Force reset sequence...\n");
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_set(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[DMA] GLO_CFG after reset: 0x%08x\n", val);

	return 0;
}

/*
 * Phase B: Allocate and setup DMA rings
 */
static int mt7927_dma_rings_init(struct mt7927_dev *dev)
{
	int ret;

	dev_info(&dev->pdev->dev, "[DMA] Phase B: Allocating DMA rings...\n");

	/* Allocate TX rings */
	dev_info(&dev->pdev->dev, "[DMA] TX Ring 16 (FWDL):\n");
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_FWDL], MT_TX_RING_SIZE_FWDL);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE,
			  &dev->tx_ring[MT_TX_RING_FWDL]);

	dev_info(&dev->pdev->dev, "[DMA] TX Ring 15 (MCU WM):\n");
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_MCU_WM], MT_TX_RING_SIZE_MCU);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE,
			  &dev->tx_ring[MT_TX_RING_MCU_WM]);

	/* Allocate RX ring for MCU events */
	dev_info(&dev->pdev->dev, "[DMA] RX Ring 0 (MCU Events):\n");
	ret = mt7927_ring_alloc(dev, &dev->rx_ring[MT_RX_RING_MCU], MT_RX_RING_SIZE_MCU);
	if (ret)
		return ret;
	mt7927_ring_setup(dev, MT_RX_RING_BASE + MT_RX_RING_MCU * MT_RX_RING_SIZE,
			  &dev->rx_ring[MT_RX_RING_MCU]);

	return 0;
}

/*
 * Phase C: Enable DMA with proper configuration
 */
static int mt7927_dma_enable(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[DMA] Phase C: Enabling DMA...\n");

	/* Reset TX/RX pointers */
	mt7927_wr(dev, MT_WFDMA0_RST_DTX_PTR, ~0);
	mt7927_wr(dev, MT_WFDMA0_RST_DRX_PTR, ~0);

	/* Clear delay interrupts */
	mt7927_wr(dev, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	/* Set global config with all required bits */
	val = MT_WFDMA0_GLO_CFG_TX_WB_DDONE |
	      MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
	      MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
	      MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
	      MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	      MT_WFDMA0_GLO_CFG_CLK_GAT_DIS |
	      (3 << 4);  /* DMA_SIZE = 3 */

	mt7927_wr(dev, MT_WFDMA0_GLO_CFG, val);

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[DMA] GLO_CFG after setup: 0x%08x\n", val);

	/* Enable DMA engines */
	mt7927_set(dev, MT_WFDMA0_GLO_CFG,
		   MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[DMA] GLO_CFG final: 0x%08x\n", val);

	/* Verify DMA enabled */
	if ((val & (MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN)) ==
	    (MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN)) {
		dev_info(&dev->pdev->dev, "[DMA] TX and RX DMA enabled successfully!\n");
		dev->dma_ready = true;
		return 0;
	}

	dev_warn(&dev->pdev->dev, "[DMA] DMA enable may have failed\n");
	return -EIO;
}

/*
 * Full DMA initialization sequence
 */
static int mt7927_dma_init(struct mt7927_dev *dev)
{
	int ret;

	dev_info(&dev->pdev->dev, "\n[DMA] ========== DMA Initialization ==========\n");

	ret = mt7927_dma_disable(dev);
	if (ret)
		return ret;

	ret = mt7927_dma_rings_init(dev);
	if (ret)
		return ret;

	ret = mt7927_dma_enable(dev);
	if (ret)
		return ret;

	dev_info(&dev->pdev->dev, "[DMA] ========== DMA Init Complete ==========\n\n");

	return 0;
}

static void mt7927_dma_cleanup(struct mt7927_dev *dev)
{
	int i;

	/* Free TX rings */
	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++)
		mt7927_ring_free(dev, &dev->tx_ring[i]);

	/* Free RX rings */
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

	dev_info(&dev->pdev->dev, "[WFSYS] Starting WiFi subsystem reset...\n");

	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	dev_info(&dev->pdev->dev, "[WFSYS] Reset reg before: 0x%08x\n", val);

	/* Assert reset */
	val &= ~WFSYS_SW_RST_B;
	mt7927_wr(dev, MT_WFSYS_RST_BAR_OFS, val);

	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	dev_info(&dev->pdev->dev, "[WFSYS] After assert: 0x%08x\n", val);

	/* MANDATORY 50ms delay */
	msleep(50);

	/* Deassert reset */
	val |= WFSYS_SW_RST_B;
	mt7927_wr(dev, MT_WFSYS_RST_BAR_OFS, val);

	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	dev_info(&dev->pdev->dev, "[WFSYS] After deassert: 0x%08x\n", val);

	/* Poll for INIT_DONE */
	for (i = 0; i < 500; i++) {
		val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);

		if (val & WFSYS_SW_INIT_DONE) {
			dev_info(&dev->pdev->dev, "[WFSYS] INIT_DONE! Reset reg: 0x%08x\n", val);
			return 0;
		}

		if (i % 50 == 0)
			dev_info(&dev->pdev->dev, "[WFSYS] Waiting for INIT_DONE: 0x%08x\n", val);

		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[WFSYS] INIT_DONE timeout - reset reg: 0x%08x\n", val);
	return -ETIMEDOUT;
}

static int mt7927_power_handoff(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[PWR] Power management handoff sequence...\n");

	val = mt7927_rr(dev, MT_LPCTL_BAR_OFS);
	dev_info(&dev->pdev->dev, "[PWR] LPCTL initial: 0x%08x\n", val);

	/* Give ownership TO firmware */
	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_SET_OWN);

	for (i = 0; i < 100; i++) {
		val = mt7927_rr(dev, MT_LPCTL_BAR_OFS);
		if (val & PCIE_LPCR_HOST_OWN_SYNC) {
			dev_info(&dev->pdev->dev, "[PWR] FW ownership confirmed: 0x%08x\n", val);
			break;
		}
		msleep(1);
	}

	/* Wait for FW to stabilize */
	msleep(50);

	/* Take ownership BACK */
	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_CLR_OWN);

	for (i = 0; i < 500; i++) {
		val = mt7927_rr(dev, MT_LPCTL_BAR_OFS);
		if (!(val & PCIE_LPCR_HOST_OWN_SYNC)) {
			dev_info(&dev->pdev->dev, "[PWR] Driver ownership acquired: 0x%08x\n", val);
			return 0;
		}
		if (i % 50 == 0)
			dev_info(&dev->pdev->dev, "[PWR] Waiting for CLR_OWN: 0x%08x\n", val);
		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[PWR] Ownership timeout - LPCTL: 0x%08x\n", val);
	return -ETIMEDOUT;
}

static int mt7927_conninfra_wakeup(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[ConnInfra] Waking up ConnInfra subsystem...\n");

	/* Write wakeup trigger */
	mt7927_wr(dev, CONN_INFRA_HOST_BAR_OFS, 0x1);
	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_CLR_OWN);

	/* Poll for ConnInfra ready */
	for (i = 0; i < CONNINFRA_WAKEUP_TIMEOUT_MS; i++) {
		val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);

		if (val != 0 && val != 0xFFFFFFFF) {
			dev_info(&dev->pdev->dev, "[ConnInfra] Subsystem responded: 0x%08x\n", val);
			dev->conninfra_ready = true;
			return 0;
		}

		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[ConnInfra] Wakeup timeout - MISC=0x%08x\n", val);
	return -ETIMEDOUT;
}

/* =============================================================================
 * Register Dump (Diagnostic)
 * =============================================================================
 */

static void mt7927_dump_regs(struct mt7927_dev *dev)
{
	dev_info(&dev->pdev->dev, "[DUMP] Key registers:\n");
	dev_info(&dev->pdev->dev, "  WFDMA0 GLO_CFG: 0x%08x\n", mt7927_rr(dev, MT_WFDMA0_GLO_CFG));
	dev_info(&dev->pdev->dev, "  LPCTL: 0x%08x\n", mt7927_rr(dev, MT_LPCTL_BAR_OFS));
	dev_info(&dev->pdev->dev, "  CONN_MISC: 0x%08x\n", mt7927_rr(dev, MT_CONN_MISC_BAR_OFS));
	dev_info(&dev->pdev->dev, "  WFSYS_RST: 0x%08x\n", mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS));
	dev_info(&dev->pdev->dev, "  MCU_CMD: 0x%08x\n", mt7927_rr(dev, MT_MCU_CMD));

	/* TX Ring 16 (FWDL) status */
	dev_info(&dev->pdev->dev, "  TX16 CIDX: 0x%08x\n",
		 mt7927_rr(dev, MT_TX_RING_BASE + 16 * MT_TX_RING_SIZE + MT_RING_CIDX));
	dev_info(&dev->pdev->dev, "  TX16 DIDX: 0x%08x\n",
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
	dev_info(&pdev->dev, "# Device: %04x:%04x\n", pdev->vendor, pdev->device);
	dev_info(&pdev->dev, "############################################\n");

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

	dev->regs_len = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "  BAR0: %pR (size: 0x%llx)\n",
		 &pdev->resource[0], (unsigned long long)dev->regs_len);

	/* === Phase 2: Power Management Handoff === */
	dev_info(&pdev->dev, "\n=== Phase 2: Power Management Handoff ===\n");

	ret = mt7927_power_handoff(dev);
	if (ret)
		dev_warn(&pdev->dev, "Power handoff incomplete, continuing...\n");

	/* === Phase 3: WFSYS Reset === */
	dev_info(&pdev->dev, "\n=== Phase 3: WFSYS Reset ===\n");

	ret = mt7927_wfsys_reset(dev);
	if (ret)
		dev_warn(&pdev->dev, "WFSYS reset incomplete, continuing...\n");

	/* === Phase 4: ConnInfra Wakeup === */
	dev_info(&pdev->dev, "\n=== Phase 4: ConnInfra Wakeup ===\n");

	ret = mt7927_conninfra_wakeup(dev);
	if (ret)
		dev_warn(&pdev->dev, "ConnInfra wakeup failed, continuing...\n");

	/* === Phase 5: DMA Initialization === */
	dev_info(&pdev->dev, "\n=== Phase 5: DMA Initialization ===\n");

	ret = mt7927_dma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "DMA init failed: %d\n", ret);
		/* Continue for diagnostic purposes */
	}

	/* === Phase 6: Final Register Dump === */
	dev_info(&pdev->dev, "\n=== Phase 6: Final Status ===\n");
	mt7927_dump_regs(dev);

	/* === Summary === */
	dev_info(&pdev->dev, "\n=== Initialization Summary ===\n");
	dev_info(&pdev->dev, "  ConnInfra ready: %s\n", dev->conninfra_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  DMA ready: %s\n", dev->dma_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "\n");

	if (dev->conninfra_ready && dev->dma_ready) {
		dev_info(&pdev->dev, "*** CHIP READY FOR FIRMWARE DOWNLOAD ***\n");
		dev_info(&pdev->dev, "Next step: Implement firmware loading via Ring 16\n");
	} else {
		dev_info(&pdev->dev, "Initialization incomplete - check dmesg for details\n");
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
MODULE_FIRMWARE(MT6639_FIRMWARE_PATCH);
MODULE_FIRMWARE(MT6639_FIRMWARE_RAM);
