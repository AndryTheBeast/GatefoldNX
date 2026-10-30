#!/usr/bin/env bash
# Songate for PC: builds the libraries into tools/windows/deps/prefix (clang-cl, the static CRT, no GPL).
# Run from devkitPro's MSYS2 bash, which has make and the shell FFmpeg's configure needs:
#   C:\devkitPro\msys2\usr\bin\bash.exe -lc "/c/Users/andre/Desktop/GateFold-NX/tools/windows/build-deps.sh"
# Sources come from tools/windows/deps/dl; one name as an argument rebuilds only that library.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="$HERE/deps"
DL="$DEPS/dl"
SRC="$DEPS/src"
BLD="$DEPS/obj"
PREFIX="$DEPS/prefix"
mkdir -p "$SRC" "$BLD" "$PREFIX/include" "$PREFIX/lib" "$PREFIX/bin"

BT="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
CMAKE_DIR="$BT/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
NINJA_DIR="$(ls -d "/c/Users/andre/AppData/Local/Microsoft/WinGet/Packages/Ninja-build.Ninja_"* | head -1)"
export PATH="/c/Program Files/LLVM/bin:$CMAKE_DIR:$NINJA_DIR:$SRC/nasm-2.16.03:$PATH"
[ -d "$SRC/nasm-2.16.03" ] || (cd "$SRC" && cmake -E tar xf "$(cygpath -m "$DL/nasm/nasm-2.16.03-win64.zip")")

# What clang-cl and lld-link need; vcvars is not run from here
MSVC="$(ls -d "$BT/VC/Tools/MSVC/"* | sort | tail -1)"
export PATH="$PATH:$MSVC/bin/Hostx64/x64"   # FFmpeg's makedef wants lib.exe and dumpbin.exe
KIT="/c/Program Files (x86)/Windows Kits/10"
SDK="$(ls "$KIT/Include" | sort | tail -1)"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$MSVC/include");$(w "$KIT/Include/$SDK/ucrt");$(w "$KIT/Include/$SDK/um");$(w "$KIT/Include/$SDK/shared");$(w "$KIT/Include/$SDK/winrt")"
export LIB="$(w "$MSVC/lib/x64");$(w "$KIT/Lib/$SDK/ucrt/x64");$(w "$KIT/Lib/$SDK/um/x64")"

P="$(cygpath -m "$PREFIX")"
NASM="$(cygpath -m "$SRC/nasm-2.16.03/nasm.exe")"
CM=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
    -DCMAKE_AR="$(cygpath -m "/c/Program Files/LLVM/bin/llvm-lib.exe")"
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P"
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_ASM_NASM_COMPILER="$NASM")

unpack() {   # unpack <dl folder> <file> <folder it unpacks to>
  [ -d "$SRC/$3" ] || tar -xf "$DL/$1/$2" -C "$SRC"
}
cm() {       # cm <name> <source folder> <cmake options...>
  local name="$1" from="$2"; shift 2
  rm -rf "$BLD/$name"
  cmake -S "$(cygpath -m "$from")" -B "$(cygpath -m "$BLD/$name")" "${CM[@]}" "$@"
  cmake --build "$(cygpath -m "$BLD/$name")" --config Release
  cmake --install "$(cygpath -m "$BLD/$name")" --config Release
}

b_zlib() {
  unpack zlib zlib-1.3.1.tar.gz zlib-1.3.1
  cm zlib "$SRC/zlib-1.3.1"
  # zlib's install makes both; the static one is the one wanted
  cp "$PREFIX/lib/zlibstatic.lib" "$PREFIX/lib/z.lib"
}
b_ogg() {
  unpack ogg libogg-1.3.5.tar.xz libogg-1.3.5
  cm ogg "$SRC/libogg-1.3.5" -DINSTALL_DOCS=OFF
}
b_opus() {
  unpack opus opus-1.5.2.tar.gz opus-1.5.2
  cm opus "$SRC/opus-1.5.2" -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_DISABLE_INTRINSICS=ON   # clang-cl refuses opus's per-file SSE4/AVX; only waveforms decode through it
}
b_opusfile() {
  unpack opusfile opusfile-0.12.tar.gz opusfile-0.12
  rm -rf "$BLD/opusfile"; mkdir -p "$BLD/opusfile"   # the release has no CMake project: four files, no http
  local objs=()
  for f in info internal opusfile stream; do
    clang-cl -nologo -O2 -MT -D_CRT_SECURE_NO_WARNINGS -DOP_DISABLE_HTTP -Wno-everything -I"$(w "$PREFIX/include")" -I"$(w "$PREFIX/include/opus")" \
      -I"$(w "$SRC/opusfile-0.12/include")" -c "$(w "$SRC/opusfile-0.12/src/$f.c")" -Fo"$(w "$BLD/opusfile/$f.obj")"
    objs+=("$(w "$BLD/opusfile/$f.obj")")
  done
  llvm-lib -nologo -out:"$(w "$PREFIX/lib/opusfile.lib")" "${objs[@]}"
  cp "$SRC/opusfile-0.12/include/opusfile.h" "$PREFIX/include/opus/"
}
b_flac() {
  unpack flac flac-1.4.3.tar.xz flac-1.4.3
  cm flac "$SRC/flac-1.4.3" -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DWITH_OGG=OFF \
    -DCMAKE_C_FLAGS="-DWIN32 -D_WINDOWS -w -Wno-error=incompatible-pointer-types"   # _BitScanReverse takes unsigned long*, the same size here
}
b_mpg123() {
  unpack mpg123 mpg123-1.32.10.tar.bz2 mpg123-1.32.10
  cm mpg123 "$SRC/mpg123-1.32.10/ports/cmake" -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF -DBUILD_SHARED_LIBS=ON   # LGPL: a DLL, replaceable
}
b_jpeg() {
  unpack jpeg libjpeg-turbo-3.0.4.tar.gz libjpeg-turbo-3.0.4
  cm jpeg "$SRC/libjpeg-turbo-3.0.4" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DREQUIRE_SIMD=ON -DWITH_CRT_DLL=OFF
}
b_curl() {
  unpack curl curl-8.11.1.tar.xz curl-8.11.1
  cm curl "$SRC/curl-8.11.1" -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON -DCURL_USE_SCHANNEL=ON -DHTTP_ONLY=ON -DCURL_DISABLE_LDAP=ON \
    -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_USE_LIBSSH2=OFF \
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DZLIB_ROOT="$P" -DZLIB_LIBRARY="$P/lib/z.lib" -DZLIB_INCLUDE_DIR="$P/include"
}
b_tremor() {   # no build system for this compiler; a short list of files
  [ -d "$SRC/tremor-master" ] || tar -xf "$DL/tremor/tremor-master.tar.gz" -C "$SRC"
  rm -rf "$BLD/tremor"; mkdir -p "$BLD/tremor" "$PREFIX/include/tremor"
  local objs=()
  for f in block codebook floor0 floor1 info mapping0 mdct registry res012 sharedbook synthesis vorbisfile window; do
    clang-cl -nologo -O2 -MT -D_CRT_SECURE_NO_WARNINGS -Wno-everything -I"$(w "$PREFIX/include")" -I"$(w "$SRC/tremor-master")" \
      -c "$(w "$SRC/tremor-master/$f.c")" -Fo"$(w "$BLD/tremor/$f.obj")"
    objs+=("$(w "$BLD/tremor/$f.obj")")
  done
  llvm-lib -nologo -out:"$(w "$PREFIX/lib/vorbisidec.lib")" "${objs[@]}"
  cp "$SRC/tremor-master/"{ivorbiscodec,ivorbisfile,config_types}.h "$PREFIX/include/tremor/" 2>/dev/null || cp "$SRC/tremor-master/"{ivorbiscodec,ivorbisfile}.h "$PREFIX/include/tremor/"
}
b_ffmpeg() {   # the console's version, audio only, LGPL, DLLs so that it stays replaceable
  unpack ffmpeg ffmpeg-7.1.tar.xz ffmpeg-7.1
  rm -rf "$BLD/ffmpeg"; mkdir -p "$BLD/ffmpeg"
  cd "$BLD/ffmpeg"
  # Kept for the licence: the options this build was made with go next to the DLLs
  "$SRC/ffmpeg-7.1/configure" --prefix="$PREFIX" --toolchain=msvc --cc=clang-cl --cxx=clang-cl --ld=lld-link --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib \
    --x86asmexe=nasm --arch=x86_64 --target-os=win64 --enable-shared --disable-static --enable-asm --enable-x86asm \
    --extra-cflags=-MT --extra-ldflags=-nodefaultlib:msvcrt \
    --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-avdevice --disable-avfilter --disable-swscale --disable-network \
    --disable-everything --enable-protocol=file \
    --enable-demuxer=mov,aac,mp3,flac,ogg,wav \
    --enable-parser=aac,mpegaudio,flac,vorbis,opus \
    --enable-decoder=aac,alac,mp3float,mp3,flac,opus,vorbis,'pcm_*'
  make -j8
  make install
  mkdir -p "$PREFIX/share/ffmpeg"
  cp "$BLD/ffmpeg/ffbuild/config.log" "$PREFIX/share/ffmpeg/config.log"
  cd "$HERE"
}

ALL=(zlib ogg opus opusfile flac mpg123 jpeg curl tremor ffmpeg)
for n in "${@:-${ALL[@]}}"; do
  echo "=== $n"
  "b_$n"
done
echo "all built into $PREFIX"
