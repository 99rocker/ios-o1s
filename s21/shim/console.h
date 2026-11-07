#pragma once
#include <stdint.h>

/* S21: S-Boot's framebuffer (cmdline s3cfb.bootloaderfb=0xfa000000); DECON0's
 * DPP ch1 scans it (measured: RDMA_BASEADDR_Y8 = 0xFA000000). Same panel size
 * as sweet. Pixels only reach the command-mode panel after display_kick(). */
#define FB_BASE		0xfa000000UL
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 (test 6: red band rendered red) */

void display_kick(void);

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
/* move the cursor to text row r (to redraw a line in place); returns the old row */
int con_set_row(int r);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
