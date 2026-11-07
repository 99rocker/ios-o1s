#!/usr/bin/env python3
"""Pack the S21 shim (and optionally an xnu payload) into an Odin-flashable boot.img.

  mks21boot.py SHIM_IMAGE OUTDIR [--payload payload.bin] [--stock-boot boot.img]
                                 [--vendor-boot vendor_boot.img]

Writes OUTDIR/s21-boot.img and OUTDIR/AP_s21.tar (boot.img for Odin's AP slot).

S21 facts this relies on (shim-test, s21-facts):
  * boot.img header v3, 4K pages; the DTB comes from vendor_boot (untouched).
  * S-Boot loads vendor_boot's ramdisk, then ours directly behind it, and
    passes the pair as /chosen linux,initrd-start/-end. The vendor ramdisk is
    0xa8ac37 bytes on G991BXXSJHZC2 (an odd size), so our ramdisk starts with
    padding that makes the payload 16-byte aligned in memory; the shim scans
    for the SWEETXNU magic at 8-byte steps.
  * os_version/patch level is copied from the stock boot.img when given
    (keymaster refuses to downgrade), else the measured 0x200001a9.
Without --payload the ramdisk is a single padding block: the shim then shows
its console and stops at "no SWEETXNU payload" (a safe first on-phone test).
"""
import argparse
import io
import os
import struct
import tarfile

DEFAULT_VENDOR_RAMDISK = 0xA8AC37
DEFAULT_OS_VERSION = 0x200001A9   # Android 16, 2026-09 (measured on the phone)
ALIGN = 16


def pad(b, n=4096):
    return b + b"\0" * ((-len(b)) % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shim")
    ap.add_argument("outdir")
    ap.add_argument("--payload")
    ap.add_argument("--stock-boot")
    ap.add_argument("--vendor-boot")
    a = ap.parse_args()

    shim = open(a.shim, "rb").read()
    assert shim[0x38:0x3C] == b"ARMd", "not an arm64 Image"
    assert struct.unpack_from("<Q", shim, 8)[0] == 0x80000, "S21 needs text_offset 0x80000"

    os_version = DEFAULT_OS_VERSION
    if a.stock_boot:
        stock = open(a.stock_boot, "rb").read()
        assert stock[:8] == b"ANDROID!"
        os_version = struct.unpack_from("<I", stock, 16)[0]

    vendor_rd = DEFAULT_VENDOR_RAMDISK
    if a.vendor_boot:
        vb = open(a.vendor_boot, "rb").read()
        assert vb[:8] == b"VNDRBOOT" and struct.unpack_from("<I", vb, 8)[0] == 3
        vendor_rd = struct.unpack_from("<I", vb, 24)[0]

    lead = (-vendor_rd) % ALIGN
    payload = open(a.payload, "rb").read() if a.payload else b""
    if payload:
        assert payload[:8] == b"SWEETXNU", "payload must come from tools/mkpayload.py"
    ramdisk = b"\0" * lead + payload
    if not payload:
        ramdisk = b"\0" * 4096   # S-Boot gets a non-empty ramdisk either way

    hdr = bytearray(4096)
    hdr[0:8] = b"ANDROID!"
    struct.pack_into("<IIII", hdr, 8, len(shim), len(ramdisk), os_version, 1580)
    struct.pack_into("<I", hdr, 40, 3)
    boot = bytes(hdr) + pad(shim) + pad(ramdisk)
    assert len(boot) <= 64 * 1024 * 1024, "boot partition is 64 MiB"

    os.makedirs(a.outdir, exist_ok=True)
    img = os.path.join(a.outdir, "s21-boot.img")
    open(img, "wb").write(boot)
    tar = os.path.join(a.outdir, "AP_s21.tar")
    with tarfile.open(tar, "w", format=tarfile.USTAR_FORMAT) as t:
        ti = tarfile.TarInfo("boot.img")
        ti.size = len(boot)
        ti.mode = 0o644
        t.addfile(ti, io.BytesIO(boot))
    print(f"{img}: {len(boot):#x} bytes (shim {len(shim):#x}, ramdisk {len(ramdisk):#x}, "
          f"lead pad {lead} after vendor ramdisk {vendor_rd:#x}, os_version {os_version:#x})")
    print(f"{tar}: flash in Odin's AP slot")


if __name__ == "__main__":
    main()
