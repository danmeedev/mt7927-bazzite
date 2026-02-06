// SPDX-License-Identifier: GPL-2.0
/*
 * MT7927 WiFi 7 Linux Driver v2.19.0
 *
 * v2.19.0 Changes:
 * - CRITICAL: MCU_CMD writes don't stick (reads back 0x00)
 * - WFSYS+0x10 = 0x1d2 not 0x1D1E - WF subsystem may not be powered
 * - Add WF_ON power control sequence before WFDMA init
 * - Try enabling WF subsystem via ConnInfra power control registers
 * - Add bus remapping for WFDMA access through ConnInfra
 *
 * v2.18.0 Changes:
 * - Enhanced ROM interrupt configuration before FW_START
 * - Enable MCU2HOST_SW_INT_ENA and TX ring command interrupts
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
#define DRV_VERSION "2.19.0"

/* PCI IDs */
#define MT7927_VENDOR_ID	0x14c3
#define MT7927_DEVICE_ID	0x7927
#define MT6639_DEVICE_ID	0x6639

/* Module parameters */
static bool debug = true;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug output (default: true)");

static char *firmware_path = "";
module_param(firmware_path, charp, 0644);
MODULE_PARM_DESC(firmware_path, "Custom firmware directory (e.g., /var/lib/mt7927/firmware)");

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
 * WFDMA Registers - Two access methods available
 *
 * HOST WFDMA: Direct access at 0xD4000 in BAR0
 * MCU WPDMA:  Access via fixed_map at 0x2000 (MCU addr 0x54000000)
 *
 * The kernel mt76 driver uses MCU WPDMA addresses (0x54000xxx)
 * which get translated via fixed_map to BAR offset 0x2xxx.
 * =============================================================================
 */

/* Host-side WFDMA base (what we've been using) */
#define MT_WFDMA0_BASE			0xD4000

/* MCU-side WPDMA base - mapped via fixed_map to BAR 0x2000 */
#define MT_MCU_WPDMA0_BAR		0x2000	/* BAR offset after fixed_map translation */
#define MT_MCU_WPDMA0_PHYS		0x54000000  /* Physical/MCU address */

/* DUMMY_CR for DMA reinit handshake - at MCU WPDMA offset 0x120 */
#define MT_WFDMA_DUMMY_CR		(MT_MCU_WPDMA0_BAR + 0x120)  /* = 0x2120 */
#define MT_WFDMA_NEED_REINIT		BIT(1)

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

/* Host WFDMA ring registers (what we've been using - reads back 0) */
#define MT_TX_RING_BASE			(MT_WFDMA0_BASE + 0x300)
#define MT_TX_RING_SIZE			0x10
#define MT_RX_RING_BASE			(MT_WFDMA0_BASE + 0x500)
#define MT_RX_RING_SIZE			0x10

/* MCU WPDMA ring registers - try these instead! */
#define MT_MCU_TX_RING_BASE		(MT_MCU_WPDMA0_BAR + 0x300)  /* 0x2300 */
#define MT_MCU_RX_RING_BASE		(MT_MCU_WPDMA0_BAR + 0x500)  /* 0x2500 */

/* MCU WPDMA GLO_CFG for comparison */
#define MT_MCU_WPDMA0_GLO_CFG		(MT_MCU_WPDMA0_BAR + 0x208)  /* 0x2208 */
#define MT_MCU_WPDMA0_RST		(MT_MCU_WPDMA0_BAR + 0x100)  /* 0x2100 */

#define MT_RING_BASE			0x00
#define MT_RING_CNT			0x04
#define MT_RING_CIDX			0x08
#define MT_RING_DIDX			0x0c

/* =============================================================================
 * Ring Prefetch (EXT_CTRL) Registers - MUST configure before ring BASE/CNT
 * =============================================================================
 */

/* TX Ring Extended Control (prefetch config) */
#define MT_WFDMA0_TX_RING15_EXT_CTRL	(MT_WFDMA0_BASE + 0x63c)
#define MT_WFDMA0_TX_RING16_EXT_CTRL	(MT_WFDMA0_BASE + 0x640)

/* RX Ring Extended Control */
#define MT_WFDMA0_RX_RING0_EXT_CTRL	(MT_WFDMA0_BASE + 0x680)

/* Prefetch values: (base_offset << 16) | depth
 * These are memory offsets in the internal SRAM, not physical addresses
 * MT7925/MT6639 specific values from kernel driver
 */
#define PREFETCH_TX_RING15		0x05000040  /* offset 0x500, depth 64 */
#define PREFETCH_TX_RING16		0x05400040  /* offset 0x540, depth 64 */
#define PREFETCH_RX_RING0		0x00000040  /* offset 0x000, depth 64 */

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
#define MT_WFDMA0_HOST_INT_STA		(MT_WFDMA0_BASE + 0x200)
#define MT_PCIE_MAC_BASE		0x10000
#define MT_PCIE_MAC_INT_ENABLE		(MT_PCIE_MAC_BASE + 0x188)

/* MCU2HOST Software Interrupt Enable - critical for ROM to signal back */
#define MT_MCU2HOST_SW_INT_ENA		(MT_WFDMA0_BASE + 0x1f4)
#define MT_MCU2HOST_SW_INT_STA		(MT_WFDMA0_BASE + 0x1f8)
#define MT_MCU2HOST_SW_INT_SET		(MT_WFDMA0_BASE + 0x10c)

/* TX Ring interrupt enables - needed for command ring processing */
#define MT_WFDMA0_TX_RING_INBAND_CMD_INT_ENA	(MT_WFDMA0_BASE + 0x24c)

/* Interrupt enable bits */
#define HOST_INT_TX_DONE_ALL		GENMASK(31, 0)
#define MCU2HOST_SW_INT_ALL		0xFFFFFFFF

/* =============================================================================
 * ROM Bootloader State Registers (Gen4m/MT6639)
 * =============================================================================
 */

/* WF_TOP_CFG_ON base - mapped via ConnInfra fixed region */
#define WF_TOP_CFG_ON_BASE		0x184c0000

/* ROM code index/state register - offset from WF_TOP_CFG_ON */
#define WF_TOP_CFG_ON_ROMCODE_INDEX	0x604

/* ROM ready value - indicates bootloader is ready to receive commands */
#define ROM_READY_VALUE			0x1D1E

/* Alternative ROM state addresses to try (Gen4m varies by chip revision) */
#define WF_ROM_STATE_ADDR_1		0x81021604  /* Direct WF bus address */
#define WF_ROM_STATE_ADDR_2		0x18060010  /* ConnInfra mapped */
#define WF_ROM_STATE_ADDR_3		0x820600a4  /* Alternative offset */

/* ConnInfra bus read/write control for accessing WF_TOP_CFG_ON */
#define CONN_INFRA_WFSYS_ON_BASE	0x0f0000
#define CONN_INFRA_WF_BUS_ADDR		(CONN_INFRA_WFSYS_ON_BASE + 0x44)
#define CONN_INFRA_WF_BUS_DATA		(CONN_INFRA_WFSYS_ON_BASE + 0x48)

/* CONN_INFRA_CFG_ON for power/clock control */
#define CONN_INFRA_CFG_ON_BASE		0x0f0000
#define CONN_INFRA_WAKEUP_REG		(CONN_INFRA_CFG_ON_BASE + 0x10)
#define CONN_INFRA_SLEEP_REG		(CONN_INFRA_CFG_ON_BASE + 0x14)

/* ConnInfra HOST_CSR_TOP registers for ROM wakeup (Gen4m specific) */
#define CONN_HOST_CSR_TOP_BASE			0x0e0000
#define CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU	(CONN_HOST_CSR_TOP_BASE + 0x1a0)
#define CONN_HOST_CSR_TOP_WF_BAND0_IRQ_STAT	(CONN_HOST_CSR_TOP_BASE + 0x10)
#define CONN_HOST_CSR_TOP_WF_BAND0_IRQ_ENA	(CONN_HOST_CSR_TOP_BASE + 0x14)

/* WFSYS SW control registers */
#define WFSYS_SW_RST_REG		(CONN_INFRA_CFG_ON_BASE + 0x140)
#define WFSYS_CPU_SW_RST_B		BIT(0)
#define WFSYS_ON_TOP_PWR_CTL		(CONN_INFRA_CFG_ON_BASE + 0x0)

/* MCU execution trigger - alternative addresses from Gen4m */
#define MT_WF_SUBSYS_RST		(CONN_INFRA_CFG_ON_BASE + 0x610)
#define MT_WF_MCU_PC			(CONN_INFRA_CFG_ON_BASE + 0x620)

/* =============================================================================
 * WF Subsystem Power Control (Gen4m/MT6639)
 *
 * The WF subsystem needs to be powered on before WFDMA registers are accessible.
 * This is controlled via ConnInfra power management registers.
 * =============================================================================
 */

/* WF_ON Power Control - must be enabled before WFDMA works */
#define CONN_INFRA_WF_ON_PWR_CTL	(CONN_INFRA_CFG_ON_BASE + 0x0)
#define CONN_INFRA_WF_SLP_CTL		(CONN_INFRA_CFG_ON_BASE + 0x4)
#define CONN_INFRA_WF_SLP_STATUS	(CONN_INFRA_CFG_ON_BASE + 0x8)

/* WF Power state bits */
#define WF_ON_PWR_ON			BIT(0)
#define WF_ON_PWR_ACK			BIT(1)
#define WF_SLP_TOP_CK_EN		BIT(0)

/* WF_CTRL_STATUS at WFSYS+0x10 - the 0x1d2 we're seeing */
#define WFSYS_CTRL_STATUS		(FIXED_MAP_CONN_INFRA + 0x10)

/* Expected ROM ready value */
#define WF_ROM_READY			0x1D1E

/* Alternative: WF MCUSYS Power Control (from Android MTK driver) */
#define WF_MCUSYS_PWR_CTL		(FIXED_MAP_CONN_INFRA + 0x100)
#define WF_MCUSYS_PWR_ON		BIT(0)
#define WF_MCUSYS_PWR_ACK		BIT(4)

/* WF Top Clock Control */
#define WF_TOP_CLK_CTL			(FIXED_MAP_CONN_INFRA + 0x120)
#define WF_TOP_CLK_EN			BIT(0)

/* CONN_INFRA to WF bus control - for remapped access */
#define CONN_INFRA_WF_REMAP_BASE	0x18000000
#define CONN_INFRA_WF_REMAP_CTRL	(CONN_HOST_CSR_TOP_BASE + 0x1c0)

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
	/* Write lower 32 bits of DMA address - this is all we need for 32-bit DMA */
	mt7927_wr(dev, base_reg + MT_RING_BASE, lower_32_bits(ring->desc_dma));
	mt7927_wr(dev, base_reg + MT_RING_CNT, ring->size);
	mt7927_wr(dev, base_reg + MT_RING_CIDX, 0);
}

/* =============================================================================
 * DMA Initialization
 * =============================================================================
 */

static int mt7927_dma_init(struct mt7927_dev *dev)
{
	u32 val, base_reg, readback, mcu_base_reg;
	int ret, i;
	bool host_works = false, mcu_works = false;

	dev_info(&dev->pdev->dev, "[DMA] Initializing v2.14 (MCU WPDMA test)...\n");

	/*
	 * Step 0: Compare HOST WFDMA (0xD4xxx) vs MCU WPDMA (0x2xxx) access
	 *
	 * The kernel mt76 driver uses MCU physical addresses (0x54000xxx)
	 * which get translated via fixed_map to BAR offset 0x2xxx.
	 * Let's see if the MCU WPDMA window gives us writable registers.
	 */
	dev_info(&dev->pdev->dev, "[DMA] Comparing HOST vs MCU register access:\n");
	dev_info(&dev->pdev->dev, "[DMA]   HOST GLO_CFG (0x%05x) = 0x%08x\n",
		 MT_WFDMA0_GLO_CFG, mt7927_rr(dev, MT_WFDMA0_GLO_CFG));
	dev_info(&dev->pdev->dev, "[DMA]   MCU  GLO_CFG (0x%05x) = 0x%08x\n",
		 MT_MCU_WPDMA0_GLO_CFG, mt7927_rr(dev, MT_MCU_WPDMA0_GLO_CFG));
	dev_info(&dev->pdev->dev, "[DMA]   HOST RST     (0x%05x) = 0x%08x\n",
		 MT_WFDMA0_RST, mt7927_rr(dev, MT_WFDMA0_RST));
	dev_info(&dev->pdev->dev, "[DMA]   MCU  RST     (0x%05x) = 0x%08x\n",
		 MT_MCU_WPDMA0_RST, mt7927_rr(dev, MT_MCU_WPDMA0_RST));
	dev_info(&dev->pdev->dev, "[DMA]   DUMMY_CR    (0x%05x) = 0x%08x\n",
		 MT_WFDMA_DUMMY_CR, mt7927_rr(dev, MT_WFDMA_DUMMY_CR));

	/* Step 1: Disable DMA on both interfaces */
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

	/* Step 2: Reset WFDMA */
	dev_info(&dev->pdev->dev, "[DMA] Resetting WFDMA...\n");
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_set(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);
	msleep(1);
	mt7927_clear(dev, MT_WFDMA0_RST, MT_WFDMA0_RST_LOGIC_RST | MT_WFDMA0_RST_DMASHDL_ALL_RST);

	/* Step 3: Disable clock gating */
	dev_info(&dev->pdev->dev, "[DMA] Disabling clock gating...\n");
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[DMA] GLO_CFG after reset: 0x%08x\n", val);

	mt7927_set(dev, MT_WFDMA0_GLO_CFG, MT_WFDMA0_GLO_CFG_CLK_GAT_DIS);
	val = mt7927_rr(dev, MT_WFDMA0_GLO_CFG);
	dev_info(&dev->pdev->dev, "[DMA] GLO_CFG after CLK_GAT_DIS: 0x%08x\n", val);

	/* Step 4: Configure prefetch registers */
	dev_info(&dev->pdev->dev, "[DMA] Configuring prefetch registers...\n");
	mt7927_wr(dev, MT_WFDMA0_TX_RING15_EXT_CTRL, PREFETCH_TX_RING15);
	mt7927_wr(dev, MT_WFDMA0_TX_RING16_EXT_CTRL, PREFETCH_TX_RING16);
	mt7927_wr(dev, MT_WFDMA0_RX_RING0_EXT_CTRL, PREFETCH_RX_RING0);

	/* Step 5: Allocate rings */
	dev_info(&dev->pdev->dev, "[DMA] Allocating ring descriptors...\n");
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_FWDL], MT_TX_RING_SIZE_FWDL);
	if (ret)
		return ret;
	ret = mt7927_ring_alloc(dev, &dev->tx_ring[MT_TX_RING_MCU_WM], MT_TX_RING_SIZE_MCU);
	if (ret)
		return ret;
	ret = mt7927_ring_alloc(dev, &dev->rx_ring[MT_RX_RING_MCU], MT_RX_RING_SIZE_MCU);
	if (ret)
		return ret;

	/*
	 * Step 6: Try BOTH ring register locations and see which one works
	 *
	 * HOST WFDMA: Ring16 at 0xD4400 (what we've been using)
	 * MCU WPDMA:  Ring16 at 0x2400  (kernel driver's MCU address)
	 */
	dev_info(&dev->pdev->dev, "\n[DMA] === Testing HOST WFDMA registers (0xD4xxx) ===\n");

	/* Test HOST Ring16 */
	base_reg = MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE;
	dev_info(&dev->pdev->dev, "[DMA] HOST Ring16: Writing BASE=0x%08x to reg 0x%05x\n",
		 lower_32_bits(dev->tx_ring[MT_TX_RING_FWDL].desc_dma), base_reg);
	mt7927_ring_setup(dev, base_reg, &dev->tx_ring[MT_TX_RING_FWDL]);
	readback = mt7927_rr(dev, base_reg + MT_RING_BASE);
	dev_info(&dev->pdev->dev, "[DMA] HOST Ring16: Readback = 0x%08x %s\n",
		 readback, (readback != 0) ? "OK!" : "FAILED");
	if (readback != 0)
		host_works = true;

	dev_info(&dev->pdev->dev, "\n[DMA] === Testing MCU WPDMA registers (0x2xxx) ===\n");

	/* Test MCU Ring16 */
	mcu_base_reg = MT_MCU_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE;
	dev_info(&dev->pdev->dev, "[DMA] MCU Ring16: Writing BASE=0x%08x to reg 0x%05x\n",
		 lower_32_bits(dev->tx_ring[MT_TX_RING_FWDL].desc_dma), mcu_base_reg);
	mt7927_ring_setup(dev, mcu_base_reg, &dev->tx_ring[MT_TX_RING_FWDL]);
	readback = mt7927_rr(dev, mcu_base_reg + MT_RING_BASE);
	dev_info(&dev->pdev->dev, "[DMA] MCU Ring16: Readback = 0x%08x %s\n",
		 readback, (readback != 0) ? "OK!" : "FAILED");
	if (readback != 0)
		mcu_works = true;

	/* Also read HOST to see if MCU write affected it */
	readback = mt7927_rr(dev, base_reg + MT_RING_BASE);
	dev_info(&dev->pdev->dev, "[DMA] HOST Ring16 after MCU write: 0x%08x\n", readback);

	/* Configure remaining rings using whichever method works */
	dev_info(&dev->pdev->dev, "\n[DMA] === Configuring all rings ===\n");

	if (mcu_works) {
		dev_info(&dev->pdev->dev, "[DMA] Using MCU WPDMA (0x2xxx) for rings\n");

		/* TX Ring 16 (FWDL) */
		mcu_base_reg = MT_MCU_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE;
		mt7927_ring_setup(dev, mcu_base_reg, &dev->tx_ring[MT_TX_RING_FWDL]);

		/* TX Ring 15 (MCU) */
		mcu_base_reg = MT_MCU_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE;
		mt7927_ring_setup(dev, mcu_base_reg, &dev->tx_ring[MT_TX_RING_MCU_WM]);

		/* RX Ring 0 (MCU events) */
		mcu_base_reg = MT_MCU_RX_RING_BASE + MT_RX_RING_MCU * MT_RX_RING_SIZE;
		mt7927_ring_setup(dev, mcu_base_reg, &dev->rx_ring[MT_RX_RING_MCU]);
	} else if (host_works) {
		dev_info(&dev->pdev->dev, "[DMA] Using HOST WFDMA (0xD4xxx) for rings\n");

		/* TX Ring 16 already set up above */

		/* TX Ring 15 */
		base_reg = MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE;
		mt7927_ring_setup(dev, base_reg, &dev->tx_ring[MT_TX_RING_MCU_WM]);

		/* RX Ring 0 */
		base_reg = MT_RX_RING_BASE + MT_RX_RING_MCU * MT_RX_RING_SIZE;
		mt7927_ring_setup(dev, base_reg, &dev->rx_ring[MT_RX_RING_MCU]);
	} else {
		dev_err(&dev->pdev->dev, "[DMA] NEITHER HOST nor MCU ring registers work!\n");
		dev_err(&dev->pdev->dev, "[DMA] This may require different initialization\n");
		/* Continue anyway to see what happens */
	}

	/* Step 7: Reset ring pointers and enable DMA */
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
		dev_info(&dev->pdev->dev, "[DMA] DMA enabled: GLO_CFG=0x%08x\n", val);
	}

	/*
	 * Step 8: Set DUMMY_CR to indicate DMA needs reinit
	 * This is what the kernel driver does after enabling DMA
	 */
	dev_info(&dev->pdev->dev, "[DMA] Setting DUMMY_CR for reinit handshake...\n");
	mt7927_set(dev, MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);
	val = mt7927_rr(dev, MT_WFDMA_DUMMY_CR);
	dev_info(&dev->pdev->dev, "[DMA] DUMMY_CR after set: 0x%08x\n", val);

	/* Wake MCU/ROM */
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
 * This makes WPDMA registers writable and enables ROM to receive commands
 *
 * v2.18: Enhanced interrupt setup based on MT6639/Gen4m research
 * - MCU2HOST_SW_INT_ENA must be enabled for ROM to signal back
 * - TX_RING_INBAND_CMD_INT must be enabled for command ring processing
 * - HOST_INT_ENA enables TX done notifications
 */
static void mt7927_irq_setup(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[IRQ] v2.18: Enhanced interrupt setup\n");

	/* Step 1: Clear any pending interrupts first */
	val = mt7927_rr(dev, MT_WFDMA0_HOST_INT_STA);
	dev_info(&dev->pdev->dev, "[IRQ] HOST_INT_STA (pending): 0x%08x\n", val);
	mt7927_wr(dev, MT_WFDMA0_HOST_INT_STA, val);  /* W1C - write to clear */

	val = mt7927_rr(dev, MT_MCU2HOST_SW_INT_STA);
	dev_info(&dev->pdev->dev, "[IRQ] MCU2HOST_SW_INT_STA (pending): 0x%08x\n", val);
	mt7927_wr(dev, MT_MCU2HOST_SW_INT_STA, val);  /* W1C */

	/* Step 2: Enable MCU2HOST software interrupts - CRITICAL for ROM */
	dev_info(&dev->pdev->dev, "[IRQ] Enabling MCU2HOST_SW_INT_ENA...\n");
	mt7927_wr(dev, MT_MCU2HOST_SW_INT_ENA, MCU2HOST_SW_INT_ALL);
	val = mt7927_rr(dev, MT_MCU2HOST_SW_INT_ENA);
	dev_info(&dev->pdev->dev, "[IRQ] MCU2HOST_SW_INT_ENA: 0x%08x\n", val);

	/* Step 3: Enable TX ring command interrupt for Ring 15 */
	dev_info(&dev->pdev->dev, "[IRQ] Enabling TX_RING_INBAND_CMD_INT_ENA...\n");
	mt7927_wr(dev, MT_WFDMA0_TX_RING_INBAND_CMD_INT_ENA, BIT(15) | BIT(16));  /* Ring 15 + 16 */
	val = mt7927_rr(dev, MT_WFDMA0_TX_RING_INBAND_CMD_INT_ENA);
	dev_info(&dev->pdev->dev, "[IRQ] TX_RING_INBAND_CMD_INT_ENA: 0x%08x\n", val);

	/* Step 4: Enable host DMA TX done interrupts for all TX rings */
	dev_info(&dev->pdev->dev, "[IRQ] Enabling HOST_INT_ENA (TX done)...\n");
	mt7927_wr(dev, MT_WFDMA0_HOST_INT_ENA, 0xFFFFFFFF);
	val = mt7927_rr(dev, MT_WFDMA0_HOST_INT_ENA);
	dev_info(&dev->pdev->dev, "[IRQ] HOST_INT_ENA: 0x%08x\n", val);

	/* Step 5: Enable PCIe MAC interrupts (required for WPDMA) */
	mt7927_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);
	val = mt7927_rr(dev, MT_PCIE_MAC_INT_ENABLE);
	dev_info(&dev->pdev->dev, "[IRQ] PCIe MAC INT: 0x%08x\n", val);

	/* Step 6: Enable ConnInfra HOST WF interrupts */
	mt7927_wr(dev, CONN_HOST_CSR_TOP_WF_BAND0_IRQ_ENA, 0xFFFFFFFF);
	val = mt7927_rr(dev, CONN_HOST_CSR_TOP_WF_BAND0_IRQ_ENA);
	dev_info(&dev->pdev->dev, "[IRQ] ConnInfra WF_IRQ_ENA: 0x%08x\n", val);
}

/*
 * Dump key registers for debugging - helps identify what's happening
 */
static void mt7927_dump_debug_regs(struct mt7927_dev *dev, const char *label)
{
	u32 i, val;

	dev_info(&dev->pdev->dev, "\n========== %s ==========\n", label);

	/* ConnInfra HOST region (0x0E0000) */
	dev_info(&dev->pdev->dev, "ConnInfra HOST (0x0E0000):\n");
	for (i = 0; i < 0x100; i += 0x10) {
		dev_info(&dev->pdev->dev, "  +0x%03x: %08x %08x %08x %08x\n", i,
			 mt7927_rr(dev, CONN_INFRA_HOST_BAR_OFS + i),
			 mt7927_rr(dev, CONN_INFRA_HOST_BAR_OFS + i + 4),
			 mt7927_rr(dev, CONN_INFRA_HOST_BAR_OFS + i + 8),
			 mt7927_rr(dev, CONN_INFRA_HOST_BAR_OFS + i + 12));
	}

	/* WFSYS region (0x0F0000) - first 256 bytes */
	dev_info(&dev->pdev->dev, "WFSYS (0x0F0000):\n");
	for (i = 0; i < 0x200; i += 0x10) {
		dev_info(&dev->pdev->dev, "  +0x%03x: %08x %08x %08x %08x\n", i,
			 mt7927_rr(dev, FIXED_MAP_CONN_INFRA + i),
			 mt7927_rr(dev, FIXED_MAP_CONN_INFRA + i + 4),
			 mt7927_rr(dev, FIXED_MAP_CONN_INFRA + i + 8),
			 mt7927_rr(dev, FIXED_MAP_CONN_INFRA + i + 12));
	}

	/* WFDMA key registers */
	dev_info(&dev->pdev->dev, "WFDMA (0xD4000):\n");
	dev_info(&dev->pdev->dev, "  GLO_CFG (0x208): 0x%08x\n", mt7927_rr(dev, MT_WFDMA0_GLO_CFG));
	dev_info(&dev->pdev->dev, "  HOST_INT_ENA (0x204): 0x%08x\n", mt7927_rr(dev, MT_WFDMA0_HOST_INT_ENA));
	dev_info(&dev->pdev->dev, "  MCU_CMD (0x1f0): 0x%08x\n", mt7927_rr(dev, MT_MCU_CMD));

	/* Ring 15 (MCU WM) */
	val = MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE;
	dev_info(&dev->pdev->dev, "  TX Ring15 BASE: 0x%08x\n", mt7927_rr(dev, val));
	dev_info(&dev->pdev->dev, "  TX Ring15 CNT: 0x%08x\n", mt7927_rr(dev, val + 4));
	dev_info(&dev->pdev->dev, "  TX Ring15 CIDX: 0x%08x\n", mt7927_rr(dev, val + 8));
	dev_info(&dev->pdev->dev, "  TX Ring15 DIDX: 0x%08x\n", mt7927_rr(dev, val + 12));

	/* Ring 16 (FWDL) */
	val = MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE;
	dev_info(&dev->pdev->dev, "  TX Ring16 BASE: 0x%08x\n", mt7927_rr(dev, val));
	dev_info(&dev->pdev->dev, "  TX Ring16 CNT: 0x%08x\n", mt7927_rr(dev, val + 4));
	dev_info(&dev->pdev->dev, "  TX Ring16 CIDX: 0x%08x\n", mt7927_rr(dev, val + 8));
	dev_info(&dev->pdev->dev, "  TX Ring16 DIDX: 0x%08x\n", mt7927_rr(dev, val + 12));

	/* Additional MCU state registers */
	dev_info(&dev->pdev->dev, "Interrupt registers:\n");
	dev_info(&dev->pdev->dev, "  MCU2HOST_SW_INT_ENA (0x%05x): 0x%08x\n", MT_MCU2HOST_SW_INT_ENA, mt7927_rr(dev, MT_MCU2HOST_SW_INT_ENA));
	dev_info(&dev->pdev->dev, "  MCU2HOST_SW_INT_STA (0x%05x): 0x%08x\n", MT_MCU2HOST_SW_INT_STA, mt7927_rr(dev, MT_MCU2HOST_SW_INT_STA));
	dev_info(&dev->pdev->dev, "  HOST_INT_ENA (0x%05x): 0x%08x\n", MT_WFDMA0_HOST_INT_ENA, mt7927_rr(dev, MT_WFDMA0_HOST_INT_ENA));
	dev_info(&dev->pdev->dev, "  HOST_INT_STA (0x%05x): 0x%08x\n", MT_WFDMA0_HOST_INT_STA, mt7927_rr(dev, MT_WFDMA0_HOST_INT_STA));
	dev_info(&dev->pdev->dev, "  TX_RING_CMD_INT_ENA (0x%05x): 0x%08x\n", MT_WFDMA0_TX_RING_INBAND_CMD_INT_ENA, mt7927_rr(dev, MT_WFDMA0_TX_RING_INBAND_CMD_INT_ENA));
	dev_info(&dev->pdev->dev, "  PCIe MAC INT (0x10188): 0x%08x\n", mt7927_rr(dev, MT_PCIE_MAC_INT_ENABLE));
	dev_info(&dev->pdev->dev, "  ConnInfra WF_IRQ_ENA: 0x%08x\n", mt7927_rr(dev, CONN_HOST_CSR_TOP_WF_BAND0_IRQ_ENA));

	/* MCU state */
	dev_info(&dev->pdev->dev, "MCU state:\n");
	dev_info(&dev->pdev->dev, "  DUMMY_CR (0x%05x): 0x%08x\n", MT_WFDMA_DUMMY_CR, mt7927_rr(dev, MT_WFDMA_DUMMY_CR));
	dev_info(&dev->pdev->dev, "  MCU_CMD (0x%05x): 0x%08x\n", MT_MCU_CMD, mt7927_rr(dev, MT_MCU_CMD));
	dev_info(&dev->pdev->dev, "  WFSYS_SW_RST: 0x%08x\n", mt7927_rr(dev, WFSYS_SW_RST_REG));
	dev_info(&dev->pdev->dev, "  MCU_PC: 0x%08x\n", mt7927_rr(dev, MT_WF_MCU_PC));

	dev_info(&dev->pdev->dev, "========================================\n\n");
}

/*
 * Poll ROM bootloader state - Gen4m requires waiting for 0x1D1E
 * The ROM needs to be ready before it can accept firmware download commands
 */
static int mt7927_poll_rom_state(struct mt7927_dev *dev)
{
	u32 val, addr_val;
	int i;

	dev_info(&dev->pdev->dev, "[ROM] Polling bootloader state...\n");

	/*
	 * Gen4m ROM state can be read from several locations depending on
	 * how ConnInfra maps the WF subsystem. Try multiple approaches.
	 */

	/* Method 1: Check CONN_MISC register for any activity */
	val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);
	dev_info(&dev->pdev->dev, "[ROM] CONN_MISC initial: 0x%08x\n", val);

	/* Method 2: Try reading ROM state via ConnInfra WF bus window */
	/* Set up bus address to read WF_TOP_CFG_ON + 0x604 */
	mt7927_wr(dev, CONN_INFRA_WF_BUS_ADDR, WF_TOP_CFG_ON_BASE + WF_TOP_CFG_ON_ROMCODE_INDEX);
	msleep(1);
	addr_val = mt7927_rr(dev, CONN_INFRA_WF_BUS_DATA);
	dev_info(&dev->pdev->dev, "[ROM] WF_TOP via bus window: 0x%08x\n", addr_val);

	/* Method 3: Try direct offsets in ConnInfra fixed map region */
	/* The ROM state may be mapped at different fixed offsets */

	/* Check offset 0x604 from WF_TOP mapped area */
	val = mt7927_rr(dev, FIXED_MAP_CONN_INFRA + 0x604);
	dev_info(&dev->pdev->dev, "[ROM] ConnInfra+0x604: 0x%08x\n", val);

	/* Check common ROM state offset 0xa4 */
	val = mt7927_rr(dev, FIXED_MAP_CONN_INFRA + 0xa4);
	dev_info(&dev->pdev->dev, "[ROM] ConnInfra+0xa4: 0x%08x\n", val);

	/* Check offset 0x10 */
	val = mt7927_rr(dev, FIXED_MAP_CONN_INFRA + 0x10);
	dev_info(&dev->pdev->dev, "[ROM] ConnInfra+0x10: 0x%08x\n", val);

	/* Method 4: Poll waiting for ROM ready (0x1D1E) */
	for (i = 0; i < 100; i++) {
		/* Try the WFSYS_ON region offset for ROM code */
		val = mt7927_rr(dev, CONN_INFRA_WFSYS_ON_BASE + 0x604);
		if (val == ROM_READY_VALUE) {
			dev_info(&dev->pdev->dev, "[ROM] Ready! (0x%04x at WFSYS+0x604)\n", val);
			return 0;
		}

		/* Also check via bus window */
		mt7927_wr(dev, CONN_INFRA_WF_BUS_ADDR, WF_TOP_CFG_ON_BASE + WF_TOP_CFG_ON_ROMCODE_INDEX);
		val = mt7927_rr(dev, CONN_INFRA_WF_BUS_DATA);
		if (val == ROM_READY_VALUE) {
			dev_info(&dev->pdev->dev, "[ROM] Ready! (0x%04x via bus)\n", val);
			return 0;
		}

		if (i == 0 || i == 50)
			dev_info(&dev->pdev->dev, "[ROM] Poll %d: WFSYS=0x%08x bus=0x%08x\n",
				 i, mt7927_rr(dev, CONN_INFRA_WFSYS_ON_BASE + 0x604), val);

		msleep(10);
	}

	/*
	 * If we don't get 0x1D1E, the ROM might still be initializing or
	 * this chip may use a different indication. Continue anyway and
	 * see if firmware loading works.
	 */
	dev_warn(&dev->pdev->dev, "[ROM] Did not see 0x1D1E ready value, continuing...\n");
	return 0;  /* Return success to continue - we'll see if FW load works */
}

/*
 * v2.19: Enable WF subsystem power
 *
 * The WFSYS+0x10 = 0x1d2 suggests the WF subsystem is not fully powered.
 * We need to enable WF power through ConnInfra before WFDMA will work.
 */
static int mt7927_enable_wf_power(struct mt7927_dev *dev)
{
	u32 val, status;
	int i;

	dev_info(&dev->pdev->dev, "[WF_PWR] v2.19: Enabling WF subsystem power...\n");

	/* Read current power status */
	val = mt7927_rr(dev, WFSYS_CTRL_STATUS);
	dev_info(&dev->pdev->dev, "[WF_PWR] WFSYS_CTRL_STATUS: 0x%08x (want 0x1D1E)\n", val);

	/* Read WF_ON power control */
	val = mt7927_rr(dev, CONN_INFRA_WF_ON_PWR_CTL);
	dev_info(&dev->pdev->dev, "[WF_PWR] WF_ON_PWR_CTL before: 0x%08x\n", val);

	/* Read MCUSYS power control */
	val = mt7927_rr(dev, WF_MCUSYS_PWR_CTL);
	dev_info(&dev->pdev->dev, "[WF_PWR] WF_MCUSYS_PWR_CTL before: 0x%08x\n", val);

	/* Read WF Top Clock Control */
	val = mt7927_rr(dev, WF_TOP_CLK_CTL);
	dev_info(&dev->pdev->dev, "[WF_PWR] WF_TOP_CLK_CTL before: 0x%08x\n", val);

	/* Step 1: Enable WF_ON power */
	dev_info(&dev->pdev->dev, "[WF_PWR] Step 1: Enable WF_ON power\n");
	mt7927_set(dev, CONN_INFRA_WF_ON_PWR_CTL, WF_ON_PWR_ON);
	msleep(5);

	/* Check for power ACK */
	for (i = 0; i < 50; i++) {
		val = mt7927_rr(dev, CONN_INFRA_WF_ON_PWR_CTL);
		if (val & WF_ON_PWR_ACK) {
			dev_info(&dev->pdev->dev, "[WF_PWR] WF_ON power ACK received\n");
			break;
		}
		msleep(1);
	}
	dev_info(&dev->pdev->dev, "[WF_PWR] WF_ON_PWR_CTL after: 0x%08x\n", val);

	/* Step 2: Enable MCUSYS power */
	dev_info(&dev->pdev->dev, "[WF_PWR] Step 2: Enable MCUSYS power\n");
	mt7927_set(dev, WF_MCUSYS_PWR_CTL, WF_MCUSYS_PWR_ON);
	msleep(5);

	/* Check for MCUSYS power ACK */
	for (i = 0; i < 50; i++) {
		val = mt7927_rr(dev, WF_MCUSYS_PWR_CTL);
		if (val & WF_MCUSYS_PWR_ACK) {
			dev_info(&dev->pdev->dev, "[WF_PWR] MCUSYS power ACK received\n");
			break;
		}
		msleep(1);
	}
	dev_info(&dev->pdev->dev, "[WF_PWR] WF_MCUSYS_PWR_CTL after: 0x%08x\n", val);

	/* Step 3: Enable WF Top clocks */
	dev_info(&dev->pdev->dev, "[WF_PWR] Step 3: Enable WF Top clocks\n");
	mt7927_set(dev, WF_TOP_CLK_CTL, WF_TOP_CLK_EN);
	msleep(2);
	val = mt7927_rr(dev, WF_TOP_CLK_CTL);
	dev_info(&dev->pdev->dev, "[WF_PWR] WF_TOP_CLK_CTL after: 0x%08x\n", val);

	/* Step 4: Disable sleep control */
	dev_info(&dev->pdev->dev, "[WF_PWR] Step 4: Disable WF sleep\n");
	mt7927_wr(dev, CONN_INFRA_WF_SLP_CTL, 0);
	msleep(2);

	/* Step 5: Check final status */
	status = mt7927_rr(dev, WFSYS_CTRL_STATUS);
	dev_info(&dev->pdev->dev, "[WF_PWR] WFSYS_CTRL_STATUS after power: 0x%08x\n", status);

	/* Step 6: Try toggling WFSYS reset after power enable */
	dev_info(&dev->pdev->dev, "[WF_PWR] Step 6: Toggle WFSYS reset\n");
	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	dev_info(&dev->pdev->dev, "[WF_PWR] WFSYS_RST before: 0x%08x\n", val);

	/* Clear reset, wait, then set reset */
	mt7927_clear(dev, MT_WFSYS_RST_BAR_OFS, WFSYS_SW_RST_B);
	msleep(10);
	mt7927_set(dev, MT_WFSYS_RST_BAR_OFS, WFSYS_SW_RST_B);
	msleep(50);

	val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
	dev_info(&dev->pdev->dev, "[WF_PWR] WFSYS_RST after toggle: 0x%08x\n", val);

	/* Wait for INIT_DONE */
	for (i = 0; i < 100; i++) {
		val = mt7927_rr(dev, MT_WFSYS_RST_BAR_OFS);
		if (val & WFSYS_SW_INIT_DONE) {
			dev_info(&dev->pdev->dev, "[WF_PWR] WFSYS INIT_DONE!\n");
			break;
		}
		msleep(5);
	}

	/* Final status check */
	status = mt7927_rr(dev, WFSYS_CTRL_STATUS);
	dev_info(&dev->pdev->dev, "[WF_PWR] Final WFSYS_CTRL_STATUS: 0x%08x\n", status);

	if (status == WF_ROM_READY) {
		dev_info(&dev->pdev->dev, "[WF_PWR] ROM READY (0x1D1E) achieved!\n");
		return 0;
	}

	/* Even if we don't get 0x1D1E, check if MCU_CMD is now writable */
	dev_info(&dev->pdev->dev, "[WF_PWR] Testing MCU_CMD writability...\n");
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[WF_PWR] MCU_CMD before: 0x%08x\n", val);
	mt7927_wr(dev, MT_MCU_CMD, 0xDEADBEEF);
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[WF_PWR] MCU_CMD after write 0xDEADBEEF: 0x%08x\n", val);

	if (val == 0xDEADBEEF || val != 0) {
		dev_info(&dev->pdev->dev, "[WF_PWR] MCU_CMD is now writable!\n");
		mt7927_wr(dev, MT_MCU_CMD, 0);  /* Clear test value */
		return 0;
	}

	dev_warn(&dev->pdev->dev, "[WF_PWR] MCU_CMD still not writable\n");
	return -EAGAIN;  /* Continue anyway */
}

/*
 * Wake ROM bootloader via ConnInfra power management
 * This is required before the ROM will respond to DMA commands
 */
static int mt7927_wake_rom(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[ROM] Waking ROM bootloader...\n");

	/* Assert wakeup request via ConnInfra */
	mt7927_wr(dev, CONN_INFRA_WAKEUP_REG, 0x1);
	msleep(5);

	/* Check wakeup acknowledgement */
	val = mt7927_rr(dev, CONN_INFRA_WAKEUP_REG);
	dev_info(&dev->pdev->dev, "[ROM] Wakeup reg after assert: 0x%08x\n", val);

	/* Also write to HOST region to signal host is ready */
	mt7927_wr(dev, CONN_INFRA_HOST_BAR_OFS + 0x4, 0x1);
	msleep(1);

	/* Try MCU command register again after ROM wake */
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[ROM] MCU_CMD after wake: 0x%08x\n", val);

	/* Signal that host DMA is ready */
	mt7927_set(dev, MT_MCU_CMD, MT_MCU_CMD_WAKE_RX_PCIE);
	msleep(5);

	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[ROM] MCU_CMD after DMA signal: 0x%08x\n", val);

	return 0;
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
	u32 ctrl, host_cidx_addr, mcu_cidx_addr, readback;
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

	desc->buf0 = cpu_to_le32(lower_32_bits(dev->cmd_buf_dma));
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, sizeof(*txd) + len) | MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);
	desc->buf1 = cpu_to_le32(upper_32_bits(dev->cmd_buf_dma));  /* Upper bits for 64-bit addr */
	desc->info = 0;

	wmb();

	ring->idx = (idx + 1) % ring->size;

	/*
	 * v2.16: Try BOTH Host WFDMA and MCU WPDMA CIDX writes
	 * since Ring 15 HOST CIDX doesn't seem to advance
	 */
	host_cidx_addr = MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_CIDX;
	mcu_cidx_addr = MT_MCU_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_CIDX;

	/* Write to HOST CIDX (0xD43F8) */
	mt7927_wr(dev, host_cidx_addr, ring->idx);
	wmb();
	readback = mt7927_rr(dev, host_cidx_addr);
	dev_info(&dev->pdev->dev, "[MCU_CMD] Host CIDX write %d -> readback %d (addr 0x%05x)\n",
		 ring->idx, readback, host_cidx_addr);

	/* Also write to MCU WPDMA CIDX (0x23F8) */
	mt7927_wr(dev, mcu_cidx_addr, ring->idx);
	wmb();
	readback = mt7927_rr(dev, mcu_cidx_addr);
	dev_info(&dev->pdev->dev, "[MCU_CMD] MCU CIDX write %d -> readback %d (addr 0x%05x)\n",
		 ring->idx, readback, mcu_cidx_addr);

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
 * Send FW_START via Ring 16 (FWDL) - v2.16 alternative since Ring 16 works
 */
static int mt7927_fw_start_via_ring16(struct mt7927_dev *dev, u32 addr)
{
	struct mt7927_ring *ring = &dev->tx_ring[MT_TX_RING_FWDL];
	struct mt76_connac2_mcu_txd *txd;
	struct mt76_desc *desc;
	struct mt76_connac_fw_start *fw_start;
	u32 ctrl;
	int idx;

	if (!ring->allocated || !dev->fw_buf)
		return -EINVAL;

	dev_info(&dev->pdev->dev, "[FW] Trying FW_START via Ring 16 (FWDL)...\n");

	txd = (struct mt76_connac2_mcu_txd *)dev->fw_buf;
	memset(txd, 0, sizeof(*txd));

	/* Use CMD type but on FWDL queue */
	txd->txd[0] = cpu_to_le32(
		FIELD_PREP(MT_TXD0_TX_BYTES, sizeof(*txd) + sizeof(*fw_start)) |
		FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
		FIELD_PREP(MT_TXD0_Q_IDX, MT_TX_MCU_PORT_RX_FWDL)
	);

	txd->len = cpu_to_le16(sizeof(*fw_start));
	txd->cid = MCU_CMD_FW_START_REQ;
	txd->pkt_type = MCU_PKT_ID;
	txd->seq = dev->mcu_seq++;
	txd->s2d_index = MCU_S2D_H2N;

	/* Build FW_START payload */
	fw_start = (struct mt76_connac_fw_start *)(dev->fw_buf + sizeof(*txd));
	fw_start->override = cpu_to_le32(addr ? addr : 0);
	fw_start->option = cpu_to_le32(addr ? BIT(0) : 0);

	wmb();

	idx = ring->idx;
	desc = &ring->desc[idx];

	desc->buf0 = cpu_to_le32(lower_32_bits(dev->fw_buf_dma));
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, sizeof(*txd) + sizeof(*fw_start)) | MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);
	desc->buf1 = cpu_to_le32(upper_32_bits(dev->fw_buf_dma));
	desc->info = 0;

	wmb();

	ring->idx = (idx + 1) % ring->size;
	mt7927_wr(dev, MT_TX_RING_BASE + MT_TX_RING_FWDL * MT_TX_RING_SIZE + MT_RING_CIDX,
		  ring->idx);

	return mt7927_wait_tx_done(dev, MT_TX_RING_FWDL);
}

/*
 * v2.18: Enhanced ConnInfra wakeup pulse before FW_START
 * The ROM bootloader requires the ConnInfra wakeup signal to be asserted
 * before it will process commands. This is separate from initial wakeup.
 */
static void mt7927_conninfra_wakeup_pulse(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[ROM] Sending ConnInfra wakeup pulse...\n");

	/* Method 1: CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU - from Gen4m source */
	val = mt7927_rr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU);
	dev_info(&dev->pdev->dev, "[ROM] WAKEPU before: 0x%08x\n", val);
	mt7927_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU, 0x1);
	msleep(5);
	val = mt7927_rr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU);
	dev_info(&dev->pdev->dev, "[ROM] WAKEPU after: 0x%08x\n", val);

	/* Method 2: CONN_INFRA_WAKEUP_REG - standard wakeup */
	val = mt7927_rr(dev, CONN_INFRA_WAKEUP_REG);
	dev_info(&dev->pdev->dev, "[ROM] WAKEUP_REG before: 0x%08x\n", val);
	mt7927_wr(dev, CONN_INFRA_WAKEUP_REG, 0x1);
	msleep(2);

	/* Method 3: Clear own/sleep state */
	mt7927_wr(dev, MT_LPCTL_BAR_OFS, PCIE_LPCR_HOST_CLR_OWN);
	msleep(2);

	/* Read back status */
	val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);
	dev_info(&dev->pdev->dev, "[ROM] CONN_MISC after wakeup: 0x%08x\n", val);
}

/*
 * v2.18: Try triggering MCU via software interrupt
 * The MCU may need a software interrupt to begin processing queued commands
 */
static void mt7927_trigger_mcu_sw_int(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[MCU] Triggering software interrupt...\n");

	/* Write to MCU2HOST_SW_INT_SET to generate interrupt */
	mt7927_wr(dev, MT_MCU2HOST_SW_INT_SET, BIT(0));
	msleep(5);

	/* Check if interrupt was acknowledged */
	val = mt7927_rr(dev, MT_MCU2HOST_SW_INT_STA);
	dev_info(&dev->pdev->dev, "[MCU] MCU2HOST_SW_INT_STA: 0x%08x\n", val);
}

/*
 * v2.18: Try triggering MCU via WFSYS reset deassert
 * The MCU PC may need to be kicked by toggling WFSYS reset
 */
static void mt7927_kick_mcu_via_reset(struct mt7927_dev *dev)
{
	u32 val, val_pc;

	dev_info(&dev->pdev->dev, "[MCU] Trying reset-based MCU kick...\n");

	/* Read MCU PC before */
	val_pc = mt7927_rr(dev, MT_WF_MCU_PC);
	dev_info(&dev->pdev->dev, "[MCU] MCU PC before: 0x%08x\n", val_pc);

	/* Read current reset state */
	val = mt7927_rr(dev, WFSYS_SW_RST_REG);
	dev_info(&dev->pdev->dev, "[MCU] WFSYS_SW_RST before: 0x%08x\n", val);

	/* Toggle CPU reset - deassert then assert RST_B */
	mt7927_set(dev, WFSYS_SW_RST_REG, WFSYS_CPU_SW_RST_B);
	msleep(10);

	val = mt7927_rr(dev, WFSYS_SW_RST_REG);
	dev_info(&dev->pdev->dev, "[MCU] WFSYS_SW_RST after set: 0x%08x\n", val);

	/* Read MCU PC after */
	val_pc = mt7927_rr(dev, MT_WF_MCU_PC);
	dev_info(&dev->pdev->dev, "[MCU] MCU PC after: 0x%08x\n", val_pc);
}

/*
 * v2.18: DUMMY_CR handshake sequence
 * The MCU monitors DUMMY_CR for DMA reinit requests
 */
static void mt7927_dummy_cr_handshake(struct mt7927_dev *dev)
{
	u32 val;
	int i;

	dev_info(&dev->pdev->dev, "[MCU] Starting DUMMY_CR handshake...\n");

	/* Set NEED_REINIT bit */
	mt7927_set(dev, MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);
	val = mt7927_rr(dev, MT_WFDMA_DUMMY_CR);
	dev_info(&dev->pdev->dev, "[MCU] DUMMY_CR after set: 0x%08x\n", val);

	/* Wait for MCU to clear it (indicates MCU saw the request) */
	for (i = 0; i < 50; i++) {
		val = mt7927_rr(dev, MT_WFDMA_DUMMY_CR);
		if (!(val & MT_WFDMA_NEED_REINIT)) {
			dev_info(&dev->pdev->dev, "[MCU] DUMMY_CR cleared by MCU!\n");
			return;
		}
		msleep(10);
	}

	dev_info(&dev->pdev->dev, "[MCU] DUMMY_CR not cleared (MCU not responding)\n");
}

/*
 * Try direct MCU_CMD register method to kick firmware
 */
static void mt7927_try_direct_fw_kick(struct mt7927_dev *dev)
{
	u32 val;

	dev_info(&dev->pdev->dev, "[FW] Trying direct MCU_CMD register kick...\n");

	/* Set various MCU_CMD bits to try to wake/kick firmware */
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[FW] MCU_CMD before: 0x%08x\n", val);

	/* Try WAKE_RX_PCIE first - signal host is ready for RX */
	mt7927_set(dev, MT_MCU_CMD, MT_MCU_CMD_WAKE_RX_PCIE);
	msleep(5);

	/* Try NORMAL_STATE bit - signals firmware should be running */
	mt7927_set(dev, MT_MCU_CMD, MT_MCU_CMD_NORMAL_STATE);
	msleep(10);
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[FW] MCU_CMD after NORMAL_STATE: 0x%08x\n", val);

	/* Also try LMAC_DONE - indicates lower MAC ready */
	mt7927_set(dev, MT_MCU_CMD, MT_MCU_CMD_LMAC_DONE);
	msleep(10);
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[FW] MCU_CMD after LMAC_DONE: 0x%08x\n", val);

	/* Try RESET_DONE - signals reset complete */
	mt7927_set(dev, MT_MCU_CMD, MT_MCU_CMD_RESET_DONE);
	msleep(10);
	val = mt7927_rr(dev, MT_MCU_CMD);
	dev_info(&dev->pdev->dev, "[FW] MCU_CMD after all bits: 0x%08x\n", val);
}

/*
 * Send FW_START_REQ to start firmware execution - v2.18 with enhanced methods
 */
static int mt7927_mcu_start_firmware(struct mt7927_dev *dev, u32 addr)
{
	struct mt76_connac_fw_start req = {
		.override = cpu_to_le32(addr ? addr : 0),
		.option = cpu_to_le32(addr ? BIT(0) : 0),  /* Override if addr set */
	};
	u32 cidx_before, didx_before, cidx_after, didx_after;
	u32 int_sta, mcu2host_sta;
	int ret;

	dev_info(&dev->pdev->dev, "\n[FW] ========== Starting Firmware (v2.18) ==========\n");
	dev_info(&dev->pdev->dev, "[FW] Start firmware: addr=0x%08x\n", addr);

	/*
	 * v2.18: Pre-condition - send ConnInfra wakeup pulse
	 * The ROM needs to be awake to receive FW_START
	 */
	mt7927_conninfra_wakeup_pulse(dev);

	/* Check interrupt state before command */
	int_sta = mt7927_rr(dev, MT_WFDMA0_HOST_INT_STA);
	mcu2host_sta = mt7927_rr(dev, MT_MCU2HOST_SW_INT_STA);
	dev_info(&dev->pdev->dev, "[FW] INT_STA before: HOST=0x%08x MCU2HOST=0x%08x\n",
		 int_sta, mcu2host_sta);

	/* Dump Ring 15 state BEFORE command */
	cidx_before = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_CIDX);
	didx_before = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_DIDX);
	dev_info(&dev->pdev->dev, "[FW] Ring15 BEFORE: CIDX=%d DIDX=%d\n", cidx_before, didx_before);

	/* Method 1: Try standard Ring 15 (MCU WM) command */
	dev_info(&dev->pdev->dev, "[FW] Method 1: Ring 15 (MCU WM) command\n");
	ret = mt7927_mcu_send_cmd(dev, MCU_CMD_FW_START_REQ, &req, sizeof(req));

	/* Dump Ring 15 state AFTER command */
	cidx_after = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_CIDX);
	didx_after = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_DIDX);
	dev_info(&dev->pdev->dev, "[FW] Ring15 AFTER:  CIDX=%d DIDX=%d (ret=%d)\n", cidx_after, didx_after, ret);

	/* Check interrupt state after command */
	int_sta = mt7927_rr(dev, MT_WFDMA0_HOST_INT_STA);
	mcu2host_sta = mt7927_rr(dev, MT_MCU2HOST_SW_INT_STA);
	dev_info(&dev->pdev->dev, "[FW] INT_STA after: HOST=0x%08x MCU2HOST=0x%08x\n",
		 int_sta, mcu2host_sta);

	if (cidx_after == cidx_before) {
		dev_warn(&dev->pdev->dev, "[FW] WARNING: Ring15 CIDX didn't advance!\n");

		/* Method 2: Try sending FW_START via Ring 16 (FWDL) which works */
		dev_info(&dev->pdev->dev, "[FW] Method 2: Ring 16 (FWDL) command\n");
		ret = mt7927_fw_start_via_ring16(dev, addr);

		/* Method 3: Try direct MCU_CMD register kick */
		dev_info(&dev->pdev->dev, "[FW] Method 3: Direct MCU_CMD kick\n");
		mt7927_try_direct_fw_kick(dev);

		/* v2.18 Method 4: Trigger MCU via software interrupt */
		dev_info(&dev->pdev->dev, "[FW] Method 4: Software interrupt trigger\n");
		mt7927_trigger_mcu_sw_int(dev);

		/* v2.18 Method 5: DUMMY_CR handshake */
		dev_info(&dev->pdev->dev, "[FW] Method 5: DUMMY_CR handshake\n");
		mt7927_dummy_cr_handshake(dev);

		/* v2.18 Method 6: MCU reset kick */
		dev_info(&dev->pdev->dev, "[FW] Method 6: Reset-based MCU kick\n");
		mt7927_kick_mcu_via_reset(dev);
	}

	/* Final interrupt state */
	int_sta = mt7927_rr(dev, MT_WFDMA0_HOST_INT_STA);
	mcu2host_sta = mt7927_rr(dev, MT_MCU2HOST_SW_INT_STA);
	dev_info(&dev->pdev->dev, "[FW] INT_STA final: HOST=0x%08x MCU2HOST=0x%08x\n",
		 int_sta, mcu2host_sta);

	return ret;
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

	desc->buf0 = cpu_to_le32(lower_32_bits(dev->fw_buf_dma));
	ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, sizeof(*txd) + len) | MT_DMA_CTL_LAST_SEC0;
	desc->ctrl = cpu_to_le32(ctrl);
	desc->buf1 = cpu_to_le32(upper_32_bits(dev->fw_buf_dma));  /* Upper bits for 64-bit addr */
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
	u32 val, val2, val3, mcu_cmd, ring15_cidx, ring15_didx;
	int i;

	/* Get Ring 15 state to verify FW_START was sent */
	ring15_cidx = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_CIDX);
	ring15_didx = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_DIDX);
	dev_info(&dev->pdev->dev, "[FW] Ring15 state: CIDX=%d DIDX=%d\n", ring15_cidx, ring15_didx);

	for (i = 0; i < FW_READY_TIMEOUT_MS; i++) {
		/* Poll primary location: ConnInfra MISC */
		val = mt7927_rr(dev, MT_CONN_MISC_BAR_OFS);
		if ((val & MT_TOP_MISC2_FW_N9_RDY) == MT_TOP_MISC2_FW_N9_RDY) {
			dev_info(&dev->pdev->dev, "[FW] Ready! MISC=0x%08x\n", val);
			return 0;
		}

		/* Also check alternative locations */
		val2 = mt7927_rr(dev, FIXED_MAP_CONN_INFRA + 0x10);  /* WFSYS region +0x10 */
		val3 = mt7927_rr(dev, FIXED_MAP_CONN_INFRA + 0x140); /* WFSYS_RST status */
		mcu_cmd = mt7927_rr(dev, MT_MCU_CMD);

		/* Check if MCU_CMD changed (firmware may signal via this register) */
		if (mcu_cmd != 0) {
			dev_info(&dev->pdev->dev, "[FW] MCU_CMD changed to 0x%08x!\n", mcu_cmd);
		}

		/* Check WFSYS +0x10 for potential FW ready bits */
		if ((val2 & 0x3) == 0x3) {
			dev_info(&dev->pdev->dev, "[FW] Ready via WFSYS+0x10=0x%08x!\n", val2);
			return 0;
		}

		if (i % 500 == 0) {
			/* Check Ring 15 DIDX to see if MCU processed command */
			ring15_didx = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_DIDX);
			dev_info(&dev->pdev->dev, "[FW] Waiting... MISC=0x%08x WFSYS+0x10=0x%08x RST=0x%08x MCU_CMD=0x%08x Ring15_DIDX=%d\n",
				 val, val2, val3, mcu_cmd, ring15_didx);
		}
		msleep(1);
	}

	/* Final state dump on timeout */
	ring15_didx = mt7927_rr(dev, MT_TX_RING_BASE + MT_TX_RING_MCU_WM * MT_TX_RING_SIZE + MT_RING_DIDX);
	dev_warn(&dev->pdev->dev, "[FW] Timeout: MISC=0x%08x Ring15 CIDX=%d DIDX=%d\n",
		 val, ring15_cidx, ring15_didx);
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
	char fw_path[256];
	int ret;

	dev_info(&dev->pdev->dev, "\n[PATCH] ========== Loading Patch ==========\n");

	/* Build firmware path - use custom path if specified */
	if (firmware_path && firmware_path[0]) {
		snprintf(fw_path, sizeof(fw_path), "%s/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin",
			 firmware_path);
		dev_info(&dev->pdev->dev, "[PATCH] Using custom path: %s\n", fw_path);
		ret = request_firmware_direct(&fw, fw_path, &dev->pdev->dev);
	} else {
		ret = request_firmware(&fw, MT6639_FIRMWARE_PATCH, &dev->pdev->dev);
	}
	if (ret) {
		dev_err(&dev->pdev->dev, "[PATCH] Failed to load firmware: %d\n", ret);
		if (firmware_path && firmware_path[0])
			dev_err(&dev->pdev->dev, "[PATCH] Tried: %s\n", fw_path);
		else
			dev_err(&dev->pdev->dev, "[PATCH] Tried: %s\n", MT6639_FIRMWARE_PATCH);
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
	char fw_path[256];
	int ret, i;
	u32 addr, len, mode;

	dev_info(&dev->pdev->dev, "\n[FW] ========== Loading Main Firmware ==========\n");

	/* Build firmware path - use custom path if specified */
	if (firmware_path && firmware_path[0]) {
		snprintf(fw_path, sizeof(fw_path), "%s/WIFI_RAM_CODE_MT6639_2_1.bin",
			 firmware_path);
		dev_info(&dev->pdev->dev, "[FW] Using custom path: %s\n", fw_path);
		ret = request_firmware_direct(&fw, fw_path, &dev->pdev->dev);
	} else {
		ret = request_firmware(&fw, MT6639_FIRMWARE_RAM, &dev->pdev->dev);
	}
	if (ret) {
		dev_err(&dev->pdev->dev, "[FW] Failed to load firmware: %d\n", ret);
		if (firmware_path && firmware_path[0])
			dev_err(&dev->pdev->dev, "[FW] Tried: %s\n", fw_path);
		else
			dev_err(&dev->pdev->dev, "[FW] Tried: %s\n", MT6639_FIRMWARE_RAM);
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

	/* Dump registers BEFORE any further initialization */
	mt7927_dump_debug_regs(dev, "AFTER WFSYS RESET + CONNINFRA WAKEUP");

	/* Interrupt setup MUST happen before DMA init */
	mt7927_irq_setup(dev);

	/* Poll ROM bootloader state - Gen4m requires ROM to be ready */
	ret = mt7927_poll_rom_state(dev);
	if (ret)
		dev_warn(&pdev->dev, "ROM state poll issue\n");

	/* Wake ROM bootloader before DMA init */
	ret = mt7927_wake_rom(dev);
	if (ret)
		dev_warn(&pdev->dev, "ROM wake issue\n");

	/* v2.19: Enable WF subsystem power before DMA init */
	ret = mt7927_enable_wf_power(dev);
	if (ret)
		dev_warn(&pdev->dev, "WF power enable issue\n");

	ret = mt7927_dma_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "DMA init failed\n");
		goto err_free;
	}

	/* Dump registers AFTER DMA init to compare */
	mt7927_dump_debug_regs(dev, "AFTER DMA INIT");

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
