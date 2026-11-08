#!/usr/bin/env python3
"""Build the vili (SM8350) shim with Zig and wrap it in an Android boot.img v2
that Qualcomm's `fastboot boot` runs WITHOUT flashing anything.

  python build_vili.py [--colortest] [OUTDIR]

Needs the venv Zig (python -m ziglang) and vili's live DTB at ../../mi11-facts-out/live.dtb.
Flash-free test:  fastboot boot OUTDIR/shim-boot.img
"""
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VENV_PY = r"C:/Users/DADAHY~1/AppData/Local/Temp/claude/41c9e94c6a8106cc/36a9c048-f491-44b1-8d77-0a96b94cd75c/scratchpad/venv/Scripts/python"
DTB = os.path.join(HERE, "..", "..", "mi11-facts-out", "live.dtb")

CFLAGS = ["-target", "aarch64-freestanding-none", "-mcpu=generic", "-O2", "-ffreestanding",
          "-nostdlib", "-fno-pic", "-mgeneral-regs-only", "-mstrict-align",
          "-fno-stack-protector", "-fno-jump-tables", "-g0", "-Wall", "-Wextra"]
SOURCES = ["start.S", "shim.c", "console.c", "mmu.c", "fdt.c", "loader.c", "ufs.c"]


def zig(*args):
    subprocess.run([VENV_PY, "-m", "ziglang", *args], cwd=HERE, check=True)


def abs64(obj):
    r = subprocess.run([VENV_PY, "-m", "ziglang", "objdump", "-r", obj],
                       cwd=HERE, capture_output=True, text=True)
    return r.stdout.count("ABS64") + r.stdout.count("abs64")


def sym(elf, want):
    """Find a symbol's value by parsing the ELF symbol table (nm text format varies)."""
    blob = open(elf, "rb").read()
    e_shoff = struct.unpack_from("<Q", blob, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", blob, 0x3a)[0]
    e_shnum = struct.unpack_from("<H", blob, 0x3c)[0]
    secs = [struct.unpack_from("<IIQQQQIIQQ", blob, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    for sh in secs:
        if sh[1] == 2:  # SHT_SYMTAB
            strtab = secs[sh[6]]
            for i in range(sh[5] // sh[9]):
                st_name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", blob, sh[4] + i * sh[9])
                o = strtab[4] + st_name
                if blob[o:blob.index(b"\0", o)].decode() == want:
                    return value
    raise SystemExit(f"{want} not found in {elf}")


def bootimg_v2(kernel, ramdisk, dtb, out, cmdline=b"console=null"):
    PAGE = 4096

    def pad(b):
        return b + b"\0" * (-len(b) % PAGE)

    hdr = bytearray(PAGE)
    struct.pack_into("<8s", hdr, 0, b"ANDROID!")
    struct.pack_into("<II", hdr, 8, len(kernel), 0x8000)        # kernel size/addr
    struct.pack_into("<II", hdr, 16, len(ramdisk), 0x1000000)   # ramdisk
    struct.pack_into("<II", hdr, 24, 0, 0)                       # second
    struct.pack_into("<II", hdr, 32, 0x100, PAGE)               # tags addr, page size
    struct.pack_into("<I", hdr, 40, 2)                          # header version
    struct.pack_into("<I", hdr, 44, (17 << 25))                 # os version 17.0.0
    struct.pack_into("<512s", hdr, 64, cmdline[:511])
    struct.pack_into("<IQI", hdr, 1632, 0, 0, 1648 + 12)       # recovery dtbo + header_size
    struct.pack_into("<IQ", hdr, 1648, len(dtb), 0x1f00000)    # dtb size/addr
    body = pad(kernel) + pad(ramdisk) + pad(dtb)
    open(out, "wb").write(bytes(hdr) + body)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    defines = [a[2:].upper() for a in sys.argv[1:] if a.startswith("--") and a != "--"]
    out = os.path.abspath(args[0]) if args else os.path.join(HERE, "out")
    os.makedirs(out, exist_ok=True)

    objs = []
    for src in SOURCES:
        obj = os.path.join(out, os.path.splitext(src)[0] + ".o")
        zig("cc", *CFLAGS, *[f"-D{d}" for d in defines], "-c", src, "-o", obj)
        objs.append(obj)
    bad = {os.path.basename(o): abs64(o) for o in objs if os.path.basename(o) != "start.o"}
    if any(bad.values()):
        raise SystemExit(f"absolute relocations in C objects: {bad}")

    elf = os.path.join(out, "shim.elf")
    zig("cc", *CFLAGS, "-static", "-T", "shim.ld", "-Wl,--build-id=none", *objs, "-o", elf)
    image = os.path.join(out, "Image")
    zig("objcopy", "-O", "binary", elf, image)
    data = open(image, "rb").read()
    end = sym(elf, "_end")
    data += b"\0" * (end - len(data))
    open(image, "wb").write(data)
    assert data[0x38:0x3C] == b"ARMd", "missing arm64 Image magic"

    dtb = open(DTB, "rb").read()
    rd = b""  # empty ramdisk; ABL is fine with it for a flash-free boot
    boot = os.path.join(out, "shim-boot.img")
    bootimg_v2(data, rd, dtb, boot)
    print(f"{image}: {len(data):#x} bytes, ABS64 in C objects: 0")
    print(f"{boot}: boot.img v2 ({os.path.getsize(boot)} bytes); flash-free test: fastboot boot {boot}")


if __name__ == "__main__":
    main()
