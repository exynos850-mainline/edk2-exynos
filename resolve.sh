#!/bin/bash
# usage: ./resolve.sh <config> <offset>
#   e.g. ./resolve.sh DEBUG_GCC5 0xB5D34
CFG=${1:?config, e.g. DEBUG_GCC5}
OFF=${2:?offset, e.g. 0xB5D34}
D=workspace/Build/a145f/$CFG/AARCH64/src/main/SimpleInitMain/DEBUG
F=$D/SimpleInitMain.debug

echo "== function"
aarch64-linux-gnu-addr2line -f -i -C -e "$F" "$OFF"

START=$(printf "0x%x" $((OFF - 0x40)))
STOP=$(printf "0x%x" $((OFF + 0x20)))
echo "== disassembly $START .. $STOP"
aarch64-linux-gnu-objdump -d -C -l --no-show-raw-insn \
  --start-address=$START --stop-address=$STOP "$F"
