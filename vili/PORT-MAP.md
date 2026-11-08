# Mi 11T Pro (vili, SM8350) — iOS/XNU port map

Hardware facts from the read-only `facts/vili_facts.py collect` run (phone in OrangeFox
recovery). SoC reports as **"Qualcomm Lahaina V2.1"** = SM8350 / Snapdragon 888.

This is a **Qualcomm** target, same family as upstream ios-sweet (Redmi Note 10 Pro,
SM7150). So the port should start from **upstream sweet's Qualcomm code**, not the S21's
Exynos code, adjusting addresses for the newer (Lahaina) generation.

## CPU / SMP  — the big win vs the S21
- 8 cores, MPIDR Aff1:Aff0 = `0x000,0x100,0x200,…,0x700`.
  Parts: `0xd05` A55 ×4, `0xd41` A78 ×3, `0xd44` X1 ×1.
- `enable-method = "psci"`, **`psci { method = "smc" }`** → ARM Trusted Firmware (BL31) at EL3.
- **No Samsung-style H-Arx hypervisor.** `smc` CPU_ON is the path sweet uses for 6 cores,
  so multi-core is expected to work here (confirm on hardware). This is why the Mi 11T Pro
  can be faster than the S21, which is permanently stuck at 1 core.

## Memory (8 GB)
- `/proc/iomem` System RAM tops out at `0x27fffffff` (so RAM end = `0x280000000`).
- Large high System-RAM block `0xe2800000–0x27fffffff` with carveouts:
  `e8800000–ea3fffff`, **`ea600000–ec9fffff` (bootloader splash fb)**, `ee400000–ffbfffff`,
  and `0x276000000–0x27fffffff` near the top.
- **Clean region for XNU:** `0x100000000 … 0x276000000` (≈5.4 GB) is RAM-backed and
  unreserved. Candidate XNU base `0x100000000`, size up to a few GiB.

## Display
- **Bootloader framebuffer (cont_splash): `0xEA600000`, size `0x2300000` (≈36 MB).**
  The shim draws its text console here (S21 analogue: `0xFA000000`).
- Panel: `qcom,mdss_dsi_k3s_42_02_0a_dsc_cmd` — **DSC-compressed, command-mode** DSI panel
  (like the S21, needs a TE-triggered refresh; cont_splash keeps the pipe scanning).
- Display controller (SDE/DPU): `qcom,sde-kms @ 0xAE00000` (SSPP VIG+DMA pipes, layer mixer,
  DSPP) — the target for later hardware compositing ("partial accel").
- DSI ctrl0 `0xAE94000`, ctrl1 `0xAE96000`; DSI phy0 `0xAE94400`.

## Interrupts / timer
- GIC v3: distributor `0x17A00000`, redistributor `0x17A60000`.
- Qualcomm PDC `0xB220000`.
- Generic timer **19.2 MHz** (`clock-frequency = 0x124F800`).

## Storage
- UFS host `qcom,ufshc @ 0x1D84000` (reg ranges 0x1d84000/3000, 0x1d88000/8000, 0x1d90000/9000).
  `androidboot.bootdevice=1d84000.ufshc`. Same family as sweet's UFS driver (adapt, don't rewrite).

## PMIC / keys / power — matches sweet almost exactly
- SPMI arb `qcom,spmi-pmic-arb @ 0xC440000`: core `0xC440000`, channels `0xC600000`,
  **observer `0xE600000`**, intr `0xE700000`, **config `0xC40A000`**.
- These are the **same bases sweet_keys.c already uses** (`KEYS_CORE_PA 0x0c440000`,
  `KEYS_OBS_PA 0x0e600000`, `KEYS_CNFG_PA 0x0c40a000`). Power/volume keys and PMIC rail
  control (touch power) should port with minimal change — no ACPM rewrite like the S21 needed.

## Partitions (slot A/B, suffix `_a`)
- `userdata` **112,759,648,256 B (≈105 GiB)** — iOS install target (plenty of room).
- `super` 8704 MiB, `metadata` 16 MiB, `boot_a` 192 MiB, `vendor_boot_a` 96 MiB, `dtbo_a` 24 MiB.
- Read-only copies saved: boot_a, vendor_boot_a, dtbo_a, vbmeta_a.

## cmdline highlights
`console=ttyMSM0,115200n8 androidboot.hardware=qcom androidboot.bootdevice=1d84000.ufshc`
`androidboot.slot_suffix=_a androidboot.verifiedbootstate=green`
`msm_drm.dsi_display0=qcom,mdss_dsi_k3s_42_02_0a_dsc_cmd`

## Plan (order)
1. **Shim bring-up:** build a Qualcomm shim (from sweet's, not S21's), entry as arm64 Linux
   Image; paint the `0xEA600000` framebuffer and get an on-screen text console. Confirm entry
   EL, SCTLR, and the DTB pointer, like the S21 color/console tests.
2. **GIC + timer** probe (v3 @ 0x17A00000, 19.2 MHz).
3. **SMP test:** `smc` PSCI CPU_ON of cores 1–7 into the shim — the thing that failed on the S21.
4. **XNU payload:** reuse the sweet XNU + iPhone X userland; new ADT for vili (mem base, fb,
   gic, timer, ufs, cpus).
5. **UFS storage** (adapt sweet's Qualcomm UFS), write the iOS root to `userdata`.
6. **Touch / keys / power / clock** (SPMI, matches sweet).
7. **DPU hardware compositing** (perf, late) — the realistic "partial GPU accel".

## Progress log
- **2026-11-08 — Milestone 1 PASSED (first try):** the vili shim boots flash-free via
  `fastboot boot`; Qualcomm ABL enters at **EL1**, and writing `0xEA600000` shows on the panel
  immediately (no display "kick" needed, unlike the S21). Entry + display confirmed.
- **2026-11-08 — Milestone 2 PASSED: all 8 cores start.** `smc` PSCI CPU_ON of cores 1–7 each
  returns `ret=0` and the secondary runs our `secondary_main` (MPIDR `0x…0100`…`0x…0700`).
  No H-Arx: unlike the S21 (all secondaries dead), **the Mi 11T Pro gets full multi-core.**
  Boot CPU MPIDR Aff1=0; PSCI v1.1 (ver=0x00010001). This makes 8-core iOS realistic here.
