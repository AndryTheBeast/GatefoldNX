#!/usr/bin/env bash
# Gatefold NX: builds bgjoin.exe, the background player's engine running an Automix join on the PC (see bgjoin.cpp).
# Usage: bash tools/bgtest/join/build.sh [<out-folder>]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SYS="$HERE/../../../sysmodule"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -DNOMINMAX -DGFBG_NO_CLIENT -D_USE_MATH_DEFINES
       -I"$HERE" -I"$HERE/../shim" -I"$SYS/include" -I"$SYS/source" -I"$SYS/lib/helix-aac" -I"$SYS/lib/single" -include "$HERE/pre.h"
       -Wno-deprecated-declarations)
SRC=("$HERE/bgjoin.cpp" "$SYS/source/engine.cpp" "$SYS/source/automix_shared.cpp" "$SYS/source/decoder.cpp" "$SYS/source/dec_flac.cpp"
     "$SYS/source/dec_mp3.cpp" "$SYS/source/dec_mp4.cpp" "$SYS/source/dec_wav.cpp")
OBJ=()
for c in "$SYS"/lib/helix-aac/*.c; do   # Helix (AAC) is C, built as the sysmodule builds it
  o="$OUT/$(basename "$c" .c).o"
  clang -O2 -DGF_HELIX_GENERIC -DUSE_DEFAULT_STDLIB -D_CRT_SECURE_NO_WARNINGS -I"$SYS/lib/helix-aac" -c "$c" -o "$o"
  OBJ+=("$o")
done
clang++ "${FLAGS[@]}" "${SRC[@]}" "${OBJ[@]}" -o "$OUT/bgjoin.exe"
echo "built $OUT/bgjoin.exe"
