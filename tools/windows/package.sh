#!/usr/bin/env bash
# Songate for PC, release: a build without dev extras, the licences beside it, the installer. Checks the program for the reports token.
#   C:\devkitPro\msys2\usr\bin\bash.exe -lc "/c/Users/andre/Desktop/GateFold-NX/tools/windows/package.sh"
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/build-windows"
SRC="$HERE/deps/src"
SDL="$ROOT/tools/preview/deps/sdl"
ISCC="/c/Users/andre/AppData/Local/Programs/Inno Setup 6/ISCC.exe"

DEV=0 bash "$HERE/build.sh"

# The licences of everything that ships. FFmpeg and mpg123 are LGPL: DLLs, built as written in README-licences.txt
L="$OUT/licences"
rm -rf "$L"
mkdir -p "$L"
cp "$ROOT/LICENSE" "$L/Songate-LICENSE.txt" 2>/dev/null || true
cp "$ROOT/LICENSE-gatefold" "$L/Gatefold-LICENSE.txt" 2>/dev/null || true
cp "$ROOT/romfs/fonts/OFL.txt" "$L/Inter-OFL.txt"
cp "$SRC/ffmpeg-7.1/COPYING.LGPLv2.1" "$L/FFmpeg-LGPL-2.1.txt"
cp "$SRC/ffmpeg-7.1/LICENSE.md" "$L/FFmpeg-LICENSE.md"
cp "$SRC/mpg123-1.32.10/COPYING" "$L/mpg123-LGPL-2.1.txt"
cp "$SRC/curl-8.11.1/COPYING" "$L/curl.txt"
cp "$SRC/zlib-1.3.1/LICENSE" "$L/zlib.txt"
cp "$SRC/flac-1.4.3/COPYING.Xiph" "$L/FLAC.txt"
cp "$SRC/opus-1.5.2/COPYING" "$L/Opus.txt"
cp "$SRC/opusfile-0.12/COPYING" "$L/opusfile.txt"
cp "$SRC/libogg-1.3.5/COPYING" "$L/Ogg.txt"
cp "$SRC/tremor-master/COPYING" "$L/Tremor.txt"
cp "$SRC/libjpeg-turbo-3.0.4/LICENSE.md" "$L/libjpeg-turbo.md"
cp "$HERE/licence-SDL.txt" "$L/SDL.txt"
{
  echo "Songate for Windows includes FFmpeg 7.1 and mpg123 1.32.10 under the LGPL, as separate DLLs you may replace."
  echo "Their sources: https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz and https://mpg123.org/download/mpg123-1.32.10.tar.bz2"
  echo "FFmpeg was configured with:"
  grep -m1 '^FFMPEG_CONFIGURATION=' "$HERE/deps/obj/ffmpeg/ffbuild/config.mak" | sed 's|^FFMPEG_CONFIGURATION=||; s|--prefix=[^ ]* ||'
  echo "Songate's own source: https://github.com/AndryTheBeast/Songate"
} > "$L/README-licences.txt"

VERSION="$(grep -E '^APP_VERSION' "$ROOT/Makefile" | sed -E 's/^APP_VERSION[[:space:]:=]*//; s/[[:space:]]+$//')"
# 2.0.0 BETA 1 → SongateWIN-2.0.0-beta.1, the spelling of the tag (plat::isInstaller finds it by that start)
NAME="SongateWIN-$(echo "$VERSION" | tr 'A-Z' 'a-z' | sed -E 's/ ([a-z]+) ([0-9]+)$/-\1.\2/; s/ /-/g')"
"$ISCC" //Q "//DAppVersion=$VERSION" "//DOutName=$NAME" "$(cygpath -w "$HERE/songate.iss")"
echo "built $OUT/release/$NAME.exe"
