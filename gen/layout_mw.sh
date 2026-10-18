#!/bin/bash
# Compile the layout probes with Metrowerks (the PS2 compiler) in a decomp copy; keep each object's .data.
# usage (WSL): layout_mw.sh <decomp_copy> <work_dir>
set -uo pipefail
SRC=${1:?decomp copy}; WORK=${2:?work dir}
. ~/decomp/toolchain/env.sh
cd "$SRC"
n=0; fail=0
for c in "$WORK"/mw/*.c; do
  b=${c%.c}
  if nice -n 19 tools/cc.sh "$c" "$b.o" > "$b.log" 2>&1 && mipsel-none-elf-objcopy -O binary -j .data "$b.o" "$b.bin"; then
    n=$((n+1))
  else
    fail=$((fail+1)); echo "MW FAIL: $(basename "$c")"; tail -3 "$b.log"
  fi
done
echo "metrowerks probes: $n ok, $fail failed"
