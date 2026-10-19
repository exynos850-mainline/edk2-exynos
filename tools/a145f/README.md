# SM-A145F (Galaxy A14 4G) UEFI

1. Build/prepare uniLoader's inputs (from uniLoader-a145f-bootloader/Documentation/samsung-a145f.md):
   back up the phone, capture the live DTB, then generate `blob/dtb`:
   `python3 tools/a145f/make_a145f_blobs.py --boot ... --vendor-boot ... --init-boot ... --dtbo ... --getprop ... --live-dtb live-dtb/stock_live.dtb --out <uniLoader>/blob`
   (only `blob/dtb` is used; the Image/ramdisk are replaced by UEFI and a dummy ramdisk).
2. `export UNILOADER_DIR=/path/to/uniLoader-a145f-bootloader STOCK_BOOT_IMG=/path/to/stock/boot.img`
3. `./build.sh -d a145f --skip-rootfs-gen` (or without it for SimpleInit)
4. Flash `workspace/boot-a145f.tar` in Odin's AP slot. Keep `AP_stock.tar` ready to restore.
