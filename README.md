# ios-o1s

**Apple's iOS 16 booting on a Samsung Galaxy S21 5G (Exynos 2100, `o1s`, SM-G991B).**

This is a port of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (iOS 16 on the
Redmi Note 10 Pro) to the Exynos Galaxy S21. It runs Apple's open-source XNU kernel (xnu-8796,
the iOS 16 generation) with the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**.

As of **2026-11-07** the S21 boots iOS 16 from its own storage, and **touch and the volume keys
work**: you can unlock it, use the home screen and open Settings.

> Experimental bring-up, not a usable phone OS. Apple's security
> model is mostly switched off. iOS is written into the `userdata` partition, which **erases
> Android's user data**. Needs an unlocked bootloader. The original Redmi README is in
> [README-sweet.md](README-sweet.md).

## Status on the S21

| Area | State |
| --- | --- |
| Boot | Samsung S-Boot loads our boot.img (flashed with Odin, AP slot); a small shim acts as iBoot and starts XNU |
| CPU | 8 cores start (PSCI `hvc`), but cpus=8 panics later; images use 1 core for now |
| Interrupts / timer | GICv3 at 0x10200000, 26 MHz generic timer |
| Display | S-Boot's framebuffer (0xFA000000, 1080×2400); the shim unmasks the DECON0 hardware trigger so the command-mode panel refreshes from memory |
| Storage | Exynos UFS support in the kernel UFS driver (patch 0083): byte-granular PRDT, 4 KiB PRD entries, nexus bits, OCS quirk, waking the device through the W-LUN |
| iOS userland | iPhone X 16.7.16 root (journaled HFS+) in `userdata`, read/write |
| GUI | backboardd + SpringBoard: lock screen, home screen, Settings |
| Buttons | Volume keys (GPIO gpa0-3/4, patch 0084): both together = Home, hold vol- = power. The real power key (PMIC) is not read yet |
| Touch | Samsung LSI touch IC at I²C 0x48, bit-banged, powered through ACPM by the shim (patch 0085): **works** |
| PMIC | ACPM IPC client in the shim (touch rails on s2mps23/s2mps24) |
| Real-time clock | Missing (the clock starts at the Unix epoch: "Wednesday, December 31") |
| Battery, Wi-Fi, audio, cellular, cameras, GPU | Not supported |

## Layout

- `xnu/patches/`: the ios-sweet XNU patch series (0001–0081) plus the S21 patches (0082+).
  GitHub Actions builds the kernel (`.github/workflows/xnu.yml`, macOS 15 / Xcode 16.0).
- `s21/`: the S21 port. [s21/README.md](s21/README.md) has the bring-up log, every test and the
  hardware facts it relies on.
  - `s21/shim/`: the bare-metal shim (Linux arm64 Image header, `text_offset 0x80000`)
  - `s21/tools/`: device tree (`mkadt_s21.py`), payload, boot.img/Odin tar (`mks21boot.py`),
    iOS root image build (WSL), and `write-userdata.sh`
  - `s21/xnu/`: S21 patch sources and generators

## Credits

The kernel bring-up, IOKit display/HID services and userland work are from
[ios-sweet](https://github.com/dipa-studio/ios-sweet). iOS is Apple's software: no Apple
binaries are in this repository; the root image is built locally from Apple's IPSW.
