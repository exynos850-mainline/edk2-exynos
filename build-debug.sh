#!/usr/bin/env bash
# Verbose uniLoader build with maximum bootloader printk verbosity and a saved log.
set -Eeuo pipefail
cd "$(dirname "$(readlink -f "$0")")"
: "${CROSS_COMPILE:=aarch64-linux-gnu-}"
: "${JOBS:=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
LOG_FILE="${LOG_FILE:-build-debug-$(date +%Y%m%d-%H%M%S).log}"
for f in blob/Image blob/dtb blob/ramdisk blob/logo.bmp; do
  [[ -s "$f" ]] || { echo "Missing required asset: $f" >&2; echo "Provide an A145F-compatible kernel as blob/Image, DTB as blob/dtb, and initramfs as blob/ramdisk." >&2; exit 2; }
done
make ARCH=aarch64 CROSS_COMPILE="$CROSS_COMPILE" a145f_defconfig
# Raise the bootloader's printk threshold to include KERN_DEBUG messages.
if grep -q '^CONFIG_LOGLEVEL=' .config; then
  sed -i 's/^CONFIG_LOGLEVEL=.*/CONFIG_LOGLEVEL=7/' .config
else
  printf '\nCONFIG_LOGLEVEL=7\n' >> .config
fi
make ARCH=aarch64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig
set -o pipefail
make -j"$JOBS" ARCH=aarch64 CROSS_COMPILE="$CROSS_COMPILE" V=1 2>&1 | tee "$LOG_FILE"
printf '\nDebug build finished. Full build log: %s\n' "$LOG_FILE"
