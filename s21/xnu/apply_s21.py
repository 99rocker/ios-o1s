#!/usr/bin/env python3
"""Turn ios-sweet's xnu patch series into the S21 (o1s) series.

  python apply_s21.py IOS_SWEET_CHECKOUT/xnu/patches

Rewrites, in place and verified by exact match counts, the few constants
that are Redmi Note 10 Pro hardware rather than ADT-driven. Each change
keeps line counts, so every hunk still applies and later patches that
touch the same code (0012 edits 0010's sweet_early.c) still match.

  0010, 0012  early framebuffer console: 0x9c000000 (sweet cont_splash)
              -> 0xfa000000 (S-Boot fb, s3cfb.bootloaderfb). Same 1080x2400
              panel and 4320-byte stride, so the geometry is unchanged.
              The 32 MiB block mapping stays valid: 0xfa000000 is 32 MiB
              aligned and, like RAM at 0x8c0000000, sits in L1 entry 0
              (< 64 GiB) of the 16K-granule V=P bootstrap tables.
  0065        PSCI CPU_ON conduit: smc -> hvc (S21 live DT /psci method =
              "hvc": Samsung's EL2 hypervisor forwards PSCI).

Everything else the kernel needs (GIC, timer, RAM, framebuffer, CPUs) comes
from the ADT (tools/mkadt_s21.py). Redmi-only drivers stay compiled in and
are switched off with boot-args (see README): sweetkeys=0 sweettouch=0
sweetusb=0 sweetlog=0; no /sweet-ufs node means no UFS probe.
Run it once; a second run fails its count checks (by design).
"""
import os
import sys

EDITS = {
    "0010-sweet-early-framebuffer-debug.patch": [("0x9c000000", "0xfa000000", 3)],
    "0012-sweet-keep-debug-console-after-ttbr0.patch": [("0x9c000000", "0xfa000000", 3)],
    "0065-sweet-smp-psci.patch": [('__asm__ volatile("smc #0"', '__asm__ volatile("hvc #0"', 1)],
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    for name, edits in EDITS.items():
        p = os.path.join(d, name)
        s = open(p, encoding="utf-8", newline="").read()
        lines_before = s.count("\n")
        for old, new, n in edits:
            got = s.count(old)
            if got != n:
                sys.exit(f"{name}: expected {n} x {old!r}, found {got} (already applied, or upstream changed)")
            s = s.replace(old, new)
        assert s.count("\n") == lines_before
        open(p, "w", encoding="utf-8", newline="").write(s)
        print(f"{name}: " + ", ".join(f"{n} x {o!r} -> {w!r}" for o, w, n in edits))


if __name__ == "__main__":
    main()
