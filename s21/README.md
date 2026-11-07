# ios-sweet → Galaxy S21 Exynos (o1s): milestone "XNU text console"

This folder adapts [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (MIT, see `LICENSE-ios-sweet`) to your SM-G991B. Every S21 value was measured on your phone (`../s21-facts-out`, `../shim-test`) or read from its live device tree. No Apple files are included.

## What's here

| Path | What it is | Status |
|---|---|---|
| `shim/` | ios-sweet's boot shim, made S21-specific: `text_offset 0x80000`, framebuffer `0xfa000000`, DECON trigger unmask, payload found behind the vendor ramdisk. Reduced to the loader path | **Builds** (`build_shim.py`, Zig clang 22, no absolute relocations). **Emulated** with your real DTB and vendor ramdisk: finds the payload, copies kernel + ADT, fills `boot_args`, jumps |
| `tools/mkadt_s21.py` | Apple Device Tree for the S21: RAM `0x8c0000000` (+4 GiB, clean block), GIC `0x10200000`/`0x10240000`, 26 MHz timebase, vtimer INTID 27, 8 CPUs (MPIDR `core<<8`), fb `0xfa000000`, no UFS node | Generates (`out-s21.adt`). `/product` needs the D22AP device tree from the IPSW, same as upstream; not needed for this milestone |
| `tools/mks21boot.py` | Packs shim + payload into a header-v3 `boot.img` and `AP_s21.tar`, with 9 bytes of padding so the payload lands 16-byte aligned behind the 0xa8ac37-byte vendor ramdisk | Works |
| `tools/mkpayload.py`, `tools/adt.py` | Upstream, unchanged (kernel Mach-O + ADT → `SWEETXNU` payload) | — |
| `xnu/apply_s21.py` | Rewrites upstream's patch series for the S21: early fb console `0x9c000000 → 0xfa000000` (patches 0010, 0012), PSCI `smc → hvc` (0065) | Tested on a copy: 7 replacements, line counts unchanged, refuses a second run |
| `out-shim-only/AP_s21.tar` | **First on-phone test**: shim without a kernel | Ready to flash |

## Step 1: flash the shim-only image (no XNU needed)

1. In Odin, flash `out-shim-only/AP_s21.tar` in the **AP** slot.
2. **Expected:** a red screen with yellow and white text (`out-shim-only/emulated-screen.png` is the emulator's rendering):
   ```
   SHIM     S21 o1s: xnu loader
   ENTRY    el=00000001 sctlr=00C50838
   DTB      00000000xxxxxxxx
   INITRD   lo=84080000 hi=84B0BC37
   LOADER   no SWEETXNU payload
   HALT     hold Vol Down + Side to restart
   ```
   That proves the C shim, its font console and the DECON kick work on the real phone.

   **Result on the phone (2026-11-07): passed.** The screen showed exactly the expected lines:
   - `ENTRY el=00000001 sctlr=00C5183A`: EL1, MMU (bit 0) and D-cache (bit 2) off. Unlike the emulator, the **I-cache (bit 12) and alignment checking (bit 1) are on**. The shim is built `-mstrict-align` and does `ic iallu` before jumping to xnu, so both are covered.
   - `DTB 8B080000`: S-Boot places the DTB there, far below the kernel's `0x8c0000000`.
   - `INITRD lo=84080000 hi=84B0BC37`: identical to the emulator, which confirms vendor ramdisk (`0xa8ac37`) + our ramdisk are contiguous, as `mks21boot.py` assumes.
3. Back to Android: get into Download mode (Volume Down + Side until black, then Volume Up + Volume Down with USB plugged in). Flash `../shim-test/out/AP_restore_lineage_boot.tar` (or `AP_decon2.tar`).

## Step 2: build XNU (needs a GitHub account; the kernel only builds on macOS)

The SWEET kernel builds on GitHub's free macOS runners (public repos), with upstream's workflow.

1. Fork `dipa-studio/ios-sweet` on GitHub and clone your fork.
2. Apply the S21 changes:
   ```bash
   python s21-port/xnu/apply_s21.py path/to/your-fork/xnu/patches
   ```
3. Commit and push to `main`. The `xnu` workflow starts by itself (about 1–2 h). Then download the `xnu-kernel` artifact:
   ```bash
   gh run download RUN_ID -R YOU/ios-sweet -n xnu-kernel -D xnu-out
   ```

## Step 3: pack and flash the kernel

```bash
python tools/mkadt_s21.py out-s21.adt
```
```bash
python tools/mkpayload.py xnu-out/kernel.development.sweet out-s21.adt payload.bin --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam sweetkeys=0 sweettouch=0 sweetusb=0 sweetlog=0 wdt=-1"
```
```bash
python tools/mks21boot.py shim/out/Image out-xnu --payload payload.bin --stock-boot ../s21-facts-out/boot.img --vendor-boot ../s21-facts-out/vendor_boot.img
```

Flash `out-xnu/AP_s21.tar`. The boot-args switch off the Redmi-only drivers (keys, touch, USB, the UFS persistent log).

**Expected:** the shim screen, then XNU's numbered early-boot marks (`1 arm_init entered` … `20 kprintf startup complete`), then kernel text on the framebuffer console. With no root filesystem yet, it should end waiting for a root device or in a panic. Either one on screen is this milestone.

### First real kernel (2026-11-07)

- **Build:** [99rocker/ios-o1s run 42379679186](https://github.com/99rocker/ios-o1s/actions/runs/42379679186) on `macos-15` with Xcode 16.0 plus patch `0082` (an `IORPC.kernelContent` backport, since GitHub retired the `macos-14` image). Succeeded in about 8 minutes.
- **Kernel:** `kernel.development.sweet` has SHA-256 `a3008abf…570768d`, matching the CI's `SHA256SUMS`. The S21 framebuffer constants are compiled in (`movk #0xfa9c`, `#0xfa00`) and the Redmi ones (`0x9c..`) are gone, so the patches applied.
- **Flash file:** `out-xnu/AP_s21.tar` (16 MB), containing the shim, the payload (kernel + `out-s21.adt` + boot-args above) and 9 bytes of padding.
- **Emulated (Unicorn, real kernel, your DTB and vendor ramdisk):**
  - The shim loads the payload and jumps to XNU `start.s` (`x0` = boot_args at `0x8c24c8000`).
  - XNU draws its stage-0 white band at `0xfa000000`.
  - It builds its bootstrap page tables and enables the MMU. The tables are correct: `0x8c0000000` is identity-mapped, Normal memory, executable, with TCR T0SZ 17, 16K granule and IPS 40-bit.
  - The emulator then faults, because its CPU model lacks the 16K granule (`ID_AA64MMFR0.TGran16 = 0`). The S21's A55, A78 and X1 cores support 16K pages, so the rest needs the phone.

### On the phone (2026-11-07): **XNU verbose console reached**

After flashing `out-xnu/AP_s21.tar`, the red shim screen gave way to a black screen with scrolling kernel text, like a Hackintosh verbose (`-v`) boot. So XNU runs on the S21: it gets past MMU enable, `arm_vm_init` and serial/fbcon init, onto its framebuffer console at `0xfa000000`.

**Milestone reached: kernel init complete, waiting for root.** This is the same end state as ios-sweet's Redmi milestone. The console wraps, so the newest lines are at the top of the screen. From phone photos:

```
SWEET SURF: IOCoreSurfaceRoot registered
SWEET HID: buttons registered / touch registered
SWEET BATTERY: static fallback, 100% 4200mV external=1 charging=0
SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0xfa000000, default surface 1, presented as 1125x2436
SWEET CPU: requested=1 selected=1 (limit=8)
SWEET CPU: cpu=0 entered initCPU mpidr=0x0 EL1 IRQs=1
SWEET CPU: cpu=0 mpidr=0x0 came alive; SGI1 OK
Copyright (c) 1982, 1986, 1989, 1991, 1993 The Regents of the University of California.
MAC Framework successfully initialized
SWEET CPU: 1/1 CPUs initialized, SGI1 verified
Security policy loaded: SWEET built-in Mobile File Integrity (SWEET AMFI)
Security policy loaded: SWEET permissive sandbox stand-in (Sandbox)
SWEET HFS: hfs-627.100.6 registered
mbinit: done [64 MB total pool size, (42/21) split]
DLIL: running SUM16 self-tests ... PASSED
Skywalk: sk_copy*/sk_zero*/memcmp mask ... PASSED
bsd_autoconf: calling kminit
ifnet_attach: ... lo0 / gif0 / stf0 ... Proceeding.
Waiting on <dict ID="0"><key>IOProviderClass</key><string ID="1">IOMedia</string><key>Content</key><string ID="2">Apple_HFS</string></dict>
Failed to write boot-picker-bringup-reason to NVRAM.
```

What this proves on the S21:
- **GICv3 works under Samsung's hypervisor:** `SGI1 OK` / `SGI1 verified`, and threads get scheduled, so timer interrupts arrive. The separate GIC probe (`out-gictest`) is no longer needed.
- **The framebuffer path works:** `AppleCLCD` registered on `0xfa000000` (1125×2436 presented). The NVRAM warning is expected, since there's no NVRAM driver.
- **Next milestone: root filesystem → launchd.** ios-sweet's order was a RAM-disk root (`mkpayload.py --ramdisk`, `rd=md0`) first, then the S21's own storage driver (Exynos UFS at `0x13100000`).

## Milestone 2: first userspace program (PID 1)

Follows ios-sweet's own path (their patch 0021/0025 milestone, done on the Redmi before any Apple userland):
- `userland/sweet-init.S` is ios-sweet's Apple-free init: it opens `/dev/console`, prints `*** SWEET userspace: init running as PID 1 ***`, then `SWEET init: still alive` every 5 s.
- It's assembled with Zig (`zig cc -target aarch64-ios.16.0 -g0 -c`) and wrapped by `userland/mkinit.py` into a static, ad-hoc-signed `MH_EXECUTE` (16 KB `__TEXT`, entry `0x100000400`).
- `mkpayload.py --ramdisk userland/sweet-init` appends it after the kernel and adds ADT `/chosen/memory-map RAMDisk` = `0x8c24c4000`/`0x8000` (checked against the segment table).
- Boot-args add `rd=md0 -noprogress`. XNU's mockfs serves the RAM disk itself as `/sbin/launchd`.
- **Flash file:** `out-init/AP_s21.tar`.
- **Expected on screen:** `BSD root: md0` → `load_init_program: attempting to load /sbin/launchd` → `*** SWEET userspace: init running as PID 1 ***` → a `SWEET init: still alive` line every 5 s.
- **Result on the phone (2026-11-07): passed.** `*** SWEET userspace: init running as PID 1 ***` is on screen. Userspace runs on the S21 under XNU: RAM-disk root, Mach-O loading, code-signature check, `svc #0x80` syscalls and the `/dev/console` write all work.

## Milestone 3 (in progress): Exynos UFS storage

`shim/ufs.c` (build `--ufstest`, flash `out-ufstest/AP_s21.tar`) is a read-only probe that chain-boots LineageOS after 40 s. It's adapted from ios-sweet's probe, with the Exynos quirks from Samsung's `ufs-exynos.c`: HCI `0x13100000`, vendor HCI `0x13101100`, byte-granular UTRD offsets, 4 KiB PRD entries, `HCI_UTRL_NEXUS_TYPE`, and write-1 UTRLCLR.

- **Attempt 1 (2026-11-07):** host enabled (`hce=1`, `hcs=0x10F`), S-Boot list at `0xE8?2C000`, `txprdt/rxprdt=0xC`, nexus `0xFFFFFFFF`. Our list (at `0x8008xxxx`, next to the shim) was accepted, but the first TEST UNIT READY returned **OCS 7 (fatal error)** and every later command timed out. The likely cause: DMA to `0x8008xxxx` isn't allowed (Samsung S2MPU, DT `s2mpu_table`).
- **Attempt 2:** tried to borrow S-Boot's slot-0 data buffer. Phone: `utrlba=E8D2C000`, `SBOOT ucd=E8D29000 prdt@0`, meaning S-Boot's last slot-0 command had no data, so the probe stopped before sending anything.
- **Re-reading attempt 1:** the S2MPU theory was wrong. The host wrote OCS 7 into **our** UTRD and the device wrote SCSI status 0x02 into **our** response UPIU, so DMA to `0x8008xxxx` works. OCS 7 is `UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR`: Exynos reports "fatal error" whenever the response carries a result, here the usual first-command unit attention. Linux then treats OCS as success (`ufshcd_transfer_rsp_status`). The timeouts that followed came from the host waiting for its error status (IS) to be cleared.
- **Attempt 3:** keeps S-Boot's list (slot 0 at `0xE8D2C000`, list registers untouched) with our own UCD and 4 KiB DMA buffer. It applies the OCS quirk, clears IS after every command, keeps `UTRLRSR` running, and retries TEST UNIT READY up to 5 times. Emulated against a fake host that reports the first command as OCS 7 + unit attention and stalls until IS is cleared: it recovers, then reads capacity, MBR, GPT, partitions and f2fs userdata, and boots Linux.

- **Attempt 3 result (phone):** list `E8D2C000`, ucd `800FD000`. **No more timeouts or OCS errors**: every command completes with `rc=4` (status `0x02`, CHECK CONDITION), so the host path and DMA work. Five TEST UNIT READY, READ CAPACITY and READ(10) all got CHECK CONDITION. That's persistent, so it isn't the one-off unit attention.
- **Attempt 4:** decodes sense data (response UPIU byte 32 length, byte 34 data, fixed and descriptor formats) as `sense=KKAAQQ` (key, ASC, ASCQ) on every line, and scans LUNs 0–7 with one TEST UNIT READY each.

- **Attempt 4 result:** every LUN 0–7, even absent ones, answered sense `02/00/00` (NOT READY), so the whole device was in a low-power mode.
- **Attempt 5 result (2026-11-07): STORAGE READS WORK.** The wake-up sequence from `ufshcd_set_dev_pwr_mode()` works on the phone:
  - REQUEST SENSE to the device W-LUN `0xD0` → sense `02/04/02` (NOT READY, initializing command required), as S-Boot leaves it.
  - START STOP UNIT, POWER CONDITION = Active (`cdb[4] = 1 << 4`), on W-LUN `0xD0` → GOOD.
  - TEST UNIT READY: LUN 0–4 GOOD, LUN 5 `05/25/00` (not supported), so 5 LUs (Android `sda`–`sde`).
  - LUN0: READ CAPACITY gives last LBA `0x01DC93FF`, 4096-byte blocks (≈119 GiB). MBR `AA55`/`EE`, GPT `EFI PART`, 128 entries at LBA 2.
  - Partitions ≥64 MiB (LBA in 4 KiB blocks): `boot` `0x7990` 64 MiB, `vendor_boot` `0xB990` 64 MiB, `recovery` `0xF990` 64 MiB, `radio` `0x13990` 70 MiB, `super` `0x1EE00` 16 GiB, `prism` `0x41EE00` 1100 MiB, `cache` `0x465800` 600 MiB, **`userdata` `0x491400`, `0x1937C` MiB (≈101 GiB)**. userdata's superblock reads as "other": LineageOS metadata encryption.

  **Recipe for the kernel driver** (`sweet_ufs.c` port):
  - HCI at `0x13100000`, vendor HCI at `0x13101100`.
  - Set nexus bit *n* for each SCSI slot.
  - Byte-granular UTRD offsets, 4 KiB PRD entries.
  - On OCS 7 with a response result, treat OCS as success.
  - Write-1-to-clear IS after every command, and keep `UTRLRSR` set.
  - Write-1 UTRLCLR.
  - Wake the device with REQUEST SENSE + START STOP UNIT (Active) on W-LUN `0xD0` before any LU command.
  - DMA to normal RAM works (the UCD and data buffers were in `0x80xxxxxx`).

## Known risks (unverified until it runs)

- **GIC under Samsung's hypervisor.** XNU programs the GICv3 distributor and redistributor directly, as Linux does on this phone. If the hypervisor traps any of it, the marks stop around the GIC setup.
- **First RAM above 4 GiB.** ios-sweet booted from `0x100000000`; here it's `0x8c0000000`. That's still in L1 entry 0 of the 16K V=P tables, alongside the framebuffer, so the patch-0010 block mapping should hold.
- **No reset path.** PSCI reset doesn't work this early (shim-test test 4). On a hang, hold Volume Down + Side, then Download mode.
- **Not in this milestone:** storage (Exynos UFS driver), touch (Samsung LSI touch IC), keys (GPIO and ACPM), SMP (needs `cpus=N` with the `hvc` PSCI change), and the iOS root filesystem.

## Rebuild

```bash
python -m pip install ziglang
```
```bash
python shim/build_shim.py
```
