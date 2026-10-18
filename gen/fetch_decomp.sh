#!/bin/bash
# Take a pinned, read-only snapshot of the decomp for the port generator (run in WSL).
# usage: fetch_decomp.sh <decomp_copy> <dest_dir>
# The source must be a clean copy at the pinned commit (gen/DECOMP_COMMIT) with a built build/main.elf
# (make check done); the standing copy is ~/decomp/port_pin_3b81711. Nothing is written into the source.
# The snapshot is not in git: asm/matchings holds code bytes from the game's executable.
set -euo pipefail
SRC=${1:?decomp copy}
DST=${2:?dest}
. ~/decomp/toolchain/env.sh
cd "$SRC"
test -z "$(git status --porcelain -- src include config)" || { echo "decomp copy is dirty"; exit 1; }
WANT=$(tr -d '\r\n ' < "$(dirname "$0")/DECOMP_COMMIT")
HAVE=$(git rev-parse HEAD)
case "$HAVE" in "$WANT"*) ;; *) echo "decomp copy is at $HAVE, the port is pinned to $WANT"; exit 1;; esac
rm -rf "$DST"; mkdir -p "$DST/src" "$DST/asm/matchings"
cp src/*.c "$DST/src/"
cp -r include "$DST/include"
cp -r config "$DST/config"
for d in asm/matchings/*/; do
  case "$d" in */mod/) continue;; esac
  cp -r "$d" "$DST/asm/matchings/"
done
echo "$HAVE" > "$DST/COMMIT"
mkdir -p "$DST/.git" && echo "$HAVE" > "$DST/.git/HEAD"   # gfx/vu0/gen_vu0.py reads the commit id here
mipsel-none-elf-nm -n build/main.elf > "$DST/main.nm"
mipsel-none-elf-nm -n -S --defined-only build/main.elf | awk '$3 ~ /[Tt]/' > "$DST/main.funcs"
echo "snapshot of $HAVE -> $DST ($(ls "$DST/src" | wc -l) C files, $(find "$DST/asm" -name '*.s' | wc -l) asm functions)"
