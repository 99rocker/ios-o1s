#!/usr/bin/env python3
"""Extract the payload of an IM4P file (e.g. DeviceTree.d22ap.im4p) without the ipsw tool.

  im4p_extract.py IN.im4p OUT.bin

IM4P is DER: SEQUENCE { IA5String "IM4P", IA5String type, IA5String description,
OCTET STRING payload, ... }. Device trees in IPSWs are not compressed; a payload
that starts with "bvx" (LZFSE) is refused rather than written out wrong.
"""
import sys


def der(buf, off):
    tag = buf[off]
    n = buf[off + 1]
    off += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(buf[off:off + k], "big")
        off += k
    return tag, off, n


def main():
    data = open(sys.argv[1], "rb").read()
    tag, off, _ = der(data, 0)
    assert tag == 0x30, "not a DER SEQUENCE"
    fields = []
    for _ in range(4):
        tag, start, n = der(data, off)
        fields.append((tag, data[start:start + n]))
        off = start + n
    assert fields[0] == (0x16, b"IM4P"), f"not an IM4P: {fields[0]}"
    kind = fields[1][1].decode()
    tag, payload = fields[3]
    assert tag == 0x04, "payload is not an OCTET STRING"
    if payload[:3] == b"bvx":
        sys.exit("payload is LZFSE-compressed; decompress it first")
    open(sys.argv[2], "wb").write(payload)
    print(f"{sys.argv[1]}: type {kind}, desc {fields[2][1].decode()!r}, payload {len(payload)} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
