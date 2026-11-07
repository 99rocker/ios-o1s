/*
 * S21 shim PMIC test (--pmictest): over ACPM (shim/acpm.c), read the power-key
 * status and the RTC time, so the kernel power-button and clock drivers can use
 * confirmed registers before an 8-minute kernel build.
 *
 * Main PMIC (s2mps23, ACPM channel 0, type/addr 0x01):
 *   STATUS1 reg 0x0E, bit0 PWRON = 1 while the side key is held (s2mps23-regulator.h).
 * RTC (ACPM channel 0, type/addr 0x02; s2mps23_core.c creates it at I2C_ADDR_RTC):
 *   UPDATE reg 0x02; set RUDR (bit0) to latch the current time into the count regs,
 *   then read 7 BCD bytes sec/min/hour/week/date/mon/year.
 *   CONFIG_RTC_DRV_S2MPS23 (not _S2MP) -> count base 0x05 (the #else layout); this
 *   test also shows base 0x04 so the right one is obvious.
 */
#include <stdint.h>
#include "console.h"
#include "acpm.h"
#include "pmictest.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define PM_MAIN   1   /* type/addr of the main PMIC page */
#define PM_RTC    2   /* type/addr of the RTC page */

static void delay_ms(unsigned ms)
{
	uint64_t t0 = rsr("cntpct_el0");
	while (rsr("cntpct_el0") - t0 < (uint64_t)ms * 26000)
		;
}

static void rtc_dump(const char *label, unsigned base)
{
	uint8_t b[7] = {0};
	for (int i = 0; i < 7; i++)
		(void)acpm_pmic(ACPM_READ, 0, PM_RTC, base + i, 0, 0, &b[i]);
	/* BCD: value = high nibble*10 + low nibble; hour low 5 bits, pm bit 6 */
	con_hex2(label, "raw0-3=", (uint32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]),
	    "raw4-6=", (uint32_t)(b[4] << 16 | b[5] << 8 | b[6]));
	unsigned yr = 2000 + (b[6] >> 4) * 10 + (b[6] & 0xf);
	unsigned mo = ((b[5] >> 4) & 1) * 10 + (b[5] & 0xf);
	unsigned da = ((b[4] >> 4) & 3) * 10 + (b[4] & 0xf);
	unsigned hr = ((b[2] >> 4) & 1) * 10 + (b[2] & 0xf);
	unsigned mi = (b[1] >> 4) * 10 + (b[1] & 0xf);
	unsigned se = (b[0] >> 4) * 10 + (b[0] & 0xf);
	con_hex2("", "Y=", yr, "date=", mo * 100 + da);
	con_hex2("", "time=", hr * 10000 + mi * 100 + se, "pm=", (b[2] >> 6) & 1);
}

void test_pmic(void)
{
	con_str("PMIC", "power key + RTC over ACPM");
	int err = acpm_init();
	if (err) {
		con_hex2("ACPM", "init err=", (uint32_t)err, "", 0);
		return;
	}

	uint8_t ctrl = 0, upd = 0, st1 = 0, st2 = 0;
	(void)acpm_pmic(ACPM_READ, 0, PM_MAIN, 0x0E, 0, 0, &st1);
	(void)acpm_pmic(ACPM_READ, 0, PM_MAIN, 0x0F, 0, 0, &st2);
	con_hex2("STATUS", "s1=", st1, "s2=", st2);

	/* RTC: latch, then read both candidate layouts */
	(void)acpm_pmic(ACPM_READ, 0, PM_RTC, 0x00, 0, 0, &ctrl);
	int r = acpm_pmic(ACPM_UPDATE, 0, PM_RTC, 0x02, 0x01, 0x01, 0);
	delay_ms(20);
	(void)acpm_pmic(ACPM_READ, 0, PM_RTC, 0x02, 0, 0, &upd);
	con_hex2("RTC", "ctrl=", ctrl, "updr=", (uint32_t)r);
	con_hex2("", "upd=", upd, "", 0);
	rtc_dump("RTC@04", 0x04);
	rtc_dump("RTC@05", 0x05);

	con_skip();
	con_str("KEY", "press/hold side button (8 s)");
	int shown = con_set_row(0);
	con_set_row(shown);
	con_skip();
	int live = con_set_row(shown);
	(void)live;
	uint64_t t_end = rsr("cntpct_el0") + 8ull * 26000000;
	uint8_t last = 0xff;
	unsigned presses = 0;
	while (rsr("cntpct_el0") < t_end) {
		uint8_t s = 0;
		(void)acpm_pmic(ACPM_READ, 0, PM_MAIN, 0x0E, 0, 0, &s);
		if ((s & 1) != (last & 1)) {
			if (s & 1)
				presses++;
			last = s;
			con_set_row(shown);
			con_hex2("PWRON", "status=", s, "presses=", presses);
		}
		delay_ms(20);
	}
	con_set_row(shown + 1);
	con_hex2("DONE", "presses=", presses, "", 0);
}
