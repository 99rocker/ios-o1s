#!/usr/bin/env python3
"""Turn ios-sweet's xnu patch series into the Lenovo Tab P11 (TB-J606F) series.

  python apply_j606F.py IOS_SWEET_CHECKOUT/xnu/patches

Rewrites, in place and verified by exact match counts, the few constants that
are Redmi Note 10 Pro (sweet, SM7150) hardware rather than ADT-driven. Each
change keeps line counts, so every hunk still applies and later patches that
touch the same file still match.

  0010, 0012  early framebuffer console: 0x9c000000 (sweet cont_splash)
              -> 0x5c000000 (j606F cont_splash_region@5c000000, measured on the
              device). Everything else the kernel needs (GIC 0x0f200000, timer
              19.2 MHz, vtimer INTID 19, RAM, CPUs, fb geometry 1200x2000) comes
              from the ADT (j606F/tools/mkadt_j606F.py).

Unlike the S21 (Exynos) series, j606F does NOT change the PSCI conduit: Qualcomm
TrustZone uses `smc` (confirmed on device: PSCI v1.0 via smc). So patch 0065 is
left untouched (sweet already uses smc).

Milestone 1 (XNU text console) also leaves the Redmi-only drivers compiled but
OFF via boot-args (sweetkeys=0 sweettouch=0 sweetusb=0 sweetlog=0) and omits the
/sweet-ufs node so there is no UFS probe. The j606F boot CPU must be a gold A73
core (16K granule); the shim migrates there before handing off (see j606F/shim).

Run it once; a second run fails its count checks (by design).
"""
import os
import sys

EDITS = {
    "0010-sweet-early-framebuffer-debug.patch": [("0x9c000000", "0x5c000000", 3)],
    "0012-sweet-keep-debug-console-after-ttbr0.patch": [("0x9c000000", "0x5c000000", 3)],
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
                sys.exit(f"{name}: expected {n} x {old!r}, found {got} "
                         f"(already applied, or upstream changed)")
            s = s.replace(old, new)
        assert s.count("\n") == lines_before
        open(p, "w", encoding="utf-8", newline="").write(s)
        print(f"{name}: " + ", ".join(f"{n} x {o!r} -> {w!r}" for o, w, n in edits))


if __name__ == "__main__":
    main()
