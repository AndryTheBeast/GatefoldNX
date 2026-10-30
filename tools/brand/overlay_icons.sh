#!/usr/bin/env bash
# Songate: overlay/source/pl_symbols.hpp, the app's playlist icons for the overlay (see overlay_icons.cpp).
# Usage: bash tools/brand/overlay_icons.sh   (uses the preview's SDL: tools/preview/deps/sdl and tools/preview/out's DLLs)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
SDL="$ROOT/tools/preview/deps/sdl"
OUT="$ROOT/tools/preview/out"
clang++ -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -D__PRFCHWINTRIN_H -I"$OUT/include" -I"$ROOT/source" "$HERE/overlay_icons.cpp" \
  -L"$SDL/SDL2-2.28.5/lib/x64" -L"$SDL/SDL2_image-2.8.2/lib/x64" -lSDL2main -lSDL2 -lSDL2_image -lshell32 -Xlinker /subsystem:console -o "$OUT/overlay_icons.exe"
"$OUT/overlay_icons.exe" "$ROOT/overlay/source/pl_symbols.hpp" 24
