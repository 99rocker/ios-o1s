#!/usr/bin/env python3
"""Build the S21 shim Image with Zig's clang (pip install ziglang), on any OS.

  python build_shim.py [OUTDIR]     -> OUTDIR/Image (default: out/)

Same flags and checks as ios-sweet's shim/build.sh: S-Boot runs the image
wherever it loads it with no relocation pass, so the C objects must have no
absolute (R_AARCH64_ABS64) relocations; only the header's image_size in
start.o (linked at 0) may be absolute. The Image is padded to _end so
.bss and the stacks exist as zeroed bytes.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES = ["start.S", "shim.c", "console.c", "fdt.c", "loader.c"]
CFLAGS = ["-target", "aarch64-freestanding-none", "-mcpu=generic", "-O2", "-ffreestanding",
          "-nostdlib", "-fno-pic", "-mgeneral-regs-only", "-mstrict-align",
          "-fno-stack-protector", "-fno-jump-tables", "-g0", "-Wall", "-Wextra", "-Werror"]
R_AARCH64_ABS64 = 257


def zig(*args):
    cmd = [sys.executable, "-m", "ziglang", *args]
    r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(args)}\n{r.stdout}{r.stderr}")
    return r


def elf_sections(blob):
    shoff, = struct.unpack_from("<Q", blob, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", blob, shoff + i * shentsize) for i in range(shnum)]
    names = secs[shstrndx]
    def name(s):
        o = names[4] + s[0]
        return blob[o:blob.index(b"\0", o)].decode()
    return [(name(s), s) for s in secs]


def abs64_relocs(path):
    blob = open(path, "rb").read()
    n = 0
    for name, (_, sh_type, _, _, off, size, _, _, _, entsize) in elf_sections(blob):
        if sh_type == 4:  # SHT_RELA
            for i in range(size // entsize):
                _, info, _ = struct.unpack_from("<QQq", blob, off + i * entsize)
                n += (info & 0xFFFFFFFF) == R_AARCH64_ABS64
    return n


def symbol(path, want):
    blob = open(path, "rb").read()
    secs = [s for _, s in elf_sections(blob)]
    for sh in secs:
        if sh[1] == 2:  # SHT_SYMTAB
            strtab = secs[sh[6]]
            for i in range(sh[5] // sh[9]):
                st_name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", blob, sh[4] + i * sh[9])
                o = strtab[4] + st_name
                if blob[o:blob.index(b"\0", o)].decode() == want:
                    return value
    sys.exit(f"{want} not found in {path}")


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "out"))
    os.makedirs(out, exist_ok=True)
    objs = []
    for src in SOURCES:
        obj = os.path.join(out, os.path.splitext(src)[0] + ".o")
        zig("cc", *CFLAGS, "-c", src, "-o", obj)
        objs.append(obj)
    bad = {os.path.basename(o): abs64_relocs(o) for o in objs[1:]}
    if any(bad.values()):
        sys.exit(f"absolute relocations in C objects: {bad}")
    elf = os.path.join(out, "shim.elf")
    zig("cc", *CFLAGS, "-static", "-T", "shim.ld", "-Wl,--build-id=none", *objs, "-o", elf)
    image = os.path.join(out, "Image")
    zig("objcopy", "-O", "binary", elf, image)
    end = symbol(elf, "_end")
    data = open(image, "rb").read()
    assert len(data) <= end, (len(data), end)
    data += b"\0" * (end - len(data))
    open(image, "wb").write(data)
    c0, _, text_offset, image_size = struct.unpack_from("<IIQQ", data, 0)
    assert data[0x38:0x3C] == b"ARMd" and text_offset == 0x80000 and image_size == end
    print(f"{image}: {len(data):#x} bytes, text_offset {text_offset:#x}, image_size {image_size:#x}, "
          f"ABS64 in C objects: 0")


if __name__ == "__main__":
    main()
