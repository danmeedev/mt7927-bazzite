// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 WiFi 7 Linux Driver v2.0
 *
 * CRITICAL DISCOVERY: MT7927 uses MT6639 architecture (Gen4m), NOT MT7925 (Gen4).
 * This requires ConnInfra subsystem initialization before MCU communication.
 *
 * Based on analysis of:
 * - MediaTek gen4m driver: https://github.com/Fede2782/MTK_modules
 * - Windows driver: mtkwlan.dat firmware uses MT6639 binaries
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
#define DRV_VERSION "2.0.0"

/* PCI IDs */
#define MT7927_VENDOR_ID	0x14c3
#define MT7927_DEVICE_ID	0x7927
#define MT6639_DEVICE_ID	0x6639

/* Module parameters */
static bool debug = true;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug output (default: true)");

/* =============================================================================
 * MT6639/MT7927 Register Definitions (from gen4m source)
 * =============================================================================
 */

/* Chip identification */
#define MT6639_CHIP_ID			0x6639
#define MT6639_CONNINFRA_VERSION_ID	0x03010001
#define MT6639_CONNINFRA_VERSION_ID_E2	0x03010002
#define MT6639_WF_VERSION_ID		0x03010001

/* ConnInfra base addresses - CRITICAL for Gen4m */
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
#define HOST_CSR_MCU_SW_MAILBOX_2	(HOST_CSR_BASE + 0x124)
#define HOST_CSR_MCU_SW_MAILBOX_3	(HOST_CSR_BASE + 0x128)

/* Address remapping registers */
#define CONN_HIF_ON_ADDR_REMAP1		(HOST_CSR_BASE + 0x00C)
#define CONN_HIF_ON_ADDR_REMAP2		(HOST_CSR_BASE + 0x010)

/* Low Power Control - MT6639 uses CONNAC3X base */
#define CONNAC3X_BN0_LPCTL_ADDR		(CONNAC3X_CONN_CFG_ON_BASE + 0x10)  /* 0x7C060010 */
#define CONNAC3X_BN0_IRQ_STAT_ADDR	(CONNAC3X_CONN_CFG_ON_BASE + 0x14)
#define CONNAC3X_BN0_IRQ_ENA_ADDR	(CONNAC3X_CONN_CFG_ON_BASE + 0x18)
#define CONNAC3X_CONN_CFG_ON_MISC_ADDR	(CONNAC3X_CONN_CFG_ON_BASE + 0xF0)
#define CONNAC3X_CONN_CFG_ON_EMI_ADDR	(CONNAC3X_CONN_CFG_ON_BASE + 0xD0C)

/* LPCTL bits */
#define PCIE_LPCR_HOST_SET_OWN		BIT(0)
#define PCIE_LPCR_HOST_CLR_OWN		BIT(1)
#define PCIE_LPCR_HOST_OWN_SYNC		BIT(2)

/* MCU Software Control Registers */
#define MCU_SW_CR_BASE			0x7C05B100
#define MT6639_EMI_SIZE_ADDR		(MCU_SW_CR_BASE + 0x01E0)
#define MT6639_MCIF_MD_STATE_ADDR	(MCU_SW_CR_BASE + 0x01E8)

/* ConnInfra wakeup register */
#define CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR	0x7C060000

/* ROM code ready status */
#define WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR	0x7C00124C
#define ROM_CODE_READY_VALUE		0x1D1E

/* PCIe2AP Remap for semaphore access */
#define CONN_INFRA_BUS_CR_PCIE2AP_REMAP_WF_0_54_ADDR	0x7C00F054

/* WFDMA registers (same as MT7925 but may have different offsets) */
#define MT_WFDMA0_BASE			0xD4000
#define MT_WFDMA0_GLO_CFG		(MT_WFDMA0_BASE + 0x208)
#define MT_WFDMA0_HOST_INT_ENA		(MT_WFDMA0_BASE + 0x204)
#define MT_WFDMA0_HOST_INT_STA		(MT_WFDMA0_BASE + 0x200)

/* WFDMA Global Config bits */
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN	BIT(0)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN	BIT(2)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO	BIT(28)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO	BIT(27)

/* HIF Remap register for accessing high addresses */
#define MT_HIF_REMAP_L1			0x1008c
#define MT_HIF_REMAP_L1_MASK		GENMASK(31, 16)
#define MT_HIF_REMAP_L1_OFFSET		GENMASK(15, 0)
#define MT_HIF_REMAP_BASE		0xe0000
#define MT_HIF_REMAP_SIZE		0x10000

/* Chip ID registers */
#define CONNAC3X_TOP_HCR		0x88000000
#define CONNAC3X_TOP_HVR		0x88000004

/* Patch start address for MT6639 */
#define MT6639_PATCH_START_ADDR		0x00900000
#define MT6639_REMAP_BASE_ADDR		0x7C500000
#define MT6639_PCIE2AP_REMAP_BASE_ADDR	0x60000

/* MCU Command Register */
#define MT_MCU_CMD			(MT_WFDMA0_BASE + 0x1F0)
#define MT_MCU_CMD_STOP_DMA		BIT(2)
#define MT_MCU_CMD_RESET_DONE		BIT(3)

/* Firmware paths */
#define MT6639_FIRMWARE_PATCH		"mediatek/mt7925/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin"
#define MT6639_FIRMWARE_RAM		"mediatek/mt7925/WIFI_RAM_CODE_MT6639_2_1.bin"

/* Timeouts */
#define POLL_TIMEOUT_US			1000000	/* 1 second */
#define CONNINFRA_WAKEUP_TIMEOUT_MS	50
#define ROM_READY_TIMEOUT_MS		500
#define DRV_OWN_TIMEOUT_MS		500

/* =============================================================================
 * Device Structure
 * =============================================================================
 */

struct mt7927_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_len;

	u32 chip_id;
	u32 chip_rev;
	u32 conninfra_version;

	bool aspm_supported;
	bool conninfra_ready;
	bool rom_ready;
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

/*
 * Remapped register access for high addresses (0x7cxxxxxx, 0x18xxxxxx, etc.)
 * Uses HIF_REMAP_L1 to access addresses outside direct BAR0 range.
 */
static u32 mt7927_rr_remap(struct mt7927_dev *dev, u32 addr)
{
	u32 remap_addr, offset, val;

	/* Calculate remap base and offset */
	remap_addr = (addr & MT_HIF_REMAP_L1_MASK) >> 16;
	offset = addr & MT_HIF_REMAP_L1_OFFSET;

	/* Set remap register */
	mt7927_wr(dev, MT_HIF_REMAP_L1, remap_addr);

	/* Small delay for remap to take effect */
	udelay(1);

	/* Read from remapped window */
	val = mt7927_rr(dev, MT_HIF_REMAP_BASE + offset);

	return val;
}

static void mt7927_wr_remap(struct mt7927_dev *dev, u32 addr, u32 val)
{
	u32 remap_addr, offset;

	remap_addr = (addr & MT_HIF_REMAP_L1_MASK) >> 16;
	offset = addr & MT_HIF_REMAP_L1_OFFSET;

	mt7927_wr(dev, MT_HIF_REMAP_L1, remap_addr);
	udelay(1);
	mt7927_wr(dev, MT_HIF_REMAP_BASE + offset, val);
}

static bool mt7927_poll_remap(struct mt7927_dev *dev, u32 addr, u32 mask,
			      u32 expect, int timeout_ms)
{
	int i;
	u32 val;

	for (i = 0; i < timeout_ms; i++) {
		val = mt7927_rr_remap(dev, addr);
		if ((val & mask) == expect)
			return true;
		usleep_range(1000, 1500);
	}

	return false;
}

/* =============================================================================
 * ConnInfra Initialization - CRITICAL FOR MT6639/MT7927
 * =============================================================================
 */

/*
 * Wake up ConnInfra subsystem.
 * This is the FIRST thing that must happen for Gen4m chips.
 * Without ConnInfra awake, no MCU communication is possible.
 */
static int mt7927_conninfra_wakeup(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[ConnInfra] Waking up ConnInfra subsystem...\n");

	/* Write to ConnInfra wakeup register */
	mt7927_wr_remap(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR, 0x1);

	/* Poll for ConnInfra to become ready */
	for (i = 0; i < CONNINFRA_WAKEUP_TIMEOUT_MS; i++) {
		val = mt7927_rr_remap(dev, CONNAC3X_CONN_CFG_ON_MISC_ADDR);

		if (debug && (i % 10 == 0))
			dev_info(&dev->pdev->dev, "[ConnInfra] MISC=0x%08x (attempt %d)\n", val, i);

		/* Check if ConnInfra reports ready state */
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

/*
 * Verify ConnInfra version ID.
 * This confirms we're talking to the right chip.
 */
static int mt7927_conninfra_check_version(struct mt7927_dev *dev)
{
	u32 version;
	int i;

	dev_info(&dev->pdev->dev, "[ConnInfra] Checking version ID...\n");

	for (i = 0; i < 10; i++) {
		/* Try reading version from ConnInfra config space */
		version = mt7927_rr_remap(dev, CONN_INFRA_CFG_BASE);

		dev_info(&dev->pdev->dev, "[ConnInfra] Version read: 0x%08x (attempt %d)\n",
			 version, i + 1);

		if (version == MT6639_CONNINFRA_VERSION_ID ||
		    version == MT6639_CONNINFRA_VERSION_ID_E2) {
			dev_info(&dev->pdev->dev, "[ConnInfra] Version match: MT6639 %s\n",
				 version == MT6639_CONNINFRA_VERSION_ID_E2 ? "E2" : "E1");
			dev->conninfra_version = version;
			return 0;
		}

		msleep(10);
	}

	/* Version doesn't match - this might still work, log warning */
	dev_warn(&dev->pdev->dev, "[ConnInfra] Unexpected version: 0x%08x\n", version);
	dev->conninfra_version = version;
	return 0;  /* Continue anyway */
}

/* =============================================================================
 * ROM Bootloader Ready Check
 * =============================================================================
 */

/*
 * Wait for ROM code to be ready.
 * The ROM bootloader indicates ready state by writing 0x1D1E to a status register.
 */
static int mt7927_wait_rom_ready(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[ROM] Waiting for ROM bootloader ready (expect 0x%x)...\n",
		 ROM_CODE_READY_VALUE);

	for (i = 0; i < ROM_READY_TIMEOUT_MS; i++) {
		val = mt7927_rr_remap(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);

		if (debug && (i % 50 == 0))
			dev_info(&dev->pdev->dev, "[ROM] ROMCODE_INDEX=0x%08x (attempt %d)\n", val, i);

		if (val == ROM_CODE_READY_VALUE) {
			dev_info(&dev->pdev->dev, "[ROM] ROM bootloader ready!\n");
			dev->rom_ready = true;
			return 0;
		}

		msleep(1);
	}

	dev_warn(&dev->pdev->dev, "[ROM] Timeout - ROMCODE_INDEX=0x%08x (expected 0x%x)\n",
		 val, ROM_CODE_READY_VALUE);
	return -ETIMEDOUT;
}

/* =============================================================================
 * Driver/Firmware Ownership Handoff (Gen4m style)
 * =============================================================================
 */

static int mt7927_driver_own(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[OWN] Acquiring driver ownership...\n");

	/* Read current LPCTL state */
	val = mt7927_rr_remap(dev, CONNAC3X_BN0_LPCTL_ADDR);
	dev_info(&dev->pdev->dev, "[OWN] LPCTL before: 0x%08x\n", val);

	/* Clear OWN bit to acquire driver ownership */
	for (i = 0; i < DRV_OWN_TIMEOUT_MS; i++) {
		mt7927_wr_remap(dev, CONNAC3X_BN0_LPCTL_ADDR, PCIE_LPCR_HOST_CLR_OWN);

		/* ASPM delay */
		usleep_range(2000, 3000);

		val = mt7927_rr_remap(dev, CONNAC3X_BN0_LPCTL_ADDR);

		if (!(val & PCIE_LPCR_HOST_OWN_SYNC)) {
			dev_info(&dev->pdev->dev, "[OWN] Driver ownership acquired! LPCTL=0x%08x\n", val);
			return 0;
		}

		if (debug && (i % 100 == 0))
			dev_info(&dev->pdev->dev, "[OWN] Still waiting... LPCTL=0x%08x\n", val);

		msleep(1);
	}

	dev_err(&dev->pdev->dev, "[OWN] Failed to acquire driver ownership, LPCTL=0x%08x\n", val);
	return -ETIMEDOUT;
}

/* =============================================================================
 * Chip Identification
 * =============================================================================
 */

static void mt7927_read_chip_info(struct mt7927_dev *dev)
{
	u32 hw_id, hw_rev;

	dev_info(&dev->pdev->dev, "[CHIP] Reading chip information...\n");

	/* Try multiple chip ID register locations */

	/* Method 1: Standard location for MT6639 */
	hw_id = mt7927_rr_remap(dev, CONNAC3X_TOP_HCR);
	hw_rev = mt7927_rr_remap(dev, CONNAC3X_TOP_HVR);
	dev_info(&dev->pdev->dev, "[CHIP] TOP_HCR=0x%08x TOP_HVR=0x%08x\n", hw_id, hw_rev);

	/* Method 2: Alternative location */
	hw_id = mt7927_rr_remap(dev, 0x70010200);
	hw_rev = mt7927_rr_remap(dev, 0x70010204);
	dev_info(&dev->pdev->dev, "[CHIP] 0x70010200=0x%08x 0x70010204=0x%08x\n", hw_id, hw_rev);

	/* Method 3: Host CSR mailbox (might contain chip info) */
	hw_id = mt7927_rr(dev, HOST_CSR_MCU_SW_MAILBOX_0);
	dev_info(&dev->pdev->dev, "[CHIP] MCU_MAILBOX_0=0x%08x\n", hw_id);

	/* Store whatever we found */
	dev->chip_id = hw_id;
	dev->chip_rev = hw_rev;
}

/* =============================================================================
 * WFDMA Initialization
 * =============================================================================
 */

static int mt7927_wfdma_init(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[WFDMA] Initializing WFDMA...\n");

	/* Read current GLO_CFG */
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[WFDMA] GLO_CFG before: 0x%08x\n", val);

	/* Disable DMA first */
	mt7927_wr(dev, MT_WFDMA0_GLO_CFG, 0);
	msleep(10);

	/* Clear interrupts */
	mt7927_wr(dev, MT_WFDMA0_HOST_INT_STA, 0xFFFFFFFF);
	mt7927_wr(dev, MT_WFDMA0_HOST_INT_ENA, 0);

	/* Enable TX/RX DMA with appropriate flags */
	val = MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN |
	      MT_WFDMA0_GLO_CFG_OMIT_TX_INFO | MT_WFDMA0_GLO_CFG_OMIT_RX_INFO;
	mt7927_wr(dev, MT_WFDMA0_GLO_CFG, val);

	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[WFDMA] GLO_CFG after: 0x%08x\n", val);

	return 0;
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

	/* === Phase 2: ConnInfra Wakeup (CRITICAL FOR MT6639) === */
	dev_info(&pdev->dev, "\n=== Phase 2: ConnInfra Wakeup ===\n");

	ret = mt7927_conninfra_wakeup(dev);
	if (ret) {
		dev_warn(&pdev->dev, "ConnInfra wakeup failed, but continuing...\n");
	}

	/* === Phase 3: ConnInfra Version Check === */
	dev_info(&pdev->dev, "\n=== Phase 3: ConnInfra Version Check ===\n");

	ret = mt7927_conninfra_check_version(dev);
	if (ret) {
		dev_warn(&pdev->dev, "Version check failed, but continuing...\n");
	}

	/* === Phase 4: Wait for ROM Ready === */
	dev_info(&pdev->dev, "\n=== Phase 4: ROM Bootloader Ready Check ===\n");

	ret = mt7927_wait_rom_ready(dev);
	if (ret) {
		dev_warn(&pdev->dev, "ROM not ready, but continuing...\n");
	}

	/* === Phase 5: Driver Ownership === */
	dev_info(&pdev->dev, "\n=== Phase 5: Driver Ownership ===\n");

	ret = mt7927_driver_own(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to acquire driver ownership\n");
		/* Continue for debugging */
	}

	/* === Phase 6: Chip Identification === */
	dev_info(&pdev->dev, "\n=== Phase 6: Chip Identification ===\n");

	mt7927_read_chip_info(dev);

	/* === Phase 7: WFDMA Init === */
	dev_info(&pdev->dev, "\n=== Phase 7: WFDMA Initialization ===\n");

	ret = mt7927_wfdma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "WFDMA init failed\n");
	}

	/* === Summary === */
	dev_info(&pdev->dev, "\n=== Initialization Summary ===\n");
	dev_info(&pdev->dev, "  ConnInfra ready: %s\n", dev->conninfra_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  ConnInfra version: 0x%08x\n", dev->conninfra_version);
	dev_info(&pdev->dev, "  ROM ready: %s\n", dev->rom_ready ? "YES" : "NO");
	dev_info(&pdev->dev, "  Chip ID: 0x%08x\n", dev->chip_id);
	dev_info(&pdev->dev, "\n");
	dev_info(&pdev->dev, "Driver probe completed (diagnostic mode)\n");
	dev_info(&pdev->dev, "Check dmesg output for initialization status\n");

	return 0;

err_free:
	kfree(dev);
	return ret;
}

static void mt7927_remove(struct pci_dev *pdev)
{
	struct mt7927_dev *dev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "MT7927 driver unloading\n");

	if (dev)
		kfree(dev);
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
