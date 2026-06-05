// SPDX-License-Identifier: GPL-2.0+
/*
 * AXP15060 driver based on AXP221 driver
 *
 *
 * (C) Copyright 2023 Nikita Maslov <nikita.maslov@wirenboard.ru>
 *
 * Based on axp221.c
 * (C) Copyright 2014 Hans de Goede <hdegoede@redhat.com>
 * (C) Copyright 2013 Oliver Schinagl <oliver@schinagl.nl>
 */

#include <command.h>
#include <errno.h>
#include <asm/arch/pmic_bus.h>
#include <axp_pmic.h>

static u8 axp15060_mvolt_to_cfg(int mvolt, int min, int max, int div)
{
	if (mvolt < min)
		mvolt = min;
	else if (mvolt > max)
		mvolt = max;

	return (mvolt - min) / div;
}

int axp_set_dcdc1(unsigned int mvolt)
{
	int ret;
	u8 cfg = axp15060_mvolt_to_cfg(mvolt, 1500, 3400, 100);

	if (mvolt == 0)
		return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL1,
					AXP15060_OUTPUT_CTRL1_DCDC1_EN);

	ret = pmic_bus_write(AXP15060_DCDC1_CTRL, cfg);
	if (ret)
		return ret;

	return pmic_bus_setbits(AXP15060_OUTPUT_CTRL1,
				AXP15060_OUTPUT_CTRL1_DCDC1_EN);
}

int axp_set_dcdc2(unsigned int mvolt)
{
	int ret;
	u8 cfg = axp15060_mvolt_to_cfg(mvolt, 500, 1540, 10);

	if (cfg >= 70)
		cfg = 70 + (cfg - 70) * 2;

	if (mvolt == 0)
		return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL1,
					AXP15060_OUTPUT_CTRL1_DCDC2_EN);

	ret = pmic_bus_write(AXP15060_DCDC2_CTRL, cfg);
	if (ret)
		return ret;

	return pmic_bus_setbits(AXP15060_OUTPUT_CTRL1,
				AXP15060_OUTPUT_CTRL1_DCDC2_EN);
}

int axp_set_dcdc3(unsigned int mvolt)
{
	int ret;
	u8 cfg = axp15060_mvolt_to_cfg(mvolt, 500, 1540, 10);

	if (cfg >= 70)
		cfg = 70 + (cfg - 70) * 2;

	if (mvolt == 0)
		return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL1,
					AXP15060_OUTPUT_CTRL1_DCDC3_EN);

	ret = pmic_bus_write(AXP15060_DCDC3_CTRL, cfg);
	if (ret)
		return ret;

	return pmic_bus_setbits(AXP15060_OUTPUT_CTRL1,
				AXP15060_OUTPUT_CTRL1_DCDC3_EN);
}

int axp_set_dcdc4(unsigned int mvolt)
{
	int ret;
	u8 cfg = axp15060_mvolt_to_cfg(mvolt, 500, 1540, 10);

	if (cfg >= 70)
		cfg = 70 + (cfg - 70) * 2;

	if (mvolt == 0)
		return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL1,
					AXP15060_OUTPUT_CTRL1_DCDC4_EN);

	ret = pmic_bus_write(AXP15060_DCDC4_CTRL, cfg);
	if (ret)
		return ret;

	return pmic_bus_setbits(AXP15060_OUTPUT_CTRL1,
				AXP15060_OUTPUT_CTRL1_DCDC4_EN);
}

int axp_set_dcdc5(unsigned int mvolt)
{
	int ret;
	u8 cfg = axp15060_mvolt_to_cfg(mvolt, 800, 1840, 10);

	if (cfg >= 32)
		cfg = 32 + (cfg - 32) / 2;

	if (mvolt == 0)
		return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL1,
					AXP15060_OUTPUT_CTRL1_DCDC5_EN);

	ret = pmic_bus_write(AXP15060_DCDC5_CTRL, cfg);
	if (ret)
		return ret;

	return pmic_bus_setbits(AXP15060_OUTPUT_CTRL1,
				AXP15060_OUTPUT_CTRL1_DCDC5_EN);
}

int axp_set_aldo(int aldo_num, unsigned int mvolt)
{
	int ret;
	u8 cfg;

	if (aldo_num < 1 || aldo_num > 4)
		return -EINVAL;

	if (mvolt == 0)
		return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL2,
				AXP15060_OUTPUT_CTRL2_ALDO1_EN << (aldo_num - 1));

	cfg = axp15060_mvolt_to_cfg(mvolt, 700, 3300, 100);

	if (aldo_num == 1)
		ret = pmic_bus_write(AXP15060_ALDO1_CTRL, cfg);
	else
		ret = pmic_bus_write(AXP15060_ALDO2_CTRL + (aldo_num - 2), cfg);

	if (ret)
		return ret;

	return pmic_bus_setbits(AXP15060_OUTPUT_CTRL2,
				AXP15060_OUTPUT_CTRL2_ALDO1_EN << (aldo_num - 1));
}

/* TODO: re-work other AXP drivers to consolidate ALDO functions. */
int axp_set_aldo1(unsigned int mvolt)
{
	return axp_set_aldo(1, mvolt);
}

int axp_set_aldo2(unsigned int mvolt)
{
	return axp_set_aldo(2, mvolt);
}

int axp_set_aldo3(unsigned int mvolt)
{
	return axp_set_aldo(3, mvolt);
}

int axp_set_aldo4(unsigned int mvolt)
{
	return axp_set_aldo(4, mvolt);
}

int axp_set_dldo(int dldo_num, unsigned int mvolt)
{
	return -1;  // unsupported
}

int axp_set_eldo(int eldo_num, unsigned int mvolt)
{
	return -1;  // unsupported
}

int axp_set_sw(bool on)
{
	if (on)
		return pmic_bus_setbits(AXP15060_OUTPUT_CTRL3,
					AXP15060_OUTPUT_CTRL3_SWOUT_EN);

	return pmic_bus_clrbits(AXP15060_OUTPUT_CTRL3,
				AXP15060_OUTPUT_CTRL3_SWOUT_EN);
}

int axp_init(void)
{
	return pmic_bus_init();
}
