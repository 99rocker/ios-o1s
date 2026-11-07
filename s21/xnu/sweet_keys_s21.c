/*
 * SWEET keys, Samsung Galaxy S21 (o1s, Exynos 2100): polled volume keys.
 *
 * From the phone's live device tree (gpio_keys, pinctrl@14EC0000):
 *   volume down = gpa0-4, volume up = gpa0-3, both active low, function 0xf
 *   (EINT), no pull. gpa0 is the first bank of the alive pinctrl block, so its
 *   DAT register is 0x14EC0004 (bank + 0x04, one bit per pin).
 * The power key belongs to the s2mps23 PMIC behind ACPM firmware: not read yet.
 *
 * READ ONLY: this file never writes a GPIO or pinctrl register.
 *
 * Events (sweet_hid_key, iokit/Kernel/arm/hid/SweetHID.cpp):
 *   volume up / down          -> 1 / 2 (consumer volume increment / decrement)
 *   both held together        -> 3 (Home, consumer Menu) while both are down
 *   volume down held 1.5 s    -> 0 (power / side button), one press
 * Boot-arg sweetkeys=0 turns the poller off.
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
extern void sweet_hid_key(int key, int down);
void sweet_keys_init(void);

#define KEYS_PINCTRL_PA  0x14ec0000u    /* alive pinctrl, bank gpa0 at +0 */
#define KEYS_GPA0_CON    0x00u
#define KEYS_GPA0_DAT    0x04u
#define KEYS_PIN_UP      3
#define KEYS_PIN_DOWN    4
#define KEYS_POLL_MS     20
#define KEYS_POWER_MS    1500

enum { K_POWER = 0, K_UP = 1, K_DOWN = 2, K_HOME = 3 };

static vm_offset_t keys_base;
static bool keys_started;
static int keys_sleep_chan;

static uint32_t
keys_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(keys_base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(KEYS_POLL_MS, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
keys_send(int key, int down)
{
	printf("SWEET KEYS: %s %s\n",
	    key == K_POWER ? "power" : key == K_UP ? "volume-up" : key == K_DOWN ? "volume-down" : "home",
	    down ? "down" : "up");
	sweet_hid_key(key, down);
}

static void
keys_thread(void *arg, wait_result_t result)
{
	int up = 0, down = 0;           /* debounced state */
	int last_up = 0, last_down = 0; /* previous raw sample */
	int rep_up = 0, rep_down = 0;   /* volume state reported to HID */
	int home = 0, power_sent = 0;
	unsigned down_ms = 0;
	(void)arg;
	(void)result;
	for (;;) {
		uint32_t dat = keys_rd(KEYS_GPA0_DAT);
		int raw_up = !(dat & (1u << KEYS_PIN_UP));
		int raw_down = !(dat & (1u << KEYS_PIN_DOWN));

		/* Debounce: a level counts once two samples 20 ms apart agree. */
		if (raw_up == last_up && raw_up != up) {
			up = raw_up;
		}
		if (raw_down == last_down && raw_down != down) {
			down = raw_down;
			down_ms = 0;
			power_sent = 0;
		}
		last_up = raw_up;
		last_down = raw_down;

		/* Both volume keys together = Home, for as long as either stays down. */
		if (up && down && !home) {
			home = 1;
		}
		int want_up = home ? 0 : up, want_down = home ? 0 : down;
		if (rep_up != want_up) {
			rep_up = want_up;
			keys_send(K_UP, rep_up);
		}
		if (rep_down != want_down) {
			rep_down = want_down;
			keys_send(K_DOWN, rep_down);
		}
		if (home == 1) {
			home = 2;
			keys_send(K_HOME, 1);
		} else if (home == 2 && !up && !down) {
			home = 0;
			keys_send(K_HOME, 0);
		}

		/* Volume down held alone = one power (side button) press. */
		if (down && !up && !home && !power_sent) {
			down_ms += KEYS_POLL_MS;
			if (down_ms >= KEYS_POWER_MS) {
				power_sent = 1;
				keys_send(K_POWER, 1);
				keys_send(K_POWER, 0);
			}
		}
		keys_wait();
	}
}

void
sweet_keys_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (keys_started) {
		return;
	}
	keys_started = true;
	(void)PE_parse_boot_argn("sweetkeys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("SWEET KEYS: disabled (sweetkeys=0)\n");
		return;
	}
	keys_base = ml_io_map(KEYS_PINCTRL_PA, 0x1000);
	if (!keys_base) {
		printf("SWEET KEYS: MMIO mapping failed\n");
		return;
	}
	uint32_t con = keys_rd(KEYS_GPA0_CON), dat = keys_rd(KEYS_GPA0_DAT);
	printf("SWEET KEYS: S21 gpa0 con=%#x dat=%#x (up pin3 func %#x, down pin4 func %#x)\n",
	    con, dat, (con >> (4 * KEYS_PIN_UP)) & 0xfu, (con >> (4 * KEYS_PIN_DOWN)) & 0xfu);
	printf("SWEET KEYS: vol+/vol- = volume, both = Home, hold vol- 1.5 s = power\n");
	kern_return_t kr = kernel_thread_start(keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET KEYS: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
