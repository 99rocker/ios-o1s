#!/usr/bin/env python3
"""Collect the boot facts an XNU port to the Xiaomi Mi 11T Pro (vili, SM8350) needs.

READ-ONLY. Nothing is written to the phone. Designed to run with the phone in
a custom recovery (OrangeFox/TWRP) over adb, which already has root; it also
works on a rooted booted system.

  python vili_facts.py collect [OUTDIR]
  python vili_facts.py bootimg boot.img
  python vili_facts.py fdt file.dtb

The S21's collector (s21_facts.py) adapted for Qualcomm: Xiaomi 'vili' model
check, Qualcomm display/splash/SMMU/SPMI device-tree keywords, and the
bootdevice by-name partition path.
"""
import os
import struct
import subprocess
import sys


def adb(*args, binary=False):
    r = subprocess.run(["adb", *args], capture_output=True)
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


def sh(cmd):
    return adb("shell", cmd).strip()


def su_bin(cmd):
    """Recovery adb is already root; exec-out keeps binary output intact."""
    return adb("exec-out", cmd, binary=True)


SECRET_KEYS = ("serial", "imei", "meid", "did", "ucs", "uid", "mac", "wifimac", "btmac",
               "hw_id", "cpuid", "unique", "sn=", "csc_id", "carrierid", "boot_recovery")


def redact(text):
    words = []
    for w in text.split():
        k = w.split("=", 1)[0].lower()
        if "=" in w and any(s.rstrip("=") in k for s in SECRET_KEYS):
            w = w.split("=", 1)[0] + "=<redacted>"
        words.append(w)
    return " ".join(words)


FDT_MAGIC = 0xD00DFEED


def parse_fdt(blob):
    magic, totalsize, off_struct, off_strings = struct.unpack_from(">IIII", blob, 0)
    if magic != FDT_MAGIC:
        raise ValueError("not a DTB (bad magic)")
    strings = blob[off_strings:]
    pos, stack, nodes = off_struct, [], []
    while True:
        (tok,) = struct.unpack_from(">I", blob, pos)
        pos += 4
        if tok == 1:
            end = blob.index(b"\0", pos)
            name = blob[pos:end].decode("ascii", "replace")
            pos = (end + 4) & ~3
            path = "/" if not stack else (stack[-1][0].rstrip("/") + "/" + name)
            props = {}
            stack.append((path, props))
            nodes.append((path, props))
        elif tok == 2:
            stack.pop()
        elif tok == 3:
            ln, nameoff = struct.unpack_from(">II", blob, pos)
            pos += 8
            pname = strings[nameoff:strings.index(b"\0", nameoff)].decode("ascii", "replace")
            stack[-1][1][pname] = blob[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            return nodes
        else:
            raise ValueError(f"bad FDT token {tok:#x} at {pos - 4:#x}")


def find_dtbs(blob):
    i = 0
    magic = struct.pack(">I", FDT_MAGIC)
    while True:
        i = blob.find(magic, i)
        if i < 0:
            return
        if i + 8 <= len(blob):
            (size,) = struct.unpack_from(">I", blob, i + 4)
            if 64 < size <= len(blob) - i:
                yield i, blob[i:i + size]
        i += 4


def _cells(v):
    return [struct.unpack_from(">I", v, k)[0] for k in range(0, len(v) - len(v) % 4, 4)]


def _str(v):
    return v.rstrip(b"\0").replace(b"\0", b" | ").decode("ascii", "replace")


# Qualcomm bring-up nodes: cont_splash (bootloader framebuffer), mdss/sde (display),
# memory, timer, chosen, psci, GIC, SPMI/PMIC, UFS.
INTERESTING = ("framebuffer", "splash", "cont_splash", "mdss", "sde", "display", "dsi",
               "memory", "timer", "chosen", "psci", "interrupt-controller", "spmi",
               "ufshc", "qcom,gcc", "cpus", "reserved-memory")


def summarize_fdt(blob, out):
    nodes = parse_fdt(blob)
    model = next((p.get("model") for path, p in nodes if path == "/"), None)
    out(f"  model: {_str(model) if model else '?'}  ({len(nodes)} nodes)")
    for path, props in nodes:
        leaf = path.rsplit("/", 1)[-1].lower()
        compat = _str(props.get("compatible", b"")).lower()
        hit = any(k in leaf for k in INTERESTING) or "framebuffer" in compat or "timer" in compat
        if not hit or path.count("/") > 3:
            continue
        line = f"  {path}"
        if "compatible" in props:
            line += f"  compat={_str(props['compatible'])}"
        if "reg" in props:
            line += "  reg=" + " ".join(f"{c:#x}" for c in _cells(props["reg"])[:12])
        for k in ("clock-frequency", "width", "height", "stride", "format", "bootargs",
                  "linux,initrd-start", "use-physical-timer", "no-map"):
            if k in props:
                v = props[k]
                shown = redact(_str(v)) if k in ("format", "bootargs") else (" ".join(f"{c:#x}" for c in _cells(v)) or "true")
                line += f"  {k}={shown}"
        out(line)


def inspect_bootimg(blob, out):
    if blob[:4] == b"\x04\x22\x4d\x18":
        out("  This file is LZ4-compressed; decompress first.")
        return
    if blob[:8] != b"ANDROID!":
        out("  No ANDROID! magic: not a raw Android boot image.")
        return
    (hv,) = struct.unpack_from("<I", blob, 40)
    out(f"  header_version: {hv}")
    if hv >= 3:
        ksz, rsz, osv, hsz = struct.unpack_from("<IIII", blob, 8)
        cmdline = blob[44:44 + 1536].split(b"\0")[0].decode("ascii", "replace")
        out(f"  page_size: 4096 (v3+)  kernel={ksz:#x}  ramdisk={rsz:#x}  header_size={hsz}")
        out("  NOTE: v3+ boot images keep the DTB in vendor_boot, not boot.")
    else:
        ksz, kaddr, rsz, raddr, ssz, saddr, tags, page = struct.unpack_from("<8I", blob, 8)
        name = blob[48:64].split(b"\0")[0].decode("ascii", "replace")
        cmdline = blob[64:64 + 512].split(b"\0")[0].decode("ascii", "replace")
        extra = blob[608:608 + 1024].split(b"\0")[0].decode("ascii", "replace")
        cmdline = (cmdline + " " + extra).strip()
        out(f"  page_size: {page:#x}  name: {name!r}")
        out(f"  kernel: size={ksz:#x} load={kaddr:#x}")
        out(f"  ramdisk: size={rsz:#x} load={raddr:#x}  tags={tags:#x}")
        if hv == 2:
            dtb_sz, dtb_addr = struct.unpack_from("<IQ", blob, 1648)
            out(f"  dtb: size={dtb_sz:#x} load={dtb_addr:#x}")
    out(f"  cmdline: {redact(cmdline) or '(empty)'}")


def report_dtbs(blob, out, label):
    found = list(find_dtbs(blob))
    out(f"  embedded DTBs in {label}: {len(found)}")
    for off, d in found[:8]:
        out(f"  --- DTB at offset {off:#x} ({len(d)} bytes)")
        try:
            summarize_fdt(d, out)
        except Exception as e:  # noqa: BLE001
            out(f"  (could not parse: {e})")


PROPS = ["ro.product.model", "ro.product.device", "ro.boot.hardware", "ro.hardware",
         "ro.bootloader", "ro.build.display.id", "ro.build.version.release",
         "ro.boot.verifiedbootstate", "ro.boot.flash.locked", "ro.boot.slot_suffix",
         "ro.boot.product.hardware.sku", "ro.product.first_api_level", "ro.boot.dtbo_idx"]

PARTITIONS = ["boot", "vendor_boot", "dtbo", "vbmeta"]  # read-only copies only


def collect(outdir):
    os.makedirs(outdir, exist_ok=True)
    report = []

    def out(s=""):
        print(s)
        report.append(s)

    devs = [l for l in adb("devices").splitlines()[1:] if l.strip().split("\t")[-1] in ("device", "recovery")]
    if not devs:
        print("No device. Put the phone in recovery (OrangeFox/TWRP) with USB debugging, "
              "then check `adb devices`.")
        sys.exit(1)

    out("== Device")
    for p in PROPS:
        v = sh(f"getprop {p}")
        if v:
            out(f"  {p} = {v}")
    device = sh("getprop ro.product.device")
    if device != "vili":
        out(f"  WARNING: device {device!r} is not 'vili' (Mi 11T Pro). Facts may not apply.")

    out("\n== CPU")
    cpuinfo = su_bin("cat /proc/cpuinfo").decode("utf-8", "replace")
    open(os.path.join(outdir, "cpuinfo.txt"), "w", encoding="utf-8").write(cpuinfo)
    parts = sorted({l.split(":")[1].strip() for l in cpuinfo.splitlines() if l.startswith("CPU part")})
    out(f"  CPU part ids: {', '.join(parts)}  (0xd05=A55, 0xd41=A78, 0xd44=X1)")

    out("\n== Partitions (names + sizes)")
    for base in ("/dev/block/by-name", "/dev/block/bootdevice/by-name",
                 "/dev/block/platform/soc/1d84000.ufshc/by-name"):
        listing = sh(f"ls -l {base} 2>/dev/null")
        if listing and "No such" not in listing:
            open(os.path.join(outdir, "partitions.txt"), "w", encoding="utf-8").write(listing)
            out(f"  (from {base})")
            # sizes of the interesting ones
            for p in ("boot", "vendor_boot", "dtbo", "vbmeta", "super", "userdata", "metadata"):
                sz = sh(f"blockdev --getsize64 {base}/{p} 2>/dev/null")
                if sz.isdigit():
                    out(f"  {p:12s} {int(sz):>14d} bytes ({int(sz)//(1024*1024)} MiB)")
            break

    out("\n== Kernel cmdline (bootloader handover to recovery)")
    cmdline = redact(su_bin("cat /proc/cmdline").decode("utf-8", "replace").strip())
    open(os.path.join(outdir, "cmdline.txt"), "w", encoding="utf-8").write(cmdline)
    out("  " + cmdline)

    out("\n== dmesg: timer / framebuffer / splash / psci / GIC / UFS")
    dmesg = su_bin("dmesg").decode("utf-8", "replace")
    open(os.path.join(outdir, "dmesg.txt"), "w", encoding="utf-8").write(dmesg)
    keys = ("arch_timer", "cntfrq", "cont_splash", "splash", "framebuffer", "simplefb",
            "sde", "mdss", "Reserved memory", "psci", "GICv3", "gic", "ufshcd", "CPU features")
    for l in dmesg.splitlines():
        if any(k.lower() in l.lower() for k in keys):
            out("  " + redact(l.strip()))

    out("\n== /proc/iomem (RAM + reserved; cont_splash = bootloader framebuffer)")
    iomem = su_bin("cat /proc/iomem").decode("utf-8", "replace")
    open(os.path.join(outdir, "iomem.txt"), "w", encoding="utf-8").write(iomem)
    for l in iomem.splitlines():
        if not l.startswith(" ") or any(k in l.lower() for k in ("reserved", "splash", "mdss", "sde")):
            out("  " + l.rstrip())

    out("\n== Live device tree (/sys/firmware/fdt)")
    fdt = su_bin("cat /sys/firmware/fdt")
    if fdt[:4] == struct.pack(">I", FDT_MAGIC):
        open(os.path.join(outdir, "live.dtb"), "wb").write(fdt)
        summarize_fdt(fdt, out)
    else:
        out("  (could not read /sys/firmware/fdt)")

    out("\n== Partition copies (read-only dd)")
    base = None
    for b in ("/dev/block/by-name", "/dev/block/bootdevice/by-name"):
        if "No such" not in sh(f"ls {b}/boot 2>&1"):
            base = b
            break
    slot = sh("getprop ro.boot.slot_suffix")
    for name in PARTITIONS:
        src = f"{base}/{name}{slot}"
        if "No such" in sh(f"ls {src} 2>&1"):
            src = f"{base}/{name}"
        data = su_bin(f"dd if={src} bs=1M 2>/dev/null")
        if len(data) > 4096:
            open(os.path.join(outdir, f"{name}.img"), "wb").write(data)
            out(f"  saved {name}.img ({len(data)} bytes) from {src}")

    bp = os.path.join(outdir, "boot.img")
    if os.path.exists(bp):
        out("\n== boot.img")
        blob = open(bp, "rb").read()
        inspect_bootimg(blob, out)
        report_dtbs(blob, out, "boot")
    vp = os.path.join(outdir, "vendor_boot.img")
    if os.path.exists(vp):
        out("\n== vendor_boot (holds the DTB on v3/v4)")
        vb = open(vp, "rb").read()
        if vb[:8] == b"VNDRBOOT":
            hv, page = struct.unpack_from("<II", vb, 8)
            out(f"  vendor_boot header_version={hv} page_size={page:#x}")
        report_dtbs(vb, out, "vendor_boot")

    open(os.path.join(outdir, "REPORT.txt"), "w", encoding="utf-8").write("\n".join(report) + "\n")
    print(f"\nSaved to {outdir}/ . REPORT.txt has no serials or IMEIs.")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("collect", "bootimg", "fdt"):
        print(__doc__)
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "collect":
        collect(sys.argv[2] if len(sys.argv) > 2 else "mi11-facts-out")
    elif cmd == "bootimg":
        blob = open(sys.argv[2], "rb").read()
        inspect_bootimg(blob, print)
        report_dtbs(blob, print, os.path.basename(sys.argv[2]))
    else:
        blob = open(sys.argv[2], "rb").read()
        report_dtbs(blob, print, os.path.basename(sys.argv[2]))


if __name__ == "__main__":
    main()
