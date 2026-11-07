/*
 * Minimal ACPM IPC client for the S21 shim: PMIC register access through the
 * APM firmware, polled, one command at a time.
 *
 * From the phone's live DT and Samsung's o1s kernel (drivers/soc/samsung/acpm/
 * acpm_ipc.c, acpm_mfd.c, fw_header/framework.h):
 *   acpm_ipc: mailbox 0x15900000, SRAM 0x02039000, initdata at SRAM + 0x8000
 *   struct acpm_framework: +0x08 ipc_channels (SRAM offset), +0x18 ipc_ap_max
 *   struct ipc_channel (72 bytes): id, field, owner, type, channel_info
 *     {rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base,
 *      rx_indr_buf_size, tx_rear, tx_front, tx_base, q_len, q_elem_size,
 *      credit, mbox_addr}, ap_poll. "rx" is the APM's receive side, so the AP
 *     sends into rx_* and reads replies from tx_*.
 *   doorbell: INTGR0 (+0x08) = (1 << id) << 16; reply pending: INTSR1 (+0x28)
 *   bit id, cleared through INTCR1 (+0x20).
 *   PMIC channel: acpm_mfd_bus acpm-ipc-channel = 2.
 *   PMIC command: cmd0 = type << 8 | reg | chan << 12 | seq << 16,
 *   cmd1 = func | value << 8 | mask << 16; reply cmd1 = value << 8 | ret << 24.
 *   func: 0 read, 1 write, 2 update.
 */
#include <stdint.h>
#include "console.h"
#include "acpm.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define MBOX		0x15900000UL
#define SRAM		0x02039000UL
#define INITDATA	(SRAM + 0x8000)
#define INTGR0		0x08
#define INTCR1		0x20
#define INTSR1		0x28
#define PMIC_IPC_ID	2

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { __asm__ volatile("dsb sy" ::: "memory"); *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static struct {
	int ok;
	uint32_t id, q_len, elem;
	uintptr_t tx_rear, tx_front, tx_base;	/* AP -> APM */
	uintptr_t rx_rear, rx_front, rx_base;	/* APM -> AP */
	uint32_t seq;
} ch;

int acpm_init(void)
{
	uint32_t chans = rd(INITDATA + 0x08), ap_max = rd(INITDATA + 0x18);
	con_hex2("ACPM", "chans=", chans, "apmax=", ap_max);
	if (ap_max == 0 || ap_max > 32 || chans == 0 || chans > 0x40000)
		return -1;
	for (uint32_t i = 0; i < ap_max; i++) {
		uintptr_t e = SRAM + chans + i * 72;
		if (rd(e) != PMIC_IPC_ID)
			continue;
		ch.id = rd(e);
		ch.tx_rear = SRAM + rd(e + 16);
		ch.tx_front = SRAM + rd(e + 20);
		ch.tx_base = SRAM + rd(e + 24);
		ch.rx_rear = SRAM + rd(e + 40);
		ch.rx_front = SRAM + rd(e + 44);
		ch.rx_base = SRAM + rd(e + 48);
		ch.q_len = rd(e + 52);
		ch.elem = rd(e + 56);
		con_hex2("PMIC CH", "idx=", i, "poll=", rd(e + 68));
		con_hex2("", "qlen=", ch.q_len, "elem=", ch.elem);
		con_hex2("", "txf=", rd(ch.tx_front), "rxf=", rd(ch.rx_front));
		if (ch.q_len == 0 || ch.q_len > 64 || ch.elem < 16 || ch.elem > 64)
			return -2;
		ch.ok = 1;
		ch.seq = 0;
		return 0;
	}
	return -3;
}

/* one PMIC command; returns the APM's ret byte (0 ok), or <0 on timeout */
int acpm_pmic(unsigned func, unsigned chan, unsigned type, unsigned reg,
	      unsigned value, unsigned mask, uint8_t *out)
{
	if (!ch.ok)
		return -10;
	ch.seq = ch.seq % 63 + 1;
	uint32_t cmd[4] = {
		(type & 0xf) << 8 | (reg & 0xff) | (chan & 0xf) << 12 | ch.seq << 16,
		(func & 0xff) | (value & 0xff) << 8 | (mask & 0xff) << 16,
		0, 0,
	};
	uint32_t front = rd(ch.tx_front), next = (front + 1) % ch.q_len;
	uint64_t t0 = rsr("cntpct_el0");
	while (rd(ch.tx_rear) == next)		/* queue full */
		if (rsr("cntpct_el0") - t0 > 26000000)
			return -11;
	uintptr_t slot = ch.tx_base + ch.elem * front;
	for (unsigned i = 0; i < ch.elem / 4; i++)
		wr(slot + 4 * i, i < 4 ? cmd[i] : 0);
	wr(ch.tx_front, next);
	wr(MBOX + INTGR0, (1u << ch.id) << 16);

	t0 = rsr("cntpct_el0");
	for (;;) {
		uint32_t rear = rd(ch.rx_rear), rfront = rd(ch.rx_front);
		for (uint32_t i = rear; i != rfront; i = (i + 1) % ch.q_len) {
			uintptr_t r = ch.rx_base + ch.elem * i;
			uint32_t r0 = rd(r);
			if (((r0 >> 16) & 0x3f) != ch.seq)
				continue;
			uint32_t r1 = rd(r + 4);
			if (i != rear)			/* keep the queue compact, as Linux does */
				for (unsigned k = 0; k < ch.elem / 4; k++)
					wr(r + 4 * k, rd(ch.rx_base + ch.elem * rear + 4 * k));
			wr(ch.rx_rear, (rear + 1) % ch.q_len);
			wr(MBOX + INTCR1, 1u << ch.id);
			if (out)
				*out = (uint8_t)(r1 >> 8);
			return (int)(r1 >> 24);
		}
		if (rsr("cntpct_el0") - t0 > 26000000)	/* 1 s */
			return -12;
	}
}
