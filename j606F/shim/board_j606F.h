/*
 * board_j606F.h -- Lenovo Tab P11 (TB-J606F, Qualcomm SM6115) board constants
 * for the ios-sweet shim.  Replaces the hardcoded sweet (SM7150) addresses.
 *
 * Fill each TODO from tools/collect-j606F-facts.sh output, then make the shim
 * include this header instead of the inline sweet #defines (see PORT_README.md
 * "shim changes").  Values here MUST match the J_* values in mkadt_j606F.py.
 */
#ifndef BOARD_J606F_H
#define BOARD_J606F_H

/* --- UFS host the Lenovo ABL leaves running (shim/ufs.c UFS_BASE). ----------
 * sweet: 0x01d84000.  SM6115 ufshc@ offset -- see j606F-facts/ufs.txt.
 * Same value as J_UFS in mkadt_j606F.py. */
#define J606F_UFS_BASE      0x04804000 /* soc/ufshc@4804000 reg base (live FDT) */

/* --- Framebuffer ABL left at the cont_splash region (shim/console.c). -------
 * sweet: 0x9c000000, 1080x2400, stride 1080*4.  P11 panel is 1200x2000.
 * See j606F-facts/framebuffer.txt (reserved-memory cont_splash_region reg). */
#define J606F_FB_BASE       0x5c000000 /* cont_splash_region@5c000000 (live FDT) */
#define J606F_FB_WIDTH      1200 /* confirmed: panel timing 0x4b0 x 0x7d0     */
#define J606F_FB_HEIGHT     2000
#define J606F_FB_STRIDE     (J606F_FB_WIDTH * 4) /* 4800; confirm byte order on first paint */

/* --- GICv3 (shim early IRQ setup, if any; XNU gets these via the ADT). ------
 * sweet: GICD 0x17a00000 / GICR 0x17a60000.  See j606F-facts/gic.txt. */
#define J606F_GICD_BASE     0x0f200000  /* interrupt-controller@f200000 reg[0] */
#define J606F_GICR_BASE     0x0f300000  /* reg[2] */

/* --- Kernel-managed DRAM base (shim MMU map). Must match J_PHYS_BASE /
 * J_HIGH_PHYS_BASE in mkadt_j606F.py.  Above all SM6115 XPU carve-outs.
 * See j606F-facts/reserved-memory.txt + memory.txt. */
#define J606F_DRAM_BASE     0x100000000ULL /* upper bank (--highmem), 3 GiB clean */

/* --- Arch timer. 19.2 MHz is the Qualcomm standard; confirm in cpus.txt. --- */
#define J606F_TIMEBASE_HZ   19200000u

#endif /* BOARD_J606F_H */
