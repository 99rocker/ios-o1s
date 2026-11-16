#!/usr/bin/env python3
"""Build the minimal Apple Device Tree for the Lenovo Tab P11 (TB-J606F) port.

This is the `sweet` (Redmi Note 10 Pro, SM7150) mkadt.py adapted for TB-J606F
(Qualcomm SM6115 / Snapdragon 662).  The GENERIC structure is unchanged; every
BOARD-SPECIFIC constant that differs between SM7150 and SM6115 has been pulled
into the `J606F` block below and set to a TODO sentinel.

Fill each TODO from a dump of YOUR tablet (run tools/collect-j606F-facts.sh in
recovery/fastboot, then read the values out of the tarball it produces).  Until
a value is filled, build() raises with the exact name and where to find it.

The iOS userland stays the iPhone X (iPhone10,3 / D22AP) IPSW, exactly as on
sweet -- that identity is NOT board-specific and must not change, or the stock
userland (MobileGestalt, SpringBoard artwork, activation path) breaks.

Usage:  mkadt_j606F.py OUT.adt [--dump] [--qemu] [--highmem]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # the upstream ios-sweet adt.py, unchanged

# --------------------------------------------------------------------------
# TODO sentinel: a board value that still has to be read off the real tablet.
# --------------------------------------------------------------------------
class TODO:
    def __init__(self, where):
        self.where = where

def need(name, val):
    if isinstance(val, TODO):
        raise SystemExit(
            f"mkadt_j606F: board value '{name}' is not filled in yet.\n"
            f"  -> {val.where}\n"
            f"  Dump it with tools/collect-j606F-facts.sh and edit the J606F block."
        )
    return val

# ==========================================================================
# J606F BOARD BLOCK  --  everything here is SM6115 / TB-J606F specific.
# Replace each TODO(...) with the real value from your device dump.
# ==========================================================================

# --- RAM: pick a base ABOVE every SM6115 XPU carve-out, like sweet does. -----
# sweet used low-bank 0xb0000000 + 0x4d000000, and --highmem = the whole upper
# bank 0x1_00000000 + 0xfcc00000.  SM6115's reserved-memory map is different.
# Source: reserved-memory/ and memory@ nodes in the FDT dump (see the script's
# `reserved-memory.txt`).  Choose a span that overlaps NO reserved region.
J_PHYS_BASE      = 0x80000000   # bank 0x80000000 + 0x80000000 (2 GiB), carve-out free
J_MEM_SIZE       = 0x80000000   # (all reserved-memory regions live below 0x80000000)
J_HIGH_PHYS_BASE = 0x100000000  # upper DDR bank 0x1_00000000 + 0xc0000000 (3 GiB), clean
J_HIGH_MEM_SIZE  = 0xc0000000   # from live /proc/device-tree/memory reg (6 GiB unit)

# --- GICv3.  SM6115 GICD/GICR physical bases. -------------------------------
# sweet: GICD 0x17a00000 (64K), GICR 0x17a60000 (1M).  SM6115 is usually in the
# same 0x17a00000 neighbourhood but VERIFY -- do not assume.
# Source: soc/interrupt-controller@... `reg` in the FDT dump (gic.txt).
J_GICD      = 0x0f200000   # interrupt-controller@f200000 reg[0] (live FDT)
J_GICD_SIZE = 0x10000
J_GICR      = 0x0f300000   # reg[2]; #redistributor-regions = 1
J_GICR_SIZE = 0x100000
J_VTIMER_INTID = 19   # reused from sweet (SM7150); VERIFY on first boot vs the panic log

# --- Framebuffer that Lenovo's ABL leaves running (continuous splash). -------
# sweet: 0x9c000000, 1080x2400x4.  On Qualcomm the FB lives in the
# `cont_splash` / `splash_region` reserved-memory region, and the panel is
# 1200x2000 on the P11.
# Source: reserved-memory/cont_splash_region `reg` (framebuffer.txt), and the
# panel timing in the display/dsi node.
J_FB        = 0x5c000000   # reserved-memory cont_splash_region@5c000000 (live FDT)
J_FB_SIZE   = 0xf00000     # 15 MiB splash carve-out (holds 1200x2000x4 = 9.6 MiB)
J_FB_W      = 1200         # confirmed: both panel timings = 0x4b0 x 0x7d0 = 1200 x 2000
J_FB_H      = 2000
J_FB_STRIDE = J_FB_W * 4   # 4800; XRGB8888 (confirm byte order on first paint)

# --- Internal UFS host (TB-J606F is UFS 2.1, so the sweet UFS driver reuses). -
# sweet: ufshc@1d84000.  SM6115's controller offset differs.
# Source: soc/ufshc@<addr> `reg` in the FDT dump (ufs.txt).  Put the SAME value
# in shim/board_j606F.h (UFS_BASE) so the shim probe and the ADT agree.
J_UFS       = 0x04804000   # ufshc@4804000 reg[0] (live FDT); phy at 0x4807000
J_UFS_SIZE  = 0x3000

# --- Timebase. 19.2 MHz is the Qualcomm standard; confirm in cpus.txt. -------
J_TIMEBASE  = 19200000

# --- CPU topology: Snapdragon 662 = 4x Cortex-A73 (gold, 2.0 GHz) + ----------
#     4x Cortex-A53 (silver, 1.8 GHz), one DynamIQ cluster, shared L3.
# Bring-up default mirrors sweet: list the 4 little (silver) cores FIRST so
# `cpus=4` runs only them as the safe initial prefix, then the 4 gold cores.
# The `reg` field is the REAL MPIDR (Aff1:Aff0) -- read each cpu@N node's `reg`
# from the FDT (cpus.txt); SM6115 may encode the core in Aff0, not Aff1 like
# sweet, so DO NOT copy sweet's `core << 8`.
# GRANULE CONSTRAINT (device-measured): the A53 cores report ID_AA64MMFR0.TGran16=0
# (no 16KB pages); only the A73 gold cores report TGran16=1. iOS/XNU need 16K, so
# XNU runs GOLD-ONLY. List the 4 gold cores (cpu@100-103); the shim must migrate
# to a gold core before enabling the MMU. (Set to the 8-core list only if a future
# kernel can tolerate a mixed-granule topology, which stock iOS cannot.)
J_CPU_MPIDR = [0x100, 0x101, 0x102, 0x103]   # A73 gold only (16K-capable)
J_CPU_SILVER_HZ = 1804800000   # A53 @ 1.8 GHz (not listed; parked)
J_CPU_GOLD_HZ   = 2000000000   # A73 @ 2.0 GHz
J_CPU_SILVER_N  = 0            # none of the listed cores are silver
J_CPU_COMPAT    = 'qcom,kryo260'   # SM6115 Kryo 260; sweet was kryo470

# --- Presented-to-iOS display canvas.  KEEP the iPhone X logical size so the
#     userland stays happy; the CPU present path scales the real panel into it.
#     (This is an Apple identity value, NOT a board value -- leave as-is.)
CANVAS_W, CANVAS_H = 1125, 2436     # iPhone X, unchanged from sweet

# ==========================================================================
# Below here is upstream-generic. Do not edit for the port except where it
# reads a J_* value above.
# ==========================================================================

PANIC_LOG_SIZE = 0x100000
SOC_BASE = 0x01000000
SWEET_ECID = 0x5357454554000001  # synthetic ECID; identity, keep as-is

# QEMU virt fallback (kernel-only testing; tools/qemu-sweet.py)
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False):
    if qemu:
        phys_base, mem_size = need('J_PHYS_BASE', J_PHYS_BASE), need('J_MEM_SIZE', J_MEM_SIZE)
    elif highmem:
        phys_base = need('J_HIGH_PHYS_BASE', J_HIGH_PHYS_BASE)
        mem_size = need('J_HIGH_MEM_SIZE', J_HIGH_MEM_SIZE)
    else:
        phys_base, mem_size = need('J_PHYS_BASE', J_PHYS_BASE), need('J_MEM_SIZE', J_MEM_SIZE)

    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    if qemu:
        gicd, gicr, timebase = QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE
        gicd_sz, gicr_sz = 0x10000, 0x100000
        vtimer = QEMU_VTIMER_INTID
    else:
        gicd = need('J_GICD', J_GICD); gicr = need('J_GICR', J_GICR)
        gicd_sz = need('J_GICD_SIZE', J_GICD_SIZE); gicr_sz = need('J_GICR_SIZE', J_GICR_SIZE)
        timebase, vtimer = J_TIMEBASE, J_VTIMER_INTID

    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('j606F-shim')),
        ('system-firmware-version', cstr('j606F-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),              # iPhone X class @3x; identity, keep
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),               # A11 chip-id the iPhone X userland expects
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SMP: silver (A53) cores first, then gold (A73). `reg` = real MPIDR.
    mpidrs = need('J_CPU_MPIDR', J_CPU_MPIDR)
    if not mpidrs:
        raise SystemExit('J_CPU_MPIDR must list at least the boot core MPIDR')
    cpu_nodes = []
    for core in range(1 if qemu else len(mpidrs)):
        is_gold = core >= J_CPU_SILVER_N
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(J_CPU_COMPAT) + cstr('ARM,v8')),
            ('reg', u32(mpidrs[core])),         # REAL MPIDR from FDT, not core<<8
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),           # homogeneous SMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(J_CPU_GOLD_HZ if is_gold else J_CPU_SILVER_HZ)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else vtimer)),
        ('reg', u64(gicd - SOC_BASE, gicd_sz, gicr - SOC_BASE, gicr_sz)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('j606F-io')),
        ('compatible', cstr('j606F-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('j606F,display')),
            ('canvas-width', u32(CANVAS_W)),
            ('canvas-height', u32(CANVAS_H)),
        ])])

    fbcon = node('j606F-fbcon', [
        ph(),
        ('reg', u64(need('J_FB', J_FB), need('J_FB_SIZE', J_FB_SIZE))),
        ('width', u32(J_FB_W)),
        ('height', u32(J_FB_H)),
        ('stride', u32(need('J_FB_STRIDE', J_FB_STRIDE))),
    ])

    ufs = node('j606F-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(need('J_UFS', J_UFS), need('J_UFS_SIZE', J_UFS_SIZE))),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('lenovo,j606F') + cstr('qcom,sm6115') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
