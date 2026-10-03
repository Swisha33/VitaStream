#!/bin/sh
# Host-Tests: Blockliste, DNS-Resolver, Lua-Plugins (ohne Vita/VitaSDK).
# Benoetigt gcc, python3 und Lua 5.4-Quellen in $LUA_SRC (gebaut mit "make").
set -e
cd "$(dirname "$0")"
LUA_SRC=${LUA_SRC:-./lua}
SAN="-fsanitize=address,undefined -g -O1"

gcc -Wall $SAN -o test_adblock test_adblock.c ../src/adblock.c
./test_adblock | tail -1

gcc -Wall $SAN -o test_sub test_sub.c ../src/sub.c -lpthread
./test_sub

gcc -Wall $SAN -o test_history test_history.c ../src/history.c
./test_history

gcc -Wall $SAN -o test_secure test_secure.c ../src/secure.c ../third_party/aes/aes.c
./test_secure

gcc -Wall $SAN -o test_watched test_watched.c ../src/watched.c
./test_watched

python3 fake_dns.py 5353 & DNSPID=$!
sleep 0.5
gcc -Wall $SAN -DDNS_PORT=5353 -o test_dns test_dns.c ../src/dns.c
./test_dns || { kill $DNSPID; exit 1; }
kill $DNSPID

gcc -Wall $SAN -I"$LUA_SRC" -o test_plugins test_plugins.c ../src/plugins.c ../src/adblock.c ../src/secure.c ../third_party/aes/aes.c \
    "$LUA_SRC/liblua.a" -lm -ldl -lpthread
rm -rf run && D="run/ux0:data/VitaStream" && mkdir -p "$D/plugins"
cp ../data/*.txt ../data/*.ini "$D/" && cp ../data/plugins/*.lua "$D/plugins/"
# Beispielseite aktivieren, Testplaylist + Testsperre ergänzen
sed -i 's/^#\(\[Beispielseite\|start   = https:\/\/example.org\|search \|item    = <a class\|embed \|stream \|referer\)/\1/' "$D/sites.txt"
echo "Test-Liste|https://test.example/liste.m3u" >> "$D/playlists.txt"
echo "ads.adnet.example" >> "$D/blocklist.txt"
echo "Lokal|file:lokal.m3u" >> "$D/playlists.txt"
printf '#EXTM3U\n#EXTINF:-1 group-title="X",Eins\nhttps://ok.example/1.m3u8\n#EXTINF:-1,Zwei\nhttps://ok.example/2.m3u8\n#EXTINF:-1,Drei\nhttps://ok.example/3.m3u8\n#EXTINF:-1,Kaputt\nhttps://kaputt.example/4.m3u8\n' > "$D/lokal.m3u"
cd run && ../test_plugins
