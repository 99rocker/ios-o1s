/*
 * SWEET touch, Samsung Galaxy S21 (o1s, Exynos 2100): Samsung LSI touch IC
 * (slsi_ts, y792 firmware) at I2C address 0x48 on hsi2c8.
 *
 * From the phone's live device tree:
 *   bus pins tspi2c-bus = gpp4-0 (SCL), gpp4-1 (SDA): pinctrl@10430000 bank
 *   gpp4 at +0x80 (Samsung banks are 0x20 apart; gpp4 is the 5th).
 *   interrupt (attn) = gpa1-0, active low: pinctrl@14EC0000 + 0x20.
 *   rails tsp_io (LDO27) / tsp_avdd (LDO21) with sec,regulator_boot_on: S-Boot
 *   leaves them on; this driver does not touch the PMIC.
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

	ts.peric = ml_io_map(TS_PERIC0_PA, 0x1000);
	ts.alive = ml_io_map(TS_ALIVE_PA, 0x1000);
	if (!ts.peric || !ts.alive) {
		printf("SWEET TOUCH: MMIO mapping failed\n");
		goto out;
	}
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
