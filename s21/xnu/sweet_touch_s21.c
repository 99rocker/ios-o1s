/*
 * SWEET touch, Samsung Galaxy S21 (o1s, Exynos 2100): Samsung LSI touch IC
 * (slsi_ts, y792 firmware) at I2C address 0x48 on hsi2c8.
 *
 * From the phone's live device tree:
 *   bus pins tspi2c-bus = gpp4-0 (SCL), gpp4-1 (SDA): pinctrl@10430000 bank
 *   gpp4 at +0x80 (Samsung banks are 0x20 apart; gpp4 is the 5th).
 *   interrupt (attn) = gpa1-0, active low: pinctrl@14EC0000 + 0x20.
 *   rails tsp_io = s2mps23 LDO27 (1.8 V), tsp_avdd = s2mps24 LDO21 (3.0 V): S-Boot
 *   leaves them OFF. They are switched on here through ACPM (below), after
 *   the scheduler and the secondary CPUs are up: ACPM traffic before XNU's
 *   PSCI CPU_ON made cpu1 fail to start (phone panic "cpu=1 startup timeout").
 * The bus is bit-banged (open drain: low = output 0, high = input + bus
 * pull-up), so no HSI2C clock, USI mode or controller setup is needed. Pins
 * are switched from the I2C function to GPIO while the driver runs.
 *
 * slsi_ts protocol (Samsung sec_input): 0x52 device ID, 0x55 boot status,
 * 0x62 clear event stack, 0x10 sense on, 0x60 read one 16-byte event.
 * Coordinate event: b0 [1:0] eid (0), [5:2] touch id, [7:6] status
 * (1 press, 2 move, 3 release); x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0xf;
 * b7 [5:0] events left. Coordinates are scaled from sweettouchmaxx/maxy
 * (default 4096, DT sec,max_coords) to the 1080x2400 panel.
 *
 * Boot-args: sweettouch=0 off, sweettouchdelay=N (s, default 20),
 * sweettouchirq=0 (poll the IC every 10 ms instead of waiting for attn).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern void sweet_hid_key(int key, int down);
extern unsigned int processor_avail_count;      /* osfmk/kern/processor.c */
extern void clock_set_calendar_microtime(long secs, int usecs);  /* kern/clock.h */
void sweet_touch_init(void);

#define TS_PERIC0_PA     0x10430000u
#define TS_GPP4          0x80u
#define TS_ALIVE_PA      0x14ec0000u
#define TS_GPA1          0x20u
#define TS_CON           0x00u
#define TS_DAT           0x04u
#define TS_PUD           0x08u
#define TS_SCL           0
#define TS_SDA           1
#define TS_ADDR          0x48
#define TS_HALF_US       3            /* ~150 kHz */
#define TS_FINGERS       10
#define TS_PANEL_W       1080
#define TS_PANEL_H       2400

static struct {
	vm_offset_t peric, alive;
	bool started;
	int irq;
	int maxx, maxy;
	uint32_t active;
	int x[TS_FINGERS], y[TS_FINGERS];
	int sleep_chan;
} ts = { .irq = 1, .maxx = 4096, .maxy = 4096 };

static uint32_t
ts_rd(vm_offset_t a)
{
	uint32_t v = *(volatile uint32_t *)a;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
ts_wr(vm_offset_t a, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
ts_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ts.sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* open drain: high = input (released), low = output with DAT 0 */
static void
ts_line(int pin, int high)
{
	vm_offset_t con = ts.peric + TS_GPP4 + TS_CON;
	uint32_t v = ts_rd(con) & ~(0xfu << (4 * pin));
	if (!high) {
		v |= 1u << (4 * pin);
	}
	ts_wr(con, v);
}

static int
ts_sense(int pin)
{
	return (ts_rd(ts.peric + TS_GPP4 + TS_DAT) >> pin) & 1;
}

static void
ts_scl_high(void)
{
	ts_line(TS_SCL, 1);
	for (unsigned i = 0; i < 2000 && !ts_sense(TS_SCL); i++) {
		delay(1);                     /* clock stretching, up to 2 ms */
	}
	delay(TS_HALF_US);
}

static void
ts_start(void)
{
	ts_line(TS_SDA, 1);
	ts_scl_high();
	ts_line(TS_SDA, 0);
	delay(TS_HALF_US);
	ts_line(TS_SCL, 0);
	delay(TS_HALF_US);
}

static void
ts_stop(void)
{
	ts_line(TS_SDA, 0);
	delay(TS_HALF_US);
	ts_scl_high();
	ts_line(TS_SDA, 1);
	delay(TS_HALF_US);
}

static int
ts_tx(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		ts_line(TS_SDA, (b >> i) & 1);
		delay(TS_HALF_US);
		ts_scl_high();
		ts_line(TS_SCL, 0);
	}
	ts_line(TS_SDA, 1);
	delay(TS_HALF_US);
	ts_scl_high();
	int ack = !ts_sense(TS_SDA);
	ts_line(TS_SCL, 0);
	delay(TS_HALF_US);
	return ack;
}

static uint8_t
ts_rx(int ack)
{
	uint8_t b = 0;
	ts_line(TS_SDA, 1);
	for (int i = 0; i < 8; i++) {
		delay(TS_HALF_US);
		ts_scl_high();
		b = (uint8_t)(b << 1 | ts_sense(TS_SDA));
		ts_line(TS_SCL, 0);
	}
	ts_line(TS_SDA, !ack);
	delay(TS_HALF_US);
	ts_scl_high();
	ts_line(TS_SCL, 0);
	ts_line(TS_SDA, 1);
	delay(TS_HALF_US);
	return b;
}

/* command byte, then optionally read n bytes; 0 = ok, <0 = no ACK at step */
static int
ts_xfer(uint8_t cmd, uint8_t *rx, int n)
{
	ts_start();
	if (!ts_tx(TS_ADDR << 1)) {
		ts_stop();
		return -1;
	}
	if (!ts_tx(cmd)) {
		ts_stop();
		return -2;
	}
	if (n) {
		ts_stop();
		delay(50);
		ts_start();
		if (!ts_tx(TS_ADDR << 1 | 1)) {
			ts_stop();
			return -4;
		}
		for (int i = 0; i < n; i++) {
			rx[i] = ts_rx(i < n - 1);
		}
	}
	ts_stop();
	return 0;
}

static int
ts_attn(void)
{
	return !(ts_rd(ts.alive + TS_GPA1 + TS_DAT) & 1u);
}

static void
ts_report(int id, int down, int x, int y)
{
	uint32_t bit = 1u << id;
	if (down) {
		if (!(ts.active & bit) || x != ts.x[id] || y != ts.y[id]) {
			sweet_hid_touch(id, 1, x, y);
		}
		ts.active |= bit;
		ts.x[id] = x;
		ts.y[id] = y;
	} else if (ts.active & bit) {
		sweet_hid_touch(id, 0, ts.x[id], ts.y[id]);
		ts.active &= ~bit;
	}
}

static void
ts_release_all(void)
{
	for (int id = 0; id < TS_FINGERS; id++) {
		ts_report(id, 0, 0, 0);
	}
}

/* Read events until the IC's stack is empty; returns <0 on bus error. */
static int
ts_events(unsigned *count)
{
	for (int n = 0; n < 32; n++) {
		uint8_t ev[16];
		int err = ts_xfer(0x60, ev, sizeof(ev));
		if (err) {
			return err;
		}
		if (!(ev[0] | ev[1] | ev[2] | ev[3] | ev[7])) {
			return 0;
		}
		unsigned eid = ev[0] & 3u, id = (ev[0] >> 2) & 0xfu, status = ev[0] >> 6;
		if (eid == 0 && id < TS_FINGERS) {
			int rx = ev[1] << 4 | ev[3] >> 4;
			int ry = ev[2] << 4 | (ev[3] & 0xf);
			int x = (int)((int64_t)rx * TS_PANEL_W / ts.maxx);
			int y = (int)((int64_t)ry * TS_PANEL_H / ts.maxy);
			if (++*count <= 8) {
				printf("SWEET TOUCH: ev %02x %02x %02x %02x .. left %u: id %u status %u raw %d,%d -> %d,%d\n",
				    ev[0], ev[1], ev[2], ev[3], ev[7] & 0x3fu, id, status, rx, ry, x, y);
			}
			ts_report((int)id, status == 1 || status == 2, x, y);
		} else if (++*count <= 8) {
			printf("SWEET TOUCH: event %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
		}
		if (!(ev[7] & 0x3fu)) {
			return 0;
		}
	}
	return 0;
}

/*
 * ACPM IPC, polled, PMIC channel only (Samsung o1s kernel acpm_ipc.c /
 * acpm_mfd.c; verified from the shim on the phone):
 *   mailbox 0x15900000: doorbell INTGR0 (+0x08) = (1 << id) << 16, INTCR1 (+0x20)
 *   SRAM 0x02039000, initdata +0x8000: +0x08 ipc_channels, +0x18 ipc_ap_max
 *   ipc_channel (72 bytes): id, field, owner, type, {rx_rear, rx_front,
 *   rx_base, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size, tx_rear,
 *   tx_front, tx_base, q_len, q_elem_size, credit, mbox_addr}, ap_poll.
 *   The AP sends into the rx_* queue and reads replies from tx_*.
 *   PMIC command: cmd0 = chan << 12 | type << 8 | reg | seq << 16,
 *   cmd1 = func | value << 8 | mask << 16; reply cmd1 = value << 8 | ret << 24.
 */
#define ACPM_MBOX_PA     0x15900000u
#define ACPM_SRAM_PA     0x02039000u
#define ACPM_SRAM_SIZE   0x41000u
#define ACPM_INITDATA    0x8000u
#define ACPM_PMIC_ID     2
#define ACPM_READ        0
#define ACPM_UPDATE      2

static struct {
	vm_offset_t mbox, sram;
	uint32_t id, q_len, elem, seq;
	uint32_t tx_rear, tx_front, tx_base, rx_rear, rx_front, rx_base;
} acpm;

static uint32_t
acpm_rd(uint32_t off)
{
	return ts_rd(acpm.sram + off);
}

static int
acpm_init(void)
{
	vm_offset_t page = ACPM_SRAM_PA & ~(vm_offset_t)PAGE_MASK;
	acpm.mbox = ml_io_map(ACPM_MBOX_PA, PAGE_SIZE);
	acpm.sram = ml_io_map(page, round_page(ACPM_SRAM_PA + ACPM_SRAM_SIZE) - page);
	if (!acpm.mbox || !acpm.sram) {
		return -1;
	}
	acpm.sram += ACPM_SRAM_PA - page;
	uint32_t chans = acpm_rd(ACPM_INITDATA + 0x08), ap_max = acpm_rd(ACPM_INITDATA + 0x18);
	if (ap_max == 0 || ap_max > 32 || chans == 0 || chans >= ACPM_SRAM_SIZE) {
		printf("SWEET TOUCH: ACPM initdata chans=%#x ap_max=%u unexpected\n", chans, ap_max);
		return -2;
	}
	for (uint32_t i = 0; i < ap_max; i++) {
		uint32_t e = chans + i * 72;
		if (acpm_rd(e) != ACPM_PMIC_ID) {
			continue;
		}
		acpm.id = ACPM_PMIC_ID;
		acpm.tx_rear = acpm_rd(e + 16);
		acpm.tx_front = acpm_rd(e + 20);
		acpm.tx_base = acpm_rd(e + 24);
		acpm.rx_rear = acpm_rd(e + 40);
		acpm.rx_front = acpm_rd(e + 44);
		acpm.rx_base = acpm_rd(e + 48);
		acpm.q_len = acpm_rd(e + 52);
		acpm.elem = acpm_rd(e + 56);
		if (acpm.q_len == 0 || acpm.q_len > 64 || acpm.elem < 16 || acpm.elem > 64) {
			return -3;
		}
		printf("SWEET TOUCH: ACPM PMIC channel idx %u qlen %u elem %u\n", i, acpm.q_len, acpm.elem);
		return 0;
	}
	return -4;
}

/* one PMIC command; returns the APM's ret byte (0 = ok) or < 0 on timeout */
static int
acpm_pmic(unsigned func, unsigned type, unsigned chan, unsigned reg, unsigned value, unsigned mask, uint8_t *out)
{
	acpm.seq = acpm.seq % 63 + 1;
	uint32_t cmd0 = (type & 0xfu) << 8 | (reg & 0xffu) | (chan & 0xfu) << 12 | acpm.seq << 16;
	uint32_t cmd1 = (func & 0xffu) | (value & 0xffu) << 8 | (mask & 0xffu) << 16;
	uint32_t front = acpm_rd(acpm.tx_front), next = (front + 1) % acpm.q_len;
	for (unsigned i = 0; acpm_rd(acpm.tx_rear) == next; i++) {
		if (i > 100000) {
			return -1;
		}
		delay(10);
	}
	vm_offset_t slot = acpm.sram + acpm.tx_base + acpm.elem * front;
	for (unsigned i = 0; i < acpm.elem / 4; i++) {
		ts_wr(slot + 4 * i, i == 0 ? cmd0 : i == 1 ? cmd1 : 0);
	}
	ts_wr(acpm.sram + acpm.tx_front, next);
	ts_wr(acpm.mbox + 0x08, (1u << acpm.id) << 16);
	for (unsigned t = 0; t < 100000; t++) {        /* ~1 s */
		uint32_t rear = acpm_rd(acpm.rx_rear), rfront = acpm_rd(acpm.rx_front);
		for (uint32_t i = rear; i != rfront; i = (i + 1) % acpm.q_len) {
			vm_offset_t r = acpm.sram + acpm.rx_base + acpm.elem * i;
			if (((ts_rd(r) >> 16) & 0x3fu) != acpm.seq) {
				continue;
			}
			uint32_t r1 = ts_rd(r + 4);
			if (i != rear) {
				for (unsigned k = 0; k < acpm.elem / 4; k++) {
					ts_wr(r + 4 * k, ts_rd(acpm.sram + acpm.rx_base + acpm.elem * rear + 4 * k));
				}
			}
			ts_wr(acpm.sram + acpm.rx_rear, (rear + 1) % acpm.q_len);
			ts_wr(acpm.mbox + 0x20, 1u << acpm.id);
			if (out) {
				*out = (uint8_t)(r1 >> 8);
			}
			return (int)(r1 >> 24);
		}
		delay(10);
	}
	return -2;
}

/*
 * Rail on at the DT voltage, if it is off and set near it (phone: LDO21S
 * read 0x28 = 2.8 V off; L27 0x2C = 1.8 V off). Enable = bits 7:6.
 */
static void
ts_rail(const char *name, unsigned chan, unsigned reg, unsigned vsel)
{
	uint8_t v = 0, v2 = 0;
	int r = acpm_pmic(ACPM_READ, 1, chan, reg, 0, 0, &v);
	bool ok = r == 0 && ((v & 0x3fu) == vsel ||
	    ((v & 0xc0u) == 0 && (v & 0x3fu) + 0x10u >= vsel && (v & 0x3fu) <= vsel + 4u));
	if (!ok) {
		printf("SWEET TOUCH: %s ret %d reg %#x unexpected, left alone\n", name, r, v);
		return;
	}
	if (v != (0xc0u | vsel)) {
		r = acpm_pmic(ACPM_UPDATE, 1, chan, reg, 0xc0u | vsel, 0xffu, NULL);
		(void)acpm_pmic(ACPM_READ, 1, chan, reg, 0, 0, &v2);
	} else {
		v2 = v;
	}
	printf("SWEET TOUCH: %s %#x -> %#x (ret %d)\n", name, v, v2, r);
}

static void
ts_power(void)
{
	int err = acpm_init();
	if (err) {
		printf("SWEET TOUCH: ACPM init failed %d; rails unchanged\n", err);
		return;
	}
	ts_rail("tsp_avdd (s2mps24 LDO21S 3.0 V)", 1, 0x54, 0x30);
	delay(2000);
	ts_rail("tsp_io (s2mps23 LDO27 1.8 V)", 0, 0x5f, 0x2c);
	ts_sleep(400);                          /* IC boot */
}

/*
 * S21 RTC + power key, both on the main PMIC (ACPM channel 0, type 0x01)
 * except the RTC page (type 0x02), confirmed on the phone (shim pmictest):
 *   RTC: CTRL 0x00 has BCD off, 24-hour; set UPDATE 0x02 bit0 (RUDR) to latch,
 *        then read 7 binary bytes from 0x05: sec,min,hour,week,date,mon,year+2000.
 *   Power key: STATUS1 0x0E bit0 = side button held (1) / released (0).
 */
#define PM_RTC          2
#define RTC_BASE        0x05
#define PM_STATUS1      0x0e

static long
days_from_civil(int y, int m, int d)   /* days since 1970-01-01, Howard Hinnant's algorithm */
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

static void
sweet_rtc_set(void)
{
	uint8_t b[7] = {0};
	(void)acpm_pmic(ACPM_UPDATE, PM_RTC, 0, 0x02, 0x01, 0x01, NULL);   /* RUDR latch */
	ts_sleep(20);
	for (int i = 0; i < 7; i++) {
		if (acpm_pmic(ACPM_READ, PM_RTC, 0, RTC_BASE + i, 0, 0, &b[i])) {
			printf("SWEET RTC: read failed\n");
			return;
		}
	}
	int sec = b[0] & 0x7f, min = b[1] & 0x7f, hour = b[2] & 0x1f;
	int date = b[4] & 0x3f, mon = b[5] & 0x0f, year = 2000 + (b[6] & 0x7f);
	if (mon < 1 || mon > 12 || date < 1 || date > 31 || hour > 23) {
		printf("SWEET RTC: implausible %04d-%02d-%02d %02d:%02d:%02d; clock left at epoch\n",
		    year, mon, date, hour, min, sec);
		return;
	}
	long days = days_from_civil(year, mon, date);
	long secs = days * 86400L + hour * 3600L + min * 60L + sec;
	clock_set_calendar_microtime(secs, 0);
	printf("SWEET RTC: %04d-%02d-%02d %02d:%02d:%02d UTC -> %ld s; calendar set\n",
	    year, mon, date, hour, min, sec, secs);
}

static void
ts_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 20, count = 0, errors = 0, ticks = 0;
	uint8_t id[5] = {0}, boot = 0;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("sweettouchdelay", &delay_s, sizeof(delay_s));
	printf("SWEET TOUCH: S21 slsi_ts, starting in %u s\n", delay_s);
	ts_sleep(delay_s * 1000u);

	/*
	 * ACPM before PSCI CPU_ON breaks CPU_ON (phone: "cpu=1 startup timeout",
	 * both from the shim and from this thread at +20 s; SweetCPU starts the
	 * secondaries only ~45 s after boot). Wait until every requested CPU is
	 * up (boot-arg cpus, default 1), at most 180 s.
	 */
	unsigned want = 1, waited = 0;
	(void)PE_parse_boot_argn("cpus", &want, sizeof(want));
	while (processor_avail_count < want && waited < 180) {
		ts_sleep(1000);
		waited++;
	}
	printf("SWEET TOUCH: %u/%u CPUs up after %u s more; powering the IC\n",
	    processor_avail_count, want, waited);
	if (processor_avail_count < want) {
		printf("SWEET TOUCH: CPUs not all up; ACPM skipped to keep CPU_ON working\n");
		goto out;
	}

	ts.peric = ml_io_map(TS_PERIC0_PA, 0x1000);
	ts.alive = ml_io_map(TS_ALIVE_PA, 0x1000);
	if (!ts.peric || !ts.alive) {
		printf("SWEET TOUCH: MMIO mapping failed\n");
		goto out;
	}
	ts_power();
	vm_offset_t g = ts.peric + TS_GPP4;
	printf("SWEET TOUCH: gpp4 con=%#x dat=%#x pud=%#x, gpa1 con=%#x dat=%#x\n",
	    ts_rd(g + TS_CON), ts_rd(g + TS_DAT), ts_rd(g + TS_PUD),
	    ts_rd(ts.alive + TS_GPA1 + TS_CON), ts_rd(ts.alive + TS_GPA1 + TS_DAT));
	/* SCL/SDA: GPIO, no pull, DAT 0 so "output" drives low; start released. */
	ts_wr(g + TS_PUD, ts_rd(g + TS_PUD) & ~0xffu);
	ts_wr(g + TS_DAT, ts_rd(g + TS_DAT) & ~3u);
	ts_line(TS_SCL, 1);
	ts_line(TS_SDA, 1);
	delay(100);
	if (!ts_sense(TS_SCL)) {
		printf("SWEET TOUCH: SCL stays low: bus not powered; stopping\n");
		goto out;
	}
	for (int i = 0; i < 9 && !ts_sense(TS_SDA); i++) {
		ts_line(TS_SCL, 0);
		delay(TS_HALF_US);
		ts_scl_high();
	}
	ts_stop();

	int err = ts_xfer(0x52, id, sizeof(id));
	int berr = ts_xfer(0x55, &boot, 1);
	printf("SWEET TOUCH: id err=%d %02x %02x %02x %02x %02x, boot err=%d status %#x\n",
	    err, id[0], id[1], id[2], id[3], id[4], berr, boot);
	if (err == -1) {
		printf("SWEET TOUCH: no ACK at 0x48; stopping\n");
		goto out;
	}
	(void)ts_xfer(0x62, NULL, 0);           /* clear event stack */
	err = ts_xfer(0x10, NULL, 0);           /* sense on */
	ts_sleep(50);
	printf("SWEET TOUCH: sense on err=%d; %s, scale %dx%d -> %dx%d\n", err,
	    ts.irq ? "reading on attn (gpa1-0 low)" : "polling every 10 ms",
	    ts.maxx, ts.maxy, TS_PANEL_W, TS_PANEL_H);

	/* The RTC and power key share the ACPM path, so set the clock here too. */
	sweet_rtc_set();
	printf("SWEET KEYS: side button via PMIC STATUS1\n");

	int pwr = 0;
	for (;;) {
		/* attn mode still reads every 200 ms and while a finger is down */
		if (!ts.irq || ts_attn() || ts.active || (++ticks % 20u) == 0) {
			err = ts_events(&count);
			if (err) {
				if ((errors++ % 100u) == 0) {
					printf("SWEET TOUCH: read error %d\n", err);
				}
				if (errors == 10) {
					ts_release_all();
				}
			} else {
				errors = 0;
			}
		}
		/* Power key (side button) = HID key 0: live level of STATUS1 bit0. */
		if ((ticks & 1u) == 0) {
			uint8_t st = 0;
			if (acpm_pmic(ACPM_READ, 1, 0, PM_STATUS1, 0, 0, &st) == 0 &&
			    (st & 1) != pwr) {
				pwr = st & 1;
				printf("SWEET KEYS: power %s\n", pwr ? "down" : "up");
				sweet_hid_key(0, pwr);
			}
		}
		ts_sleep(10);
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_touch_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (ts.started) {
		return;
	}
	ts.started = true;
	(void)PE_parse_boot_argn("sweettouch", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("sweettouchirq", &ts.irq, sizeof(ts.irq));
	(void)PE_parse_boot_argn("sweettouchmaxx", &ts.maxx, sizeof(ts.maxx));
	(void)PE_parse_boot_argn("sweettouchmaxy", &ts.maxy, sizeof(ts.maxy));
	if (ts.maxx <= 0 || ts.maxy <= 0) {
		ts.maxx = ts.maxy = 4096;
	}
	if (!enabled) {
		printf("SWEET TOUCH: disabled (sweettouch=0)\n");
		return;
	}
	kern_return_t kr = kernel_thread_start(ts_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET TOUCH: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
