# TB-J606F board values (from live device HA1P42GM)

Read from `/proc/device-tree` in LineageOS recovery. Model `Lenovo P11 WIFI PVT`,
SoC `qcom,bengalp` = **SM6115 / Snapdragon 662**, 6 GiB RAM. All big-endian regs
decoded (root cells `<2 2>`, soc cells `<1 1>`).

| What | Value | Source node | Filled into |
|---|---|---|---|
| GICD | `0x0f200000` size `0x10000` | `soc/interrupt-controller@f200000` reg[0] | `J_GICD`, `J606F_GICD_BASE` |
| GICR | `0x0f300000` size `0x100000` | same, reg[1]; `#redistributor-regions=1` | `J_GICR`, `J606F_GICR_BASE` |
| UFS host | `0x04804000` size `0x3000` | `soc/ufshc@4804000` reg[0] (phy @`0x4807000`) | `J_UFS`, `J606F_UFS_BASE` |
| Framebuffer | `0x5c000000` size `0xf00000` | `reserved-memory/cont_splash_region@5c000000` | `J_FB`, `J606F_FB_BASE` |
| Panel | `1200 x 2000` (`0x4b0 x 0x7d0`) | mdss dsi panel timing (nt36523w / himax83102p) | `J_FB_W/H`, stride `4800` |
| Memory | `0x40000000+0x3c900000`, `0x80000000+0x80000000`, `0x100000000+0xc0000000` | `/memory` reg (6 GiB) | `J_PHYS_BASE=0x80000000`, `J_HIGH_PHYS_BASE=0x100000000` (3 GiB, clean) |
| Timer | `19.2 MHz` (`0x0124f800`) | `soc/timer@f120000` clock-frequency | `J_TIMEBASE` |
| CPUs | silver `0x0 0x1 0x2 0x3` (cap `0x400`), gold `0x100 0x101 0x102 0x103` (cap `0x666`) | `cpus/cpu@*` reg (MPIDR) | `J_CPU_MPIDR` (silver first; boot `cpus=4`) |
| SPMI | `0x01c40000` | `soc/spmi@1c40000` | `SPMI_BASE` (PMIC PM6125) |

## Touch — Novatek NT36523 over **SPI** (not I2C)
`soc/spi@4a88000` (size `0x4000`), child **`novatek@1`** active (`status=okay`):
- chip-select **1**, spi-max-frequency **9.6 MHz**
- IRQ **TLMM gpio 86** (`0x56`), reset **TLMM gpio 54** (`0x36`)
- (alternate `himax_ts@0` on CS0 is for Himax-panel units)

Filled addresses into `xnu/j606F/j606F_touch.c`; the Novatek **SPI protocol**
itself is still to be implemented (the template's I2C read path is a placeholder).

## Still unknown / to verify on first boot
- `J_VTIMER_INTID` (reused sweet's 19 — verify against the panic log).
- ~~Framebuffer byte order~~ — **CONFIRMED a8r8g8b8/XRGB** (milestone-0 red paint).
- Touch regulator rail (`TOUCH_VDD_RPMH_LDO`) and PMIC PON SIDs/offsets.
- ~~Bootloader unlock state~~ — **CONFIRMED unlocked** (`verifiedbootstate=orange`).
