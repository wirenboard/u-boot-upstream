/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * DRAM DST (2D eye-scan) characterisation handoff: SPL -> U-Boot -> Linux.
 *
 * For LPDDR4-class DRAM the SPL runs a 2D eye-scan purely to characterise the
 * fitted memory (statistics) - the controller still runs on the fixed profile
 * values, the scan only measures. The handoff then has three steps:
 *
 *   1. SPL fills a struct wb_dram_dst with the measured values and writes it
 *      to a fixed DRAM address (WB_DRAM_DST_ADDR).
 *   2. U-Boot proper, in ft_board_setup(), reads the struct back from that
 *      address and adds it to the kernel device tree as /chosen/wb-dram-dst.
 *   3. Linux reads it from /proc/device-tree/chosen/wb-dram-dst.
 *
 * Modelled on the WB6 spl_handoff_data mechanism
 * (board/wirenboard/mx6ul_wirenboard).
 */
#ifndef _WB_DRAM_DST_H
#define _WB_DRAM_DST_H

#include <linux/types.h>

/*
 * Fixed scratch address: DRAM base (0x40000000) + 0xe000000 = 224 MiB.
 * Chosen to sit in the gap above the bootm/kernel region (kernel_addr_r
 * 0x40080000 .. +BOOTM_SIZE ends ~0x4a080000) and below fdt_addr_r
 * (0x4fa00000), clear of BL31 (0x40000000), U-Boot proper (0x4a000000) and
 * the ramdisk (0x4ff00000), so it survives from the SPL until
 * ft_board_setup() runs in U-Boot proper.
 */
#define WB_DRAM_DST_ADDR	0x4e000000

/*
 * "WBDS" - validity guard against garbage. The SPL writes this magic only
 * after a successful scan, so ft_board_setup() trusts the slot only if it
 * reads back exactly this value; a board that never wrote it (DDR3, <512 MB,
 * or a failed scan) reads back garbage != WBDS and is skipped.
 */
#define WB_DRAM_DST_MAGIC	0x57424453	/* "WBDS" */

struct wb_dram_dst {
	u32 magic;
	u32 tpr6;
	u32 tpr11;
	u32 tpr12;
	u32 mr14;
	u32 r_eye_width;
	u32 w_eye_width;
	u32 clk_mhz;
	u32 size_mib;
};

#endif /* _WB_DRAM_DST_H */
