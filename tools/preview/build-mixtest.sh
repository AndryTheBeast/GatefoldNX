#!/usr/bin/env bash
# Gatefold NX: builds tools/preview/out/mixtest.exe, Automix's analysis on the PC (see mixtest.cpp).
# Usage: FFMPEG=tools/preview/deps/ffmpeg bash tools/preview/build-mixtest.sh [<out-folder>]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SRC="$HERE/../../source"
FFMPEG="${FFMPEG:?an FFmpeg win64 shared build folder}"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DGF_FFMPEG -I"$SRC" -I"$FFMPEG/include")
clang++ "${FLAGS[@]}" "$HERE/mixtest.cpp" "$SRC/mixan.cpp" "$SRC/automix.cpp" "$SRC/loudness.cpp" "$SRC/ffdecoder.cpp" \
  -L"$FFMPEG/lib" -lavformat -lavcodec -lswresample -lavutil -Xlinker /subsystem:console -o "$OUT/mixtest.exe"
cp "$FFMPEG/bin/"*.dll "$OUT/"
echo "built $OUT/mixtest.exe"
