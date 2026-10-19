# Galaxy A14 4G (SM-A145F) boot chain:
#   Samsung bootloader -> boot.img (header v4, kernel = uniLoader)
#     -> uniLoader copies its payload (BootShim + UEFI FD) to 0x80200000 and
#        jumps there with the DTB in x0 -> BootShim relocates UEFI to FD_BASE.
# Samsung's v4 boot.img cannot carry a DTB/ramdisk, so unlike the a13 the boot
# image is produced by uniLoader. Set:
#   UNILOADER_DIR   uniLoader-a145f-bootloader checkout (default ../uniLoader-a145f-bootloader)
#   STOCK_BOOT_IMG  your stock boot.img (os_version is copied from it)
# and prepare $UNILOADER_DIR/blob/dtb (see tools/a145f/README.md).

function platform_build_bootimg(){
	local UNI="${UNILOADER_DIR:-${ROOTDIR}/../uniLoader-a145f-bootloader}"
	local OUT="${WORKSPACE}/boot-${DEVICE}${EXT}"

	[ -d "${UNI}" ]          || { echo "UNILOADER_DIR not found: ${UNI}" >&2; return 1; }
	[ -f "${STOCK_BOOT_IMG}" ] || { echo "Set STOCK_BOOT_IMG to your stock boot.img" >&2; return 1; }
	[ -f "${UNI}/blob/dtb" ] || { echo "${UNI}/blob/dtb missing, see tools/a145f/README.md" >&2; return 1; }

	cp "${WORKSPACE}/uefi-${DEVICE}-kernel" "${UNI}/blob/Image"        || return 1
	# UEFI needs no ramdisk; keep uniLoader's embed step happy with a dummy one
	[ -f "${UNI}/blob/ramdisk" ] || cp "${ROOTDIR}/Platform/Samsung/exynos850/dummyramdisk" "${UNI}/blob/ramdisk"

	make -C "${UNI}" a145f_defconfig CROSS_COMPILE="${CROSS_COMPILE}" || return 1
	make -C "${UNI}" -j"$(nproc)" CROSS_COMPILE="${CROSS_COMPILE}"    || return 1

	python3 "${ROOTDIR}/tools/a145f/pack_boot_v4.py" "${UNI}/uniLoader" \
		--stock "${STOCK_BOOT_IMG}" -o "${OUT}.img"                    || return 1

	# Odin picks the partition from the file name inside the tar: must be boot.img
	mkdir -p "${WORKSPACE}/odin"
	cp "${OUT}.img" "${WORKSPACE}/odin/boot.img"
	( cd "${WORKSPACE}/odin" && tar -H ustar -cf "${OUT}.tar" boot.img ) || return 1
	cp "${OUT}.img" "${OUTDIR}/boot-${DEVICE}${EXT}.img"
	echo "Odin AP file: ${OUT}.tar"
}
