// SPDX-License-Identifier: GPL-2.0+
/*
 * sun50i H616 platform dram controller driver
 *
 * While controller is very similar to that in H6, PHY is completely
 * unknown. That's why this driver has plenty of magic numbers. Some
 * meaning was nevertheless deduced from strings found in boot0 and
 * known meaning of some dram parameters.
 * This driver supports DDR3, LPDDR3 and LPDDR4 memory. There is no
 * DDR4 support yet.
 *
 * (C) Copyright 2020 Jernej Skrabec <jernej.skrabec@siol.net>
 *
 */
#include <cpu_func.h>
#include <init.h>
#include <log.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/dram.h>
#include <asm/arch/dram_dw_helpers.h>
#include <asm/arch/cpu.h>
#include <asm/arch/prcm.h>
#include <asm/arch/wb_dram_dst.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/sizes.h>

enum {
	MBUS_QOS_LOWEST = 0,
	MBUS_QOS_LOW,
	MBUS_QOS_HIGH,
	MBUS_QOS_HIGHEST
};

struct dram_runtime_profile {
	const char *name;
	struct dram_para para;
};

void mctl_set_timing_params_ddr3_runtime(const struct dram_para *para);
void mctl_set_timing_params_lpddr4_runtime(const struct dram_para *para);

static void mbus_configure_port(u8 port,
				bool bwlimit,
				bool priority,
				u8 qos,
				u8 waittime,
				u8 acs,
				u16 bwl0,
				u16 bwl1,
				u16 bwl2)
{
	struct sunxi_mctl_com_reg * const mctl_com =
			(struct sunxi_mctl_com_reg *)SUNXI_DRAM_COM_BASE;

	const u32 cfg0 = ( (bwlimit ? (1 << 0) : 0)
			   | (priority ? (1 << 1) : 0)
			   | ((qos & 0x3) << 2)
			   | ((waittime & 0xf) << 4)
			   | ((acs & 0xff) << 8)
			   | (bwl0 << 16) );
	const u32 cfg1 = ((u32)bwl2 << 16) | (bwl1 & 0xffff);

	debug("MBUS port %d cfg0 %08x cfg1 %08x\n", port, cfg0, cfg1);
	writel_relaxed(cfg0, &mctl_com->master[port].cfg0);
	writel_relaxed(cfg1, &mctl_com->master[port].cfg1);
}

#define MBUS_CONF(port, bwlimit, qos, acs, bwl0, bwl1, bwl2)	\
	mbus_configure_port(port, bwlimit, false, \
			    MBUS_QOS_ ## qos, 0, acs, bwl0, bwl1, bwl2)

static void mctl_set_master_priority(void)
{
	struct sunxi_mctl_com_reg * const mctl_com =
			(struct sunxi_mctl_com_reg *)SUNXI_DRAM_COM_BASE;

	/* enable bandwidth limit windows and set windows size 1us */
	writel(399, &mctl_com->tmr);
	writel(BIT(16), &mctl_com->bwcr);

	MBUS_CONF( 0, false, HIGHEST, 0,  256,  128,  100);
	MBUS_CONF( 1, false,    HIGH, 0, 1536, 1400,  256);
	MBUS_CONF( 2, false, HIGHEST, 0,  512,  256,   96);
	MBUS_CONF( 3, false,    HIGH, 0,  256,  100,   80);
	MBUS_CONF( 4, false,    HIGH, 2, 8192, 5500, 5000);
	MBUS_CONF( 5, false,    HIGH, 2,  100,   64,   32);
	MBUS_CONF( 6, false,    HIGH, 2,  100,   64,   32);
	MBUS_CONF( 8, false,    HIGH, 0,  256,  128,   64);
	MBUS_CONF(11, false,    HIGH, 0,  256,  128,  100);
	MBUS_CONF(14, false,    HIGH, 0, 1024,  256,   64);
	MBUS_CONF(16, false, HIGHEST, 6, 8192, 2800, 2400);
	MBUS_CONF(21, false, HIGHEST, 6, 2048,  768,  512);
	MBUS_CONF(22, false,    HIGH, 0,  256,  128,  100);
	MBUS_CONF(25, true, HIGHEST, 0,  100,   64,   32);
	MBUS_CONF(26, false,    HIGH, 2, 8192, 5500, 5000);
	MBUS_CONF(37, false,    HIGH, 0,  256,  128,   64);
	MBUS_CONF(38, false,    HIGH, 2,  100,   64,   32);
	MBUS_CONF(39, false,    HIGH, 2, 8192, 5500, 5000);
	MBUS_CONF(40, false,    HIGH, 2,  100,   64,   32);

	dmb();
}

static void mctl_sys_init(u32 clk_rate)
{
	struct sunxi_ccm_reg * const ccm =
			(struct sunxi_ccm_reg *)SUNXI_CCM_BASE;
	struct sunxi_mctl_com_reg * const mctl_com =
			(struct sunxi_mctl_com_reg *)SUNXI_DRAM_COM_BASE;
	struct sunxi_mctl_ctl_reg * const mctl_ctl =
			(struct sunxi_mctl_ctl_reg *)SUNXI_DRAM_CTL0_BASE;

	/* Put all DRAM-related blocks to reset state */
	clrbits_le32(&ccm->mbus_cfg, MBUS_ENABLE);
	clrbits_le32(&ccm->mbus_cfg, MBUS_RESET);
	clrbits_le32(&ccm->dram_gate_reset, BIT(GATE_SHIFT));
	udelay(5);
	clrbits_le32(&ccm->dram_gate_reset, BIT(RESET_SHIFT));
	clrbits_le32(&ccm->pll5_cfg, CCM_PLL5_CTRL_EN);
	clrbits_le32(&ccm->dram_clk_cfg, DRAM_MOD_RESET);

	udelay(5);

	/* Set PLL5 rate to doubled DRAM clock rate */
	writel(CCM_PLL5_CTRL_EN | CCM_PLL5_LOCK_EN | CCM_PLL5_OUT_EN |
	       CCM_PLL5_CTRL_N(clk_rate * 2 / 24), &ccm->pll5_cfg);
	mctl_await_completion(&ccm->pll5_cfg,
			      CCM_PLL5_LOCK, CCM_PLL5_LOCK);

	/* Configure DRAM mod clock */
	writel(DRAM_CLK_SRC_PLL5, &ccm->dram_clk_cfg);
	writel(BIT(RESET_SHIFT), &ccm->dram_gate_reset);
	udelay(5);
	setbits_le32(&ccm->dram_gate_reset, BIT(GATE_SHIFT));

	/* Disable all channels */
	writel(0, &mctl_com->maer0);
	writel(0, &mctl_com->maer1);
	writel(0, &mctl_com->maer2);

	/* Configure MBUS and enable DRAM mod reset */
	setbits_le32(&ccm->mbus_cfg, MBUS_RESET);
	setbits_le32(&ccm->mbus_cfg, MBUS_ENABLE);

	clrbits_le32(&mctl_com->unk_0x500, BIT(25));

	setbits_le32(&ccm->dram_clk_cfg, DRAM_MOD_RESET);
	udelay(5);

	/* Unknown hack, which enables access of mctl_ctl regs */
	writel(0x8000, &mctl_ctl->clken);
}

static void mctl_set_addrmap(const struct dram_config *config)
{
	struct sunxi_mctl_ctl_reg * const mctl_ctl =
			(struct sunxi_mctl_ctl_reg *)SUNXI_DRAM_CTL0_BASE;
	u8 cols = config->cols;
	u8 rows = config->rows;
	u8 ranks = config->ranks;

	if (!config->bus_full_width)
		cols -= 1;

	/* Ranks */
	if (ranks == 2)
		mctl_ctl->addrmap[0] = rows + cols - 3;
	else
		mctl_ctl->addrmap[0] = 0x1F;

	/* Banks, hardcoded to 8 banks now */
	mctl_ctl->addrmap[1] = (cols - 2) | (cols - 2) << 8 | (cols - 2) << 16;

	/* Columns */
	mctl_ctl->addrmap[2] = 0;
	switch (cols) {
	case 7:
		mctl_ctl->addrmap[3] = 0x1F1F1F00;
		mctl_ctl->addrmap[4] = 0x1F1F;
		break;
	case 8:
		mctl_ctl->addrmap[3] = 0x1F1F0000;
		mctl_ctl->addrmap[4] = 0x1F1F;
		break;
	case 9:
		mctl_ctl->addrmap[3] = 0x1F000000;
		mctl_ctl->addrmap[4] = 0x1F1F;
		break;
	case 10:
		mctl_ctl->addrmap[3] = 0;
		mctl_ctl->addrmap[4] = 0x1F1F;
		break;
	case 11:
		mctl_ctl->addrmap[3] = 0;
		mctl_ctl->addrmap[4] = 0x1F00;
		break;
	case 12:
		mctl_ctl->addrmap[3] = 0;
		mctl_ctl->addrmap[4] = 0;
		break;
	default:
		panic("Unsupported DRAM configuration: column number invalid\n");
	}

	/* Rows */
	mctl_ctl->addrmap[5] = (cols - 3) | ((cols - 3) << 8) | ((cols - 3) << 16) | ((cols - 3) << 24);
	switch (rows) {
	case 13:
		mctl_ctl->addrmap[6] = (cols - 3) | 0x0F0F0F00;
		mctl_ctl->addrmap[7] = 0x0F0F;
		break;
	case 14:
		mctl_ctl->addrmap[6] = (cols - 3) | ((cols - 3) << 8) | 0x0F0F0000;
		mctl_ctl->addrmap[7] = 0x0F0F;
		break;
	case 15:
		mctl_ctl->addrmap[6] = (cols - 3) | ((cols - 3) << 8) | ((cols - 3) << 16) | 0x0F000000;
		mctl_ctl->addrmap[7] = 0x0F0F;
		break;
	case 16:
		mctl_ctl->addrmap[6] = (cols - 3) | ((cols - 3) << 8) | ((cols - 3) << 16) | ((cols - 3) << 24);
		mctl_ctl->addrmap[7] = 0x0F0F;
		break;
	case 17:
		mctl_ctl->addrmap[6] = (cols - 3) | ((cols - 3) << 8) | ((cols - 3) << 16) | ((cols - 3) << 24);
		mctl_ctl->addrmap[7] = (cols - 3) | 0x0F00;
		break;
	case 18:
		mctl_ctl->addrmap[6] = (cols - 3) | ((cols - 3) << 8) | ((cols - 3) << 16) | ((cols - 3) << 24);
		mctl_ctl->addrmap[7] = (cols - 3) | ((cols - 3) << 8);
		break;
	default:
		panic("Unsupported DRAM configuration: row number invalid\n");
	}

	/* Bank groups, DDR4 only */
	mctl_ctl->addrmap[8] = 0x3F3F;
}

static const u8 phy_init_map1_ddr3[] = {
	0x08, 0x02, 0x12, 0x05, 0x15, 0x17, 0x18, 0x0b,
	0x14, 0x07, 0x04, 0x13, 0x0c, 0x00, 0x16, 0x1a,
	0x0a, 0x11, 0x03, 0x10, 0x0e, 0x01, 0x0d, 0x19,
	0x06, 0x09, 0x0f
};

static const u8 phy_init_map0_t507_lpddr4[] = {
	0x03, 0x00, 0x17, 0x05, 0x02, 0x19, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x01,
	0x18, 0x04, 0x1a
};

static const u8 *dram_phy_init_map(const struct dram_para *para, size_t *len)
{
	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		*len = ARRAY_SIZE(phy_init_map1_ddr3);
		return phy_init_map1_ddr3;
	case SUNXI_DRAM_TYPE_LPDDR4:
		*len = ARRAY_SIZE(phy_init_map0_t507_lpddr4);
		return phy_init_map0_t507_lpddr4;
	case SUNXI_DRAM_TYPE_LPDDR3:
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	}
}

static void dram_set_timing_params_runtime(const struct dram_para *para)
{
	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		mctl_set_timing_params_ddr3_runtime(para);
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		mctl_set_timing_params_lpddr4_runtime(para);
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	}
}
#define MASK_BYTE(reg, nr) (((reg) >> ((nr) * 8)) & 0x1f)
static void mctl_phy_configure_odt(const struct dram_para *para)
{
	uint32_t val_lo, val_hi;

	/*
	 * This part should be applicable to all memory types, but is
	 * usually found in LPDDR4 bootloaders. Therefore, we will leave
	 * only for this type of memory.
	 */
	if (para->type == SUNXI_DRAM_TYPE_LPDDR4) {
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x390, BIT(5), BIT(4));
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x3d0, BIT(5), BIT(4));
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x410, BIT(5), BIT(4));
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x450, BIT(5), BIT(4));
	}

	val_lo = para->dx_dri;
	val_hi = (para->type == SUNXI_DRAM_TYPE_LPDDR4) ? para->dx_dri_hi : para->dx_dri;
	writel_relaxed(MASK_BYTE(val_lo, 0), SUNXI_DRAM_PHY0_BASE + 0x388);
	writel_relaxed(MASK_BYTE(val_hi, 0), SUNXI_DRAM_PHY0_BASE + 0x38c);
	writel_relaxed(MASK_BYTE(val_lo, 1), SUNXI_DRAM_PHY0_BASE + 0x3c8);
	writel_relaxed(MASK_BYTE(val_hi, 1), SUNXI_DRAM_PHY0_BASE + 0x3cc);
	writel_relaxed(MASK_BYTE(val_lo, 2), SUNXI_DRAM_PHY0_BASE + 0x408);
	writel_relaxed(MASK_BYTE(val_hi, 2), SUNXI_DRAM_PHY0_BASE + 0x40c);
	writel_relaxed(MASK_BYTE(val_lo, 3), SUNXI_DRAM_PHY0_BASE + 0x448);
	writel_relaxed(MASK_BYTE(val_hi, 3), SUNXI_DRAM_PHY0_BASE + 0x44c);

	val_lo = para->ca_dri;
	val_hi = para->ca_dri;
	writel_relaxed(MASK_BYTE(val_lo, 0), SUNXI_DRAM_PHY0_BASE + 0x340);
	writel_relaxed(MASK_BYTE(val_hi, 0), SUNXI_DRAM_PHY0_BASE + 0x344);
	writel_relaxed(MASK_BYTE(val_lo, 1), SUNXI_DRAM_PHY0_BASE + 0x348);
	writel_relaxed(MASK_BYTE(val_hi, 1), SUNXI_DRAM_PHY0_BASE + 0x34c);

	val_lo = (para->type == SUNXI_DRAM_TYPE_LPDDR3) ? 0 : para->dx_odt;
	val_hi = (para->type == SUNXI_DRAM_TYPE_LPDDR4) ? 0 : para->dx_odt;
	writel_relaxed(MASK_BYTE(val_lo, 0), SUNXI_DRAM_PHY0_BASE + 0x380);
	writel_relaxed(MASK_BYTE(val_hi, 0), SUNXI_DRAM_PHY0_BASE + 0x384);
	writel_relaxed(MASK_BYTE(val_lo, 1), SUNXI_DRAM_PHY0_BASE + 0x3c0);
	writel_relaxed(MASK_BYTE(val_hi, 1), SUNXI_DRAM_PHY0_BASE + 0x3c4);
	writel_relaxed(MASK_BYTE(val_lo, 2), SUNXI_DRAM_PHY0_BASE + 0x400);
	writel_relaxed(MASK_BYTE(val_hi, 2), SUNXI_DRAM_PHY0_BASE + 0x404);
	writel_relaxed(MASK_BYTE(val_lo, 3), SUNXI_DRAM_PHY0_BASE + 0x440);
	writel_relaxed(MASK_BYTE(val_hi, 3), SUNXI_DRAM_PHY0_BASE + 0x444);

	dmb();
}

static bool mctl_phy_write_leveling(const struct dram_para *para,
				    const struct dram_config *config)
{
	bool result = true;
	u32 val;

	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0xc0, 0x80);
	if (para->type == SUNXI_DRAM_TYPE_LPDDR4) {
		/* MR2 value */
		writel(0x1b, SUNXI_DRAM_PHY0_BASE + 0xc);
		writel(0, SUNXI_DRAM_PHY0_BASE + 0x10);
	} else {
		writel(4, SUNXI_DRAM_PHY0_BASE + 0xc);
		writel(0x40, SUNXI_DRAM_PHY0_BASE + 0x10);
	}

	setbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 4);

	if (config->bus_full_width)
		val = 0xf;
	else
		val = 3;

	mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x188), val, val);

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 4);

	val = readl(SUNXI_DRAM_PHY0_BASE + 0x258);
	if (val == 0 || val == 0x3f)
		result = false;
	val = readl(SUNXI_DRAM_PHY0_BASE + 0x25c);
	if (val == 0 || val == 0x3f)
		result = false;
	val = readl(SUNXI_DRAM_PHY0_BASE + 0x318);
	if (val == 0 || val == 0x3f)
		result = false;
	val = readl(SUNXI_DRAM_PHY0_BASE + 0x31c);
	if (val == 0 || val == 0x3f)
		result = false;

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0xc0);

	if (config->ranks == 2) {
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0xc0, 0x40);

		setbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 4);

		if (config->bus_full_width)
			val = 0xf;
		else
			val = 3;

		mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x188), val, val);

		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 4);
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0xc0);

	return result;
}

static bool mctl_phy_read_calibration(const struct dram_config *config)
{
	bool result = true;
	u32 val, tmp;

	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0x30, 0x20);

	setbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 1);

	if (config->bus_full_width)
		val = 0xf;
	else
		val = 3;

	while ((readl(SUNXI_DRAM_PHY0_BASE + 0x184) & val) != val) {
		if (readl(SUNXI_DRAM_PHY0_BASE + 0x184) & 0x20) {
			result = false;
			break;
		}
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 1);

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0x30);

	if (config->ranks == 2) {
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0x30, 0x10);

		setbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 1);

		while ((readl(SUNXI_DRAM_PHY0_BASE + 0x184) & val) != val) {
			if (readl(SUNXI_DRAM_PHY0_BASE + 0x184) & 0x20) {
				result = false;
				break;
			}
		}

		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 1);
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 0x30);

	val = readl(SUNXI_DRAM_PHY0_BASE + 0x274) & 7;
	tmp = readl(SUNXI_DRAM_PHY0_BASE + 0x26c) & 7;
	if (val < tmp)
		val = tmp;
	tmp = readl(SUNXI_DRAM_PHY0_BASE + 0x32c) & 7;
	if (val < tmp)
		val = tmp;
	tmp = readl(SUNXI_DRAM_PHY0_BASE + 0x334) & 7;
	if (val < tmp)
		val = tmp;
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x38, 0x7, (val + 2) & 7);

	setbits_le32(SUNXI_DRAM_PHY0_BASE + 4, 0x20);

	return result;
}

static bool mctl_phy_read_training(const struct dram_para *para,
			   const struct dram_config *config)
{
	u32 val1, val2, *ptr1, *ptr2;
	bool result = true;
	int i;

	if (para->type == SUNXI_DRAM_TYPE_LPDDR4) {
		writel(0, SUNXI_DRAM_PHY0_BASE + 0x800);
		writel(0, SUNXI_DRAM_PHY0_BASE + 0x81c);
	}

	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x198, 3, 2);
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x804, 0x3f, 0xf);
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x808, 0x3f, 0xf);
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0xa04, 0x3f, 0xf);
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0xa08, 0x3f, 0xf);

	setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 6);
	setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 1);

	mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x840), 0xc, 0xc);
	if (readl(SUNXI_DRAM_PHY0_BASE + 0x840) & 3)
		result = false;

	if (config->bus_full_width) {
		mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0xa40), 0xc, 0xc);
		if (readl(SUNXI_DRAM_PHY0_BASE + 0xa40) & 3)
			result = false;
	}

	ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x898);
	ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x850);
	for (i = 0; i < 9; i++) {
		val1 = readl(&ptr1[i]);
		val2 = readl(&ptr2[i]);
		if (val1 - val2 <= 6)
			result = false;
	}
	ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x8bc);
	ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x874);
	for (i = 0; i < 9; i++) {
		val1 = readl(&ptr1[i]);
		val2 = readl(&ptr2[i]);
		if (val1 - val2 <= 6)
			result = false;
	}

	if (config->bus_full_width) {
		ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xa98);
		ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xa50);
		for (i = 0; i < 9; i++) {
			val1 = readl(&ptr1[i]);
			val2 = readl(&ptr2[i]);
			if (val1 - val2 <= 6)
				result = false;
		}

		ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xabc);
		ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xa74);
		for (i = 0; i < 9; i++) {
			val1 = readl(&ptr1[i]);
			val2 = readl(&ptr2[i]);
			if (val1 - val2 <= 6)
				result = false;
		}
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 3);

	if (config->ranks == 2) {
		/* maybe last parameter should be 1? */
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x198, 3, 2);

		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 6);
		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 1);

		mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x840), 0xc, 0xc);
		if (readl(SUNXI_DRAM_PHY0_BASE + 0x840) & 3)
			result = false;

		if (config->bus_full_width) {
			mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0xa40), 0xc, 0xc);
			if (readl(SUNXI_DRAM_PHY0_BASE + 0xa40) & 3)
				result = false;
		}

		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 3);
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x198, 3);

	return result;
}

static bool mctl_phy_write_training(const struct dram_config *config)
{
	u32 val1, val2, *ptr1, *ptr2;
	bool result = true;
	int i;

	writel(0, SUNXI_DRAM_PHY0_BASE + 0x134);
	writel(0, SUNXI_DRAM_PHY0_BASE + 0x138);
	writel(0, SUNXI_DRAM_PHY0_BASE + 0x19c);
	writel(0, SUNXI_DRAM_PHY0_BASE + 0x1a0);

	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x198, 0xc, 8);

	setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x10);
	setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x20);

	mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x8e0), 3, 3);
	if (readl(SUNXI_DRAM_PHY0_BASE + 0x8e0) & 0xc)
		result = false;

	if (config->bus_full_width) {
		mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0xae0), 3, 3);
		if (readl(SUNXI_DRAM_PHY0_BASE + 0xae0) & 0xc)
			result = false;
	}

	ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x938);
	ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x8f0);
	for (i = 0; i < 9; i++) {
		val1 = readl(&ptr1[i]);
		val2 = readl(&ptr2[i]);
		if (val1 - val2 <= 6)
			result = false;
	}
	ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x95c);
	ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x914);
	for (i = 0; i < 9; i++) {
		val1 = readl(&ptr1[i]);
		val2 = readl(&ptr2[i]);
		if (val1 - val2 <= 6)
			result = false;
	}

	if (config->bus_full_width) {
		ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xb38);
		ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xaf0);
		for (i = 0; i < 9; i++) {
			val1 = readl(&ptr1[i]);
			val2 = readl(&ptr2[i]);
			if (val1 - val2 <= 6)
				result = false;
		}
		ptr1 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xb5c);
		ptr2 = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xb14);
		for (i = 0; i < 9; i++) {
			val1 = readl(&ptr1[i]);
			val2 = readl(&ptr2[i]);
			if (val1 - val2 <= 6)
				result = false;
		}
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x60);

	if (config->ranks == 2) {
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x198, 0xc, 4);

		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x10);
		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x20);

		mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x8e0), 3, 3);
		if (readl(SUNXI_DRAM_PHY0_BASE + 0x8e0) & 0xc)
			result = false;

		if (config->bus_full_width) {
			mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0xae0), 3, 3);
			if (readl(SUNXI_DRAM_PHY0_BASE + 0xae0) & 0xc)
				result = false;
		}

		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x60);
	}

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x198, 0xc);

	return result;
}

static void mctl_phy_bit_delay_compensation(const struct dram_para *para)
{
	u32 *ptr, val;
	int i;

	if (para->tpr10 & TPR10_DX_BIT_DELAY1) {
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x60, 1);
		setbits_le32(SUNXI_DRAM_PHY0_BASE + 8, 8);
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 0x10);
		if (para->type == SUNXI_DRAM_TYPE_LPDDR4)
			clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x4, 0x80);

		if (para->tpr10 & BIT(30))
			val = para->tpr11 & 0x3f;
		else
			val = (para->tpr11 & 0xf) << 1;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x484);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 15) & 0x1e;
		else
			val = (para->tpr11 >> 15) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x4d0);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x590);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x4cc);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x58c);

		if (para->tpr10 & BIT(30))
			val = (para->tpr11 >> 8) & 0x3f;
		else
			val = (para->tpr11 >> 3) & 0x1e;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x4d8);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 19) & 0x1e;
		else
			val = (para->tpr11 >> 19) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x524);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x5e4);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x520);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x5e0);

		if (para->tpr10 & BIT(30))
			val = (para->tpr11 >> 16) & 0x3f;
		else
			val = (para->tpr11 >> 7) & 0x1e;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x604);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 23) & 0x1e;
		else
			val = (para->tpr11 >> 23) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x650);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x710);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x64c);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x70c);

		if (para->tpr10 & BIT(30))
			val = (para->tpr11 >> 24) & 0x3f;
		else
			val = (para->tpr11 >> 11) & 0x1e;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x658);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 27) & 0x1e;
		else
			val = (para->tpr11 >> 27) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x6a4);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x764);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x6a0);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x760);

		dmb();

		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x60, 1);
	}

	if (para->tpr10 & TPR10_DX_BIT_DELAY0) {
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x54, 0x80);
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x190, 4);

		if (para->tpr10 & BIT(30))
			val = para->tpr12 & 0x3f;
		else
			val = (para->tpr12 & 0xf) << 1;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x480);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en << 1) & 0x1e;
		else
			val = (para->tpr12 >> 15) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x528);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x5e8);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x4c8);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x588);

		if (para->tpr10 & BIT(30))
			val = (para->tpr12 >> 8) & 0x3f;
		else
			val = (para->tpr12 >> 3) & 0x1e;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x4d4);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 3) & 0x1e;
		else
			val = (para->tpr12 >> 19) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x52c);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x5ec);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x51c);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x5dc);

		if (para->tpr10 & BIT(30))
			val = (para->tpr12 >> 16) & 0x3f;
		else
			val = (para->tpr12 >> 7) & 0x1e;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x600);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 7) & 0x1e;
		else
			val = (para->tpr12 >> 23) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x6a8);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x768);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x648);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x708);

		if (para->tpr10 & BIT(30))
			val = (para->tpr12 >> 24) & 0x3f;
		else
			val = (para->tpr12 >> 11) & 0x1e;

		ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x654);
		for (i = 0; i < 9; i++) {
			writel_relaxed(val, ptr);
			writel_relaxed(val, ptr + 0x30);
			ptr += 2;
		}

		if (para->tpr10 & BIT(30))
			val = (para->odt_en >> 11) & 0x1e;
		else
			val = (para->tpr12 >> 27) & 0x1e;

		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x6ac);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x76c);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x69c);
		writel_relaxed(val, SUNXI_DRAM_PHY0_BASE + 0x75c);

		dmb();

		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x54, 0x80);
	}
}

static void mctl_phy_ca_bit_delay_compensation(const struct dram_para *para,
					       const struct dram_config *config)
{
	u32 val, *ptr;
	int i;

	if (para->tpr0 & BIT(30))
		val = (para->tpr0 >> 7) & 0x3e;
	else
		val = (para->tpr10 >> 3) & 0x1e;

	ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0x780);
	for (i = 0; i < 32; i++)
		writel(val, &ptr[i]);

	val = (para->tpr10 << 1) & 0x1e;
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x7d8);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x7dc);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x7e0);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x7f4);

	val = (para->tpr10 >> 7) & 0x1e;
	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		if (para->tpr2 & 1) {
			writel(val, SUNXI_DRAM_PHY0_BASE + 0x794);
			if (config->ranks == 2) {
				val = (para->tpr10 >> 11) & 0x1e;
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x7e4);
			}
			if (para->tpr0 & BIT(31)) {
				val = (para->tpr0 << 1) & 0x3e;
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x790);
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x7b8);
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x7cc);
			}
		} else {
			writel(val, SUNXI_DRAM_PHY0_BASE + 0x7d4);
			if (config->ranks == 2) {
				val = (para->tpr10 >> 11) & 0x1e;
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x79c);
			}
			if (para->tpr0 & BIT(31)) {
				val = (para->tpr0 << 1) & 0x3e;
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x78c);
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x7a4);
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x7b8);
			}
		}
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		if (para->tpr2 & 1) {
			writel(val, SUNXI_DRAM_PHY0_BASE + 0x7a0);
			if (config->ranks == 2) {
				val = (para->tpr10 >> 11) & 0x1e;
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x79c);
			}
		} else {
			writel(val, SUNXI_DRAM_PHY0_BASE + 0x7e8);
			if (config->ranks == 2) {
				val = (para->tpr10 >> 11) & 0x1e;
				writel(val, SUNXI_DRAM_PHY0_BASE + 0x7f8);
			}
		}
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		writel(val, SUNXI_DRAM_PHY0_BASE + 0x788);
		if (config->ranks == 2) {
			val = (para->tpr10 >> 11) & 0x1e;
			writel(val, SUNXI_DRAM_PHY0_BASE + 0x794);
		};
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};
}

static bool mctl_phy_init(const struct dram_para *para,
			  const struct dram_config *config)
{
	struct sunxi_mctl_com_reg * const mctl_com =
			(struct sunxi_mctl_com_reg *)SUNXI_DRAM_COM_BASE;
	struct sunxi_mctl_ctl_reg * const mctl_ctl =
			(struct sunxi_mctl_ctl_reg *)SUNXI_DRAM_CTL0_BASE;
	u32 val, val2, *ptr, mr0, mr2;
	const u8 *phy_init;
	size_t phy_init_len;
	int i;

	if (para->type == SUNXI_DRAM_TYPE_LPDDR4)
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x4,0x80);

	if (config->bus_full_width)
		val = 0xf;
	else
		val = 3;
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x3c, 0xf, val);

	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		if (para->tpr2 & 0x100) {
			val = 9;
			val2 = 7;
		} else {
			val = 13;
			val2 = 9;
		}
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		if (para->tpr2 & 0x100) {
			val = 12;
			val2 = 6;
		} else {
			val = 14;
			val2 = 8;
		}
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		val = 20;
		val2 = 10;
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};

	writel(val, SUNXI_DRAM_PHY0_BASE + 0x14);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x35c);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x368);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x374);

	writel(0, SUNXI_DRAM_PHY0_BASE + 0x18);
	writel(0, SUNXI_DRAM_PHY0_BASE + 0x360);
	writel(0, SUNXI_DRAM_PHY0_BASE + 0x36c);
	writel(0, SUNXI_DRAM_PHY0_BASE + 0x378);

	writel(val2, SUNXI_DRAM_PHY0_BASE + 0x1c);
	writel(val2, SUNXI_DRAM_PHY0_BASE + 0x364);
	writel(val2, SUNXI_DRAM_PHY0_BASE + 0x370);
	writel(val2, SUNXI_DRAM_PHY0_BASE + 0x37c);

	phy_init = dram_phy_init_map(para, &phy_init_len);
	ptr = (u32 *)(SUNXI_DRAM_PHY0_BASE + 0xc0);
	for (i = 0; i < phy_init_len; i++)
		writel(phy_init[i], &ptr[i]);

	if (para->tpr10 & TPR10_CA_BIT_DELAY)
		mctl_phy_ca_bit_delay_compensation(para, config);

	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		val = para->tpr6 & 0xff;
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		val = para->tpr6 >> 8 & 0xff;
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		val = para->tpr6 >> 24 & 0xff;
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};

	writel(val, SUNXI_DRAM_PHY0_BASE + 0x3dc);
	writel(val, SUNXI_DRAM_PHY0_BASE + 0x45c);

	mctl_phy_configure_odt(para);

	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		val = 0x0a;
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		val = 0x0b;
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		val = 0x0d;
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};
	clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 4, 0x7, val);

	if (para->clk <= 672)
		writel(0xf, SUNXI_DRAM_PHY0_BASE + 0x20);
	if (para->clk > 500) {
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x144, BIT(7));
		clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x14c, 0xe0);
	} else {
		setbits_le32(SUNXI_DRAM_PHY0_BASE + 0x144, BIT(7));
		clrsetbits_le32(SUNXI_DRAM_PHY0_BASE + 0x14c, 0xe0, 0x20);
	}

	clrbits_le32(&mctl_com->unk_0x500, 0x200);
	udelay(1);

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x14c, 8);

	mctl_await_completion((u32 *)(SUNXI_DRAM_PHY0_BASE + 0x180), 4, 4);

	udelay(1000);

	writel(0x37, SUNXI_DRAM_PHY0_BASE + 0x58);
	clrbits_le32(&mctl_com->unk_0x500, 0x200);

	writel(0, &mctl_ctl->swctl);
	setbits_le32(&mctl_ctl->dfimisc, 1);

	/* start DFI init */
	setbits_le32(&mctl_ctl->dfimisc, 0x20);
	writel(1, &mctl_ctl->swctl);
	mctl_await_completion(&mctl_ctl->swstat, 1, 1);
	/* poll DFI init complete */
	mctl_await_completion(&mctl_ctl->dfistat, 1, 1);
	writel(0, &mctl_ctl->swctl);
	clrbits_le32(&mctl_ctl->dfimisc, 0x20);

	clrbits_le32(&mctl_ctl->pwrctl, 0x20);
	writel(1, &mctl_ctl->swctl);
	mctl_await_completion(&mctl_ctl->swstat, 1, 1);
	mctl_await_completion(&mctl_ctl->statr, 3, 1);

	udelay(200);

	writel(0, &mctl_ctl->swctl);
	clrbits_le32(&mctl_ctl->dfimisc, 1);

	writel(1, &mctl_ctl->swctl);
	mctl_await_completion(&mctl_ctl->swstat, 1, 1);

	if (para->tpr2 & 0x100) {
		mr0 = 0x1b50;
		mr2 = 0x10;
	} else {
		mr0 = 0x1f14;
		mr2 = 0x20;
	}
	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		writel(mr0, &mctl_ctl->mrctrl1);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(4, &mctl_ctl->mrctrl1);
		writel(0x80001030, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(mr2, &mctl_ctl->mrctrl1);
		writel(0x80002030, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0, &mctl_ctl->mrctrl1);
		writel(0x80003030, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		/* MR0 is read-only */
		/* MR1: nWR=14, BL8 */
		writel(0x183, &mctl_ctl->mrctrl1);
		writel(0x800000f0, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		/* MR2: no WR leveling, WL set A, use nWR>9, nRL=14/nWL=8 */
		writel(0x21c, &mctl_ctl->mrctrl1);
		writel(0x800000f0, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		/* MR3: 34.3 Ohm pull-up/pull-down resistor */
		writel(0x301, &mctl_ctl->mrctrl1);
		writel(0x800000f0, &mctl_ctl->mrctrl0);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		writel(0x0, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0x134, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0x21b, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0x333, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0x403, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0xb04, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0xc72, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0xe00 | (para->mr14 & 0xff), &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);

		writel(0x1624, &mctl_ctl->mrctrl1);
		udelay(10);
		writel(0x80000030, &mctl_ctl->mrctrl0);
		udelay(10);
		mctl_await_completion(&mctl_ctl->mrctrl0, BIT(31), 0);
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};

	writel(0, SUNXI_DRAM_PHY0_BASE + 0x54);

	writel(0, &mctl_ctl->swctl);
	clrbits_le32(&mctl_ctl->rfshctl3, 1);
	writel(1, &mctl_ctl->swctl);

	if (para->tpr10 & TPR10_WRITE_LEVELING) {
		for (i = 0; i < 5; i++)
			if (mctl_phy_write_leveling(para, config))
				break;
		if (i == 5) {
			debug("write leveling failed!\n");
			return false;
		}
	}

	if (para->tpr10 & TPR10_READ_CALIBRATION) {
		for (i = 0; i < 5; i++)
			if (mctl_phy_read_calibration(config))
				break;
		if (i == 5) {
			debug("read calibration failed!\n");
			return false;
		}
	}

	if (para->tpr10 & TPR10_READ_TRAINING) {
		for (i = 0; i < 5; i++)
			if (mctl_phy_read_training(para, config))
				break;
		if (i == 5) {
			debug("read training failed!\n");
			return false;
		}
	}

	if (para->tpr10 & TPR10_WRITE_TRAINING) {
		for (i = 0; i < 5; i++)
			if (mctl_phy_write_training(config))
				break;
		if (i == 5) {
			debug("write training failed!\n");
			return false;
		}
	}

	mctl_phy_bit_delay_compensation(para);

	clrbits_le32(SUNXI_DRAM_PHY0_BASE + 0x60, 4);

	return true;
}

static bool mctl_ctrl_init(const struct dram_para *para,
			   const struct dram_config *config)
{
	struct sunxi_mctl_com_reg * const mctl_com =
			(struct sunxi_mctl_com_reg *)SUNXI_DRAM_COM_BASE;
	struct sunxi_mctl_ctl_reg * const mctl_ctl =
			(struct sunxi_mctl_ctl_reg *)SUNXI_DRAM_CTL0_BASE;
	u32 reg_val;

	clrsetbits_le32(&mctl_com->unk_0x500, BIT(24), 0x200);
	writel(0x8000, &mctl_ctl->clken);

	setbits_le32(&mctl_com->unk_0x008, 0xff00);

	if (para->type == SUNXI_DRAM_TYPE_LPDDR4)
		writel(1, SUNXI_DRAM_COM_BASE + 0x50);
	clrsetbits_le32(&mctl_ctl->sched[0], 0xff00, 0x3000);

	writel(0, &mctl_ctl->hwlpctl);

	setbits_le32(&mctl_com->unk_0x008, 0xff00);

	reg_val = MSTR_ACTIVE_RANKS(config->ranks);
	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		reg_val |= MSTR_BURST_LENGTH(8) | MSTR_DEVICETYPE_DDR3 | MSTR_2TMODE;
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		reg_val |= MSTR_BURST_LENGTH(8) | MSTR_DEVICETYPE_LPDDR3;
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		reg_val |= MSTR_BURST_LENGTH(16) | MSTR_DEVICETYPE_LPDDR4;
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};
	if (config->bus_full_width)
		reg_val |= MSTR_BUSWIDTH_FULL;
	else
		reg_val |= MSTR_BUSWIDTH_HALF;
	writel(BIT(31) | BIT(30) | reg_val, &mctl_ctl->mstr);

	if (config->ranks == 2)
		writel(0x0303, &mctl_ctl->odtmap);
	else
		writel(0x0201, &mctl_ctl->odtmap);

	switch (para->type) {
	case SUNXI_DRAM_TYPE_DDR3:
		reg_val = 0x06000400;
		break;
	case SUNXI_DRAM_TYPE_LPDDR3:
		reg_val = 0x09020400;
		break;
	case SUNXI_DRAM_TYPE_LPDDR4:
		reg_val = 0x04000400;
		break;
	case SUNXI_DRAM_TYPE_DDR4:
	default:
		panic("This DRAM setup is currently not supported.\n");
	};
	writel(reg_val, &mctl_ctl->odtcfg);
	writel(reg_val, &mctl_ctl->unk_0x2240);
	writel(reg_val, &mctl_ctl->unk_0x3240);
	writel(reg_val, &mctl_ctl->unk_0x4240);

	writel(BIT(31), &mctl_com->cr);

	mctl_set_addrmap(config);

	dram_set_timing_params_runtime(para);

	writel(0, &mctl_ctl->pwrctl);

	setbits_le32(&mctl_ctl->dfiupd[0], BIT(31) | BIT(30));
	setbits_le32(&mctl_ctl->zqctl[0], BIT(31) | BIT(30));
	setbits_le32(&mctl_ctl->unk_0x2180, BIT(31) | BIT(30));
	setbits_le32(&mctl_ctl->unk_0x3180, BIT(31) | BIT(30));
	setbits_le32(&mctl_ctl->unk_0x4180, BIT(31) | BIT(30));

	setbits_le32(&mctl_ctl->rfshctl3, BIT(0));
	clrbits_le32(&mctl_ctl->dfimisc, BIT(0));

	writel(0, &mctl_com->maer0);
	writel(0, &mctl_com->maer1);
	writel(0, &mctl_com->maer2);

	writel(0x20, &mctl_ctl->pwrctl);
	setbits_le32(&mctl_ctl->clken, BIT(8));

	clrsetbits_le32(&mctl_com->unk_0x500, BIT(24), 0x300);
	udelay(1);
	/* this write seems to enable PHY MMIO region */
	setbits_le32(&mctl_com->unk_0x500, BIT(24));
	udelay(1);

	if (!mctl_phy_init(para, config))
		return false;

	writel(0, &mctl_ctl->swctl);
	clrbits_le32(&mctl_ctl->rfshctl3, BIT(0));

	setbits_le32(&mctl_com->unk_0x014, BIT(31));
	writel(0xffffffff, &mctl_com->maer0);
	writel(0x7ff, &mctl_com->maer1);
	writel(0xffff, &mctl_com->maer2);

	writel(1, &mctl_ctl->swctl);
	mctl_await_completion(&mctl_ctl->swstat, 1, 1);

	return true;
}

bool mctl_core_init(const struct dram_para *para,
		    const struct dram_config *config)
{
	mctl_sys_init(para->clk);

	return mctl_ctrl_init(para, config);
}

static const struct dram_runtime_profile profile_lpddr4 = {
	.name = "LPDDR4",
	.para = {
		.clk = 792,
		.type = SUNXI_DRAM_TYPE_LPDDR4,
		.dx_odt = 0x07070707,
		.dx_dri = 0x0e0e0e0e,
		.dx_dri_hi = 0x04040404,	/* DQ pull-up, was hardcoded */
		.ca_dri = 0x0e0e,
		.odt_en = 0xaaaaeeee,
		.tpr0 = 0x0,
		.tpr2 = 0x0,
		.tpr6 = 0x44000000,
		.tpr10 = 0x402f6633,
		.tpr11 = 0x24242624,
		.tpr12 = 0x0f0f100f,
		.mr14 = 0x09,			/* MR14, was hardcoded 0xe09 */
	},
};

/*
 * KOWIN LPDDR4X 16Gb run as LPDDR4 at VDDQ=1.1V (out-of-spec crutch).
 * Validated parameter set (V2), 2026-05/06 (DST eye-scan over -40..+95 C,
 * write-VREF wall ~30 codes away, hot-retention >=8x margin). NORMAL LPDDR4
 * does NOT work on these — selected only on the dedicated LPDDR4X strap.
 * .type stays LPDDR4 (electrical mode); selection key is the strap.
 */
static const struct dram_runtime_profile profile_lpddr4x_16gb = {
	.name = "LPDDR4X-16Gb",
	.para = {
		.clk = 792,
		.type = SUNXI_DRAM_TYPE_LPDDR4,
		.dx_odt = 0x07070707,
		.dx_dri = 0x0e0e0e0e,
		.dx_dri_hi = 0x09090909,	/* DQ pull-up 0x09 (vs 0x04 normal) */
		.ca_dri = 0x0e0e,
		.odt_en = 0xaaaaeeee,
		.tpr0 = 0x0,
		.tpr2 = 0x0,
		.tpr6 = 0x50000000,
		.tpr10 = 0x402f6633,
		.tpr11 = 0x25272725,
		.tpr12 = 0x0e0f0f0e,
		.mr14 = 0x02,
	},
};

static const struct dram_runtime_profile profile_ddr3 = {
	.name = "DDR3",
	.para = {
		.clk = 720,
		.type = SUNXI_DRAM_TYPE_DDR3,
		.dx_odt = 0x08080808,
		.dx_dri = 0x0e0e0e0e,
		.ca_dri = 0x0e0e,
		.odt_en = 0x1,
		.tpr0 = 0x0,
		.tpr2 = 0x0,
		.tpr6 = 0x3300c080,
		.tpr10 = 0x00f83438,
		.tpr11 = 0x0,
		.tpr12 = 0x0,
	},
};

static const char *dram_type_name(enum sunxi_dram_type type)
{
	switch (type) {
	case SUNXI_DRAM_TYPE_DDR3:
		return "DDR3";
	case SUNXI_DRAM_TYPE_LPDDR3:
		return "LPDDR3";
	case SUNXI_DRAM_TYPE_LPDDR4:
		return "LPDDR4";
	case SUNXI_DRAM_TYPE_DDR4:
		return "DDR4";
	default:
		return "unknown";
	}
}

/*
 * Board code may select DRAM type and switch power rails before probing.
 * Keep this hook generic so the H616 DRAM driver does not depend on PMIC code.
 */
__weak int sunxi_dram_prepare_type(enum sunxi_dram_type *type)
{
	return 0;
}

static bool try_dram_profile(const struct dram_runtime_profile *profile,
			     struct dram_config *config, unsigned long *size)
{
	const char *type = dram_type_name(profile->para.type);

	if (!mctl_auto_detect_rank_width(&profile->para, config)) {
		printf("DRAM probe: %s rank/width detection failed\n", type);
		return false;
	}

	mctl_auto_detect_dram_size(&profile->para, config);

	if (!mctl_core_init(&profile->para, config)) {
		printf("DRAM probe: %s initialization failed\n", type);
		return false;
	}

	*size = mctl_calc_size(config);

	return *size != 0;
}


/*
 * DST (DRAM Stability Test) - 2D eye-scan calibration.
 *
 * Ported from the vendor Allwinner boot0 (boot0_sdcard.elf, sun50iw9p1)
 * dst_dq_eye_scan (32-bit ARM Thumb-2). A "2D eye" sweeps two axes - the data
 * reference voltage (VREF) and the per-byte read/write delays - re-initing the
 * PHY at each step and running a short memtest, to map the window in which
 * data is sampled reliably; the center of the widest window (the "eye") gives
 * the best voltage/timing margin. Phases: read VREF (tpr6) -> read delays
 * (tpr12) -> write VREF (MR14) -> write delays (tpr11) -> read delays refined.
 *
 * Measurement only: the controller keeps running on the fixed profile values;
 * the found optimum is just handed to Linux (see asm/arch/wb_dram_dst.h).
 */

/* Fill DRAM region with a constant 32-bit pattern */
static void dst_memfill(volatile u32 *start, u32 count, u32 pattern)
{
	u32 i;

	for (i = 0; i < count; i++)
		start[i] = pattern;
	dsb();
}

/* Copy DRAM region word-by-word */
static void dst_memcopy(volatile u32 *dst, volatile u32 *src, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++)
		dst[i] = src[i];
	dsb();
}

/* Compare two DRAM regions, return 0 if match, non-zero bitmask on mismatch */
static u32 dst_memcmp_region(volatile u32 *a, volatile u32 *b, u32 count)
{
	u32 i, diff = 0;

	for (i = 0; i < count; i++)
		diff |= a[i] ^ b[i];
	return diff;
}

/*
 * dst_read_cmp — write incrementing pattern to DRAM, read back, compare.
 * Returns 0 if the specified byte lane reads correctly, non-zero on error.
 *
 * Vendor writes pattern[i] = i*0x01010101 + 0x01234567 to base,
 * and pattern[i] XOR 0xfdb97531 to base+ref_offset, then reads back
 * and checks per-byte-lane.
 */
static u32 dst_read_cmp(ulong base, ulong ref_offset, u32 count, int byte_lane)
{
	volatile u32 *p = (volatile u32 *)base;
	volatile u32 *r = (volatile u32 *)(base + ref_offset);
	u32 accum = 0;
	u32 pat_p = 0xfedcba98;
	u32 pat_r = 0x01234567;
	u32 i;

	/* Write patterns */
	for (i = 0; i < count; i++) {
		u32 key = i * 0x01010101;

		p[i] = key + pat_p;
		r[i] = key + pat_r;
	}
	dsb();

	/* Read back and compare */
	for (i = 0; i < count; i++) {
		u32 key = i * 0x01010101;
		u32 err_p = p[i] ^ (key + pat_p);
		u32 err_r = r[i] ^ (key + pat_r);

		accum |= err_p | err_r;
	}

	return (accum >> (byte_lane * 8)) & 0xff;
}

/*
 * dst_memtester — memory test with multiple patterns.
 *
 * Writes patterns to a source region, copies through DRAM, compares.
 * Tests 6 fixed patterns, then bit-walking patterns for the specified byte lane.
 *
 * @dram_half:   half of detected DRAM size (0 for default addresses)
 * @test_len:    test region size in bytes
 * @byte_lane:   byte lane to test (0-3), or 8 for full-word test
 * @is_byte:     true = test specific byte lane only
 * Returns 0 on success.
 */
static int dst_memtester(ulong dram_half, u32 test_len, int byte_lane,
			 bool is_byte)
{
	static const u32 patterns[] = {
		0x55555555, 0xAAAAAAAA, 0x33333333,
		0xCCCCCCCC, 0x0F0F0F0F, 0xF0F0F0F0,
	};
	ulong dram_base = CFG_SYS_SDRAM_BASE;
	volatile u32 *src, *dst_r, *ref;
	u32 word_count = test_len / 4;
	u32 diff;
	int p, bit;

	/* buffers at half of the detected DRAM (not always 2 GiB) */
	ref   = (volatile u32 *)(dram_base + dram_half);           /* copy source */
	dst_r = (volatile u32 *)(dram_base + dram_half + 0x08UL);  /* copy destination */
	src   = (volatile u32 *)(dram_base + dram_half + 0x0cUL);  /* expected pattern */

	/* Phase 1: fixed pattern tests */
	for (p = 0; p < ARRAY_SIZE(patterns); p++) {
		dst_memfill(src, word_count, patterns[p]);
		dst_memfill(ref, word_count, patterns[p]);
		dst_memcopy(dst_r, ref, word_count);
		diff = dst_memcmp_region(src, dst_r, word_count);
		if (diff) {
			if (is_byte)
				return 1;
			if ((diff >> (byte_lane * 8)) & 0xff)
				return 1;
		}
	}

	/* Phase 2: bit-walking patterns */
	if (!is_byte) {
		int start_bit = byte_lane * 8;
		int end_bit = start_bit + 8;

		for (bit = start_bit; bit < end_bit; bit++) {
			u32 pat = 1u << bit;
			u32 ipat = ~pat;

			/* Test with single bit set */
			dst_memfill(src, word_count, pat);
			dst_memfill(ref, word_count, pat);
			dst_memcopy(dst_r, ref, word_count);
			diff = dst_memcmp_region(src, dst_r, word_count);
			if ((diff >> (byte_lane * 8)) & 0xff)
				return 1;

			/* Test with single bit clear */
			dst_memfill(src, word_count, ipat);
			dst_memfill(ref, word_count, ipat);
			dst_memcopy(dst_r, ref, word_count);
			diff = dst_memcmp_region(src, dst_r, word_count);
			if ((diff >> (byte_lane * 8)) & 0xff)
				return 1;
		}
	}

	return 0;
}

/*
 * Per-byte delay scan (1D).
 *
 * For each DQ byte lane, sweep delay 0..63, test memory,
 * find the passing window, compute center delay.
 *
 * @para:     mutable dram_para
 * @config:   dram config
 * @is_write: true = scan write path (tpr11), false = read path (tpr12)
 * @num_bytes: number of byte lanes (2 or 4)
 * @dram_half: half of DRAM size for memtester addressing
 * Returns the packed tpr11 or tpr12 value.
 */
static u32 dst_per_byte_scan(struct dram_para *para,
			     const struct dram_config *config,
			     bool is_write, int num_bytes, ulong dram_half)
{
	u32 result = 0;
	u32 *tpr = is_write ? &para->tpr11 : &para->tpr12;
	u32 saved_tpr11 = para->tpr11;
	u32 saved_tpr12 = para->tpr12;
	int bl;

	for (bl = 0; bl < num_bytes; bl++) {
		int shift = bl * 8;
		u32 mask = ~(0x3fu << shift);
		u32 saved_byte = (saved_tpr11 >> shift) & 0x3f;
		int first_pass = -1, last_pass = -1;
		int delay, eye_width, center;

		/*
		 * Init DRAM once with known-good params before sweep.
		 */
		para->tpr11 = saved_tpr11;
		para->tpr12 = saved_tpr12;
		mctl_core_init(para, config);

		for (delay = 0; delay < 64; delay++) {
			int pass;
			u32 prev = *tpr;

			*tpr = (prev & mask) | ((u32)delay << shift);
			mctl_phy_bit_delay_compensation(para);

			pass = (dst_read_cmp(
				CFG_SYS_SDRAM_BASE,
				dram_half + 0x0c, 256, bl) == 0);

			if (pass) {
				if (first_pass < 0)
					first_pass = delay;
				last_pass = delay;
			}
		}

		eye_width = (first_pass >= 0) ? last_pass - first_pass + 1 : 0;
		center = (first_pass >= 0) ?
			 (first_pass + last_pass) / 2 : (int)saved_byte;

		if (eye_width < 3)
			result |= (saved_byte & 0x3f) << shift;
		else
			result |= ((u32)center & 0x3f) << shift;
	}

	if (is_write)
		para->tpr11 = result;
	else
		para->tpr12 = result;
	return result;
}

/*
 * 2D Eye Scan: sweep VREF × delay.
 *
 * For read: sweeps PHY VREF (packed in tpr6) and read delay (tpr12).
 * For write: sweeps DRAM VREF (MR14) and write delay (tpr11).
 *
 * Returns the optimal VREF value (to be packed into tpr6 or mr14).
 */
static int dst_2d_eye_scan(struct dram_para *para,
			   const struct dram_config *config,
			   bool is_write, int num_bytes,
			   int vref_start, int vref_end, int vref_step,
			   ulong dram_half, int *out_width)
{
	int best_vref = -1, best_width = 0;
	int vref;
	u8 row[64];

	for (vref = vref_start; vref <= vref_end; vref += vref_step) {
		int width = 0, first = -1, last = -1;
		int cur_w, cur_f;
		int delay;

		if (is_write) {
			/* Set DRAM write VREF via MR14 */
			para->mr14 = vref;
		} else {
			/* Set PHY read VREF in tpr6 (LPDDR4 uses byte 3) */
			switch (para->type) {
			case SUNXI_DRAM_TYPE_DDR3:
				para->tpr6 = (para->tpr6 & ~0xff) | (vref & 0xff);
				break;
			case SUNXI_DRAM_TYPE_LPDDR3:
				para->tpr6 = (para->tpr6 & ~0xff00) |
					     ((vref & 0xff) << 8);
				break;
			case SUNXI_DRAM_TYPE_LPDDR4:
			default:
				para->tpr6 = (para->tpr6 & ~0xff000000) |
					     ((vref & 0xff) << 24);
				break;
			}
		}

		/* Reinit DRAM with new VREF */
		if (!mctl_core_init(para, config)) {
			/* Init failed — skip this VREF */
			continue;
		}

		/* Sweep delay 0..63 */
		for (delay = 0; delay < 64; delay++) {
			u32 *tpr = is_write ? &para->tpr11 : &para->tpr12;
			u32 saved = *tpr;

			/* Set all byte lanes to same delay for 2D scan */
			*tpr = delay * 0x01010101;
			mctl_phy_bit_delay_compensation(para);

			row[delay] = (dst_read_cmp(
				CFG_SYS_SDRAM_BASE,
				dram_half + 0x0c, 256, 0) == 0);

			*tpr = saved;
		}

		/* Find longest run of passes in this row */
		cur_w = 0;
		cur_f = 0;
		for (delay = 0; delay < 64; delay++) {
			if (row[delay]) {
				if (cur_w == 0)
					cur_f = delay;
				cur_w++;
			} else {
				if (cur_w > width) {
					width = cur_w;
					first = cur_f;
					last = delay - 1;
				}
				cur_w = 0;
			}
		}
		if (cur_w > width) {
			width = cur_w;
			first = cur_f;
			last = 63;
		}

		if (width > best_width) {
			best_width = width;
			best_vref = vref;
		}

	}

	if (out_width)
		*out_width = best_width;
	if (best_width < 4)
		printf("[ERROR DST] %s 2D eye too narrow: %d\n",
		       is_write ? "W" : "R", best_width);

	return best_vref;
}

/*
 * dst_dq_eye_scan — main DST entry point.
 *
 * Runs all calibration phases, updates para with optimal values.
 * Returns 0 on success, -1 on failure.
 */
static int dst_dq_eye_scan(struct dram_para *para,
			   struct dram_config *config,
			   int *out_r_width, int *out_w_width)
{
	u32 orig_clk = para->clk;
	u32 orig_tpr6 = para->tpr6;
	u32 orig_tpr11 = para->tpr11;
	u32 orig_tpr12 = para->tpr12;
	u32 orig_odt_en = para->odt_en;
	u32 orig_mr14 = para->mr14;
	ulong dram_half;
	int num_bytes;
	bool has_vref_scan;
	bool has_write_scan;
	int opt_vref;
	int r_width = 0, w_width = 0;

	/*
	 * Read VREF (tpr6) and the per-byte delay scans (tpr11/tpr12) apply to
	 * every DRAM type. The write-VREF scan trains MR14, which only LPDDR4/
	 * LPDDR4X have - keep it gated on the type so this stays safe if the
	 * call-site gate is ever widened (DDR3/LPDDR3 have no MR14). The
	 * original v2024.10 tpr13 VREF/WRITE_SCAN flags do not exist here.
	 */
	has_vref_scan = true;
	has_write_scan = (para->type == SUNXI_DRAM_TYPE_LPDDR4);

	/*
	 * Phase 0: Preparation — detect rank/width/size, verify basic
	 * memory operation with original parameters.
	 */
	mctl_auto_detect_rank_width(para, config);
	mctl_auto_detect_dram_size(para, config);
	dram_half = mctl_calc_size(config) >> 1;
	num_bytes = config->bus_full_width ? 4 : 2;

	/* Reinit at full config to verify basic operation */
	if (!mctl_core_init(para, config)) {
		printf("[ERROR DST] Init failed\n");
		goto fail;
	}

	if (dst_memtester(dram_half, 0x8000, 8, true)) {
		printf("[ERROR DST] Initial memtest fail\n");
		goto fail;
	}

	/*
	 * Phase 1: Read 2D Eye Scan — find optimal PHY VREF (tpr6).
	 */
	if (has_vref_scan) {
		int vref_start, vref_end, vref_step;

		/* VREF sweep range depends on DRAM type */
		switch (para->type) {
		case SUNXI_DRAM_TYPE_LPDDR4:
			vref_start = 0x10;
			vref_end = 0x60;
			vref_step = 4;
			break;
		case SUNXI_DRAM_TYPE_LPDDR3:
			vref_start = 0x10;
			vref_end = 0x60;
			vref_step = 4;
			break;
		default: /* DDR3/DDR4 */
			vref_start = 0x10;
			vref_end = 0x60;
			vref_step = 4;
			break;
		}

		opt_vref = dst_2d_eye_scan(para, config, false, num_bytes,
					   vref_start, vref_end, vref_step,
					   dram_half, &r_width);
		if (opt_vref < 0) {
			printf("[ERROR DST] Read 2D scan failed\n");
			goto fail;
		}

		/* Apply optimal VREF to tpr6 */
		switch (para->type) {
		case SUNXI_DRAM_TYPE_DDR3:
			para->tpr6 = (para->tpr6 & ~0xff) | opt_vref;
			break;
		case SUNXI_DRAM_TYPE_LPDDR3:
			para->tpr6 = (para->tpr6 & ~0xff00) |
				     (opt_vref << 8);
			break;
		case SUNXI_DRAM_TYPE_LPDDR4:
		default:
			para->tpr6 = (para->tpr6 & ~0xff000000) |
				     (opt_vref << 24);
			break;
		}
	}

	/* Reinit with optimal VREF before per-byte scan */
	if (!mctl_core_init(para, config)) {
		printf("[ERROR DST] Dram Init Fail!\n");
		goto fail;
	}

	/*
	 * Phase 2: Read 1st Pass — per-byte read delay scan → tpr12.
	 */
	para->tpr12 = dst_per_byte_scan(para, config, false, num_bytes,
					dram_half);

	/*
	 * Phase 3: Write 2D Eye Scan — find optimal DRAM write VREF (MR14).
	 */
	if (has_write_scan) {
		int vref_start_w, vref_end_w, vref_step_w;

		switch (para->type) {
		case SUNXI_DRAM_TYPE_LPDDR4:
			vref_start_w = 0;
			vref_end_w = 50;
			vref_step_w = 2;
			break;
		default:
			vref_start_w = 0;
			vref_end_w = 50;
			vref_step_w = 2;
			break;
		}

		opt_vref = dst_2d_eye_scan(para, config, true, num_bytes,
					   vref_start_w, vref_end_w,
					   vref_step_w, dram_half, &w_width);
		if (opt_vref >= 0) {
			para->mr14 = opt_vref;
		} else {
			printf("[ERROR DST] Write 2D scan failed\n");
			para->mr14 = orig_mr14;
		}
	}

	/* Reinit with found MR14 */
	if (!mctl_core_init(para, config)) {
		printf("[ERROR DST] Dram Init Fail!\n");
		goto fail;
	}

	/*
	 * Phase 4: Write 2nd Pass — per-byte write delay scan → tpr11.
	 */
	para->tpr11 = dst_per_byte_scan(para, config, true, num_bytes,
					dram_half);

	/*
	 * Phase 5: Read 2nd Pass — refine tpr12 with new tpr11.
	 */
	para->tpr12 = dst_per_byte_scan(para, config, false, num_bytes,
					dram_half);

	/* Final reinit with all optimal parameters */
	if (!mctl_core_init(para, config)) {
		printf("[ERROR DST] Final init failed\n");
		goto fail;
	}

	/* Final stability check */
	if (dst_memtester(dram_half, 0x8000, 8, true)) {
		printf("[ERROR DST] Stable Memtest Fail\n");
		goto fail;
	}

	if (out_r_width)
		*out_r_width = r_width;
	if (out_w_width)
		*out_w_width = w_width;

	printf("[DST] tpr6=0x%08x tpr11=0x%08x tpr12=0x%08x mr14=0x%02x R=%d W=%d\n",
	       para->tpr6, para->tpr11, para->tpr12, para->mr14,
	       r_width, w_width);
	return 0;

fail:
	para->clk = orig_clk;
	para->tpr6 = orig_tpr6;
	para->tpr11 = orig_tpr11;
	para->tpr12 = orig_tpr12;
	para->odt_en = orig_odt_en;
	para->mr14 = orig_mr14;
	printf("[ERROR DST] Dram DST Fail\n");
	return -1;
}


unsigned long sunxi_dram_init(void)
{
	struct sunxi_prcm_reg *const prcm =
		(struct sunxi_prcm_reg *)SUNXI_PRCM_BASE;
	struct dram_config config;
	unsigned long size;
	bool dst_slot_ok;
	const struct dram_runtime_profile *profile = &profile_lpddr4;
	enum sunxi_dram_type strap_type = 0;
	const char *type;
	unsigned int width;
	unsigned int page_size;

	setbits_le32(&prcm->res_cal_ctrl, BIT(8));
	clrbits_le32(&prcm->ohms240, 0x3f);

	if (sunxi_dram_prepare_type(&strap_type))
		panic("Failed to prepare DRAM power rails.\n");

	if (strap_type == SUNXI_DRAM_TYPE_DDR3) {
		profile = &profile_ddr3;
		if (!try_dram_profile(profile, &config, &size))
			panic("This DRAM setup is currently not supported.\n");
	} else if (strap_type == SUNXI_DRAM_TYPE_LPDDR4) {
		profile = &profile_lpddr4;
		if (!try_dram_profile(profile, &config, &size))
			panic("This DRAM setup is currently not supported.\n");
	} else if (strap_type == SUNXI_DRAM_TYPE_LPDDR4X) {
		/*
		 * KOWIN LPDDR4X 16Gb — dedicated tuned profile (LPDDR4 mode).
		 * TODO 32Gb: branch here by detected geometry (ranks/size) once
		 * that hardware exists; profile selection is the only difference.
		 */
		profile = &profile_lpddr4x_16gb;
		if (!try_dram_profile(profile, &config, &size))
			panic("This DRAM setup is currently not supported.\n");
	} else if (!try_dram_profile(profile, &config, &size)) {
		profile = &profile_ddr3;
		if (!try_dram_profile(profile, &config, &size))
			panic("This DRAM setup is currently not supported.\n");
	}

	/*
	 * Memory characterisation (statistics only) for LPDDR4-class DRAM.
	 * Gated on the electrical type, so it covers both the LPDDR4 and the
	 * tuned LPDDR4X profiles at any density without changes here. DDR3/
	 * LPDDR3 boards have no host write-VREF (MR14) and skip this entirely;
	 * they boot normally.
	 *
	 * Invalidate the handoff slot up front: the magic is written only when
	 * the DST eye-scan runs and succeeds, so any other case - a non-LPDDR4
	 * board or a failed scan - leaves it cleared and ft_board_setup() finds
	 * no stash.
	 *
	 * Run the DST 2D eye-scan on a *copy* of the parameters: DST re-inits
	 * the controller at the swept/found values to measure, so afterwards
	 * re-init on the fixed profile values and keep operating on those.
	 *
	 * The slot sits at a fixed 224 MB offset; it is only within usable DRAM
	 * and below U-Boot's top-of-RAM reservation on boards with >= 512 MB.
	 * Smaller boards skip the handoff entirely - this matches the read-side
	 * check against gd->relocaddr in ft_board_setup (never write a slot that
	 * U-Boot proper would not read), and on the 128 MB variant the address
	 * is past the RAM top, where a write would alias into live DRAM.
	 */
	dst_slot_ok = size >= SZ_512M;

	if (dst_slot_ok) {
		((struct wb_dram_dst *)WB_DRAM_DST_ADDR)->magic = 0;
		flush_dcache_range(WB_DRAM_DST_ADDR,
				   WB_DRAM_DST_ADDR + sizeof(struct wb_dram_dst));
	}

	if (profile->para.type == SUNXI_DRAM_TYPE_LPDDR4) {
		struct dram_para dst_para = profile->para;
		int r_width = 0, w_width = 0;
		int dst_ok;

		dst_ok = dst_dq_eye_scan(&dst_para, &config,
					 &r_width, &w_width);

		/*
		 * DST left the controller on the swept values; re-init on the
		 * fixed profile values and keep operating on those. This is
		 * the last DRAM controller init, so the handoff is stashed
		 * only afterwards.
		 */
		if (!mctl_core_init(&profile->para, &config))
			panic("DRAM re-init after DST failed\n");

		/*
		 * Stash the measured eye parameters at a fixed DRAM address;
		 * U-Boot proper picks them up in ft_board_setup() and exposes
		 * them under /chosen/wb-dram-dst for Linux (WB6-style handoff).
		 */
		if (dst_ok == 0 && dst_slot_ok) {
			struct wb_dram_dst *h =
				(struct wb_dram_dst *)WB_DRAM_DST_ADDR;

			h->magic = WB_DRAM_DST_MAGIC;
			h->tpr6 = dst_para.tpr6;
			h->tpr11 = dst_para.tpr11;
			h->tpr12 = dst_para.tpr12;
			h->mr14 = dst_para.mr14;
			h->r_eye_width = r_width;
			h->w_eye_width = w_width;
			h->clk_mhz = profile->para.clk;
			h->size_mib = size >> 20;
			flush_dcache_range(WB_DRAM_DST_ADDR,
					   WB_DRAM_DST_ADDR +
					   sizeof(struct wb_dram_dst));
		}
	}

	type = dram_type_name(profile->para.type);
	width = config.bus_full_width ? 32 : 16;
	page_size = (1U << config.cols) * (width / 8);

	printf("DRAM topo: type=%s clk=%uMHz rank=%u width=%u rows=%u cols=%u banks=8 page=%uB size=%luMiB\n",
	       type, profile->para.clk, config.ranks, width, config.rows,
	       config.cols,
	       page_size, size >> 20);

	mctl_set_master_priority();

	return size;
};
