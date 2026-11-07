#!/usr/bin/env python3
"""Generate xnu patch 0083 (Exynos UFS in bsd/dev/sweet_ufs.c) from the file as
it stands after ios-sweet's patches 0028..0077.

  python make_0083.py SWEET_UFS_AFTER_0077.c OUT_EDITED.c

Every replacement must match exactly once. Run `git diff` on the result to
produce the patch (see README).
"""
import sys

T = "\t"
NL = "\\n"   # the two characters backslash, n (inside C string literals)

EDITS = [
    # header comment
    (" * outside the named partition can be written.\n */\n",
     " * outside the named partition can be written.\n"
     " *\n"
     " * Samsung Exynos 2100 (Galaxy S21, o1s): selected when /sweet-ufs has a\n"
     " * second reg pair, the \"exynos-specific hci\" block. Quirks from Samsung's\n"
     " * o1s kernel (ufs-exynos.c / ufshcd.c), each verified on the phone with the\n"
     " * s21 shim probe (s21/shim/ufs.c):\n"
     " *   - UFSHCD_QUIRK_PRDT_BYTE_GRAN: UTRD offsets/lengths in bytes\n"
     " *   - PRDT_SET_SIZE(12): 4 KiB per PRD entry\n"
     " *   - HCI_UTRL_NEXUS_TYPE (vendor +0x40): bit n = SCSI command in slot n\n"
     " *   - UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR: OCS 7 + response result = OCS ok\n"
     " *   - UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR: clear slot n by writing 1 << n\n"
     " *   - S-Boot leaves the device asleep (sense 02/04/02 on every LU): wake it\n"
     " *     with REQUEST SENSE + START STOP UNIT (Active) on the device W-LUN 0xD0,\n"
     " *     as ufshcd_set_dev_pwr_mode() does.\n"
     " */\n"),
    # constants
    ("#define PRDT_MAX        (BOUNCE_SIZE / PRDT_CHUNK)\n",
     "#define PRDT_CHUNK_EXY  4096u                   /* Exynos: PRDT_SET_SIZE(12) */\n"
     "#define PRDT_MAX        (BOUNCE_SIZE / PRDT_CHUNK_EXY)\n"
     "#define VS_UTRL_NEXUS   0x40                    /* Exynos vendor HCI_UTRL_NEXUS_TYPE */\n"
     "#define UFS_DEVICE_WLUN 0xd0\n"),
    # state
    (T + "vm_offset_t regs;\n",
     T + "vm_offset_t regs;\n"
     + T + "vm_offset_t vs;                         /* Exynos vendor HCI, 0 on Qualcomm */\n"
     + T + "bool exynos;\n"),
    # UTRD response + PRDT encoding
    (T + "utrd->resp = (512u / 4) << 16 | (512u / 4);\n"
     + T + "if (len) {\n"
     + T + T + "uint32_t n = 0;\n"
     + T + T + "for (uint32_t done = 0; done < len; done += PRDT_CHUNK, n++) {\n"
     + T + T + T + "uint32_t chunk = MIN(PRDT_CHUNK, len - done);\n",
     T + "/* Exynos: offsets and lengths in bytes (UFSHCD_QUIRK_PRDT_BYTE_GRAN). */\n"
     + T + "utrd->resp = su.exynos ? (512u << 16 | 512u) : ((512u / 4) << 16 | (512u / 4));\n"
     + T + "uint32_t n = 0;\n"
     + T + "if (len) {\n"
     + T + T + "uint32_t step = su.exynos ? PRDT_CHUNK_EXY : PRDT_CHUNK;\n"
     + T + T + "for (uint32_t done = 0; done < len; done += step, n++) {\n"
     + T + T + T + "uint32_t chunk = MIN(step, len - done);\n"),
    (T + T + "utrd->prdt = (1024u / 4) << 16 | n;\n",
     T + T + "utrd->prdt = su.exynos ? (1024u << 16 | n * 16u) : ((1024u / 4) << 16 | n);\n"),
    (T + T + T + "    (utrd->prdt & 0xffff) * sizeof(ucd->prdt[0]));\n",
     T + T + T + "    n * sizeof(ucd->prdt[0]));\n"),
    # nexus bit + keep the list running, before the doorbell
    (T + "__builtin_arm_dsb(0xf);\n\n" + T + "ufs_wr(REG_UTRLDBR, 1);\n",
     T + "if (su.exynos) {\n"
     + T + T + "/* Slot 0 carries a SCSI command; keep the list running after errors. */\n"
     + T + T + "*(volatile uint32_t *)(su.vs + VS_UTRL_NEXUS) |= 1u;\n"
     + T + T + "if (!(ufs_rd(REG_UTRLRSR) & 1)) {\n"
     + T + T + T + "ufs_wr(REG_UTRLRSR, 1);\n"
     + T + T + "}\n"
     + T + "}\n"
     + T + "__builtin_arm_dsb(0xf);\n\n" + T + "ufs_wr(REG_UTRLDBR, 1);\n"),
    # timeout: Exynos clears slot n with 1 << n
    (T + T + T + "ufs_wr(REG_UTRLCLR, ~1u);\n",
     T + T + T + "ufs_wr(REG_UTRLCLR, su.exynos ? 1u : ~1u);     /* Exynos: BROKEN_REQ_LIST_CLR */\n"
     + T + T + T + "ufs_wr(REG_IS, ufs_rd(REG_IS));\n"),
    # OCS quirk
    (T + "uint32_t ocs = utrd->dw2 & 0xff;\n",
     T + "uint32_t ocs = utrd->dw2 & 0xff;\n"
     + T + "if (su.exynos && ocs == 7 && (ucd->resp[6] || ucd->resp[7])) {\n"
     + T + T + "ocs = 0;                        /* UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR */\n"
     + T + "}\n"),
    # failure message with LUN and sense
    (T + T + 'printf("SWEET UFS: command 0x%02x failed: ocs 0x%x upiu 0x%x response 0x%x status 0x%x' + NL + '",\n'
     + T + T + "    cdb[0], ocs, ucd->resp[0], ucd->resp[6], ucd->resp[7]);\n",
     T + T + "const uint8_t *sd = &ucd->resp[34];     /* fixed-format sense data */\n"
     + T + T + 'printf("SWEET UFS: command 0x%02x lun 0x%x failed: ocs 0x%x upiu 0x%x response 0x%x status 0x%x sense %x/%02x/%02x' + NL + '",\n'
     + T + T + "    cdb[0], su.lun, ocs, ucd->resp[0], ucd->resp[6], ucd->resp[7],\n"
     + T + T + "    sd[2] & 0xf, sd[12], sd[13]);\n"),
    # wake-up helper
    ("static int\nufs_take_over_host(void)\n{\n",
     "/*\n"
     " * Exynos: S-Boot leaves the UFS device in a low-power mode; every LU answers\n"
     " * NOT READY until the device W-LUN gets START STOP UNIT (POWER CONDITION =\n"
     " * Active), after a REQUEST SENSE that clears its unit attention.\n"
     " */\n"
     "static int\nufs_wake_device(void)\n{\n"
     + T + "static const uint8_t req_sense[6] = { 0x03, 0, 0, 0, 18, 0 };\n"
     + T + "static const uint8_t ssu_active[6] = { 0x1b, 0, 0, 0, 1u << 4, 0 };\n"
     + T + "uint8_t lun = su.lun;\n"
     + T + "int error = EIO;\n\n"
     + T + "su.lun = UFS_DEVICE_WLUN;\n"
     + T + "(void)ufs_scsi(req_sense, sizeof(req_sense), 18, false);\n"
     + T + "for (int i = 0; i < 3 && (error = ufs_scsi(ssu_active, sizeof(ssu_active), 0, false)); i++) {\n"
     + T + "}\n"
     + T + "su.lun = lun;\n"
     + T + 'printf("SWEET UFS: device W-LUN START STOP UNIT (active): %d' + NL + '", error);\n'
     + T + "return error;\n"
     "}\n\n"
     "static int\nufs_take_over_host(void)\n{\n"),
    # map the vendor block (second reg pair)
    (T + "su.regs = ml_io_map(reg[0], reg[1]);\n",
     T + "su.regs = ml_io_map(reg[0], reg[1]);\n"
     + T + "if (size >= 4 * sizeof(uintptr_t)) {\n"
     + T + T + "/*\n"
     + T + T + " * Second reg pair: Exynos vendor HCI (\"exynos-specific hci\",\n"
     + T + T + " * 0x13101100 on o1s, not page aligned): map its page, keep the offset.\n"
     + T + T + " */\n"
     + T + T + "vm_offset_t vs_page = reg[2] & ~(vm_offset_t)PAGE_MASK;\n"
     + T + T + "su.vs = ml_io_map(vs_page, round_page(reg[2] + reg[3]) - vs_page) + (reg[2] - vs_page);\n"
     + T + T + "su.exynos = true;\n"
     + T + "}\n"),
    # wake before the first LU command
    (T + "error = ufs_take_over_host();\n" + T + "if (!error) {\n",
     T + "error = ufs_take_over_host();\n"
     + T + "if (!error && su.exynos) {\n"
     + T + T + "error = ufs_wake_device();\n"
     + T + "}\n"
     + T + "if (!error) {\n"),
    # success message
    ('%s, DMA at 0x%llx' + NL + '",\n',
     '%s, DMA at 0x%llx%s' + NL + '",\n'),
    (T + '    su.writable ? "read-write" : "read-only", su.dma_pa);\n',
     T + '    su.writable ? "read-write" : "read-only", su.dma_pa, su.exynos ? " (Exynos)" : "");\n'),
]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = open(src, encoding="utf-8", newline="").read()
    for old, new in EDITS:
        n = s.count(old)
        if n != 1:
            sys.exit(f"expected 1 match, found {n}: {old[:80]!r}")
        s = s.replace(old, new)
    open(dst, "w", encoding="utf-8", newline="").write(s)
    print(f"{len(EDITS)} edits applied -> {dst}")


if __name__ == "__main__":
    main()
