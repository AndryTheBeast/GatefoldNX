#!/usr/bin/env bash
# Songate for PC: configures and builds with clang, then gathers what the program needs beside it in build/windows/.
#   C:\devkitPro\msys2\usr\bin\bash.exe -lc "/c/Users/andre/Desktop/GateFold-NX/tools/windows/build.sh"
# DEV=0: a release build (no console window, no dev extras).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/build-windows${WARN:+-check}"   # WARN=1: a build of its own with every warning on
DEPS="$HERE/deps/prefix"
SDL="$ROOT/tools/preview/deps/sdl"

BT="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
CMAKE_DIR="$BT/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
NINJA_DIR="$(ls -d "/c/Users/andre/AppData/Local/Microsoft/WinGet/Packages/Ninja-build.Ninja_"* | head -1)"
export PATH="/c/Program Files/LLVM/bin:$CMAKE_DIR:$NINJA_DIR:$PATH"
MSVC="$(ls -d "$BT/VC/Tools/MSVC/"* | sort | tail -1)"
KIT="/c/Program Files (x86)/Windows Kits/10"
SDK="$(ls "$KIT/Include" | sort | tail -1)"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$MSVC/include");$(w "$KIT/Include/$SDK/ucrt");$(w "$KIT/Include/$SDK/um");$(w "$KIT/Include/$SDK/shared");$(w "$KIT/Include/$SDK/winrt")"
export LIB="$(w "$MSVC/lib/x64");$(w "$KIT/Lib/$SDK/ucrt/x64");$(w "$KIT/Lib/$SDK/um/x64")"

DEVFLAG=ON
[ "${DEV:-1}" = "0" ] && DEVFLAG=OFF
cmake -S "$(cygpath -m "$ROOT")" -B "$(cygpath -m "$OUT/obj")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_RC_COMPILER=llvm-rc \
  -DGF_DEV=$DEVFLAG ${WARN:+"-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare"} >/dev/null
cmake --build "$(cygpath -m "$OUT/obj")"

mkdir -p "$OUT/romfs"
rm -f "$OUT/"*.dll
cp "$OUT/obj/Songate.exe" "$OUT/"
for p in SDL2-2.28.5 SDL2_ttf-2.22.0 SDL2_image-2.8.2 SDL2_mixer-2.8.0; do
  cp "$SDL/$p/lib/x64/"*.dll "$OUT/" 2>/dev/null || true
done
# What SDL_image needs for WebP covers; the other optional libraries are formats nothing here opens
cp "$SDL/SDL2_image-2.8.2/lib/x64/optional/"libwebp*.dll "$OUT/"
cp "$DEPS/bin/"av*.dll "$DEPS/bin/"swresample*.dll "$DEPS/bin/"*mpg123*.dll "$OUT/"
cp -r "$ROOT/romfs/." "$OUT/romfs/"
echo "built $OUT/Songate.exe"
