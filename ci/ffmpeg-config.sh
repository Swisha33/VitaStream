#!/bin/bash
# Gemeinsame FFmpeg-Konfiguration (Vita und Host-Tests).
# Nur Demuxing (MP4, MPEG-TS, AAC, MP3) und Audio-Dekodierung - Video dekodiert die Vita-Hardware.
FFMPEG_COMMON_FLAGS=(
  --disable-everything --disable-programs --disable-doc --disable-network
  --disable-avdevice --disable-swscale --disable-postproc --disable-avfilter
  --enable-swresample --enable-small --disable-debug --enable-pthreads
  --enable-demuxer=mov,mpegts,aac,mp3
  --enable-parser=h264,aac,aac_latm,mpegaudio,ac3
  --enable-decoder=aac,aac_latm,mp3,mp3float,mp2,mp2float,ac3
  --enable-bsf=h264_mp4toannexb
  --enable-protocol=file
)
