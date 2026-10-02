#!/bin/sh
# Baut den Media-Test gegen ein Host-FFmpeg (FF=Prefix) und curl-Header (CURL_INC)
cd "$(dirname "$0")"
gcc -Wall -Wno-deprecated-declarations -O1 -g -fsanitize=address,undefined -I"$FF/include" -I"$CURL_INC" -o test_media \
  test_media.c platform_host.c ../src/media.c ../src/hls.c ../src/net.c ../src/dns.c ../src/adblock.c ../src/config.c \
  "$FF/lib/libavformat.a" "$FF/lib/libavcodec.a" "$FF/lib/libswresample.a" "$FF/lib/libavutil.a" \
  -l:libcurl.so.4 -lpthread -lm -lz
