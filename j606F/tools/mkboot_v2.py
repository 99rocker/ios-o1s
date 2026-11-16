#!/usr/bin/env python3
"""Minimal Android boot.img v2 assembler for the TB-J606F shim test.

Reuses the STOCK boot.img header (load addresses, page size, os version) and its
embedded DTB, swapping in our shim kernel + an empty ramdisk. Using the device's
own load addresses + DTB maximises the chance Lenovo's ABL boots it like stock.
Pure stdlib; reads/writes local files only.

Usage:
  mkboot_v2.py --template stock-boot.img --kernel shim-Image \
               --ramdisk empty-ramdisk.bin --out shim-boot.img [--cmdline "..."]
"""
import argparse, hashlib, struct, sys

def rd(path): return open(path, "rb").read()
def pad(b, p): return b + b"\0" * ((-len(b)) % p)
def npage(n, p): return (n + p - 1) // p

def parse_v2(d):
    assert d[:8] == b"ANDROID!", "not an Android boot image"
    (ks, ka, rs, ra, ss, sa, tags, page, hv, osv) = struct.unpack_from("<10I", d, 8)
    assert hv == 2, f"template must be boot header v2 (got v{hv})"
    name = d[48:64]; cmdline = d[64:576]; extra = d[608:1632]
    rdtbo_size, rdtbo_off, hsize = struct.unpack_from("<I Q I", d, 1632)
    dtb_size, dtb_addr = struct.unpack_from("<I Q", d, 1648)
    p = page
    off = p * (1 + npage(ks, p) + npage(rs, p) + npage(ss, p) + npage(rdtbo_size, p))
    dtb = d[off:off + dtb_size]
    assert len(dtb) == dtb_size and dtb[:4] == b"\xd0\x0d\xfe\xed", "DTB not found/!magic"
    return dict(ka=ka, ra=ra, sa=sa, tags=tags, page=p, osv=osv, name=name,
                cmdline=cmdline, extra=extra, dtb_addr=dtb_addr, dtb=dtb)

def build(t, kernel, ramdisk, cmdline):
    p = t["page"]; second = b""
    if cmdline is not None:
        cb = cmdline.encode()[:511]; t["cmdline"] = cb + b"\0" * (512 - len(cb))
    # mkbootimg id: sha1 over each section and its length (v2 incl. recovery_dtbo + dtb)
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        sha.update(blob); sha.update(struct.pack("<I", len(blob)))
    sha.update(struct.pack("<I", 0))          # recovery_dtbo_size = 0
    sha.update(t["dtb"]); sha.update(struct.pack("<I", len(t["dtb"])))
    img_id = sha.digest() + b"\0" * 12

    hdr = bytearray(p)
    struct.pack_into("<8s", hdr, 0, b"ANDROID!")
    struct.pack_into("<10I", hdr, 8,
                     len(kernel), t["ka"], len(ramdisk), t["ra"],
                     len(second), t["sa"], t["tags"], p, 2, t["osv"])
    hdr[48:64] = t["name"]; hdr[64:576] = t["cmdline"]
    hdr[576:608] = img_id;  hdr[608:1632] = t["extra"]
    struct.pack_into("<I Q I", hdr, 1632, 0, 0, 1660)         # recovery_dtbo + header_size
    struct.pack_into("<I Q", hdr, 1648, len(t["dtb"]), t["dtb_addr"])
    return (bytes(hdr) + pad(kernel, p) + pad(ramdisk, p)
            + pad(second, p) + pad(t["dtb"], p))

def main():
    a = argparse.ArgumentParser()
    a.add_argument("--template", required=True)
    a.add_argument("--kernel", required=True)
    a.add_argument("--ramdisk", required=True)
    a.add_argument("--out", required=True)
    a.add_argument("--cmdline", default=None)
    o = a.parse_args()
    t = parse_v2(rd(o.template))
    img = build(t, rd(o.kernel), rd(o.ramdisk), o.cmdline)
    open(o.out, "wb").write(img)
    print(f"{o.out}: {len(img)} bytes  (page={t['page']} kaddr=0x{t['ka']:x} "
          f"dtb={len(t['dtb'])}B @0x{t['dtb_addr']:x})")

if __name__ == "__main__":
    main()
