# VitaStream

**Download:** [VitaStream.vpk (aktueller Build)](https://github.com/Swisha33/VitaStream/releases/download/nightly/VitaStream.vpk) – mit VitaShell installieren.

Generischer Stream-Player für die PS Vita (HENkaku/Ensō) mit Lua-Quellen-Plugins,
eigenen Websites und Playlists sowie abschaltbarem AdBlock (lokale Blockliste + eigener DNS, z. B. AdGuard).

## Funktionen

- **Quellen-Plugins in Lua** – Suchen, Durchblättern, Abspielen, mit Vorschaubildern und „Weitere laden“.
- **Mitgelieferte Quellen**
  - *Mediatheken* – ARD, ZDF, arte, 3sat, KiKA … pro Sender: Neueste, Sendungen, Kategorien (Filme, Dokus, Krimis …), ohne 80er-Grenze
  - *Sender-Finder* – tausende frei empfangbare Sender (iptv-org) nach Kategorie, Land, Sprache; Pluto TV, Samsung TV Plus, Rakuten TV; Anime & Zeichentrick
  - *South Park* – alle Staffeln von southpark.de (Deutsch/Englisch)
  - *M3U-Playlists & Favoriten* – eigene Listen, Logos, Gruppen
  - *Direkte URL & Website-Scanner* – Link abspielen oder eine Website nach Videos/Streams durchsuchen
  - *Eigene Websites* – per Muster in `sites.txt`
- **Favoriten & Playlists** – mit Quadrat einzelne Einträge oder ganze Listen speichern
- **Player** – MP4, HLS (inkl. AES-128), MPEG-TS; Hardware-H.264 bis 720p, Software-Ersatz für SD/Interlaced; AAC/MP3/AC3; Werbeunterbrechungen (Zeitsprünge) werden überbrückt
- **AdBlock** – lokale Blockliste + eigener DNS (AdGuard, Cloudflare, Quad9, eigener Server), einzeln schaltbar

## Bedienung

| Taste | Listen | Player |
|---|---|---|
| Bestätigen (✕ oder ○, je nach Region) | öffnen / abspielen | Pause |
| Zurück | eine Ebene zurück | Wiedergabe beenden |
| △ | Suche (in der Quelle) / Einstellungen (Startseite) | – |
| □ | Favoriten / Playlist-Menü | – |
| SELECT | – | technische Infos (Decoder, Puffer) |
| L / R | seitenweise blättern | ±60 s |
| ◀ / ▶ | DNS-Preset wechseln (Einstellungen) | ±10 s |
| START | App beenden | – |

## Dateien auf der Vita

Beim ersten Start werden die Standarddateien nach `ux0:data/VitaStream/` kopiert. Danach werden sie nie überschrieben, du kannst sie per FTP (VitaShell, SELECT) bearbeiten:

```
ux0:data/VitaStream/
├── config.ini        Einstellungen (auch in der App änderbar)
├── blocklist.txt     Blockliste – beliebige hosts-/AdGuard-Listen anhängen
├── playlists.txt     M3U-Playlists:  Name|URL   oder   Name|file:datei.m3u
├── sites.txt         eigene Websites (siehe unten)
├── cacert.pem        optional: eigene CA-Zertifikate statt der Systemliste
└── plugins/          Lua-Plugins (*.lua)
```

**TLS-Zertifikate:** HTTPS wird standardmäßig mit den Systemzertifikaten der Vita geprüft. Sind die für eine Seite zu alt, lege eine aktuelle `cacert.pem` (z. B. von https://curl.se/docs/caextract.html) nach `ux0:data/VitaStream/`. Nur zur Fehlersuche: `ssl_verify=0` in `config.ini`.

**Größere Blocklisten:** z. B. die StevenBlack-hosts-Datei oder die AdGuard-DNS-Filterliste herunterladen und an `blocklist.txt` anhängen, danach in den Einstellungen „Blockliste neu laden“.

## Eigene Websites (`sites.txt`)

Jeder Abschnitt wird zu einer eigenen Quelle. Die Muster sind [Lua-Patterns](https://www.lua.org/manual/5.4/manual.html#6.4.1)
(`%.` = Punkt, `.-` = möglichst wenig, `[^"]+` = alles bis zum Anführungszeichen).

```ini
[Mein Heimserver]
start   = http://192.168.1.20:8080/videos/
item    = <a href="([^"]+%.mp4)">([^<]+)</a>

[Seite mit Suche]
start   = https://example.org/neu                 ; Startliste
search  = https://example.org/suche?q={q}         ; {q} = Suchbegriff
item    = <a class="video" href="([^"]+)"[^>]*>(.-)</a>   ; 2 Captures: Link, Titel
embed   = <iframe[^>]+src="([^"]+)"               ; optional: Player-iframe folgen
stream  = (https?://[^"'%s]+%.m3u8[^"'%s]*)       ; Stream-Link auf der Detailseite
referer = https://example.org/                    ; optional
```

Bitte nutze nur Quellen, deren Inhalte du legal abrufen darfst.

## Eigenes Plugin schreiben

```lua
local json = require("json")      -- mitgelieferter JSON-Parser

return {
  name = "Meine Quelle",
  description = "Kurzbeschreibung",

  -- Startseite (id == nil) und Unterordner
  browse = function(id)
    return { { title = "Ordner", id = "x", kind = "folder" },
             { title = "Video", subtitle = "12:34", id = "https://.../v.mp4", kind = "video" } }
  end,

  search = function(query) ... return items end,     -- optional

  -- Stream-URL liefern: String oder { url = ..., headers = { Referer = ... } }
  resolve = function(item) return item.id end,
}
```

Fehler meldet man mit `return nil, "Text"` – die App zeigt den Text an.

**Lua-API (`vs.*`)**

| Funktion | Beschreibung |
|---|---|
| `vs.http_get(url [, headers])` | → `body, status, final_url` oder `nil, fehler` |
| `vs.http_post(url, body [, headers])` | wie oben; `headers` als Text, Zeilen mit `\n` getrennt |
| `vs.is_blocked(url)` | `true`, wenn AdBlock/DNS den Host sperrt |
| `vs.urlencode(s)`, `vs.html_unescape(s)` | Hilfsfunktionen |
| `vs.read_file(name)`, `vs.write_file(name, text [, append])` | nur innerhalb von `ux0:data/VitaStream` |
| `vs.log(text)` | letzte Meldung erscheint unter Einstellungen → Plugins |

Plugins laufen in einer Sandbox ohne `io`, `os`, `dofile`, `loadfile`. Alle Netzwerkzugriffe gehen über AdBlock und DNS der App.

## Bauen

Mit installiertem [VitaSDK](https://vitasdk.org) (Pakete: `curl-mbedtls mbedtls zstd zlib libvita2d libpng libjpeg-turbo`):

```sh
cmake -S . -B build -DUSE_MBEDTLS=ON && cmake --build build
# -> build/VitaStream.vpk
```

Ohne lokales SDK: Repository auf GitHub pushen – der Workflow `.github/workflows/build.yml` baut die VPK im
`vitasdk/vitasdk`-Container und stellt sie unter *Actions → Artifacts* bereit.

Host-Tests (Blockliste, DNS-Resolver, alle Lua-Plugins mit simuliertem Netzwerk;
Player-Tests mit `tests/build_media_test.sh` + `tests/test_media` gegen einen lokalen Testserver `tests/http_server.py`):

```sh
git clone --depth 1 -b v5.4.6 https://github.com/lua/lua tests/lua && make -C tests/lua
LUA_SRC=./lua tests/run_tests.sh
```

## Technische Hinweise & Grenzen

- **Formate:** Die Vita dekodiert H.264 per Hardware bis 1280×720. 1080p-Streams lassen sich nicht abspielen (bei HLS wird automatisch ≤720p gewählt). SD-Streams, die der Hardware-Decoder ablehnt (z. B. Interlaced-TV), laufen per Software. HEVC, VP9, AV1 und DRM (Widevine/SAMPLE-AES) gehen nicht.
- **MP4** wird über den eigenen Netzwerkstack (curl, Range-Anfragen mit 512-KB-Puffer) gelesen – AdBlock, DNS und Header gelten vollständig.
- **HLS (.m3u8)** wird direkt an `sceAvPlayer` übergeben und nutzt dabei den System-DNS; Plugin-Seiten, Weiterleitungen und die Stream-URL selbst werden vorher trotzdem gegen die Blockliste geprüft. Benötigt ein HLS-Stream spezielle Header (Referer), kann die Wiedergabe scheitern – ein lokaler HLS-Proxy wäre der nächste Ausbauschritt.
- Ungetestet auf echter Hardware: dieser Stand ist gegen die vita-headers syntaxgeprüft, Blockliste/DNS/Plugins sind per Host-Tests geprüft. Der Player-Teil (`player.c`) muss auf der Konsole verifiziert werden.

## Projektstruktur

```
src/main.c      Bildschirme & Ablauf (Quellen → Liste → Player, Einstellungen)
src/ui.c        vita2d-Oberfläche, Listen, Bildschirmtastatur
src/net.c       HTTP (curl), eigene Weiterleitungen, Range-Streams für den Player
src/dns.c       eigener DNS-Resolver (UDP, Cache, erkennt AdGuard-Sperrantworten)
src/adblock.c   Blockliste mit Subdomain-Abgleich und Ausnahmen
src/plugins.c   Lua-Laufzeit, Sandbox, vs.*-API, Worker-Thread
src/player.c    sceAvPlayer, YUV-Textur, Audio-Thread
third_party/lua Lua 5.4.6 (MIT-Lizenz, Lua.org, PUC-Rio)
data/           Standardkonfiguration und Plugins
tests/          Host-Tests
```
