/*
 * S21 (o1s, Exynos 2100) boot shim: act as iBoot for the SWEET xnu.
 *
 * Reduced from ios-sweet's shim.c to what the S21 needs to reach an XNU
 * text console: paint the framebuffer, start DECON scanning it, then
 * loader_boot() copies the kernel and ADT from the ramdisk payload and
 * jumps. There is no reliable reset path this early (PSCI SYSTEM_RESET via
 * hvc/smc does nothing here, shim-test test 4), so every failure stays on
 * screen until the user holds Volume Down + Side.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"
#ifdef TOUCHTEST
#include "touch.h"
#endif

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

/* DECON0 (live DT decon_0@0x19F00000; Samsung dpu30/cal_2100/regs-decon.h) */
#define DECON0_TRIG_CON		0x19f00030UL
#define HW_TRIG_MASK_DECON	(1U << 4)

static const char *stage;

/*
 * S-Boot leaves DECON0 enabled and running (GLOBAL_CON 0x133) with the
 * hardware TE trigger configured but masked (TRIG_CON 0x3071). Clearing the
 * mask makes DECON push the framebuffer to the command-mode panel on every
 * TE, so later writes (ours, then xnu's console) appear by themselves
 * (shim-test test 6). XNU never touches DECON, so the mask stays clear.
 */
static uint32_t saved_trig;

void display_kick(void)
{
	volatile uint32_t *trig = (volatile uint32_t *)DECON0_TRIG_CON;
	saved_trig = *trig;
	*trig = saved_trig & ~HW_TRIG_MASK_DECON;
	__asm__ volatile("dsb sy" ::: "memory");
}

#if defined(GICTEST) || defined(UFSTEST)
#ifdef GICTEST
#include "gictest.h"
#endif
#ifdef UFSTEST
#include "ufs.h"
#endif

extern char _head[];

static void delay_ms(unsigned ms)
{
	uint64_t t0 = rsr("cntpct_el0");
	while (rsr("cntpct_el0") - t0 < (uint64_t)ms * 26000)	/* 26 MHz counter */
		;
}

/*
 * Put TRIG_CON back as S-Boot left it before Linux runs: an unmasked
 * trigger makes Linux's dsim_irq_handler fault during probe (shim-test 6).
 */
static void display_restore(void)
{
	*(volatile uint32_t *)DECON0_TRIG_CON = saved_trig;
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(500);
}

typedef void (*linux_entry_t)(uint64_t dtb, uint64_t x1, uint64_t x2, uint64_t x3);

static void __attribute__((noreturn)) boot_linux(uint64_t dtb)
{
	/* mks21boot.py --chain places the LineageOS Image 2 MiB after our header. */
	linux_entry_t entry = (linux_entry_t)((uintptr_t)_head + 0x200000);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	entry(dtb, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}
#endif

static void __attribute__((noreturn)) halt(void)
{
	con_skip();
	con_str("HALT", "hold Vol Down + Side to restart");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
#ifdef GICTEST
	if (slot == 5) {	/* IRQ from current EL, SPx: the probe's timer/SGI */
		gictest_irq();
		return;
	}
#endif
	con_clear(BLUE);
	/* No switch: clang turns it into a table of absolute string pointers. */
	unsigned kind = slot & 3;
	if (kind == 0)
		con_str("PANIC", "SYNC");
	else if (kind == 1)
		con_str("PANIC", "IRQ");
	else if (kind == 2)
		con_str("PANIC", "FIQ");
	else
		con_str("PANIC", "SERROR");
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X0", regs[0]);
	con_hex("X30", regs[29]);
	halt();
}

/* start.S keeps the PSCI secondary entry; nothing starts secondaries yet. */
void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
	stage = "display";
	con_clear(RED);
	display_kick();
	con_str("SHIM", "S21 o1s: xnu loader");
	con_hex2("ENTRY", "el=", (uint32_t)(rsr("currentel") >> 2), "sctlr=", (uint32_t)rsr("sctlr_el1"));
	con_hex("DTB", dtb);
	con_skip();

#ifdef GICTEST
	stage = "gictest";
	gictest_run();
	con_skip();
	con_str("LINUX", "booting LineageOS in 20 s");
	delay_ms(20000);
	gictest_report(dtb);
	display_restore();
	boot_linux(dtb);
#endif

#ifdef UFSTEST
	stage = "ufstest";
	test_ufs();
	stage = "ufs restore";
	ufs_restore();
	con_skip();
	con_str("LINUX", "booting LineageOS in 40 s");
	delay_ms(40000);
	ufs_report(dtb);
	display_restore();
	boot_linux(dtb);
#endif

#ifdef TOUCHTEST
	stage = "touchtest";
	test_touch();
	con_skip();
	con_str("IOS", "booting iOS in 5 s");
	{
		uint64_t t0 = rsr("cntpct_el0");
		while (rsr("cntpct_el0") - t0 < 5ull * 26000000)
			;
	}
#endif

	stage = "loader";
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	halt();
}
