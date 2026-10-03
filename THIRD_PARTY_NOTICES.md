# Verwendete Fremdsoftware

VitaStream enthält bzw. linkt folgende Bibliotheken. Ihre Lizenzen gelten für die jeweiligen Teile.

| Komponente | Lizenz | Hinweis |
|---|---|---|
| [FFmpeg](https://ffmpeg.org) 7.1.1 (libavformat, libavcodec, libswresample, libavutil) | LGPL 2.1 oder später | Nur LGPL-Komponenten (kein `--enable-gpl`). Statisch gelinkt; Quellen und Build-Konfiguration: `ci/build-ffmpeg-vita.sh`, `ci/ffmpeg-config.sh`. Mit dem Quellcode dieses Repositorys lässt sich die App gegen eine geänderte FFmpeg-Version neu bauen (LGPL §6). |
| [Lua](https://www.lua.org) 5.4.6 | MIT | Copyright © 1994–2023 Lua.org, PUC-Rio. `third_party/lua` |
| [tiny-AES-c](https://github.com/kokke/tiny-AES-c) | Unlicense (gemeinfrei) | `third_party/aes` |
| [stb_image](https://github.com/nothings/stb) 2.30 | Public Domain / MIT | `third_party/stb` |
| Mozilla CA-Zertifikatsliste (über [certifi](https://github.com/certifi/python-certifi)) | MPL 2.0 | `data/cacert.pem`, unverändert |
| [libcurl](https://curl.se) | curl-Lizenz (MIT-artig) | aus dem VitaSDK |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | Apache 2.0 | aus dem VitaSDK |
| [libvita2d](https://github.com/xerpi/libvita2d) | MIT | aus dem VitaSDK |
| zlib, libpng, libjpeg-turbo, zstd | zlib / libpng / IJG+BSD / BSD | aus dem VitaSDK |

Inhalte, die über die App abgerufen werden, gehören den jeweiligen Anbietern. VitaStream enthält keine Inhalte
und umgeht keinen Kopierschutz (DRM-geschützte Streams werden ausdrücklich nicht abgespielt).

## Vorbilder (kein Code übernommen)

- **ViTube** (https://github.com/shorelight82/vitube-vpk, GPL-3.0) – Ansatz des YouTube-Plugins
  (InnerTube-Schnittstelle, visionOS-Client für HLS). Das Plugin ist eine eigene Lua-Umsetzung.
- **plutotv** (https://github.com/ps5-payload-dev/plutotv, GPL-3.0) – dokumentierter Ablauf der
  Pluto-TV-Schnittstelle (Sitzung, Katalog, Stitcher-Adresse). Eigene Lua-Umsetzung.
- Offene Verzeichnisse: radio-browser.info (Radiosender), Apple-Podcastverzeichnis (Suche/Charts),
  MediathekViewWeb, Internet Archive.
