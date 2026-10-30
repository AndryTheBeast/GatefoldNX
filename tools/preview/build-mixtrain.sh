#!/usr/bin/env bash
# Gatefold NX: builds mixtrain.exe, training Automix on the user's ratings (see mixtrain.cpp).
# Usage: FFMPEG=tools/preview/deps/ffmpeg bash tools/preview/build-mixtrain.sh [<out-folder>]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SRC="$HERE/../../source"
FFMPEG="${FFMPEG:?an FFmpeg win64 shared build folder}"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DGF_FFMPEG -D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING -I"$SRC" -I"$FFMPEG/include")
clang++ "${FLAGS[@]}" "$HERE/mixtrain.cpp" "$SRC/mixan.cpp" "$SRC/mood.cpp" "$SRC/automix.cpp" "$SRC/loudness.cpp" "$SRC/ffdecoder.cpp" \
  -L"$FFMPEG/lib" -lavformat -lavcodec -lswresample -lavutil -Xlinker /subsystem:console -o "$OUT/mixtrain.exe"
cp "$FFMPEG/bin/"*.dll "$OUT/"
echo "built $OUT/mixtrain.exe"
