#!/usr/bin/env python3
"""Turn ios-sweet's xnu patch series into the Lenovo Tab P11 (TB-J606F) series.

  python apply_j606F.py IOS_SWEET_CHECKOUT/xnu/patches

Rewrites, in place and verified by exact match counts, the few constants that
are Redmi Note 10 Pro (sweet, SM7150) hardware rather than ADT-driven. Each
change keeps line counts, so every hunk still applies and later patches that
touch the same file still match.

  0010, 0012  early framebuffer console: 0x9c000000 (sweet cont_splash)
              -> 0x5c000000 (j606F cont_splash_region@5c000000, measured on the
              device). The early console (sweet_early.c + the start.s Stage-0
              band) hardcodes the PANEL geometry too, and it is NOT ADT-driven:
              sweet is 1080x2400 stride 4320, j606F is 1200x2000 stride 4800.
              Left unfixed, every early marker renders sheared or off-screen on
              the P11 panel (row 2368 is past its 2000 rows), so XNU looks hung
              even while it runs. So also rewrite:
                EARLY_WIDTH 1080 -> 1200, EARLY_HEIGHT 2400 -> 2000,
                EARLY_STRIDE 4320/4 -> 4800/4, and the Stage-0 band in start.s
                (0x..+2368*4320, mov x4 #4320) -> (0x..+1000*4800, #4800).
              The GIC 0x0f200000, timer 19.2 MHz, vtimer INTID 19, RAM and CPUs
              still come from the ADT (j606F/tools/mkadt_j606F.py).

  MEMORY: build the ADT WITHOUT --highmem. The start.s bootstrap framebuffer
              mapping assumes "all bring-up RAM is below 4GB"; phys_base at
              0x100000000 (highmem) breaks that bootstrap page-table math and
              hangs arm_vm_init. j606F boots on the low bank 0x80000000 (2GB,
              carve-out free).

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
    # CPU arch: sweet runs on Kryo 470 (Cortex-A76, ARMv8.2) which has FEAT_LSE;
    # j606F runs XNU on the Snapdragon 662 Cortex-A73 gold cores, which are
    # ARMv8.0 and do NOT implement LSE atomics (ldadd/cas/swp) or PAN. Build at
    # armv8-a so clang emits ll/sc (ldxr/stxr) atomics instead. Confirmed on
    # device: an armv8.2 build panics "Undefined kernel instruction" on the
    # first LDADD (sleh.c), four stages into arm_init.
    "0004-sweet-machine-config.patch": [
        ("-march=armv8.2-a+crypto", "-march=armv8-a+crypto", 1),
    ],
    "0010-sweet-early-framebuffer-debug.patch": [
        ("0x9c000000", "0x5c000000", 3),
        ("2368 * 4320", "1000 * 4800", 1),   # Stage-0 band: visible row + j606F stride
        ("#4320", "#4800", 1),               # Stage-0 band stride (mov x4)
        ("EARLY_WIDTH 1080u", "EARLY_WIDTH 1200u", 1),
        ("EARLY_HEIGHT 2400u", "EARLY_HEIGHT 2000u", 1),
        ("(4320u / 4)", "(4800u / 4)", 1),
    ],
    "0012-sweet-keep-debug-console-after-ttbr0.patch": [
        ("0x9c000000", "0x5c000000", 3),
        ("EARLY_WIDTH 1080u", "EARLY_WIDTH 1200u", 1),
        ("EARLY_HEIGHT 2400u", "EARLY_HEIGHT 2000u", 1),
        ("(4320u / 4)", "(4800u / 4)", 1),
    ],
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
