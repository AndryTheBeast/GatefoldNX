#!/usr/bin/env bash
# Gatefold NX preview: builds the app's UI for Windows with clang + the MSVC libraries.
#
# Needs the SDL2 "devel VC" packages unpacked into one folder:
#   SDL2-2.28.5, SDL2_ttf-2.22.0, SDL2_image-2.8.2, SDL2_mixer-2.8.0
# Usage: tools/preview/build.sh <sdl-folder> [<out-folder>]
# Optional: FFMPEG=<an FFmpeg win64 "shared" build folder> adds M4A/AAC like the console build.
set -euo pipefail
SDL="${1:?folder with the SDL2 devel VC packages}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${2:-$HERE/out}"
SRC="$HERE/../../source"
mkdir -p "$OUT" "$OUT/include/SDL2"

for p in SDL2-2.28.5 SDL2_ttf-2.22.0 SDL2_image-2.8.2 SDL2_mixer-2.8.0; do
  cp "$SDL/$p/include/"*.h "$OUT/include/SDL2/"
  cp "$SDL/$p/lib/x64/"*.dll "$OUT/" 2>/dev/null || true
  [ -d "$SDL/$p/lib/x64/optional" ] && cp "$SDL/$p/lib/x64/optional/"*.dll "$OUT/" 2>/dev/null || true
done

# __PRFCHWINTRIN_H: skip SDL_endian.h's _m_prefetch, which newer clang already provides
FLAGS=(-std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D__PRFCHWINTRIN_H -Wno-deprecated-declarations
       -I"$OUT/include" -I"$HERE/include" -I"$SRC" -I"$HERE/../../sysmodule/include")
APP=(app tour stats levels loudness mixan mood automix m3u shell view_smart widgets view_now view_canvas view_lists view_grid view_settings view_sheet view_tracks view_library view_files view_playlists view_join view_automix view_updates view_storage view_support net updater report gfx ui palette library metacache player spectrum store bgclient thumbs loader)

# A dev build unless DEV=0 (the Makefile's switch): DEV=0 for README screenshots, as a release looks.
[ "${DEV:-1}" != "0" ] && FLAGS+=(-DGF_DEV)
FFLIBS=()
if [ -n "${FFMPEG:-}" ]; then
  FLAGS+=(-DGF_FFMPEG -I"$FFMPEG/include")
  FFLIBS=(-L"$FFMPEG/lib" -lavformat -lavcodec -lswresample -lavutil)
  cp "$FFMPEG/bin/"*.dll "$OUT/"
  APP+=(ffdecoder)
fi

objs=()
for f in "${APP[@]}"; do
  clang++ "${FLAGS[@]}" -include "$HERE/hostshim.h" -c "$SRC/$f.cpp" -o "$OUT/$f.obj" &
  objs+=("$OUT/$f.obj")
done
for f in metadata_host waveform_host; do
  clang++ "${FLAGS[@]}" -include "$HERE/hostshim.h" -c "$HERE/$f.cpp" -o "$OUT/$f.obj" &
  objs+=("$OUT/$f.obj")
done
clang++ "${FLAGS[@]}" -c "$HERE/hostshim.cpp" -o "$OUT/hostshim.obj" &
objs+=("$OUT/hostshim.obj")
clang++ "${FLAGS[@]}" -c "$HERE/streamshim.cpp" -o "$OUT/streamshim.obj" &   # the live test environment (live/)
objs+=("$OUT/streamshim.obj")
if [ -z "${FFMPEG:-}" ]; then
  clang++ "${FLAGS[@]}" -c "$HERE/ffdecoder_stub.cpp" -o "$OUT/ffdecoder_stub.obj" &
  objs+=("$OUT/ffdecoder_stub.obj")
fi
wait

LIBS=()
for p in SDL2-2.28.5 SDL2_ttf-2.22.0 SDL2_image-2.8.2 SDL2_mixer-2.8.0; do LIBS+=(-L"$SDL/$p/lib/x64"); done
clang++ "${objs[@]}" "${LIBS[@]}" -lSDL2 -lSDL2main -lSDL2_ttf -lSDL2_image -lSDL2_mixer "${FFLIBS[@]}" -lshell32 -lws2_32 -lwinmm \
  -Xlinker /subsystem:console -o "$OUT/gatefold-preview.exe"
echo "built $OUT/gatefold-preview.exe"
