/*
 * Unpack an LZFSE-compressed IM4P payload (DeviceTree.d22ap.im4p in the iPhone X IPSW)
 * to a raw Apple Device Tree, for tools/mkadt_s21.py's /product node.
 *
 *   gcc -O2 -o im4p_lzfse im4p_lzfse.c -I ~/wslk/apfs-fuse/3rdparty/lzfse/src ~/wslk/apfs-fuse/build/liblzfse.a
 *   ./im4p_lzfse DeviceTree.d22ap.im4p d22ap.adt
 *
 * IM4P is DER: SEQUENCE { IA5 "IM4P", IA5 type, IA5 desc, OCTET STRING payload, ... }.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lzfse.h"

static size_t der(const unsigned char *b, size_t off, unsigned char *tag, size_t *len)
{
	*tag = b[off++];
	size_t n = b[off++];
	if (n & 0x80) {
		int k = n & 0x7f;
		n = 0;
		while (k--)
			n = n << 8 | b[off++];
	}
	*len = n;
	return off;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s IN.im4p OUT.bin\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	unsigned char *in = malloc(size);
	if (fread(in, 1, size, f) != (size_t)size) { perror("read"); return 1; }
	fclose(f);

	unsigned char tag;
	size_t len, off = der(in, 0, &tag, &len), start = 0;
	if (tag != 0x30) { fprintf(stderr, "not DER\n"); return 1; }
	for (int i = 0; i < 4; i++) {
		start = der(in, off, &tag, &len);
		if (i == 0 && (tag != 0x16 || len != 4 || memcmp(in + start, "IM4P", 4))) {
			fprintf(stderr, "not an IM4P\n");
			return 1;
		}
		if (i < 3)
			off = start + len;
	}
	if (tag != 0x04) { fprintf(stderr, "payload is not an OCTET STRING\n"); return 1; }

	size_t cap = 16 << 20;
	unsigned char *out = malloc(cap);
	size_t n = lzfse_decode_buffer(out, cap, in + start, len, NULL);
	if (n == 0 || n == cap) { fprintf(stderr, "LZFSE decode failed (%zu)\n", n); return 1; }
	f = fopen(argv[2], "wb");
	fwrite(out, 1, n, f);
	fclose(f);
	printf("%s: payload %zu bytes LZFSE -> %zu bytes\n", argv[1], len, n);
	return 0;
}
