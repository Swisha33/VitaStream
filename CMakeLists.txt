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
set(VITA_VERSION  "00.10")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-unused-function")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12") # erweitertes RAM

add_executable(${PROJECT_NAME}
  src/main.c
  src/ui.c
  src/config.c
  src/adblock.c
  src/dns.c
  src/net.c
  src/plugins.c
  src/player.c
)

target_link_libraries(${PROJECT_NAME}
  lua
  curl ssl crypto
  vita2d
  png jpeg z
  SceAvPlayer_stub
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
  FILE data/plugins/direct.lua data/plugins/direct.lua
  FILE data/plugins/m3u.lua data/plugins/m3u.lua
  FILE data/plugins/mediathek.lua data/plugins/mediathek.lua
  FILE data/plugins/website.lua data/plugins/website.lua
  FILE data/config.ini data/config.ini
  FILE data/blocklist.txt data/blocklist.txt
  FILE data/sites.txt data/sites.txt
  FILE data/playlists.txt data/playlists.txt
)
