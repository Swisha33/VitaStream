#!/bin/bash
# Baut ein minimales FFmpeg fuer die Vita nach $1 (Standard: ./ffmpeg-vita)
set -e
PREFIX=$(realpath -m "${1:-ffmpeg-vita}")
VER=n7.1.1
source "$(dirname "$0")/ffmpeg-config.sh"
[ -d ffmpeg-src ] || git clone --depth 1 -b $VER https://github.com/FFmpeg/FFmpeg ffmpeg-src
cd ffmpeg-src
./configure --prefix="$PREFIX" "${FFMPEG_COMMON_FLAGS[@]}" \
  --enable-cross-compile --cross-prefix=$VITASDK/bin/arm-vita-eabi- \
  --arch=armv7-a --cpu=cortex-a9 --target-os=none \
  --disable-runtime-cpudetect --disable-armv5te --disable-armv6t2 \
  --enable-static --disable-shared \
  --extra-cflags="-std=gnu11 -O2 -ftree-vectorize -fomit-frame-pointer -D_BSD_SOURCE -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"
make -j$(nproc)
make install
