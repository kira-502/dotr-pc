#!/bin/bash
# Take a pinned, read-only snapshot of the decomp for the port generator (run in WSL).
# usage: fetch_decomp.sh <decomp_copy> <dest_dir>
# The source must be a clean copy at the pinned commit (gen/DECOMP_COMMIT) with a built build/main.elf
# (make check done); the standing copy is ~/decomp/port_pin_32a9475b. Nothing is written into the source.
# The snapshot is not in git: asm/matchings holds code bytes from the game's executable.
set -euo pipefail
SRC=${1:?decomp copy}
DST=${2:?dest}
. ~/decomp/toolchain/env.sh
cd "$SRC"
test -z "$(git status --porcelain -- src include config ':!src/mod')" || { echo "decomp copy is dirty"; exit 1; }
WANT=$(tr -d '\r\n ' < "$(dirname "$0")/DECOMP_COMMIT")
HAVE=$(git rev-parse HEAD)
case "$HAVE" in "$WANT"*) ;; *) echo "decomp copy is at $HAVE, the port is pinned to $WANT"; exit 1;; esac
rm -rf "$DST"; mkdir -p "$DST/src" "$DST/asm/matchings"
# the main program's C, flat: src/<area>/<file>.c -> <area>_<file>.c (the modules, src/mod, are lane R's)
for f in $(find src -name '*.c' -not -path 'src/mod/*' | sort); do
  n=${f#src/}; cp "$f" "$DST/src/${n//\//_}"
done
cp -r include "$DST/include"
cp -r config "$DST/config"
for d in asm/matchings/*/; do
  case "$d" in */mod/) continue;; esac
  cp -r "$d" "$DST/asm/matchings/"
done
echo "$HAVE" > "$DST/COMMIT"
mkdir -p "$DST/.git" && echo "$HAVE" > "$DST/.git/HEAD"   # gfx/vu0/gen_vu0.py reads the commit id here
# config/ghidra_names.txt adds batzpup's names as linker aliases of ours (same address): the port keeps one name per
# function, the one the C defines, so the aliases are left out of the symbol lists
awk '/^[A-Za-z_][A-Za-z0-9_]* = [A-Za-z_][A-Za-z0-9_]*;/ { print $1 }' config/ghidra_names.txt > "$DST/ghidra_aliases.txt"
mipsel-none-elf-nm -n build/main.elf | awk 'NR==FNR { a[$1] = 1; next } !($3 in a)' "$DST/ghidra_aliases.txt" - > "$DST/main.nm"
mipsel-none-elf-nm -n -S --defined-only build/main.elf |
  awk 'NR==FNR { a[$1] = 1; next } $3 ~ /[Tt]/ && !($4 in a)' "$DST/ghidra_aliases.txt" - > "$DST/main.funcs"
echo "snapshot of $HAVE -> $DST ($(ls "$DST/src" | wc -l) C files, $(find "$DST/asm" -name '*.s' | wc -l) asm functions)"
