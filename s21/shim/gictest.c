/*
 * S21 GIC + timer probe: does Samsung's EL2 hypervisor let EL1 drive the
 * GICv3 the way SWEET xnu does (distributor, redistributor, ICC system
 * registers, virtual-timer PPI, SGI)? Adapted from ios-sweet's milestone-2
 * shim.c test_gic()/irq_window() (MIT), with the S21 live-FDT values.
 *
 * Every step prints a line before it touches hardware, so a hang shows on
 * screen where it happened. Results also go into the DTB /model string for
 * `adb shell su -c "dmesg | grep 'Machine model'"` after LineageOS boots.
 */
#include <stdint.h>
#include "console.h"
#include "gictest.h"

#define GICD_BASE		0x10200000UL	/* interrupt-controller@10200000 */
#define GICR_BASE		0x10240000UL
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x20000UL
#define GICD_TYPER		0x04
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define VTIMER_INTID		27	/* timer PPI 11 */
#define PTIMER_INTID		30	/* timer PPI 14 */
#define SGI_INTID		1

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* results for the /model summary */
static uint32_t res_vtimer, res_sgi, res_sre, res_gicr_found;

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static uint64_t freq(void)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? f : 26000000;	/* S-Boot may leave CNTFRQ unset (Exynos 2200 does) */
}

void gictest_delay_ms(unsigned ms)
{
	uint64_t end = now() + freq() / 1000 * ms;
	while (now() < end)
		;
}

static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 | ((m >> 8) & 0xff) << 16 |
	       (uint64_t)id << 24 | (1U << (m & 0xf));
}

void gictest_irq(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", freq() / 100);
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", freq() / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static uint32_t irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = freq();

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	}
	isb();
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	uint32_t pend = rd32(r + GICR_ISPENDR0);
	uint32_t hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "hppi=", hppi);
	return irq_count;
}

void gictest_run(void)
{
	con_str("GICTEST", "S21 GICv3 + timer probe");
	con_hex2("TIMER", "cntfrq=", (uint32_t)rsr("cntfrq_el0"), "cnt=", (uint32_t)now());
	con_hex("MPIDR", rsr("mpidr_el1"));

	con_str("STEP", "icc_sre_el1");
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	res_sre = rsr("icc_sre_el1") & 1;
	con_hex2("SRE", "was=", sre, "now=", res_sre);
	if (!res_sre) {
		con_str("GIC", "no sysreg interface: STOP");
		return;
	}

	con_str("STEP", "gicd read");
	uint32_t dctlr = rd32(GICD_BASE);
	con_hex2("GICD", "ctlr=", dctlr, "typer=", rd32(GICD_BASE + GICD_TYPER));
	con_str("STEP", "gicd write");
	wr32(GICD_BASE, dctlr | 0x13);	/* EnableGrp1NS | EnableGrp1A | ARE_NS, as Linux */
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	con_str("STEP", "gicr find");
	uintptr_t r = find_gicr();
	res_gicr_found = r != 0;
	if (!r) {
		con_str("GICR", "no redistributor matched MPIDR: STOP");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	con_str("STEP", "gicr config");
	wr32(r + GICR_IGROUPR0, 0xffffffff);
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "now=", rd32(r + GICR_ISENABLER0), "grp=", rd32(r + GICR_IGROUPR0));

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	con_str("STEP", "vtimer irq window");
	res_vtimer = irq_window("VTIMER", r, VTIMER_INTID);
	con_str("STEP", "sgi irq window");
	res_sgi = irq_window("SGI1", r, SGI_INTID);

	/* hand the GIC back quiet: our PPIs/SGI off, CPU interface group 1 off */
	wr32(r + GICR_ICENABLER0, (1U << VTIMER_INTID) | (1U << SGI_INTID));
	wsr("icc_igrpen1_el1", 0);
	isb();
	con_skip();
	con_str("RESULT", res_vtimer >= 20 && res_sgi >= 8 ? "GIC + timer OK for xnu" : "GIC/timer PROBLEM, see above");
}

/* "GT s1 r1 vt1E sg0A " -> overwrite the start of the DTB root /model */
void gictest_report(uintptr_t dtb)
{
	static const char m[] = "Samsung O1S";
	char out[24];
	int n = 0;
	const char *hex = "0123456789ABCDEF";

	out[n++] = 'G'; out[n++] = 'T'; out[n++] = ' ';
	out[n++] = 's'; out[n++] = '0' + (res_sre & 1); out[n++] = ' ';
	out[n++] = 'r'; out[n++] = '0' + (res_gicr_found & 1); out[n++] = ' ';
	out[n++] = 'v'; out[n++] = 't';
	out[n++] = hex[(res_vtimer >> 4) & 15]; out[n++] = hex[res_vtimer & 15]; out[n++] = ' ';
	out[n++] = 's'; out[n++] = 'g';
	out[n++] = hex[(res_sgi >> 4) & 15]; out[n++] = hex[res_sgi & 15]; out[n++] = ' ';

	for (uintptr_t a = dtb; a < dtb + 0x200000; a++) {
		int i = 0;
		while (i < 11 && *(volatile char *)(a + i) == m[i])
			i++;
		if (i == 11) {	/* same length as before: 37 chars + NUL stay */
			for (int k = 0; k < n; k++)
				*(volatile char *)(a + k) = out[k];
			__asm__ volatile("dsb sy" ::: "memory");
			return;
		}
	}
}
