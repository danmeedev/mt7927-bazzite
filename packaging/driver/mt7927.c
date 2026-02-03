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
#define DRV_VERSION "0.2.2"

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
	void __iomem *regs;
	resource_size_t regs_len;	/* BAR0 length for bounds checking */

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
	u32 chip_id;
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

	/* Set DMASHDL bypass */
	mt7927_wr_debug(dev, MT_DMASHDL_SW_CONTROL,
			mt7927_rr(dev, MT_DMASHDL_SW_CONTROL) |
			MT_DMASHDL_DMASHDL_BYPASS,
			"DMASHDL_SW_CONTROL");

	if (force) {
		dev_info(&dev->pdev->dev, "  Force reset sequence...\n");

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

		dev_info(&dev->pdev->dev, "  WFDMA0_RST: 0x%08x\n",
			 mt7927_rr(dev, MT_WFDMA0_RST));
	}

	return 0;
}

static int mt7927_dma_enable(struct mt7927_dev *dev)
{
	u32 val_before, val_after, expected;

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

	return 0;
}

static int mt7927_dma_init(struct mt7927_dev *dev)
{
	int ret;

	dev_info(&dev->pdev->dev, "=== DMA Initialization ===\n");

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
		dev_err(&dev->pdev->dev, "  Failed to allocate TX ring\n");
		return -ENOMEM;
	}

	memset(dev->tx_ring, 0, dev->tx_ring_size * sizeof(struct mt76_desc));

	dev_info(&dev->pdev->dev, "  TX ring allocated: %d descriptors at %pad\n",
		 dev->tx_ring_size, &dev->tx_ring_dma);

	/* Configure FWDL ring (ring 16) */
	dev_info(&dev->pdev->dev, "  Configuring FWDL ring (ring 16)...\n");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE,
			lower_32_bits(dev->tx_ring_dma), "RING16_BASE");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x04,
			dev->tx_ring_size, "RING16_CNT");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x08,
			0, "RING16_CIDX");
	mt7927_wr_debug(dev, MT_TX_RING_BASE + 16 * MT_RING_SIZE + 0x0c,
			0, "RING16_DIDX");

	/* Enable DMA */
	ret = mt7927_dma_enable(dev);
	if (ret)
		goto err_free_ring;

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

	dev_info(&dev->pdev->dev, "=== Firmware Loading ===\n");

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

	dev_info(&dev->pdev->dev, "  Firmware DMA buffer at %pad\n", &dev->fw_dma);

	/* Check current firmware status - use remapped access */
	status = mt7927_rr_remap(dev, MT_CONN_ON_MISC);
	dev_info(&dev->pdev->dev, "  MT_CONN_ON_MISC: 0x%08x\n", status);

	/*
	 * TODO: Implement full MCU command protocol
	 * For now, just verify the setup works.
	 */

	return 0;
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

	dev->chip_id = mt7927_rr(dev, MT_HW_CHIPID);
	dev->chip_rev = (dev->chip_id << 16) | (mt7927_rr(dev, MT_HW_REV) & 0xff);

	dev_info(&pdev->dev, "  Chip ID: 0x%08x\n", dev->chip_id);
	dev_info(&pdev->dev, "  Chip Rev: 0x%08x\n", dev->chip_rev);

	if (dev->chip_id == 0xffffffff) {
		dev_err(&pdev->dev, "  ERROR: Chip not responding (0xffffffff)\n");
	}

	/* === Phase 4: EMI Sleep Protection === */
	dev_info(&pdev->dev, "\n=== Phase 4: EMI Sleep Protection ===\n");

	/* EMI Control is at 0x18011100 - use remapped access */
	dev_info(&pdev->dev, "  Setting EMI sleep protection via remap...\n");
	mt7927_wr_remap(dev, MT_HW_EMI_CTL,
			mt7927_rr_remap(dev, MT_HW_EMI_CTL) | MT_HW_EMI_CTL_SLPPROT_EN);

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
