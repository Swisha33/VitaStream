#!/bin/bash
# CI-Build im vitasdk/vitasdk-Container. Bei Fehlern werden die letzten Logzeilen
# als GitHub-Annotationen ausgegeben (die Logs selbst sind nicht immer abrufbar).
set -o pipefail
LOG=ci.log
: > $LOG
annotate() {
  grep -E "error|Error|undefined reference|not found|No such|cannot find|conflict|failed" $LOG | head -30 | \
    sed 's/%/%25/g' | while IFS= read -r l; do echo "::error::$l"; done
  tail -20 $LOG | sed 's/%/%25/g' | while IFS= read -r l; do echo "::warning::$l"; done
}
trap 'annotate' ERR
set -e
L=$VITASDK/arm-vita-eabi
{
  echo "== SDK-Bibliotheken"; ls $L/lib | grep -E "ssl|crypto|curl|mbed|zstd" || true
  grep -h "define OPENSSL_VERSION_TEXT" $L/include/openssl/opensslv.h 2>/dev/null | head -2 || true
} 2>&1 | tee -a $LOG

# Das curl-Paket im Container passt nicht zum installierten OpenSSL 1.1.1 und laesst
# sich nicht sauber tauschen. curl mit mbedTLS ist konfliktfrei und unterstuetzt TLS 1.2/1.3.
if [ ! -f $L/lib/libmbedtls.a ] || ! grep -rq "mbedtls" $L/lib/pkgconfig/libcurl.pc 2>/dev/null; then
  echo "== curl-mbedtls installieren" | tee -a $LOG
  vdpm mbedtls curl-mbedtls < <(yes y) 2>&1 | tee -a $LOG
fi
USE_MBEDTLS=1

# FFmpeg (aus dem Cache oder neu bauen)
if [ ! -f ffmpeg-vita/lib/libavformat.a ]; then
  echo "== FFmpeg bauen" | tee -a $LOG
  ci/build-ffmpeg-vita.sh ffmpeg-vita > ffmpeg-build.log 2>&1 || { tail -40 ffmpeg-build.log >> $LOG; false; }
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ${USE_MBEDTLS:+-DUSE_MBEDTLS=ON} 2>&1 | tee -a $LOG
cmake --build build -j1 2>&1 | tee -a $LOG
ls -la build/*.vpk | tee -a $LOG
