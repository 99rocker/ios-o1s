# ios-sweet → Lenovo Tab P11 (TB-J606F): XNU bring-up

Adapts [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (MIT) to
the **Lenovo Tab P11, TB-J606F** — Qualcomm **SM6115 / Snapdragon 662** (`bengalp`).
Every value was measured on the device (serial HA1P42GM) or read from its live
device tree. No Apple files are included.

This is the `j606F` branch (one branch per device, like `main`=s21, `vili`).

## Key device fact: boot GOLD-only (16K granule)

SD662 is **8 cores: 4× Cortex-A73 "gold" + 4× Cortex-A53 "silver"**. Measured on
the device: the **A53 cores do NOT support the 16KB page granule** iOS requires
(`ID_AA64MMFR0.TGran16=0`); the **A73 gold cores DO** (`TGran16=1`). ABL boots us
on an A53, so the **shim migrates to a gold A73 core before enabling the MMU /
handing off to XNU**, and XNU runs **gold-only** (`cpus=4`, gold MPIDRs
`0x100–0x103`). Confirmed on hardware: `16K MMU UP ON A73` (SCTLR.M=1, PAR ok).

## What's here

| Path | What |
|---|---|
| `tools/mkadt_j606F.py` | Apple Device Tree: RAM highmem `0x100000000`+3GiB, GIC `0x0f200000`/`0x0f300000`, 19.2 MHz timebase, vtimer **INTID 19**, fb `0x5c000000` 1200×2000, 4 gold CPUs. |
| `tools/mkboot_v2.py` | Assembles an Android boot.img v2 from the stock header+DTB, swapping in kernel+ramdisk (no mkbootimg needed). |
| `shim/shim_*.c`, `board_j606F.h` | Shim milestones validated on device: `shim_min` (first light), `shim_m1` (GIC/timer/PSCI), `shim_probe` (granule), `shim_m2` (16K MMU on A73). `board_j606F.h` holds the board addresses. |
| `xnu/apply_j606F.py` | Rewrites the sweet patch series for j606F: early fb console `0x9c000000 → 0x5c000000` (0010, 0012). **No PSCI change** — j606F uses `smc` (confirmed), unlike the S21's `hvc`. |
| `DEVICE_VALUES.md`, `J606F_PROGRESS.md` | All measured addresses + the on-device bring-up log. |

## Status (hardware-validated via the shim)

- ✅ ABL hand-off, framebuffer, PSCI (smc, v1.0), GICv3 + vtimer (INTID 19), SGI.
- ✅ **16K MMU enables on the A73 gold cores.** No architectural blocker remains.
- ▶ **This branch builds the XNU kernel** (sweet base + fb swap, `MACHINE_CONFIG=SWEET`).
  Milestone 1 = "XNU text console": drivers off via boot-args
  (`sweetkeys=0 sweettouch=0 sweetusb=0 sweetlog=0`), **no UFS node** (no probe).
- ⏭ Next: UFS bring-up at `0x04804000`, then the iPhone X IPSW root on `userdata`.

Kernel builds on GitHub Actions macOS runners (`.github/workflows/xnu.yml`,
shared with the other device branches).
