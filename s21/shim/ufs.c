/*
 * S21 storage probe: can we talk to the Exynos 2100 UFS that S-Boot booted from?
 *
 * Adapted from ios-sweet's shim/ufs.c milestone-5 probe (MIT). READ-ONLY:
 * the only SCSI commands in this file are TEST UNIT READY, READ CAPACITY(10)
 * and READ(10). There is no WRITE anywhere.
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers S-Boot left behind and, if the
 * host is enabled and the link is up, reuses S-Boot's own slot 0 (request
 * descriptor, command descriptor, data buffer) for a few commands, one 4 KiB
 * block at a time, then restores the nexus register (ufs_restore) before
 * LineageOS takes over. The list base registers are never touched.
 *
 * Exynos differences from the Qualcomm original, all from Samsung's o1s
 * kernel (drivers/scsi/ufs/ufs-exynos.c, ufshcd.c, ufs-vs-regs.h):
 *   - standard HCI at 0x13100000 (DT ufs@0x13100000 region 0), vendor
 *     "exynos-specific hci" at 0x13101100 (region 1)
 *   - UFSHCD_QUIRK_PRDT_BYTE_GRAN: UTRD response/PRDT offsets and lengths
 *     are in bytes, PRDT length = entries * 16
 *   - PRDT_SET_SIZE(12): one PRD entry per 4 KiB of data
 *   - HCI_UTRL_NEXUS_TYPE (vendor +0x40): bit n = 1 for a SCSI command in slot n
 *   - UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR: clear slot n by writing 1 << n
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x13100000UL
#define UFS_VS_BASE	0x13101100UL

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define VS_TXPRDT_ENTRY_SIZE	0x00
#define VS_RXPRDT_ENTRY_SIZE	0x04
#define VS_UTRL_NEXUS_TYPE	0x40

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)

#define BLOCK		4096u
#define MAX_PRD		8	/* 32 KiB per command */
#define TIMER_HZ	26000000u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }
static inline uint32_t vs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_VS_BASE + off); }
static inline void vs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_VS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (BYTES on Exynos) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] length (BYTES on Exynos) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[MAX_PRD];
};

static struct ucd ucd_buf __attribute__((aligned(128)));
static uint8_t dma_buf[BLOCK] __attribute__((aligned(4096)));
static uint8_t data[4 * BLOCK] __attribute__((aligned(4096)));

/*
 * Phone history (2026-11-07):
 *  1. Own list + buffers at 0x8008xxxx: first TUR -> OCS 7 with SCSI status
 *     0x02 in OUR response UPIU, then every command timed out. The host
 *     wrote both our UTRD (OCS) and our UCD (response), so DMA works; OCS 7
 *     is UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR (Exynos reports "fatal error"
 *     whenever the response carries a result, here the usual unit
 *     attention), and the host then waits for its error status to be
 *     cleared, which nothing did.
 *  2. Borrowing S-Boot's buffers: its slot 0 had no data PRD (prdt@0), stop.
 * Now: S-Boot's list slot 0 (list registers stay untouched), our own UCD
 * and 4 KiB DMA buffer, the OCS quirk applied, IS cleared after every
 * command and the list kept running.
 */
static struct utrd *U;		/* S-Boot's UTRL slot 0 */
static struct ucd *C;		/* our command descriptor */
static uint8_t *D;		/* our 4 KiB DMA buffer */

/* What S-Boot had, put back by ufs_restore() before Linux runs. */
static uint32_t saved_utrlba, saved_utrlbau, saved_rsr, saved_nexus, took_over;
static uint32_t res_ok, res_gpt, res_userdata_lba;
static uint32_t last_sense;	/* key << 16 | ASC << 8 | ASCQ of the last CHECK CONDITION */

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	if (len > BLOCK)		/* one PRD entry: S-Boot's buffer is one block */
		return 5;
	zero(U, sizeof(*U));
	zero(C, 1024 + 16);	/* UPIUs + one PRD entry: stay inside S-Boot's descriptor */

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	C->cmd[0] = 0x01;			/* transaction type: COMMAND */
	C->cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	C->cmd[2] = lun;
	C->cmd[3] = ++tag;
	C->cmd[12] = len >> 24;
	C->cmd[13] = len >> 16;
	C->cmd[14] = len >> 8;
	C->cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		C->cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)C;
	U->dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	U->dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	U->ucdba = (uint32_t)ucd_pa;
	U->ucdbau = (uint32_t)(ucd_pa >> 32);
	U->resp = 512u << 16 | 512u;	/* Exynos: offset/length in bytes */
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		unsigned n = 0;
		for (uint32_t done = 0; done < len; done += BLOCK, n++) {
			uint32_t chunk = len - done < BLOCK ? len - done : BLOCK;
			C->prdt[n].addr_lo = (uint32_t)(buf_pa + done);
			C->prdt[n].addr_hi = (uint32_t)((buf_pa + done) >> 32);
			C->prdt[n].dbc = chunk - 1;
		}
		U->prdt = 1024u << 16 | (n * 16u);	/* Exynos: bytes, 16 per entry */
	}
	vs_wr(VS_UTRL_NEXUS_TYPE, vs_rd(VS_UTRL_NEXUS_TYPE) | 1u);	/* slot 0 = SCSI command */
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > TIMER_HZ) {	/* 1 s */
			con_hex2(" tmo", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
			ufs_wr(REG_UTRLCLR, 1u);	/* Exynos: write 1 to clear slot 0 */
			ufs_wr(REG_IS, ufs_rd(REG_IS));
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t ocs = U->dw2 & 0xff, is = ufs_rd(REG_IS);
	uint32_t result = (uint32_t)C->resp[6] << 8 | C->resp[7];	/* response, status */
	if (ocs == 7 && result)		/* UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR */
		ocs = 0;
	ufs_wr(REG_IS, is);			/* write-1-to-clear: let the host carry on */
	if (!(ufs_rd(REG_UTRLRSR) & 1))
		ufs_wr(REG_UTRLRSR, 1);
	*ocs_out = ocs;
	*status_out = C->resp[7];		/* SCSI status in the response UPIU */
	last_sense = 0;
	if (*status_out == 0x02) {		/* CHECK CONDITION: sense data at response byte 34 */
		const uint8_t *sd = C->resp + 34;
		uint32_t sdl = (uint32_t)C->resp[32] << 8 | C->resp[33];
		if (sdl && ((sd[0] & 0x7f) == 0x70 || (sd[0] & 0x7f) == 0x71))
			last_sense = (uint32_t)(sd[2] & 0xf) << 16 | sd[12] << 8 | sd[13];
		else if (sdl && ((sd[0] & 0x7f) == 0x72 || (sd[0] & 0x7f) == 0x73))
			last_sense = (uint32_t)(sd[1] & 0xf) << 16 | sd[2] << 8 | sd[3];
		else
			last_sense = 0xff000000 | sdl;	/* no/unknown sense: show its length */
	}
	if (ocs) {
		con_hex2(" err", "is=", is, "ocs=", ocs);
		return 2;
	}
	if (C->resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	for (uint32_t i = 0; i < count; i++, lba++) {
		uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, 1, 0 };
		uint32_t ocs, st, rc = scsi(0, cdb, sizeof(cdb), D, BLOCK, &ocs, &st);
		if (rc)
			return rc;
		volatile uint64_t *to = (volatile uint64_t *)((uint8_t *)buf + i * BLOCK);
		volatile uint64_t *from = (volatile uint64_t *)D;
		for (unsigned w = 0; w < BLOCK / 8; w++)
			to[w] = from[w];
	}
	return 0;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (streq(name, "userdata")) {
			res_userdata_lba = (uint32_t)first;
			con_str(" fs", fs_type((uint32_t)first));
		}
	}
}

void test_ufs(void)
{
	con_str("UFS", "S21 Exynos UFS probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);
	con_hex2("UFS VS", "txprdt=", vs_rd(VS_TXPRDT_ENTRY_SIZE), "rxprdt=", vs_rd(VS_RXPRDT_ENTRY_SIZE));
	con_hex("NEXUS", vs_rd(VS_UTRL_NEXUS_TYPE));

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Use S-Boot's slot 0: its list, command descriptor and data buffer. */
	saved_utrlba = ufs_rd(REG_UTRLBA);
	saved_utrlbau = ufs_rd(REG_UTRLBAU);
	saved_rsr = ufs_rd(REG_UTRLRSR);
	saved_nexus = vs_rd(VS_UTRL_NEXUS_TYPE);
	took_over = 1;
	U = (struct utrd *)((uintptr_t)saved_utrlbau << 32 | saved_utrlba);
	C = &ucd_buf;
	D = dma_buf;
	con_hex2("UFS", "list=", (uint32_t)(uintptr_t)U, "ucd=", (uint32_t)(uintptr_t)C);

	uint32_t ocs = 0, st = 0, rc = 1;
	static const uint8_t tur[6] = { 0x00 };

	/*
	 * Attempt 4: every LU, even absent ones, answered NOT READY (sense
	 * 02/00/00): the device as a whole is in Sleep/PowerDown, as S-Boot
	 * leaves it. Wake it like ufshcd_set_dev_pwr_mode(): REQUEST SENSE to
	 * clear the unit attention, then START STOP UNIT with POWER CONDITION =
	 * Active (1 << 4), both to the UFS Device well-known LU (UPIU LUN 0xD0).
	 * This changes the power mode only; it reads/writes no media.
	 */
	static const uint8_t req_sense[6] = { 0x03, 0, 0, 0, 18, 0 };
	rc = scsi(0xd0, req_sense, sizeof(req_sense), D, 18, &ocs, &st);
	con_hex2("WLUN RS", "rc/st=", rc << 16 | ocs << 8 | st, "sense=", rc ? last_sense :
		 (uint32_t)(D[2] & 0xf) << 16 | D[12] << 8 | D[13]);
	static const uint8_t ssu_active[6] = { 0x1b, 0, 0, 0, 1u << 4, 0 };
	for (int i = 0; i < 3; i++) {
		rc = scsi(0xd0, ssu_active, sizeof(ssu_active), 0, 0, &ocs, &st);
		con_hex2("WLUN SSU", "rc/st=", rc << 16 | ocs << 8 | st, "sense=", last_sense);
		if (!rc)
			break;
	}
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc/st=", rc << 16 | ocs << 8 | st, "sense=", last_sense);
		if (!rc)
			break;
	}

	/* Which logical units answer? One TEST UNIT READY each (no data). */
	for (uint8_t lun = 0; lun < 6; lun++) {
		char label[] = "LUN0";
		label[3] = '0' + lun;
		rc = scsi(lun, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2(label, "rc/st=", rc << 16 | ocs << 8 | st, "sense=", last_sense);
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), D, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc/st=", rc << 16 | ocs << 8 | st, "sense=", last_sense);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(D), "blk=", be32(D + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	rc = read_blocks(0, 2, data);
	con_hex2("UFS READ", "rc=", rc, "sense=", last_sense);
	if (rc)
		return;
	res_ok = 1;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	res_gpt = streq(sig, "EFI PART");
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}

/* Put the transfer request list back the way S-Boot left it. */
void ufs_restore(void)
{
	if (!took_over)
		return;
	/* The list base was never changed (S-Boot's own list is used). */
	vs_wr(VS_UTRL_NEXUS_TYPE, saved_nexus);
	ufs_wr(REG_IS, ufs_rd(REG_IS));		/* write-1-to-clear what our commands raised */
	(void)saved_rsr;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* "UFS ok1 gpt1 ud=XXXXXXXX " over the start of the DTB root /model (37 chars kept). */
void ufs_report(uintptr_t dtb)
{
	static const char m[] = "Samsung O1S";
	const char *hex = "0123456789ABCDEF";
	char out[26];
	int n = 0;
	out[n++] = 'U'; out[n++] = 'F'; out[n++] = 'S'; out[n++] = ' ';
	out[n++] = 'o'; out[n++] = 'k'; out[n++] = '0' + (res_ok & 1); out[n++] = ' ';
	out[n++] = 'g'; out[n++] = 'p'; out[n++] = 't'; out[n++] = '0' + (res_gpt & 1); out[n++] = ' ';
	out[n++] = 'u'; out[n++] = 'd'; out[n++] = '=';
	for (int s = 28; s >= 0; s -= 4)
		out[n++] = hex[(res_userdata_lba >> s) & 15];
	out[n++] = ' ';
	for (uintptr_t a = dtb; a < dtb + 0x200000; a++) {
		int i = 0;
		while (i < 11 && *(volatile char *)(a + i) == m[i])
			i++;
		if (i == 11) {
			for (int k = 0; k < n; k++)
				*(volatile char *)(a + k) = out[k];
			__asm__ volatile("dsb sy" ::: "memory");
			return;
		}
	}
}
