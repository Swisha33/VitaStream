cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Bitte VITASDK setzen (export VITASDK=/usr/local/vitasdk)")
  endif()
endif()

project(VitaStream C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaStream")
set(VITA_TITLEID  "VSTR00001")
set(VITA_VERSION  "00.30")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-unused-function")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12") # erweitertes RAM

# Lua 5.4 wird mitgebaut (third_party/lua, MIT-Lizenz) - unabhängig von SDK-Paketen
file(GLOB LUA_SOURCES third_party/lua/*.c)
add_library(lua_vendored STATIC ${LUA_SOURCES})
target_include_directories(lua_vendored PUBLIC third_party/lua)
target_compile_options(lua_vendored PRIVATE -O2 -w)

add_executable(${PROJECT_NAME}
  src/main.c
  src/ui.c
  src/config.c
  src/adblock.c
  src/dns.c
  src/net.c
  src/plugins.c
  src/player.c
  src/media.c
  src/hls.c
  src/vdec_vita.c
  src/vdec_sw.c
  src/aout_vita.c
  src/thumbs.c
  src/thumbfetch.c
  third_party/aes/aes.c
)

# Minimal-FFmpeg (Demuxer + Audio-Decoder), gebaut von ci/build-ffmpeg-vita.sh
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/ffmpeg-vita" CACHE PATH "FFmpeg-Installationsverzeichnis")
if(NOT EXISTS "${FFMPEG_DIR}/lib/libavformat.a")
  message(FATAL_ERROR "FFmpeg fehlt: zuerst ci/build-ffmpeg-vita.sh ausfuehren (FFMPEG_DIR=${FFMPEG_DIR})")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_DIR}/include")

option(USE_MBEDTLS "curl mit mbedTLS statt OpenSSL linken" OFF)
if(USE_MBEDTLS)
  set(TLS_LIBS mbedtls mbedx509 mbedcrypto)
else()
  set(TLS_LIBS ssl crypto)
endif()

target_link_libraries(${PROJECT_NAME}
  lua_vendored
  ${FFMPEG_DIR}/lib/libavformat.a
  ${FFMPEG_DIR}/lib/libavcodec.a
  ${FFMPEG_DIR}/lib/libswresample.a
  ${FFMPEG_DIR}/lib/libavutil.a
  curl ${TLS_LIBS} zstd
  vita2d
  png jpeg z
  SceVideodec_stub
  ScePower_stub
  SceAudio_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceIme_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceSsl_stub
  SceSysmodule_stub
  ScePgf_stub
  ScePvf_stub
  SceLibKernel_stub
  SceRtc_stub
  SceShellSvc_stub
  SceAppMgr_stub
  m pthread
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE data/plugins/json.lua data/plugins/json.lua
  FILE data/plugins/m3ulib.lua data/plugins/m3ulib.lua
  FILE data/plugins/finder.lua data/plugins/finder.lua
  FILE data/plugins/southpark.lua data/plugins/southpark.lua
  FILE data/plugins/direct.lua data/plugins/direct.lua
  FILE data/plugins/m3u.lua data/plugins/m3u.lua
  FILE data/plugins/mediathek.lua data/plugins/mediathek.lua
  FILE data/plugins/website.lua data/plugins/website.lua
  FILE data/config.ini data/config.ini
  FILE data/cacert.pem data/cacert.pem
  FILE data/blocklist.txt data/blocklist.txt
  FILE data/sites.txt data/sites.txt
  FILE data/playlists.txt data/playlists.txt
)
