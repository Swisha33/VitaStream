#!/bin/sh
# Host-Tests: Blockliste, DNS-Resolver, Lua-Plugins (ohne Vita/VitaSDK).
# Benoetigt gcc, python3 und Lua 5.4-Quellen in $LUA_SRC (gebaut mit "make").
set -e
cd "$(dirname "$0")"
LUA_SRC=${LUA_SRC:-./lua}
SAN="-fsanitize=address,undefined -g -O1"

gcc -Wall $SAN -o test_adblock test_adblock.c ../src/adblock.c
./test_adblock | tail -1

python3 fake_dns.py 5353 & DNSPID=$!
sleep 0.5
gcc -Wall $SAN -DDNS_PORT=5353 -o test_dns test_dns.c ../src/dns.c
./test_dns || { kill $DNSPID; exit 1; }
kill $DNSPID

gcc -Wall $SAN -I"$LUA_SRC" -o test_plugins test_plugins.c ../src/plugins.c ../src/adblock.c \
    "$LUA_SRC/liblua.a" -lm -ldl -lpthread
rm -rf run && D="run/ux0:data/VitaStream" && mkdir -p "$D/plugins"
cp ../data/*.txt ../data/*.ini "$D/" && cp ../data/plugins/*.lua "$D/plugins/"
# Beispielseite aktivieren, Testplaylist + Testsperre ergänzen
sed -i 's/^#\(\[Beispielseite\|start   = https:\/\/example.org\|search \|item    = <a class\|embed \|stream \|referer\)/\1/' "$D/sites.txt"
echo "Test-Liste|https://test.example/liste.m3u" >> "$D/playlists.txt"
echo "ads.adnet.example" >> "$D/blocklist.txt"
cd run && ../test_plugins
