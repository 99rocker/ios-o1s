# TB-J606F bring-up log

Running log of boots on the device (HA1P42GM), newest first. Mirrors the style
of upstream `NOTES.md`.

## Milestone 1b — GIC/timer/PSCI pass, MMU blocked by 16K granule (2026-11-16)
- **GIC + timer + PSCI all work** (MMU off):
  - PSCI ver=0x10000 (v1.0), SMCCC=0x10001 (v1.1).
  - GICD enables (now=0x12), GICR@0x0f300000, ISENABLER now=0xc0002, ICC grpen1=1.
  - **VTIMER irqs=0x1e (30 delivered), hppi=0x13 → INTID 19 is correct for j606F.**
  - SGI1 irqs=0x0a (10/10). Interrupt delivery fully functional.
- **MMU enable HANGS** (red screen, frozen at the "enabling now" marker; no blue
  panic, no crashdump = silent fetch-abort loop).
  - **Root cause: `ID_AA64MMFR0_EL1.TGran16 = 0` on the boot core (cpu0, A53) →
    the 16 KB translation granule is NOT supported.** sweet's mmu_enable builds
    16K tables; the CPU can't do 16K, so enabling the MMU faults immediately.
  - PARange=2 (40-bit PA). PFR0 shows GIC sysreg iface present.
## Milestone 2 ✅ — 16K MMU runs on a gold A73 core (2026-11-16)
- A53 boot core PSCI_CPU_ON'd cpu4 (A73); the gold core ran `mmu_enable`:
  - `mmu rc=0`, `SCTLR_EL1=0x00C01805` (**M=1, MMU enabled**, C+I caches on).
  - `PAR_EL1` for the FB probe = `0x440000005C001B00` → **F=0, translation OK**
    (FB VA resolved to PA 0x5C001xxx).
  - `16K MMU UP ON A73!`
- **All hardware prerequisites for iOS are now proven on this device:** FB, PSCI,
  GICv3 + vtimer (INTID 19), SGI, and the 16K MMU on the gold cores. No remaining
  architectural blocker. The kernel is worth building.
- **Boot model locked in:** shim boots on A53 (ABL), migrates to a gold A73 before
  enabling the MMU / handing off to XNU; XNU runs gold-only (cpus=4).

## Granule probe result ✅ — gold A73 cores DO support 16K (2026-11-16)
- Per-core `ID_AA64MMFR0_EL1`:
  - cpu0 A53 (boot): `0x00001122` → **TGran16=0** (no 16K)
  - cpu1 A53:        `0x00001122` → TGran16=0
  - **cpu4 A73 gold: `0x00101122` → TGran16=1 (16K SUPPORTED)**, started clean (on=0)
- **Verdict: iOS is NOT architecturally blocked.** The A73 gold cores do 16K. XNU
  must boot and run **gold-only** (the 4 A73 cores). The A53 cores cannot join a
  16K page-table system, so they are left parked.
- **Required change:** the shim boots on cpu0 (A53, ABL's choice). It must
  `PSCI_CPU_ON` a gold core and hand XNU off to it BEFORE enabling the MMU. The
  ADT lists the 4 gold cores only; XNU runs `cpus=4` on gold.

## Next (updated)
1. Shim: migrate execution to a gold A73 core, then re-run `mmu_enable` there →
   confirm the 16K MMU comes up (milestone 2).
2. Fork ios-sweet + build the XNU kernel on macOS CI with the j606F patches
   (gold-core boot, GIC 0x0f200000, UFS 0x04804000, FB, SMP gold-only).
3. Careful UFS bring-up at 0x04804000 (the root lives here).
4. iPhone X IPSW root on userdata (DESTRUCTIVE — only once the kernel mounts it).

## Milestone 0 — first light ✅ (2026-11-15)
- **Result:** minimal shim (`shim/shim_min.c`) paints the panel **red with text**
  and parks. CPU runs our code, framebuffer is live.
- **Confirmed:**
  - ABL (unlocked, `verifiedbootstate=orange`) accepts `fastboot boot` of our
    unsigned v2 image and hands off (`Booting OKAY`, ~10 s).
  - Framebuffer base `0x5c000000`, geometry 1200×2000, **pixel format a8r8g8b8**
    (red rendered as red → XRGB, not BGRX). Resolves the DEVICE_VALUES open item.
  - Shim runs with MMU off; `con_clear`/`con_str` work.
- **Build path:** shim compiled in WSL (aarch64-linux-gnu-gcc); boot.img assembled
  locally with `tools/mkboot_v2.py` (reuses stock v2 header + stock DTB, swaps in
  our kernel + empty ramdisk — no mkbootimg needed).
- **Boot image geometry:** kernel_addr `0x8000`, dtb `0x1f00000` (from stock header).

## Earlier: full sweet shim → Qualcomm CrashDump ❌
- The upstream "milestone 5" shim does `loader_boot` → **`test_ufs()`**, which pokes
  the UFS host registers. On SM6115 that external abort is **routed to EL3 (TZ)**,
  not our EL1 handler → Qualcomm CrashDump (not our blue panic). UFS bring-up must
  be done carefully (clocks/XPU) and is deferred.

## Pitfall logged
- `build/stock-boot.img` got CRLF-corrupted (git autocrlf stripped `\r`), shifting
  the embedded DTB. Fixed with `.gitattributes` (`*.img binary`) + re-pull. Always
  verify `d.count(b"\r\n") > 0` and `dtb_off == find(d00dfeed)` after pulling.

## Next
1. **Milestone 1:** FB + safe diagnostics only — PSCI version (`smc`), CPU feature
   regs (granule, GIC sysreg iface), **MMU enable** with the j606F map. No UFS.
2. **Milestone 2:** GIC at `0x0f200000` + virtual-timer IRQ (verify INTID; sweet=19).
3. **Milestone 3:** careful UFS bring-up at `0x04804000` (check clock/XPU first).
4. Then the XNU kernel payload path.
