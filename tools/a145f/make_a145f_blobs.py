#!/usr/bin/env python3
"""Build uniLoader's blob/Image, blob/ramdisk and blob/dtb for the SM-A145F
from the stock images in your own backup.

  python3 make_a145f_blobs.py --boot boot.img --vendor-boot vendor_boot.img \
      --init-boot init_boot.img --dtbo dtbo.img --getprop getprop.txt \
      --out /path/to/uniloader/blob

Needs `fdtoverlay` and `fdtput` (sudo apt install device-tree-compiler).
Images with the stray 'dd' text appended at the end are fine; sizes come from
the headers.  Nothing identifying (serial numbers etc.) is copied from getprop.
"""
import argparse, os, re, struct, subprocess, sys, tempfile

def al(x, p): return (x + p - 1) // p * p
def die(m): sys.exit("error: " + m)

# ---- kernel from boot.img (header v4) --------------------------------------
def kernel(path):
    d = open(path, "rb").read()
    if d[:8] != b"ANDROID!": die(f"{path}: not an Android boot image")
    ks, _rs = struct.unpack_from("<2I", d, 8)
    if struct.unpack_from("<I", d, 40)[0] != 4: die("boot.img is not header v4")
    k = d[4096:4096 + ks]
    if k[56:60] != b"ARMd": die("kernel in boot.img is not an arm64 Image")
    return k

# ---- vendor_boot v4 --------------------------------------------------------
def vendor_boot(path):
    d = open(path, "rb").read()
    if d[:8] != b"VNDRBOOT": die(f"{path}: not a vendor_boot image")
    ver, page, _ka, _ra, vrs = struct.unpack_from("<5I", d, 8)
    if ver != 4: die("vendor_boot is not header v4")
    off = 28 + 2048 + 20
    hsz, dtbsz = struct.unpack_from("<2I", d, off); off += 16
    tsz, _tn, _te, bcsz = struct.unpack_from("<4I", d, off)
    ro = al(hsz, page); do = ro + al(vrs, page); to = do + al(dtbsz, page)
    bo = to + al(tsz, page)
    return d[ro:ro + vrs], d[do:do + dtbsz], d[bo:bo + bcsz].decode()

def init_boot_ramdisk(path):
    d = open(path, "rb").read()
    if d[:8] != b"ANDROID!": die(f"{path}: not an Android boot image")
    _ks, rs = struct.unpack_from("<2I", d, 8)
    return d[4096:4096 + rs]

# ---- bootconfig ------------------------------------------------------------
KEEP = ["boot_devices", "hardware", "hardware.cpu.pagesize", "hardware.sku",
        "product.hardware.sku", "revision", "dtbo_idx", "dynamic_partitions",
        "bootloader", "ddr_size", "dram_info", "carrierid", "subpcb", "svb.ver",
        "em.model", "sec_atd.tty", "ucs_mode"]

def bootconfig(vendor_text, getprop):
    lines = [l for l in vendor_text.replace("\0", "").splitlines() if l.strip()]
    props = dict(re.findall(r"\[ro\.boot\.([^\]]+)\]: \[([^\]]*)\]", getprop))
    for k in KEEP:
        if k in props: lines.append(f'androidboot.{k} = "{props[k]}"')
    # We replace the stock boot chain, so say so honestly.
    lines += ['androidboot.verifiedbootstate = "orange"',
              'androidboot.vbmeta.device_state = "unlocked"',
              'androidboot.flash.locked = "0"']
    data = ("\n".join(lines) + "\n").encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    csum = sum(data) & 0xFFFFFFFF
    return data + struct.pack("<II", len(data), csum) + b"#BOOTCONFIG\n"

# ---- dtb: base from vendor_boot + overlay from dtbo.img ---------------------
def overlay(dtbo_path, idx):
    d = open(dtbo_path, "rb").read()
    magic, _t, _h, esz, cnt, eoff, _p, _v = struct.unpack(">8I", d[:32])
    if magic != 0xD7B7AB1E: die("dtbo.img: bad magic")
    if idx >= cnt: die(f"dtbo.img has {cnt} entries, wanted {idx}")
    sz, off = struct.unpack(">2I", d[eoff + idx * esz: eoff + idx * esz + 8])
    return d[off:off + sz]

RAMOOPS_ADDR, RAMOOPS_SIZE = 0x8fe00000, 0x100000   # free DRAM; 0x8ffff000 is the bootloader's kaslr node

def add_ramoops(dtb):
    """Reserve 1 MiB for pstore/ramoops so the kernel console survives a reset.
    The layout (console zone at +0x80000) is what board-a145f.c reads back."""
    n = f"/reserved-memory/ramoops@{RAMOOPS_ADDR:x}"
    run = lambda *a: subprocess.run(["fdtput", *a], check=True)
    run("-c", dtb, n)
    run("-t", "s", dtb, n, "compatible", "ramoops")
    # reserved-memory uses 2 address cells + 1 size cell on this SoC
    run("-t", "x", dtb, n, "reg", "0", f"{RAMOOPS_ADDR:x}", f"{RAMOOPS_SIZE:x}")
    for k, v in (("record-size", 0x20000), ("console-size", 0x80000),
                 ("ftrace-size", 0), ("pmsg-size", 0)):
        run("-t", "x", dtb, n, k, f"{v:x}")

def check_ramoops_overlap(dtb_path):
    """Warn if a reserved region, or the bootloader's kaslr node, overlaps the ramoops area."""
    dts = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", dtb_path],
                         capture_output=True, text=True).stdout
    lo, hi = RAMOOPS_ADDR, RAMOOPS_ADDR + RAMOOPS_SIZE
    blocks = []
    m = re.search(r"reserved-memory \{(.*?)\n\t\};", dts, re.S)   # not the /memory nodes
    if m: blocks.append(m.group(1))
    m = re.search(r"\n\tkaslr \{(.*?)\n\t\};", dts, re.S)
    if m: blocks.append(m.group(1))
    for blk in blocks:
        for hi_a, lo_a, sz in re.findall(r"reg = <0x([0-9a-f]+) 0x([0-9a-f]+) 0x([0-9a-f]+)>;", blk):
            a = (int(hi_a, 16) << 32) | int(lo_a, 16); e = a + int(sz, 16)
            if int(sz, 16) and a < hi and e > lo:
                print(f"warning: a region at {a:#x}-{e:#x} overlaps the ramoops area {lo:#x}-{hi:#x}")

def build_dtb(vb_dtb, dtbo_path, idx, getprop, live_dtb=None):
    with tempfile.TemporaryDirectory() as t:
        b, o, m = (os.path.join(t, n) for n in ("base", "ov", "merged"))
        if live_dtb:
            # The tree Samsung's bootloader produced (adb: cat /sys/firmware/fdt on
            # stock): already merged with the overlay and patched with the real
            # reserved-memory addresses, memory size and board fields.
            open(m, "wb").write(open(live_dtb, "rb").read())
        else:
            print("note: static tree. It claims 0xbba00000-0xbfffffff and 0xe0000000-0xe0cfffff "
                  "as RAM (TrustZone-protected on this phone); use --live-dtb to avoid a TZASC panic")
            i = vb_dtb.find(b"\xd0\x0d\xfe\xed")
            if i < 0: die("vendor_boot dtb section has no device tree")
            base = vb_dtb[i:i + struct.unpack(">I", vb_dtb[i + 4:i + 8])[0]]
            open(b, "wb").write(base); open(o, "wb").write(overlay(dtbo_path, idx))
            subprocess.run(["fdtoverlay", "-i", b, "-o", m, o], check=True)
        args = subprocess.run(["fdtget", m, "/chosen", "bootargs"],
                              capture_output=True, text=True, check=True).stdout.strip()
        props = dict(re.findall(r"\[ro\.boot\.([^\]]+)\]: \[([^\]]*)\]", getprop))
        extra = ["bootconfig", "root=/dev/ram0", "lcdtype=5993024",
                 "mcd-panel.boot_panel_id=5993024", "s3cfb.bootloaderfb=0xfa000000",
                 "blic_type=-1", "pmic_info=27", "ccic_info=1", "charging_mode=0x0",
                 "factory_mode=0", "consoleblank=0",
                 # bring-up: log everything, uncompressed, into the ramoops area
                 "ignore_loglevel", "pstore.compress=none"]
        have = {w.split("=")[0] for w in args.split()}
        extra = [w for w in extra if w.split("=")[0] not in have]   # no duplicates
        subprocess.run(["fdtput", "-t", "s", m, "/chosen", "bootargs",
                        (args + " " + " ".join(extra)).strip()], check=True)
        check_ramoops_overlap(m)      # before adding our own node
        add_ramoops(m)
        return open(m, "rb").read()

def main():
    a = argparse.ArgumentParser()
    for n in ("boot", "vendor-boot", "init-boot", "dtbo", "getprop", "out"):
        a.add_argument("--" + n, required=True)
    a.add_argument("--dtbo-idx", type=int, help="default: ro.boot.dtbo_idx from getprop")
    a.add_argument("--live-dtb", help="device tree dumped from stock Android "
                   "(/sys/firmware/fdt); used instead of vendor_boot base + dtbo")
    o = a.parse_args()
    missing = [f"--{n.replace('_','-')} {getattr(o, n)}" for n in
               ("boot", "vendor_boot", "init_boot", "dtbo", "getprop")
               if not os.path.isfile(getattr(o, n))]
    if missing:
        die("file not found: " + ", ".join(missing) + "\n  (backup layout: <backup>/partitions/*.img "
            "and <backup>/info/getprop.txt - pass full paths)")
    gp = open(o.getprop, errors="replace").read()
    idx = o.dtbo_idx if o.dtbo_idx is not None else int(
        re.search(r"\[ro\.boot\.dtbo_idx\]: \[(\d+)\]", gp).group(1))
    vr, vdtb, vtext = vendor_boot(o.vendor_boot)
    os.makedirs(o.out, exist_ok=True)
    k = kernel(o.boot)
    rd = vr + init_boot_ramdisk(o.init_boot) + bootconfig(vtext, gp)
    dtb = build_dtb(vdtb, o.dtbo, idx, gp, o.live_dtb)
    for name, blob in (("Image", k), ("ramdisk", rd), ("dtb", dtb)):
        open(os.path.join(o.out, name), "wb").write(blob)
        print(f"{name:8s} {len(blob):>10d} bytes")
    print("dtb = live dtb " + o.live_dtb if o.live_dtb else f"dtb = vendor_boot base + dtbo entry {idx}")

if __name__ == "__main__":
    main()
