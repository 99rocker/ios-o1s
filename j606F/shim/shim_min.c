/*
 * shim_min.c -- TB-J606F first-light shim. Replaces the full sweet shim.c for
 * milestone 0: prove that ABL hands off, the CPU runs our code, and the panel
 * framebuffer (cont_splash @0x5c000000) is live -- WITHOUT touching any risky
 * MMIO (no UFS, no GIC, no PSCI, no MMU enable). The full shim's UFS probe
 * causes an external abort that TrustZone turns into Qualcomm CrashDump; this
 * build avoids every such access so we can confirm the basics in isolation.
 *
 * Link contract (start.S): shim_main, secondary_main, exc_handler.
 * Build it in place of shim.c; the unused mmu/loader/ufs objects still link.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

/* If any synchronous exception DOES fire, paint it so we see ESR/ELR/FAR on the
 * panel instead of a silent hang. (External aborts may still escape to EL3.) */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	con_str("PANIC", "exception (min shim)");
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X0", regs ? regs[0] : 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* No secondaries are started in the minimal shim; just park if one ever lands. */
void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
	/* First thing: paint the whole panel so "did our code run + is the FB
	 * live" is answered immediately, before anything can go wrong. */
	con_clear(RED);
	con_skip();
	con_str("J606F", "minimal shim alive");
	con_str("FB", "0x5c000000 1200x2000 a8r8g8b8");
	con_hex2("DTB", "at=", (uint32_t)dtb, "magic=",
		 dtb ? __builtin_bswap32(*(volatile uint32_t *)dtb) : 0);
	con_hex2("CPU", "mpidr=", (uint32_t)rsr("mpidr_el1"),
		 "currentel=", (uint32_t)(rsr("CurrentEL") >> 2));
	con_skip();
	con_str("STATUS", "FB + CPU ok -- parked (hold power to reboot)");

	/* Park forever. No reset, no MMIO, nothing TrustZone can fault on. */
	for (;;)
		__asm__ volatile("wfe");
}
