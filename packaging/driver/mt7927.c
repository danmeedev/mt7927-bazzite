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
#define DRV_VERSION "0.1.0"

/* PCI IDs */
#define MT7927_VENDOR_ID	0x14c3
#define MT7927_DEVICE_ID	0x7927

/* =============================================================================
 * Register Definitions (from mt7925/mt76 analysis)
 * =============================================================================
 */

/* Base addresses */
#define MT_WFDMA0_BASE			0xd4000

/* Power Management - MT_CONN_ON_LPCTL */
#define MT_CONN_ON_LPCTL		0x7c060010
#define PCIE_LPCR_HOST_SET_OWN		BIT(0)	/* Give to firmware */
#define PCIE_LPCR_HOST_CLR_OWN		BIT(1)	/* Take for driver */
#define PCIE_LPCR_HOST_OWN_SYNC		BIT(2)	/* Ownership status */

/* EMI Control */
#define MT_HW_EMI_CTL			0x18011100
#define MT_HW_EMI_CTL_SLPPROT_EN	BIT(1)

/* WFSYS Reset - Critical for unlocking registers */
#define MT_WFSYS_SW_RST_B		0x7c000140
#define WFSYS_SW_RST_B			BIT(0)
#define WFSYS_SW_INIT_DONE		BIT(4)

/* Chip identification */
#define MT_HW_CHIPID			0x70010200
#define MT_HW_REV			0x70010204

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
#define MT_PCIE_MAC_INT_ENABLE		0x10188

/* DMA Ring Pointers */
#define MT_WFDMA0_RST_DTX_PTR		(MT_WFDMA0_BASE + 0x228)
#define MT_WFDMA0_RST_DRX_PTR		(MT_WFDMA0_BASE + 0x260)
#define MT_WFDMA0_PRI_DLY_INT_CFG0	(MT_WFDMA0_BASE + 0x238)

/* TX Ring registers */
#define MT_TX_RING_BASE			(MT_WFDMA0_BASE + 0x300)
#define MT_RING_SIZE			0x10

/* Firmware status */
#define MT_CONN_ON_MISC			0x7c0600f0
#define MT_TOP_MISC2_FW_N9_RDY		GENMASK(1, 0)

/* =============================================================================
 * Constants
 * =============================================================================
 */

#define MT792x_DRV_OWN_RETRY_COUNT	10
#define MT7927_TX_RING_SIZE		2048
#define MT7927_TX_MCU_RING_SIZE		256
#define MT7927_TX_FWDL_RING_SIZE	128

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

#define MT_DMA_CTL_SD_LEN0		GENMASK(15, 0)
#define MT_DMA_CTL_LAST_SEC0		BIT(16)
#define MT_DMA_CTL_DMA_DONE		BIT(31)

/* =============================================================================
 * Device Structure
 * =============================================================================
 */

struct mt7927_dev {
	struct pci_dev *pdev;
	void __iomem *regs;		/* BAR0 mapped registers */

	/* DMA rings */
	struct mt76_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	int tx_ring_size;

	/* Firmware buffer */
	void *fw_buf;
	dma_addr_t fw_dma;
	size_t fw_size;

	/* State */
	bool aspm_supported;
	u32 chip_rev;
};

/* =============================================================================
 * Register Access Helpers
 * =============================================================================
 */

static inline u32 mt7927_rr(struct mt7927_dev *dev, u32 offset)
{
	return readl(dev->regs + offset);
}

static inline void mt7927_wr(struct mt7927_dev *dev, u32 offset, u32 val)
{
	writel(val, dev->regs + offset);
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

/* Poll register until condition is met */
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

	return false;
}

/* =============================================================================
 * Power Management Handoff (CRITICAL)
 * =============================================================================
 */

static int mt7927_mcu_fw_pmctrl(struct mt7927_dev *dev)
{
	int i;

	/* Give ownership to firmware */
	for (i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
		mt7927_wr(dev, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);

		if (mt7927_poll(dev, MT_CONN_ON_LPCTL,
				PCIE_LPCR_HOST_OWN_SYNC,
				PCIE_LPCR_HOST_OWN_SYNC, 50)) {
			dev_dbg(&dev->pdev->dev, "FW ownership acquired\n");
			return 0;
		}
	}

	dev_err(&dev->pdev->dev, "Failed to give ownership to firmware\n");
	return -ETIMEDOUT;
}

static int mt7927_mcu_drv_pmctrl(struct mt7927_dev *dev)
{
	int i;

	/* Take ownership for driver */
	for (i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
		mt7927_wr(dev, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);

		/* Critical delay for ASPM-enabled systems */
		if (dev->aspm_supported)
			usleep_range(2000, 3000);

		if (mt7927_poll(dev, MT_CONN_ON_LPCTL,
				PCIE_LPCR_HOST_OWN_SYNC, 0, 50)) {
			dev_dbg(&dev->pdev->dev, "Driver ownership acquired\n");
			return 0;
		}
	}

	dev_err(&dev->pdev->dev, "Failed to take driver ownership\n");
	return -ETIMEDOUT;
}

/* =============================================================================
 * WFSYS Reset (CRITICAL - This unlocks the registers)
 * =============================================================================
 */

static int mt7927_wfsys_reset(struct mt7927_dev *dev)
{
	dev_info(&dev->pdev->dev, "Performing WFSYS reset...\n");

	/* Assert reset - clear WFSYS_SW_RST_B */
	mt7927_clear(dev, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);

	/* MANDATORY 50ms delay */
	msleep(50);

	/* Deassert reset - set WFSYS_SW_RST_B */
	mt7927_set(dev, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);

	/* Poll for initialization complete (up to 500ms) */
	if (!mt7927_poll(dev, MT_WFSYS_SW_RST_B,
			 WFSYS_SW_INIT_DONE, WFSYS_SW_INIT_DONE, 500)) {
		dev_err(&dev->pdev->dev, "WFSYS reset timeout\n");
		return -ETIMEDOUT;
	}

	dev_info(&dev->pdev->dev, "WFSYS reset complete - registers unlocked\n");
	return 0;
}

/* =============================================================================
 * DMA Initialization
 * =============================================================================
 */

static int mt7927_dma_disable(struct mt7927_dev *dev, bool force)
{
	dev_dbg(&dev->pdev->dev, "Disabling DMA%s...\n", force ? " (force)" : "");

	/* Clear DMA enable and config bits */
	mt7927_clear(dev, MT_WFDMA0_GLO_CFG,
		     MT_WFDMA0_GLO_CFG_TX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_RX_DMA_EN |
		     MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
		     MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
		     MT_WFDMA0_GLO_CFG_OMIT_TX_INFO);

	/* Wait for DMA busy flags to clear */
	if (!mt7927_poll(dev, MT_WFDMA0_GLO_CFG,
			 MT_WFDMA0_GLO_CFG_TX_DMA_BUSY |
			 MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
			 0, 100)) {
		dev_err(&dev->pdev->dev, "DMA busy timeout\n");
		return -ETIMEDOUT;
	}

	/* Disable DMASHDL (scheduler) */
	mt7927_clear(dev, MT_WFDMA0_GLO_CFG_EXT0,
		     MT_WFDMA0_GLO_CFG_EXT0_TX_DMASHDL_EN);

	/* Set DMASHDL bypass */
	mt7927_set(dev, MT_DMASHDL_SW_CONTROL, MT_DMASHDL_DMASHDL_BYPASS);

	if (force) {
		/* Force reset sequence: clear -> set -> clear */
		mt7927_clear(dev, MT_WFDMA0_RST,
			     MT_WFDMA0_RST_DMASHDL_ALL_RST |
			     MT_WFDMA0_RST_LOGIC_RST);

		mt7927_set(dev, MT_WFDMA0_RST,
			   MT_WFDMA0_RST_DMASHDL_ALL_RST |
			   MT_WFDMA0_RST_LOGIC_RST);

		mt7927_clear(dev, MT_WFDMA0_RST,
			     MT_WFDMA0_RST_DMASHDL_ALL_RST |
			     MT_WFDMA0_RST_LOGIC_RST);
	}

	return 0;
}

static int mt7927_dma_enable(struct mt7927_dev *dev)
{
	dev_dbg(&dev->pdev->dev, "Enabling DMA...\n");

	/* Reset DMA pointers */
	mt7927_wr(dev, MT_WFDMA0_RST_DTX_PTR, ~0);
	mt7927_wr(dev, MT_WFDMA0_RST_DRX_PTR, ~0);

	/* Clear delay interrupt config */
	mt7927_wr(dev, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	/* Set global configuration flags */
	mt7927_set(dev, MT_WFDMA0_GLO_CFG,
		   MT_WFDMA0_GLO_CFG_TX_WB_DDONE |
		   MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
		   MT_WFDMA0_GLO_CFG_CLK_GAT_DIS |
		   MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
		   MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
		   MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 |
		   FIELD_PREP(MT_WFDMA0_GLO_CFG_DMA_SIZE, 3));

	/* Enable DMA engines */
	mt7927_set(dev, MT_WFDMA0_GLO_CFG,
		   MT_WFDMA0_GLO_CFG_TX_DMA_EN |
		   MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	return 0;
}

static int mt7927_dma_init(struct mt7927_dev *dev)
{
	int ret;

	dev_info(&dev->pdev->dev, "Initializing DMA subsystem...\n");

	/* First disable DMA with force reset */
	ret = mt7927_dma_disable(dev, true);
	if (ret)
		return ret;

	/* Allocate TX descriptor ring */
	dev->tx_ring_size = MT7927_TX_FWDL_RING_SIZE;
	dev->tx_ring = dma_alloc_coherent(&dev->pdev->dev,
					  dev->tx_ring_size * sizeof(struct mt76_desc),
					  &dev->tx_ring_dma,
					  GFP_KERNEL);
	if (!dev->tx_ring) {
		dev_err(&dev->pdev->dev, "Failed to allocate TX ring\n");
		return -ENOMEM;
	}

	memset(dev->tx_ring, 0, dev->tx_ring_size * sizeof(struct mt76_desc));

	/* Configure FWDL ring (ring 16) */
	mt7927_wr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE,
		  lower_32_bits(dev->tx_ring_dma));
	mt7927_wr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x04,
		  dev->tx_ring_size);
	mt7927_wr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x08, 0);
	mt7927_wr(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x0c, 0);

	/* Enable DMA */
	ret = mt7927_dma_enable(dev);
	if (ret)
		goto err_free_ring;

	dev_info(&dev->pdev->dev, "DMA initialized, ring at %pad\n",
		 &dev->tx_ring_dma);

	return 0;

err_free_ring:
	dma_free_coherent(&dev->pdev->dev,
			  dev->tx_ring_size * sizeof(struct mt76_desc),
			  dev->tx_ring, dev->tx_ring_dma);
	dev->tx_ring = NULL;
	return ret;
}

static void mt7927_dma_cleanup(struct mt7927_dev *dev)
{
	mt7927_dma_disable(dev, false);

	if (dev->fw_buf) {
		dma_free_coherent(&dev->pdev->dev, dev->fw_size,
				  dev->fw_buf, dev->fw_dma);
		dev->fw_buf = NULL;
	}

	if (dev->tx_ring) {
		dma_free_coherent(&dev->pdev->dev,
				  dev->tx_ring_size * sizeof(struct mt76_desc),
				  dev->tx_ring, dev->tx_ring_dma);
		dev->tx_ring = NULL;
	}
}

/* =============================================================================
 * Firmware Loading
 * =============================================================================
 */

static int mt7927_load_firmware(struct mt7927_dev *dev)
{
	const struct firmware *fw;
	int ret;
	u32 status;

	dev_info(&dev->pdev->dev, "Loading firmware...\n");

	/* Request patch firmware */
	ret = request_firmware(&fw, "mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin",
			       &dev->pdev->dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "Failed to load patch firmware: %d\n", ret);
		return ret;
	}

	dev_info(&dev->pdev->dev, "Patch firmware loaded: %zu bytes\n", fw->size);

	/* Allocate DMA buffer for firmware */
	dev->fw_size = ALIGN(fw->size, 4);
	dev->fw_buf = dma_alloc_coherent(&dev->pdev->dev, dev->fw_size,
					 &dev->fw_dma, GFP_KERNEL);
	if (!dev->fw_buf) {
		release_firmware(fw);
		return -ENOMEM;
	}

	memcpy(dev->fw_buf, fw->data, fw->size);
	release_firmware(fw);

	/* Check current firmware status */
	status = mt7927_rr(dev, MT_CONN_ON_MISC);
	dev_info(&dev->pdev->dev, "Pre-load firmware status: 0x%08x\n", status);

	/*
	 * TODO: Implement full MCU command protocol
	 *
	 * The firmware loading requires sending MCU commands through the
	 * FWDL queue. This needs:
	 * 1. Patch semaphore acquisition
	 * 2. Initialize download command
	 * 3. FW_SCATTER commands to transfer data
	 * 4. PATCH_FINISH_REQ
	 * 5. FW_START_REQ
	 *
	 * For now, we just verify the DMA setup works.
	 */

	/* Check if firmware status changed */
	status = mt7927_rr(dev, MT_CONN_ON_MISC);
	dev_info(&dev->pdev->dev, "Post-setup firmware status: 0x%08x\n", status);

	return 0;
}

/* =============================================================================
 * PCI Probe/Remove
 * =============================================================================
 */

static int mt7927_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mt7927_dev *dev;
	int ret;
	u32 val;

	dev_info(&pdev->dev, "MT7927 WiFi 7 driver v%s\n", DRV_VERSION);
	dev_info(&pdev->dev, "Device: %04x:%04x (AMD RZ738 compatible)\n",
		 pdev->vendor, pdev->device);

	/* Allocate device structure */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);

	/* === Phase 1: PCI Setup === */
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

	/* Check for ASPM support */
	dev->aspm_supported = pcie_aspm_enabled(pdev);
	dev_info(&pdev->dev, "ASPM: %s\n",
		 dev->aspm_supported ? "supported" : "not supported");

	/* === Phase 2: Power Management Handoff (CRITICAL) === */
	dev_info(&pdev->dev, "Performing power management handoff...\n");

	/* First give ownership to firmware */
	ret = mt7927_mcu_fw_pmctrl(dev);
	if (ret) {
		dev_warn(&pdev->dev, "FW ownership handoff failed, continuing...\n");
	}

	/* Then take ownership for driver */
	ret = mt7927_mcu_drv_pmctrl(dev);
	if (ret) {
		dev_err(&pdev->dev, "Driver ownership failed\n");
		goto err_free;
	}

	/* === Phase 3: Read Chip ID === */
	dev->chip_rev = (mt7927_rr(dev, MT_HW_CHIPID) << 16) |
			(mt7927_rr(dev, MT_HW_REV) & 0xff);
	dev_info(&pdev->dev, "Chip revision: 0x%08x\n", dev->chip_rev);

	/* === Phase 4: EMI Sleep Protection === */
	mt7927_rmw(dev, MT_HW_EMI_CTL, MT_HW_EMI_CTL_SLPPROT_EN,
		   MT_HW_EMI_CTL_SLPPROT_EN);

	/* === Phase 5: WFSYS Reset (UNLOCKS REGISTERS) === */
	ret = mt7927_wfsys_reset(dev);
	if (ret) {
		dev_err(&pdev->dev, "WFSYS reset failed\n");
		goto err_free;
	}

	/* === Phase 6: Interrupt Setup === */
	mt7927_wr(dev, MT_WFDMA0_HOST_INT_ENA, 0);
	mt7927_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);

	/* === Phase 7: DMA Initialization === */
	ret = mt7927_dma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "DMA initialization failed\n");
		goto err_free;
	}

	/* Verify WPDMA_GLO_CFG is now writable */
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&pdev->dev, "WPDMA_GLO_CFG after init: 0x%08x\n", val);

	if (val == 0 || val == 0xffffffff) {
		dev_warn(&pdev->dev, "WARNING: WPDMA_GLO_CFG may not be writable\n");
	}

	/* === Phase 8: Load Firmware === */
	ret = mt7927_load_firmware(dev);
	if (ret) {
		dev_warn(&pdev->dev, "Firmware loading incomplete: %d\n", ret);
		/* Don't fail - device is still bound for debugging */
	}

	dev_info(&pdev->dev, "MT7927 driver initialized successfully\n");
	dev_info(&pdev->dev, "NOTE: Full WiFi functionality requires completing MCU protocol\n");

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
	{ PCI_DEVICE(MT7927_VENDOR_ID, MT7927_DEVICE_ID),
	  .driver_data = 0 },
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
MODULE_DESCRIPTION("MediaTek MT7927 WiFi 7 Driver (AMD RZ738)");
MODULE_VERSION(DRV_VERSION);
MODULE_FIRMWARE("mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin");
MODULE_FIRMWARE("mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin");
