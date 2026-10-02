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

# curl aus vdpm ist gegen OpenSSL 1.0.2 gebaut; liegt eine andere Version vor, passende nachinstallieren
if ! grep -q "OpenSSL 1.0.2" $L/include/openssl/opensslv.h 2>/dev/null; then
  echo "== OpenSSL 1.0.2 nachinstallieren" | tee -a $LOG
  vdpm openssl 2>&1 | tee -a $LOG || { echo "vdpm openssl fehlgeschlagen, versuche curl-mbedtls" | tee -a $LOG; USE_MBEDTLS=1; }
fi
if [ -n "$USE_MBEDTLS" ]; then
  vdpm mbedtls curl-mbedtls 2>&1 | tee -a $LOG
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ${USE_MBEDTLS:+-DUSE_MBEDTLS=ON} 2>&1 | tee -a $LOG
cmake --build build -j1 2>&1 | tee -a $LOG
ls -la build/*.vpk | tee -a $LOG
