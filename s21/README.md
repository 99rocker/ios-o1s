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
