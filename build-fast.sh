#!/usr/bin/env bash
# Fast, quiet uniLoader build for Samsung Galaxy A14 4G (SM-A145F).
set -Eeuo pipefail
cd "$(dirname "$(readlink -f "$0")")"
: "${CROSS_COMPILE:=aarch64-linux-gnu-}"
: "${JOBS:=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
for f in blob/Image blob/dtb blob/ramdisk blob/logo.bmp; do
  [[ -s "$f" ]] || { echo "Missing required asset: $f" >&2; echo "Provide an A145F-compatible kernel as blob/Image, DTB as blob/dtb, and initramfs as blob/ramdisk." >&2; exit 2; }
done
make ARCH=aarch64 CROSS_COMPILE="$CROSS_COMPILE" a145f_defconfig
# Quiet boot-time logging in the fast profile; the debug profile uses level 7.
if grep -q '^CONFIG_LOGLEVEL=' .config; then
  sed -i 's/^CONFIG_LOGLEVEL=.*/CONFIG_LOGLEVEL=4/' .config
else
  printf '\nCONFIG_LOGLEVEL=4\n' >> .config
fi
make ARCH=aarch64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig
make -j"$JOBS" ARCH=aarch64 CROSS_COMPILE="$CROSS_COMPILE" V=0
printf '\nFast build finished. Check the Make output above for the generated uniLoader target.\n'
