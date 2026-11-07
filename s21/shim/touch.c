/*
 * S21 touch probe (shim --touchtest): talk to the Samsung LSI touch IC
 * (slsi_ts, I2C address 0x48) by bit-banging the bus pins, then show live
 * touch events on screen for TOUCH_SECONDS before the normal iOS boot.
 *
 * From the phone's live device tree:
 *   hsi2c@105D0000 (hsi2c8) touchscreen@48, pins tspi2c-bus = gpp4-0 (SCL),
 *   gpp4-1 (SDA), function 2 (I2C). gpp4 is bank 5 of pinctrl@10430000
 *   (peric0); Samsung banks are 0x20 apart, so gpp4 = 0x10430080.
 *   Interrupt: gpa1-0 (pinctrl@14EC0000 + 0x20), active low.
 *   Power: tsp_io = LDO27 (s2mps23), tsp_avdd = LDO21 (s2mps24), marked
 *   sec,regulator_boot_on: S-Boot is expected to leave them on.
 * Bit-banging needs no HSI2C clock, USI mode or controller setup. Lines are
 * driven open-drain: low = output 0, high = input (the bus pull-ups).
 *
 * slsi_ts commands (Samsung sec_input slsi_ts.h):
 *   0x52 READ_DEVICE_ID (5), 0x55 READ_BOOT_STATUS (1: 0x10 boot, 0x20 app),
 *   0x51 READ_FW_STATUS? (read and shown raw), 0x10 SENSE_ON,
 *   0x60 READ_ONE_EVENT (16 bytes), 0x62 CLEAR_EVENT_STACK.
 * Coordinate event: b0 [1:0] eid (0 = coordinate), [5:2] tid, [7:6] status
 * (1 press, 2 move, 3 release); x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0xf.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GPP4		0x10430080UL
#define GPA1		0x14ec0020UL
#define USI_SWCONF	0x10421024UL	/* sysreg-peri + samsung,usi-offset */
#define HSI2C8		0x105d0000UL
#define CON		0x00
#define DAT		0x04
#define PUD		0x08
#define PIN_SCL		0
#define PIN_SDA		1
#define TS_ADDR		0x48
#define TOUCH_SECONDS	45

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static uint32_t saved_con, saved_dat, saved_pud;

static void udelay(unsigned us)
{
	uint64_t t0 = rsr("cntpct_el0");
	while (rsr("cntpct_el0") - t0 < (uint64_t)us * 26)
		;
}

/* open drain: release = input, low = output (DAT bit already 0) */
static void line(int pin, int high)
{
	uint32_t con = rd(GPP4 + CON) & ~(0xfu << (4 * pin));
	if (!high)
		con |= 1u << (4 * pin);
	wr(GPP4 + CON, con);
}

static int sense(int pin)
{
	return (rd(GPP4 + DAT) >> pin) & 1;
}

#define HALF 5	/* us: ~100 kHz */

static int scl_high(void)
{
	line(PIN_SCL, 1);
	for (unsigned i = 0; i < 2000 && !sense(PIN_SCL); i++)	/* clock stretching, 2 ms */
		udelay(1);
	udelay(HALF);
	return sense(PIN_SCL);
}

static void i2c_start(void)
{
	line(PIN_SDA, 1);
	scl_high();
	line(PIN_SDA, 0);
	udelay(HALF);
	line(PIN_SCL, 0);
	udelay(HALF);
}

static void i2c_stop(void)
{
	line(PIN_SDA, 0);
	udelay(HALF);
	scl_high();
	line(PIN_SDA, 1);
	udelay(HALF);
}

/* returns 1 on ACK */
static int i2c_tx(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		line(PIN_SDA, (b >> i) & 1);
		udelay(HALF);
		scl_high();
		line(PIN_SCL, 0);
	}
	line(PIN_SDA, 1);
	udelay(HALF);
	scl_high();
	int ack = !sense(PIN_SDA);
	line(PIN_SCL, 0);
	udelay(HALF);
	return ack;
}

static uint8_t i2c_rx(int ack)
{
	uint8_t b = 0;
	line(PIN_SDA, 1);
	for (int i = 0; i < 8; i++) {
		udelay(HALF);
		scl_high();
		b = (uint8_t)(b << 1 | sense(PIN_SDA));
		line(PIN_SCL, 0);
	}
	line(PIN_SDA, !ack);
	udelay(HALF);
	scl_high();
	line(PIN_SCL, 0);
	line(PIN_SDA, 1);
	udelay(HALF);
	return b;
}

/* write cmd (+ optional data), then optionally read n bytes; 0 = ok */
static int ts_xfer(uint8_t cmd, const uint8_t *wdata, int wn, uint8_t *rdata, int rn)
{
	i2c_start();
	if (!i2c_tx(TS_ADDR << 1)) {
		i2c_stop();
		return -1;
	}
	if (!i2c_tx(cmd)) {
		i2c_stop();
		return -2;
	}
	for (int i = 0; i < wn; i++)
		if (!i2c_tx(wdata[i])) {
			i2c_stop();
			return -3;
		}
	if (rn) {
		i2c_stop();
		udelay(50);
		i2c_start();
		if (!i2c_tx(TS_ADDR << 1 | 1)) {
			i2c_stop();
			return -4;
		}
		for (int i = 0; i < rn; i++)
			rdata[i] = i2c_rx(i < rn - 1);
	}
	i2c_stop();
	return 0;
}

static uint32_t be32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

void test_touch(void)
{
	saved_con = rd(GPP4 + CON);
	saved_dat = rd(GPP4 + DAT);
	saved_pud = rd(GPP4 + PUD);
	con_str("TOUCH", "slsi_ts probe, bit-banged I2C");
	con_hex2("GPP4", "con=", saved_con, "dat=", saved_dat);
	con_hex2("", "pud=", saved_pud, "usi=", rd(USI_SWCONF));
	con_hex2("GPA1", "con=", rd(GPA1 + CON), "dat=", rd(GPA1 + DAT));

	/* Pins to GPIO input (released), DAT bits 0 so "output" drives low. */
	wr(GPP4 + PUD, saved_pud & ~0xffu);
	wr(GPP4 + DAT, rd(GPP4 + DAT) & ~3u);
	line(PIN_SCL, 1);
	line(PIN_SDA, 1);
	udelay(100);
	con_hex2("IDLE", "scl=", sense(PIN_SCL), "sda=", sense(PIN_SDA));
	if (!sense(PIN_SCL)) {
		con_str("TOUCH", "SCL low: bus not powered/pulled up");
		goto out;
	}
	for (int i = 0; i < 9 && !sense(PIN_SDA); i++) {	/* free a stuck slave */
		line(PIN_SCL, 0);
		udelay(HALF);
		scl_high();
	}
	i2c_stop();

	uint8_t id[8] = {0}, st[4] = {0}, fw[4] = {0};
	int r = ts_xfer(0x52, 0, 0, id, 5);
	con_hex2("ID 52", "r=", (uint32_t)r, "id=", be32(id));
	con_hex2("", "id4=", id[4], "", 0);
	if (r == -1) {
		con_str("TOUCH", "no ACK at 0x48 (IC off?)");
		goto out;
	}
	r = ts_xfer(0x55, 0, 0, st, 1);
	con_hex2("BOOT 55", "r=", (uint32_t)r, "st=", st[0]);
	r = ts_xfer(0x51, 0, 0, fw, 4);
	con_hex2("FW 51", "r=", (uint32_t)r, "v=", be32(fw));

	r = ts_xfer(0x62, 0, 0, 0, 0);	/* clear event stack */
	uint8_t on = 0;
	r = ts_xfer(0x10, &on, 0, 0, 0);	/* SENSE_ON */
	con_hex2("SENSE", "on r=", (uint32_t)r, "", 0);
	udelay(100000);

	con_str("TOUCH", "touch the screen now (45 s)");
	int live = con_set_row(0);
	con_set_row(live);
	uint32_t events = 0, errs = 0;
	uint64_t t_end = rsr("cntpct_el0") + (uint64_t)TOUCH_SECONDS * 26000000;
	while (rsr("cntpct_el0") < t_end) {
		/* Poll every 10 ms; attn (gpa1-0 low) is shown but not trusted yet. */
		int attn = !((rd(GPA1 + DAT) >> 0) & 1);
		udelay(10000);
		uint8_t ev[16] = {0};
		r = ts_xfer(0x60, 0, 0, ev, 16);
		if (r) {
			errs++;
			continue;
		}
		if (!(ev[0] | ev[1] | ev[2] | ev[3] | ev[7]))
			continue;	/* empty event stack */
		con_set_row(live + 3);
		con_hex2("ATTN", "low=", attn, "", 0);
		events++;
		con_set_row(live);
		con_hex2("EV", "b0-3=", be32(ev), "b4-7=", be32(ev + 4));
		if ((ev[0] & 3) == 0) {
			uint32_t x = (uint32_t)ev[1] << 4 | ev[3] >> 4;
			uint32_t y = (uint32_t)ev[2] << 4 | (ev[3] & 0xf);
			con_hex2("XY", "x=", x, "y=", y);
		} else {
			con_hex2("XY", "eid=", ev[0] & 3, "n=", events);
		}
		con_hex2("CNT", "events=", events, "errs=", errs);
	}
	con_set_row(live + 4);
	con_hex2("DONE", "events=", events, "errs=", errs);
out:
	/* Give the pins back as S-Boot left them. */
	wr(GPP4 + DAT, saved_dat);
	wr(GPP4 + PUD, saved_pud);
	wr(GPP4 + CON, saved_con);
	(void)HSI2C8;
}
