/* Host-Test für plugins.c + alle mitgelieferten Lua-Plugins (echtes Lua 5.4, simuliertes Netzwerk) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/net.h"
#include "../src/config.h"
#include "../src/adblock.h"
#include "../src/plugins.h"

NetStats g_net_stats;
VsConfig g_cfg;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)
static char last_post[4096], last_headers[512], last_url[2048];

static int reply(NetBuf *out, const char *s) { out->data = strdup(s); out->len = strlen(s); return NET_OK; }

int net_check_url(const char *url) {
    char h[256]; adblock_host_from_url(url, h, sizeof h);
    return (g_cfg.adblock_enabled && adblock_is_blocked(h)) ? NET_BLOCKED : NET_OK;
}
const char *net_strerror(int c) { return c == NET_BLOCKED ? "Durch AdBlock gesperrt" : "Netzwerkfehler"; }
void net_buf_free(NetBuf *b) { free(b->data); b->data = NULL; b->len = 0; }
const char *net_last_detail(void) { return ""; }
int net_probe(const char *url, const char *headers, int timeout_s, char *info, int infolen)
{
    (void)headers; (void)timeout_s;
    int ok = strstr(url, "kaputt") == NULL;
    snprintf(info, infolen, ok ? "OK (HTTP 200)" : "HTTP 404");
    return ok;
}

/* Mediathek: liefert "size" Treffer ab "offset", insgesamt 120 */
static int mediathek_reply(const char *post, NetBuf *out)
{
    const char *o = strstr(post, "\"offset\":"), *z = strstr(post, "\"size\":");
    int off = o ? atoi(o + 9) : 0, size = z ? atoi(z + 7) : 50, total = 120;
    size_t cap = 300000, len = 0;
    char *b = malloc(cap);
    len += snprintf(b + len, cap - len, "{\"result\":{\"results\":[");
    for (int i = off; i < off + size && i < total; i++) {
        len += snprintf(b + len, cap - len,
            "%s{\"channel\":\"%s\",\"topic\":\"%s\",\"title\":\"Folge %d\",\"timestamp\":1727900000,\"duration\":2700,"
            "\"url_video\":\"https://zdf.example/v%d.mp4\",\"url_website\":\"https://www.zdf.example/folge-%d.html\",\"url_subtitle\":\"https://zdf.example/ut%d.xml\"}",
            i > off ? "," : "", size >= 1000 && i == 7 ? "NeuKanal" : "ZDF", i % 3 ? "Terra X" : "Die R\\u00f6mer", i, i, i, i);
    }
    len += snprintf(b + len, cap - len, "],\"queryInfo\":{\"totalResults\":%d}},\"err\":null}", total);
    out->data = b;
    out->len = len;
    return NET_OK;
}

/* --- simulierter Jellyfin-Server --- */
static int jelly_reply(const char *url, const char *post, NetBuf *out, long *status) {
    if (strstr(url, "/Users/AuthenticateByName")) {
        snprintf(last_post, sizeof last_post, "%s", post ? post : "");
        if (!post || !strstr(post, "\"Pw\":\"geheim\"")) { if (status) *status = 401; return reply(out, "{}"); }
        return reply(out, "{\"AccessToken\":\"TOK123\",\"User\":{\"Id\":\"u1\"}}");
    }
    if (strstr(url, "/System/Info")) return reply(out, "{\"Version\":\"10.9\"}");
    if (strstr(url, "/Users/u1/Views"))
        return reply(out, "{\"Items\":[{\"Id\":\"libmov\",\"Name\":\"Filme\",\"Type\":\"CollectionFolder\",\"CollectionType\":\"movies\"},"
                          "{\"Id\":\"libtv\",\"Name\":\"Serien\",\"Type\":\"CollectionFolder\",\"CollectionType\":\"tvshows\"}]}");
    /* Einzel-Item-Info (Typ-Erkennung) */
    if (strstr(url, "/Users/u1/Items/ser1")) return reply(out, "{\"Id\":\"ser1\",\"Name\":\"Testserie\",\"Type\":\"Series\"}");
    if (strstr(url, "/Users/u1/Items/sea1")) return reply(out, "{\"Id\":\"sea1\",\"Name\":\"Staffel 1\",\"Type\":\"Season\",\"SeriesId\":\"ser1\"}");
    if (strstr(url, "/Users/u1/Items/ep1"))  return reply(out, "{\"Id\":\"ep1\",\"Name\":\"Pilot\",\"Type\":\"Episode\"}");
    if (strstr(url, "/Users/u1/Items/mov1")) return reply(out, "{\"Id\":\"mov1\",\"Name\":\"Film H264\",\"Type\":\"Movie\"}");
    if (strstr(url, "/Users/u1/Items/mov2")) return reply(out, "{\"Id\":\"mov2\",\"Name\":\"Film HEVC\",\"Type\":\"Movie\"}");
    /* Staffeln / Folgen */
    if (strstr(url, "/Shows/ser1/Seasons"))
        return reply(out, "{\"Items\":[{\"Id\":\"sea1\",\"Name\":\"Staffel 1\",\"Type\":\"Season\",\"ChildCount\":2}]}");
    if (strstr(url, "/Shows/ser1/Episodes"))
        return reply(out, "{\"Items\":[{\"Id\":\"ep1\",\"Name\":\"Pilot\",\"Type\":\"Episode\",\"IndexNumber\":1,\"ParentIndexNumber\":1,\"RunTimeTicks\":12000000000},"
                          "{\"Id\":\"ep2\",\"Name\":\"Zweite\",\"Type\":\"Episode\",\"IndexNumber\":2,\"ParentIndexNumber\":1}]}");
    /* Items unter einer Bibliothek */
    if (strstr(url, "ParentId=libtv"))
        return reply(out, "{\"Items\":[{\"Id\":\"ser1\",\"Name\":\"Testserie\",\"Type\":\"Series\",\"ProductionYear\":2020,\"ChildCount\":1,\"ImageTags\":{\"Primary\":\"abc\"}}],\"TotalRecordCount\":1}");
    if (strstr(url, "ParentId=libmov"))
        return reply(out, "{\"Items\":[{\"Id\":\"mov1\",\"Name\":\"Film H264\",\"Type\":\"Movie\",\"ProductionYear\":2021},"
                          "{\"Id\":\"mov2\",\"Name\":\"Film HEVC\",\"Type\":\"Movie\",\"ProductionYear\":2022}],\"TotalRecordCount\":2}");
    /* PlaybackInfo */
    if (strstr(url, "/Items/mov1/PlaybackInfo"))
        return reply(out, "{\"MediaSources\":[{\"Id\":\"mov1\",\"Container\":\"mp4\",\"MediaStreams\":[{\"Type\":\"Video\",\"Codec\":\"h264\",\"Height\":720}]}]}");
    if (strstr(url, "/Items/mov2/PlaybackInfo"))
        return reply(out, "{\"MediaSources\":[{\"Id\":\"src2\",\"Container\":\"mkv\",\"MediaStreams\":[{\"Type\":\"Video\",\"Codec\":\"hevc\",\"Height\":1080}]}]}");
    if (strstr(url, "/Items/ep1/PlaybackInfo"))
        return reply(out, "{\"MediaSources\":[{\"Id\":\"ep1\",\"Container\":\"mp4\",\"MediaStreams\":[{\"Type\":\"Video\",\"Codec\":\"h264\",\"Height\":1080}]}]}");
    if (strstr(url, "/Users/u1/Items?searchTerm="))
        return reply(out, "{\"Items\":[{\"Id\":\"mov2\",\"Name\":\"Film HEVC\",\"Type\":\"Movie\",\"ProductionYear\":2022}]}");
    if (status) *status = 404;
    return reply(out, "{}");
}

/* --- simulierte Audiothek-Quellen (radio-browser, iTunes, RSS) --- */
static int audio_reply(const char *url, NetBuf *out, long *status) {
    if (strstr(url, "de1.api.radio-browser.info")) { if (status) *status = 503; return reply(out, "down"); }   /* Ausweichserver testen */
    if (strstr(url, "api.radio-browser.info/json/stations/search")) {
        snprintf(last_url, sizeof last_url, "%s", url);
        return reply(out, "[{\"stationuuid\":\"a\",\"name\":\" 1LIVE \",\"url\":\"http://x/1live\",\"url_resolved\":\"http://wdr.example/1live.mp3\","
            "\"favicon\":\"https://wdr.example/logo.png\",\"codec\":\"MP3\",\"bitrate\":128,\"tags\":\"pop,charts\",\"countrycode\":\"DE\"},"
            "{\"name\":\"Opus-Sender\",\"url_resolved\":\"http://x/opus\",\"codec\":\"OGG\",\"bitrate\":96},"
            "{\"name\":\"HLS-Sender\",\"url_resolved\":\"https://x/radio/master.m3u8\",\"codec\":\"UNKNOWN\",\"favicon\":\"https://x/f.ico\"}]");
    }
    if (strstr(url, "itunes.apple.com/search"))
        return reply(out, "{\"resultCount\":1,\"results\":[{\"collectionName\":\"Tagesschau Podcast\",\"artistName\":\"ARD\","
            "\"feedUrl\":\"https://feed.example/tagesschau.xml\",\"artworkUrl600\":\"https://img.example/ts.jpg\",\"trackCount\":120,\"primaryGenreName\":\"Nachrichten\"}]}");
    if (strstr(url, "rss.applemarketingtools.com"))
        return reply(out, "{\"feed\":{\"results\":[{\"id\":\"111\",\"name\":\"Hit 1\"},{\"id\":\"222\",\"name\":\"Hit 2\"}]}}");
    if (strstr(url, "itunes.apple.com/lookup") && strstr(url, "111,222"))
        return reply(out, "{\"results\":[{\"collectionId\":222,\"collectionName\":\"Hit 2\",\"feedUrl\":\"https://feed.example/2.xml\"},"
            "{\"collectionId\":111,\"collectionName\":\"Hit 1\",\"feedUrl\":\"https://feed.example/1.xml\"}]}");
    if (strstr(url, "feed.example/tagesschau.xml"))
        return reply(out, "<?xml version=\"1.0\"?><rss xmlns:itunes=\"x\"><channel><title>Tagesschau</title>"
            "<itunes:image href=\"https://img.example/kanal.jpg\"/>"
            "<item><title><![CDATA[Folge &amp; Eins]]></title><pubDate>Fri, 02 Oct 2026 20:00:00 +0200</pubDate>"
            "<enclosure url=\"https://media.example/1.mp3?a=1&amp;b=2\" length=\"1\" type=\"audio/mpeg\"/><itunes:duration>754</itunes:duration></item>"
            "<item><title>Ohne Datei</title></item>"
            "<item><title>Zwei</title><enclosure url=\"https://media.example/2.mp3\" type=\"audio/mpeg\"/><itunes:duration>01:02:03</itunes:duration>"
            "<itunes:image href=\"https://img.example/f2.jpg\"/></item></channel></rss>");
    return NET_ERR;
}

/* --- simulierte Pluto-TV-API --- */
static int pluto_boots;
static int pluto_reply(const char *url, const char *hdr, NetBuf *out, long *status) {
    if (strstr(url, "boot.pluto.tv/v4/start")) {
        pluto_boots++;
        return reply(out, "{\"sessionToken\":\"PTOK\",\"stitcherParams\":\"?appName=web&deviceId=abc\","
                          "\"servers\":{\"stitcher\":\"https://stitcher.pluto.example\"}}");
    }
    if (!hdr || !strstr(hdr, "Authorization: Bearer PTOK")) { if (status) *status = 401; return reply(out, "{}"); }
    if (strstr(url, "/v3/vod/categories"))
        return reply(out, "{\"categories\":[{\"name\":\"Top-Filme\",\"totalItemsCount\":250,\"items\":["
            "{\"_id\":\"m1\",\"name\":\"Ein Film\",\"type\":\"movie\",\"genre\":\"Komoedie\",\"duration\":5400000,"
            "\"covers\":[{\"aspectRatio\":\"16:9\",\"url\":\"https://images.pluto.tv/m1.jpg\"}],"
            "\"stitched\":{\"path\":\"https://old.example/v1/stitch/hls/episode/m1/master.m3u8?old=1\"}},"
            "{\"_id\":\"s1\",\"name\":\"Eine Serie\",\"type\":\"series\",\"genre\":\"Krimi\",\"seasonsNumbers\":[1,2]}]},"
            "{\"name\":\"Leer\",\"items\":[]}]}");
    if (strstr(url, "/v3/vod/series/s1/seasons"))
        return reply(out, "{\"name\":\"Eine Serie\",\"seasons\":[{\"number\":2,\"episodes\":[{\"_id\":\"e21\",\"name\":\"Zwei-Eins\",\"number\":1}]},"
            "{\"number\":1,\"episodes\":[{\"_id\":\"e12\",\"name\":\"Zweite\",\"number\":2,\"duration\":1300000},{\"_id\":\"e11\",\"name\":\"Erste\",\"number\":1}]}]}");
    return reply(out, "{}");
}

/* --- simulierte YouTube-InnerTube-API --- */
static int yt_reply(const char *url, const char *post, const char *hdr, NetBuf *out) {
    snprintf(last_post, sizeof last_post, "%s", post ? post : "");
    if (strstr(url, "/youtubei/v1/guide"))
        return reply(out, "{\"responseContext\":{\"visitorData\":\"VISITOR123\"},\"items\":[]}");
    if (strstr(url, "/youtubei/v1/navigation/resolve_url")) {
        if (post && strstr(post, "@pokemon\""))
            return reply(out, "{\"endpoint\":{\"browseEndpoint\":{\"browseId\":\"UCpoke\"}}}");
        return reply(out, "{\"error\":{\"message\":\"not found\"}}");
    }
    if (strstr(url, "/youtubei/v1/search")) {
        if (post && strstr(post, "\"continuation\""))
            return reply(out, "{\"onResponseReceivedCommands\":[{\"appendContinuationItemsAction\":{\"continuationItems\":["
                "{\"itemSectionRenderer\":{\"contents\":[{\"videoRenderer\":{\"videoId\":\"vid3\",\"title\":{\"runs\":[{\"text\":\"Seite 2\"}]}}}]}}]}}]}");
        return reply(out, "{\"responseContext\":{\"visitorData\":\"VISITOR123\"},\"contents\":{\"twoColumnSearchResultsRenderer\":{\"primaryContents\":{\"sectionListRenderer\":{\"contents\":["
            "{\"itemSectionRenderer\":{\"contents\":["
            "{\"channelRenderer\":{\"channelId\":\"UCpoke\",\"title\":{\"simpleText\":\"Pokemon\"},\"videoCountText\":{\"runs\":[{\"text\":\"3000 Videos\"}]}}},"
            "{\"videoRenderer\":{\"videoId\":\"vid1\",\"title\":{\"runs\":[{\"text\":\"Pokemon Folge 1\"}]},\"ownerText\":{\"runs\":[{\"text\":\"Pokemon\"}]},"
            "\"lengthText\":{\"simpleText\":\"22:10\"},\"navigationEndpoint\":{\"watchEndpoint\":{\"videoId\":\"vid1\"}}}},"
            "{\"lockupViewModel\":{\"contentId\":\"vid2\",\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Pokemon Folge 2\"}}},"
            "\"contentImage\":{\"thumbnailViewModel\":{\"overlays\":[{\"thumbnailBadgeViewModel\":{\"text\":\"21:55\"}}]}}}}"
            "]}},"
            "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":{\"token\":\"TOKEN2\"}}}}"
            "]}}}}}");
    }
    if (strstr(url, "/youtubei/v1/browse")) {
        if (post && strstr(post, "\"VLPLstaffel1\""))
            return reply(out, "{\"contents\":{\"playlistVideoListRenderer\":{\"contents\":["
                "{\"playlistVideoRenderer\":{\"videoId\":\"e1\",\"index\":{\"simpleText\":\"1\"},\"title\":{\"runs\":[{\"text\":\"Pika!\"}]},\"lengthText\":{\"simpleText\":\"22:00\"}}},"
                "{\"playlistVideoRenderer\":{\"videoId\":\"e2\",\"index\":{\"simpleText\":\"2\"},\"title\":{\"runs\":[{\"text\":\"Glurak\"}]}}}]}}}");
        if (post && strstr(post, "EglwbGF5bGlzdHPyBgQKAkIA"))
            return reply(out, "{\"header\":{\"pageHeaderRenderer\":{\"channelId\":\"UCpoke\",\"title\":{\"simpleText\":\"Pokemon\"}}},"
                "\"contents\":[{\"lockupViewModel\":{\"contentId\":\"PLstaffel1\",\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\","
                "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Staffel 1\"}}}}}]}");
        if (post && strstr(post, "EgZ2aWRlb3PyBgQKAjoA"))
            return reply(out, "{\"contents\":[{\"richItemRenderer\":{\"content\":{\"videoRenderer\":{\"videoId\":\"cv1\",\"title\":{\"runs\":[{\"text\":\"Neuestes Video\"}]},"
                "\"publishedTimeText\":{\"simpleText\":\"vor 2 Tagen\"}}}}}]}");
    }
    if (strstr(url, "/youtubei/v1/player")) {
        if (!hdr || !strstr(hdr, "X-Youtube-Client-Name: 101") || !strstr(hdr, "X-Goog-Visitor-Id: VISITOR123"))
            return reply(out, "{\"playabilityStatus\":{\"status\":\"ERROR\",\"reason\":\"falscher Client\"}}");
        if (post && strstr(post, "\"altersfrei\""))
            return reply(out, "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Melde dich an\"}}");
        return reply(out, "{\"playabilityStatus\":{\"status\":\"OK\"},\"streamingData\":{\"hlsManifestUrl\":\"https://manifest.googlevideo.com/api/manifest/hls_variant/id/x/file/index.m3u8\"}}");
    }
    return reply(out, "{}");
}

/* --- simulierte Website fuer den Explorer --- */
static int explorer_reply(const char *url, const char *post, NetBuf *out) {
    /* Startseite mit Suchformular und Inhaltslinks */
    if (!strcmp(url, "https://kino.example/"))
        return reply(out, "<html><head><title>Kino</title></head><body>"
            "<form role='search' action='/suche' method='get'><input type='text' name='q'></form>"
            "<a href='/film/matrix'><img src='/img/m.jpg' alt='Matrix'>Matrix</a>"
            "<a href='/film/avatar'>Avatar (2009)</a>"
            "<a href='/impressum'>Impressum</a>"
            "<a href='https://fremd.example/x'>Fremd</a></body></html>");
    if (strstr(url, "kino.example/suche?") || (post && strstr(url, "kino.example")))
        return reply(out, "<html><body>"
            "<a href='/film/matrix'>Matrix</a><a href='/film/matrix-reloaded'>Matrix Reloaded</a>"
            "<a href='/film/avatar'>Avatar</a><a href='/impressum'>Impressum</a></body></html>");
    if (!strcmp(url, "https://kino.example/film/matrix"))
        return reply(out, "<html><head><meta property='og:title' content='Matrix'>"
            "<meta property='og:image' content='/img/matrix.jpg'></head><body>"
            "<iframe src='https://archive.org/embed/matrix1999'></iframe>"
            "<div class='mirror' data-src='https://voe.example/e/abcd'>Hoster 2</div>"
            "<a href='/film/matrix-staffel-nix'>x</a></body></html>");
    /* offener Hoster (archive.org) */
    if (strstr(url, "archive.org/metadata/matrix1999"))
        return reply(out, "{\"files\":[{\"name\":\"matrix_512kb.mp4\",\"size\":\"5000\"},"
            "{\"name\":\"matrix.mp4\",\"size\":\"90000\"},{\"name\":\"cover.jpg\",\"size\":\"10\"}]}");
    /* verschleierter Hoster: kein offener Videolink */
    if (strstr(url, "voe.example/e/abcd"))
        return reply(out, "<html><body><script>eval(atob('...'))</script></body></html>");
    return NET_ERR;
}

int net_request(const char *url, const char *post, const char *hdr, NetBuf *out, long *status, char *final_url, int fl) {
    memset(out, 0, sizeof *out);
    if (status) *status = 200;
    if (final_url) snprintf(final_url, fl, "%s", url);
    snprintf(last_headers, sizeof last_headers, "%s", hdr ? hdr : "");
    if (net_check_url(url) == NET_BLOCKED) return NET_BLOCKED;
    if (strstr(url, "youtube.com/youtubei/")) return yt_reply(url, post, hdr, out);
    if (strstr(url, "pluto.tv")) return pluto_reply(url, hdr, out, status);
    if (strstr(url, "radio-browser.info") || strstr(url, "itunes.apple.com") || strstr(url, "applemarketingtools") || strstr(url, "feed.example"))
        return audio_reply(url, out, status);
    if (strstr(url, "archive.org/advancedsearch.php")) {
        snprintf(last_url, sizeof last_url, "%s", url);
        return reply(out, "{\"response\":{\"numFound\":2,\"docs\":["
            "{\"identifier\":\"notld\",\"title\":\"Night of the Living Dead\",\"year\":\"1968\"},"
            "{\"identifier\":\"his_girl\",\"title\":[\"His Girl Friday\"]}]}}");
    }
    if (strstr(url, "archive.org/metadata/notld"))
        return reply(out, "{\"files\":[{\"name\":\"notld.ogv\",\"size\":\"100\"},"
            "{\"name\":\"notld_512kb.mp4\",\"size\":\"3000\"},{\"name\":\"notld.mp4\",\"size\":\"90000\"}]}");
    if (strstr(url, "kino.example") || strstr(url, "archive.org") || strstr(url, "voe.example"))
        return explorer_reply(url, post, out);
    if (strstr(url, "jelly.example")) return jelly_reply(url, post, out, status);
    if (strstr(url, "mediathekviewweb")) {
        snprintf(last_post, sizeof last_post, "%s", post ? post : "");
        return mediathek_reply(post ? post : "", out);
    }
    /* eigene Playlist */
    if (strstr(url, "liste.m3u"))
        return reply(out, "#EXTM3U\r\n#EXTINF:-1 tvg-id=\"a\" tvg-logo=\"https://logo.example/a.png\" group-title=\"News\",Kanal A\r\n"
                          "#EXTVLCOPT:http-referrer=https://ref.example/\r\nhttps://a.example/a.m3u8\r\n"
                          "#EXTINF:-1 group-title=\"Sport\",Kanal B, mit Komma\nhttps://b.example/b.m3u8\n"
                          "#EXTINF:-1 group-title=\"News\",Kanal C\nhttps://c.example/c.mp4\n");
    /* iptv-org */
    if (strstr(url, "iptv-org.github.io/iptv/countries/de.m3u") || strstr(url, "iptv-org.github.io/iptv/index.m3u")) {
        static char *big;
        if (!big) {
            size_t cap = 200000, len = 0;
            big = malloc(cap);
            len += snprintf(big + len, cap - len, "#EXTM3U\n");
            for (int i = 0; i < 130; i++)
                len += snprintf(big + len, cap - len,
                    "#EXTINF:-1 tvg-logo=\"https://logo.example/%d.png\" group-title=\"%s\",%s Sender %d\nhttps://%s.example/%d.m3u8\n",
                    i, i % 2 ? "Movies" : "Animation", i % 10 == 0 ? "Pluto TV" : "Kanal", i,
                    i % 10 == 0 ? "service-stitcher.pluto" : "tv", i);
        }
        return reply(out, big);
    }
    if (strstr(url, "iptv-org.github.io/api/categories.json"))
        return reply(out, "[{\"id\":\"animation\",\"name\":\"Animation\"},{\"id\":\"xxx\",\"name\":\"XXX\"},{\"id\":\"news\",\"name\":\"News\"}]");
    if (strstr(url, "iptv-org.github.io/api/countries.json"))
        return reply(out, "[{\"name\":\"Germany\",\"code\":\"DE\"},{\"name\":\"Austria\",\"code\":\"AT\"}]");
    /* Website-Quelle (sites.txt) */
    if (strstr(url, "example.org/neu") || strstr(url, "example.org/suche"))
        return reply(out, "<html><a class=\"video\" href=\"/video/1\">Film &amp; Eins</a> <a class=\"video\" href=\"/video/2\"><b>Zwei</b></a>"
                          "<a class=\"video\" href=\"/video/1\">Film &amp; Eins</a></html>");
    if (strstr(url, "example.org/video/1"))
        return reply(out, "<div><iframe width=1 src=\"https://player.example.net/e/42\"></iframe></div>");
    if (strstr(url, "example.org/video/2"))
        return reply(out, "<iframe src=\"https://ads.adnet.example/e/1\"></iframe>");
    if (strstr(url, "player.example.net/e/42"))
        return reply(out, "<script>var src=\"https:\\/\\/cdn.example.net\\/hls\\/master.m3u8?t=1\";</script>");
    /* Website-Scanner */
    if (!strcmp(url, "https://scan.example/"))
        return reply(out, "<html><head><title>Scan Startseite</title></head><body>"
                          "<a href=\"/impressum\">Impressum</a><a href=\"/videos/folge-1\">Folge 1</a>"
                          "<a href=\"/style.css\">x</a><a href=\"https://other.example/x\">fremd</a>"
                          "<video src=\"/media/intro.mp4\"></video></body></html>");
    if (!strcmp(url, "https://scan.example/videos/folge-1"))
        return reply(out, "<html><head><meta property=\"og:title\" content=\"Folge 1 &amp; mehr\">"
                          "<meta property=\"og:image\" content=\"/img/f1.jpg\"></head>"
                          "<script>{\"hls\":\"https:\\/\\/cdn.scan.example\\/f1\\/index.m3u8\"}</script>"
                          "<iframe src=\"https://embed.example/p/9\"></iframe></html>");
    if (!strcmp(url, "https://scan.example/impressum"))
        return reply(out, "<html><head><title>Livestream</title><meta property=\"og:video\" content=\"https://cdn.scan.example/live/stream\"></head></html>");
    if (!strcmp(url, "https://embed.example/p/9"))
        return reply(out, "<source src=\"https://cdn.embed.example/v9.mp4\" type=\"video/mp4\">");
    /* South Park */
    if (strstr(url, "southpark.de/seasons/south-park?json=true"))
        return reply(out, "{\"children\":[{\"type\":\"SeasonSelector\",\"props\":{\"items\":["
                          "{\"label\":\"Staffel 2\",\"url\":\"/seasons/south-park/a1/staffel-2\"}]}},"
                          "{\"type\":\"LineList\",\"props\":{\"items\":[{\"url\":\"/folgen/r1/south-park-rueckkehr-staffel-1-ep-1\",\"title\":\"Cartman und die Analsonde\"}]}}]}");
    if (strstr(url, "southpark.de/seasons/south-park/a1/staffel-2?json=true"))
        return reply(out, "{\"children\":[{\"type\":\"LineList\",\"props\":{\"items\":["
                          "{\"url\":\"/folgen/x3/south-park-spaeter-staffel-2-ep-3\",\"title\":\"Spaetere Folge\"},"
                          "{\"url\":\"/folgen/x1/south-park-cartman-staffel-1-ep-1\",\"meta\":{\"header\":{\"title\":\"Cartman\"},\"subHeader\":\"Staffel 1 Ep 1\"},"
                          "\"media\":{\"image\":{\"url\":\"https://images.paramount.tech/uri/mgid:x1\"}}}],"
                          "\"loadMore\":{\"url\":\"/api/more/a1/2\"}}}]}");
    if (strstr(url, "southpark.de/api/more/a1/2?json=true"))
        return reply(out, "{\"items\":[{\"url\":\"/folgen/x2/south-park-weight-staffel-1-ep-2\",\"title\":\"Weight Gain\"}]}");
    if (strstr(url, "southpark.de/folgen/x1/south-park-cartman-staffel-1-ep-1?json=true"))
        return reply(out, "{\"children\":[{\"type\":\"Player\",\"props\":{\"videoDetail\":{\"mgid\":\"mgid:x1\","
                          "\"videoServiceUrl\":\"https://topaz.example/video/x1?foo=1\"}}}]}");
    if (strstr(url, "topaz.example/video/x1?clientPlatform=desktop"))
        return reply(out, "{\"stitchedstream\":{\"manifesttype\":\"hls\",\"source\":\"https://dai.example/sp/x1/master.m3u8\"}}");
    return NET_ERR;
}

static int wait_job(void) {
    for (int i = 0; i < 1500 && plugins_job_state() == JOB_RUNNING; i++) usleep(2000);
    return plugins_job_state();
}
static int find_src(const char *name) {
    for (int i = 0; i < plugins_source_count(); i++) if (strstr(plugins_source(i)->name, name)) return i;
    return -1;
}
static int browse(int src, const char *id, PluginList *l) {
    plugins_start_browse(src, id);
    int st = wait_job();
    if (st != JOB_DONE) { printf("  browse(%s): %s\n", id ? id : "nil", plugins_job_error()); plugins_job_reset(); memset(l, 0, sizeof *l); return -1; }
    plugins_take_list(l);
    return 0;
}
static int search_ctx(int src, const char *q, const char *ctx, PluginList *l) {
    plugins_start_search_ctx(src, q, ctx);
    int st = wait_job();
    if (st != JOB_DONE) { printf("  search(%s): %s\n", q, plugins_job_error()); plugins_job_reset(); memset(l, 0, sizeof *l); return -1; }
    plugins_take_list(l);
    return 0;
}
static int search(int src, const char *q, PluginList *l) {
    plugins_start_search(src, q);
    int st = wait_job();
    if (st != JOB_DONE) { printf("  search(%s): %s\n", q, plugins_job_error()); plugins_job_reset(); memset(l, 0, sizeof *l); return -1; }
    plugins_take_list(l);
    return 0;
}
static int resolve(int src, PluginItem *it, StreamInfo *si) {
    plugins_start_resolve(src, it);
    int st = wait_job();
    if (st != JOB_DONE) { printf("  resolve: %s\n", plugins_job_error()); plugins_job_reset(); return -1; }
    plugins_take_stream(si);
    return 0;
}
static int find_item(PluginList *l, const char *title) {
    for (int i = 0; i < l->count; i++) if (strstr(l->items[i].title, title)) return i;
    return -1;
}

int main(void) {
    g_cfg.adblock_enabled = 1;
    adblock_load(VS_DATA_DIR "/blocklist.txt");
    int n = plugins_init();
    printf("Quellen: %d  (Log: %s)\n", n, plugins_last_log());
    for (int i = 0; i < n; i++) printf("  - %s [%s] search=%d browse=%d\n", plugins_source(i)->name, plugins_source(i)->file,
                                       plugins_source(i)->has_search, plugins_source(i)->has_browse);
    PluginList l, l2; StreamInfo si;

    /* ---------- Mediathek: Suche-Eintrag, Sender -> Kategorien, Reihen-Ordner, Seiten ---------- */
    int m = find_src("Mediathek"); CHECK(m >= 0);
    CHECK(browse(m, NULL, &l) == 0 && l.count >= 4 && l.items[0].kind == ITEM_SEARCH && find_item(&l, "Deutsch") == 1);
    CHECK(find_item(&l, "English") >= 0 && find_item(&l, "Kroatisch") >= 0);
    plugins_list_free(&l);
    /* englische Mediathek: Internet Archive, Kategorie + Wiedergabe + Suche */
    CHECK(browse(m, "lang:en", &l) == 0 && l.items[0].kind == ITEM_SEARCH && find_item(&l, "Dokumentationen") > 0);
    char ia_cat[64]; snprintf(ia_cat, sizeof ia_cat, "%s", l.count > 1 ? l.items[1].id : "");
    plugins_list_free(&l);
    CHECK(browse(m, ia_cat, &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "Night of the Living Dead"));
    CHECK(strstr(last_url, "advancedsearch.php") && strstr(last_url, "English"));
    if (l.count) {
        CHECK(l.items[0].thumb && strstr(l.items[0].thumb, "archive.org/services/img/notld"));
        CHECK(resolve(m, &l.items[0], &si) == 0 && !strcmp(si.url, "https://archive.org/download/notld/notld_512kb.mp4"));
    }
    plugins_list_free(&l);
    CHECK(search_ctx(m, "zombie", "iasearch:hr", &l) == 0 && l.count >= 1);
    CHECK(strstr(last_url, "Croatian") && strstr(last_url, "zombie"));
    plugins_list_free(&l);
    CHECK(browse(m, "lang:de", &l) == 0 && l.count > 10 && l.items[0].kind == ITEM_SEARCH);
    CHECK(find_item(&l, "NeuKanal") > 0 && strstr(l.items[1].subtitle, "aktualisiert"));   /* neu entdeckter Sender */
    {
        FILE *cf = fopen(VS_DATA_DIR "/mediathek_channels.txt", "r");
        char cb[4096] = ""; if (cf) { fread(cb, 1, sizeof cb - 1, cf); fclose(cf); }
        CHECK(strstr(cb, "NeuKanal") && strstr(cb, "ARD"));
    }
    int zdf = find_item(&l, "ZDF");
    char zid[64]; snprintf(zid, sizeof zid, "%s", zdf >= 0 ? l.items[zdf].id : "");
    plugins_list_free(&l);
    CHECK(browse(m, zid, &l) == 0 && find_item(&l, "Neueste") == 0 && find_item(&l, "Filme") > 0 && find_item(&l, "Sendungen") == 1);
    char new_id[64], cat_id[64], top_id[64];
    snprintf(new_id, sizeof new_id, "%s", l.items[0].id);
    snprintf(top_id, sizeof top_id, "%s", l.items[1].id);
    snprintf(cat_id, sizeof cat_id, "%s", l.items[find_item(&l, "Filme")].id);
    plugins_list_free(&l);
    /* Neueste: Folgen derselben Reihe stecken in einem Ordner */
    CHECK(browse(m, new_id, &l) == 0 && l.count == 2 && l.items[0].kind == ITEM_FOLDER && l.items[1].kind == ITEM_FOLDER);
    char series[200]; snprintf(series, sizeof series, "%s", l.count ? l.items[0].id : "");
    if (l.count == 2) printf("  Reihen: %s (%s) | %s (%s)\n", l.items[0].title, l.items[0].subtitle, l.items[1].title, l.items[1].subtitle);
    CHECK(l.count && l.items[0].thumb && !strncmp(l.items[0].thumb, "og:https://www.zdf.example/", 27));
    plugins_list_free(&l);
    /* Reihe öffnen: nur Folgen genau dieser Reihe, seitenweise */
    CHECK(browse(m, series, &l) == 0 && l.count > 1 && l.items[l.count - 1].kind == ITEM_MORE);
    for (int page = 0; page < 3 && l.count && l.items[l.count - 1].kind == ITEM_MORE; page++) {
        int mi = l.count - 1;
        CHECK(browse(m, l.items[mi].id, &l2) == 0);
        plugins_list_append(&l, &l2, mi);
    }
    int wrong = 0;
    for (int i = 0; i < l.count; i++) if (strstr(l.items[i].title, "Folge") == NULL) wrong++;
    printf("  Reihe '%s': %d Folgen nach allen Seiten, erste: %s\n", series, l.count, l.count ? l.items[0].title : "-");
    CHECK(l.count == 40 && wrong == 0 && l.items[l.count - 1].kind == ITEM_VIDEO);
    CHECK(strstr(last_post, "\"sortOrder\":\"asc\"") != NULL);
    CHECK(l.count >= 2 && !strcmp(l.items[0].title, "Folge 0") && !strcmp(l.items[1].title, "Folge 3"));
    if (l.count) {
        CHECK(resolve(m, &l.items[0], &si) == 0 && !strcmp(si.url, "https://zdf.example/v0.mp4"));
        CHECK(si.nsubs == 1 && !strcmp(si.sub_url[0], "https://zdf.example/ut0.xml") && strstr(si.sub_label[0], "Deutsch"));
    }
    plugins_list_free(&l);
    CHECK(browse(m, cat_id, &l) == 0 && l.count == 2);
    CHECK(strstr(last_post, "\"query\":\"film\"") && strstr(last_post, "\"duration_min\":4200") && strstr(last_post, "\"query\":\"ZDF\""));
    plugins_list_free(&l);
    CHECK(browse(m, top_id, &l) == 0 && l.count == 2 && l.items[0].kind == ITEM_FOLDER);
    plugins_list_free(&l);
    CHECK(search(m, "Römer", &l) == 0 && l.count == 2);
    plugins_list_free(&l);

    /* ---------- M3U-Playlists: Logos, Header, Bearbeiten ---------- */
    int p = find_src("M3U"); CHECK(p >= 0);
    CHECK(browse(p, NULL, &l) == 0 && l.count >= 3);
    int ti = find_item(&l, "Test-Liste"), li = find_item(&l, "Lokal");
    char tid[64], lid[64];
    snprintf(tid, sizeof tid, "%s", ti >= 0 ? l.items[ti].id : "");
    snprintf(lid, sizeof lid, "%s", li >= 0 ? l.items[li].id : "");
    /* Online-Liste: Aktionen auf Playlist-Ebene */
    PluginAction acts[8];
    int na = ti >= 0 ? plugins_item_actions(p, &l.items[ti], acts, 8) : 0;
    printf("  Aktionen Online-Liste: %d (%s, ...)\n", na, na ? acts[0].label : "-");
    CHECK(na == 3 && !strcmp(acts[0].id, "pl_copy"));
    /* bearbeitbare Kopie anlegen */
    char msg[256]; int refresh = 0;
    plugins_start_action(p, &l.items[ti], "pl_copy", "Meine Kopie");
    CHECK(wait_job() == JOB_DONE && plugins_take_action_result(msg, sizeof msg, &refresh) == 0 && refresh);
    printf("  Kopie: %s\n", msg);
    plugins_list_free(&l);
    CHECK(browse(p, NULL, &l) == 0 && find_item(&l, "Meine Kopie") >= 0);
    plugins_list_free(&l);

    CHECK(browse(p, tid, &l) == 0 && l.count == 3);
    if (l.count == 3) {
        CHECK(l.items[0].thumb && !strcmp(l.items[0].thumb, "https://logo.example/a.png"));
        CHECK(resolve(p, &l.items[0], &si) == 0 && !strcmp(si.url, "https://a.example/a.m3u8") && !strcmp(si.headers, "Referer: https://ref.example/"));
        CHECK(!strcmp(l.items[1].title, "Kanal B, mit Komma"));
        na = plugins_item_actions(p, &l.items[0], acts, 8);
        CHECK(na == 1 && !strcmp(acts[0].id, "check"));      /* online: nur prüfen */
    }
    plugins_list_free(&l);

    /* lokale Liste: löschen, umbenennen, verschieben, defekte entfernen */
    CHECK(browse(p, lid, &l) == 0 && l.count == 4);
    if (l.count == 4) {
        na = plugins_item_actions(p, &l.items[1], acts, 8);
        CHECK(na == 5);
        plugins_start_action(p, &l.items[1], "delete", NULL);
        CHECK(wait_job() == JOB_DONE && plugins_take_action_result(msg, sizeof msg, &refresh) == 0 && refresh);
        plugins_list_free(&l);
        CHECK(browse(p, lid, &l) == 0 && l.count == 3 && find_item(&l, "Zwei") < 0);
        plugins_start_action(p, &l.items[0], "rename", "Erster Sender");
        CHECK(wait_job() == JOB_DONE); plugins_take_action_result(msg, sizeof msg, &refresh);
        plugins_start_action(p, &l.items[2], "up", NULL);
        CHECK(wait_job() == JOB_DONE); plugins_take_action_result(msg, sizeof msg, &refresh);
        plugins_list_free(&l);
        CHECK(browse(p, lid, &l) == 0 && l.count == 3 && !strcmp(l.items[0].title, "Erster Sender") && !strcmp(l.items[1].title, "Kaputt"));
        plugins_list_free(&l);
        /* auf Playlist-Ebene: defekte Streams entfernen */
        CHECK(browse(p, NULL, &l) == 0);
        li = find_item(&l, "Lokal");
        if (li >= 0) {
            plugins_start_action(p, &l.items[li], "pl_check", NULL);
            CHECK(wait_job() == JOB_DONE && plugins_take_action_result(msg, sizeof msg, &refresh) == 0);
            printf("  Pruefung: %s\n", msg);
        }
        plugins_list_free(&l);
        CHECK(browse(p, lid, &l) == 0 && l.count == 2 && find_item(&l, "Kaputt") < 0);
        plugins_list_free(&l);
        /* Playlist löschen */
        CHECK(browse(p, NULL, &l) == 0);
        li = find_item(&l, "Lokal");
        int before = l.count;
        if (li >= 0) { plugins_start_action(p, &l.items[li], "pl_delete", NULL); CHECK(wait_job() == JOB_DONE); plugins_take_action_result(msg, sizeof msg, &refresh); }
        plugins_list_free(&l);
        CHECK(browse(p, NULL, &l) == 0 && l.count == before - 1 && find_item(&l, "Lokal") < 0);
    }
    plugins_list_free(&l);

    /* ---------- Sender-Finder ---------- */
    int f = find_src("Sender-Finder"); CHECK(f >= 0);
    CHECK(browse(f, NULL, &l) == 0 && find_item(&l, "Kostenlose Dienste") >= 0);
    plugins_list_free(&l);
    CHECK(browse(f, "list:countries/de.m3u||0", &l) == 0 && l.count == 61 && l.items[60].kind == ITEM_MORE);
    CHECK(l.items[0].thumb != NULL);
    plugins_list_free(&l);
    CHECK(browse(f, "list:countries/de.m3u|pluto|0", &l) == 0 && l.count == 13);
    printf("  Finder Pluto-Filter: %d Sender, erster: %s\n", l.count, l.count ? l.items[0].title : "-");
    plugins_list_free(&l);
    CHECK(browse(f, "cats", &l) == 0 && l.count == 2 && find_item(&l, "Zeichentrick") >= 0 && find_item(&l, "XXX") < 0);
    plugins_list_free(&l);
    CHECK(browse(f, "countries", &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "Austria"));
    plugins_list_free(&l);
    CHECK(search(f, "sender 12", &l) == 0 && l.count >= 1);
    plugins_list_free(&l);

    /* ---------- Website-Scanner ---------- */
    int d = find_src("Direkte"); CHECK(d >= 0);
    CHECK(search(d, "scan.example", &l) == 0);
    printf("  Scanner: %d Treffer\n", l.count);
    for (int i = 0; i < l.count; i++) printf("    - %s | %s | %s\n", l.items[i].title, l.items[i].subtitle, l.items[i].thumb ? l.items[i].thumb : "-");
    CHECK(l.count == 4 && find_item(&l, "Livestream") >= 0);
    int fi = find_item(&l, "Folge 1 & mehr");
    CHECK(fi >= 0);
    if (fi >= 0) {
        CHECK(l.items[fi].thumb && !strcmp(l.items[fi].thumb, "https://scan.example/img/f1.jpg"));
        CHECK(resolve(d, &l.items[fi], &si) == 0 && !strcmp(si.url, "https://cdn.scan.example/f1/index.m3u8") &&
              strstr(si.headers, "Referer: https://scan.example/videos/folge-1"));
    }
    CHECK(find_item(&l, "(Player)") >= 0);
    plugins_list_free(&l);
    CHECK(browse(d, NULL, &l) == 0 && l.count >= 2 && l.items[0].kind == ITEM_SEARCH && l.items[1].kind == ITEM_FOLDER);   /* Eingabe + Verlauf */
    plugins_list_free(&l);
    CHECK(search(d, "nichts hier", &l) < 0);
    CHECK(search(d, " https://x.example/a.mp4 ", &l) == 0 && l.count == 1);
    if (l.count) { CHECK(resolve(d, &l.items[0], &si) == 0 && !strcmp(si.url, "https://x.example/a.mp4")); }
    plugins_list_free(&l);
    plugins_start_search(d, "https://doubleclick.net/x.mp4"); wait_job(); plugins_take_list(&l);
    if (l.count) { plugins_start_resolve(d, &l.items[0]); CHECK(wait_job() == JOB_ERROR); printf("  gesperrter stream -> %s\n", plugins_job_error()); plugins_job_reset(); }
    plugins_list_free(&l);

    /* ---------- Website-Quelle (sites.txt) ---------- */
    int w = find_src("Beispielseite"); CHECK(w >= 0);
    CHECK(search(w, "a b&c", &l) == 0 && l.count == 2);
    if (l.count == 2) {
        CHECK(!strcmp(l.items[0].title, "Film & Eins") && !strcmp(l.items[1].title, "Zwei"));
        CHECK(resolve(w, &l.items[0], &si) == 0 && !strcmp(si.url, "https://cdn.example.net/hls/master.m3u8?t=1"));
        plugins_start_resolve(w, &l.items[1]); CHECK(wait_job() == JOB_ERROR); plugins_job_reset();
    }
    plugins_list_free(&l);

    /* ---------- South Park ---------- */
    int sp = find_src("South Park"); CHECK(sp >= 0);
    CHECK(browse(sp, NULL, &l) == 0 && l.count == 2);
    char root[128]; snprintf(root, sizeof root, "%s", l.count ? l.items[0].id : "");
    plugins_list_free(&l);
    CHECK(browse(sp, root, &l) == 0 && l.count == 2);
    if (l.count == 2) printf("  South Park Staffeln: %s, %s\n", l.items[0].title, l.items[1].title);
    CHECK(l.count == 2 && !strcmp(l.items[0].title, "Staffel 1") && !strcmp(l.items[1].title, "Staffel 2"));
    char s1[160], s2[160];
    snprintf(s1, sizeof s1, "%s", l.count == 2 ? l.items[0].id : "");
    snprintf(s2, sizeof s2, "%s", l.count == 2 ? l.items[1].id : "");
    plugins_list_free(&l);
    CHECK(browse(sp, s1, &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Cartman und die Analsonde"));
    plugins_list_free(&l);
    CHECK(browse(sp, s2, &l) == 0 && l.count == 3 && l.items[2].kind == ITEM_MORE);
    if (l.count == 3) printf("  Folgen sortiert: %s, %s\n", l.items[0].title, l.items[1].title);
    CHECK(l.count == 3 && !strcmp(l.items[1].title, "Spaetere Folge"));
    if (l.count == 3) {
        CHECK(!strcmp(l.items[0].title, "Cartman") && l.items[0].thumb && strstr(l.items[0].thumb, "images.paramount.tech"));
        CHECK(resolve(sp, &l.items[0], &si) == 0 && !strcmp(si.url, "https://dai.example/sp/x1/master.m3u8"));
        CHECK(browse(sp, l.items[2].id, &l2) == 0 && l2.count == 1 && !strcmp(l2.items[0].title, "Weight Gain"));
        plugins_list_free(&l2);
    }
    plugins_list_free(&l);

    /* ---------- Jellyfin ---------- */
    int jf = find_src("Jellyfin"); CHECK(jf >= 0);
    /* ohne Zugangsdaten: Einrichtungshinweis */
    CHECK(browse(jf, NULL, &l) == 0 && l.count == 4 && find_item(&l, "Noch nicht eingerichtet") == 0 && l.items[3].kind == ITEM_SEARCH);
    plugins_list_free(&l);
    /* falsche Zugangsdaten */
    CHECK(search_ctx(jf, "http://jelly.example | max | falsch", "login", &l) < 0);
    /* Anmeldung ueber Suchkontext */
    CHECK(search_ctx(jf, "http://jelly.example | max | geheim", "login", &l) == 0);
    CHECK(strstr(last_post, "\"Username\":\"max\"") && strstr(last_post, "\"Pw\":\"geheim\""));
    CHECK(find_item(&l, "Filme") >= 0 && find_item(&l, "Serien") >= 0 && find_item(&l, "Suchen") >= 0);
    char lib_tv[64] = "", lib_mov[64] = "";
    for (int i = 0; i < l.count; i++) {
        if (!strcmp(l.items[i].title, "Serien")) snprintf(lib_tv, sizeof lib_tv, "%s", l.items[i].id);
        if (!strcmp(l.items[i].title, "Filme"))  snprintf(lib_mov, sizeof lib_mov, "%s", l.items[i].id);
    }
    plugins_list_free(&l);
    /* Serien-Bibliothek -> Serie -> Staffeln -> Folgen (in Reihenfolge) */
    CHECK(browse(jf, lib_tv, &l) == 0 && l.count == 1 && l.items[0].kind == ITEM_FOLDER);
    CHECK(l.items[0].thumb && strstr(l.items[0].thumb, "/Items/ser1/Images/Primary") && strstr(l.items[0].thumb, "api_key=TOK123"));
    char ser[64]; snprintf(ser, sizeof ser, "%s", l.items[0].id);
    plugins_list_free(&l);
    CHECK(browse(jf, ser, &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Staffel 1"));
    char sea[64]; snprintf(sea, sizeof sea, "%s", l.items[0].id);
    plugins_list_free(&l);
    CHECK(browse(jf, sea, &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "1. Pilot") && !strcmp(l.items[1].title, "2. Zweite"));
    CHECK(l.items[0].kind == ITEM_VIDEO);
    /* Folge (1080p h264) -> Transkodierung erzwungen */
    CHECK(resolve(jf, &l.items[0], &si) == 0 && strstr(si.url, "/Videos/ep1/master.m3u8") && strstr(si.url, "VideoCodec=h264") && strstr(si.url, "MaxHeight=720"));
    plugins_list_free(&l);
    /* Filme: direktes Abspielen (h264/720p) vs. Transkodierung (hevc/1080p) */
    CHECK(browse(jf, lib_mov, &l) == 0 && l.count == 2);
    int im1 = find_item(&l, "Film H264"), im2 = find_item(&l, "Film HEVC");
    CHECK(im1 >= 0 && im2 >= 0);
    CHECK(resolve(jf, &l.items[im1], &si) == 0 && strstr(si.url, "/Videos/mov1/stream") && strstr(si.url, "static=true"));
    CHECK(resolve(jf, &l.items[im2], &si) == 0 && strstr(si.url, "master.m3u8") && strstr(si.url, "MediaSourceId=src2"));
    plugins_list_free(&l);
    /* Suche auf dem Server */
    CHECK(search(jf, "hevc", &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Film HEVC"));
    plugins_list_free(&l);

    /* ---------- Website-Explorer ---------- */
    int ex = find_src("Explorer"); CHECK(ex >= 0);
    /* Website oeffnen: Such-Eintrag (Formular erkannt) + Inhaltslinks */
    CHECK(search(ex, "kino.example", &l) == 0 && l.count >= 3 && l.items[0].kind == ITEM_SEARCH);
    CHECK(strstr(l.items[0].id, "site:https://kino.example/") != NULL);
    CHECK(strstr(l.items[0].subtitle, "erkannt") != NULL);
    char site_ctx[200]; snprintf(site_ctx, sizeof site_ctx, "%s", l.items[0].id);
    CHECK(find_item(&l, "Matrix") >= 0 && find_item(&l, "Avatar") >= 0 && find_item(&l, "Impressum") < 0);
    plugins_list_free(&l);
    /* auf der Seite suchen: Top-5 Treffer, Startseiten-Navlinks gefiltert */
    CHECK(search_ctx(ex, "matrix", site_ctx, &l) == 0 && l.count >= 2);
    CHECK(find_item(&l, "Matrix Reloaded") >= 0 && find_item(&l, "Impressum") < 0);
    printf("  Explorer Treffer fuer 'matrix': %d (erster: %s)\n", l.count, l.count ? l.items[0].title : "-");
    CHECK(l.count && !strcmp(l.items[0].title, "Matrix"));   /* exakter Treffer vorn */
    char page_id[200]; snprintf(page_id, sizeof page_id, "%s", l.items[0].id);
    plugins_list_free(&l);
    /* Detailseite oeffnen: offener Hoster + verschleierter Hoster gelistet */
    CHECK(browse(ex, page_id, &l) == 0 && l.count >= 2);
    for (int i = 0; i < l.count; i++) printf("    - %s | %s\n", l.items[i].title, l.items[i].subtitle);
    int arch = -1, voe = -1;
    for (int i = 0; i < l.count; i++) {
        if (strstr(l.items[i].id, "archive.org/embed/matrix1999")) arch = i;
        if (strstr(l.items[i].id, "voe.example")) voe = i;
    }
    CHECK(arch >= 0 && voe >= 0);
    /* offener Hoster: kleinste MP4-Ableitung wird aufgeloest */
    if (arch >= 0) CHECK(resolve(ex, &l.items[arch], &si) == 0 && !strcmp(si.url, "https://archive.org/download/matrix1999/matrix_512kb.mp4"));
    /* verschleierter Hoster: keine Unterstuetzung, klare Meldung */
    if (voe >= 0) { plugins_start_resolve(ex, &l.items[voe]); CHECK(wait_job() == JOB_ERROR);
        printf("  verschleierter Hoster -> %s\n", plugins_job_error()); plugins_job_reset(); }
    plugins_list_free(&l);

    /* ---------- YouTube ---------- */
    int yt = find_src("YouTube"); CHECK(yt >= 0);
    CHECK(browse(yt, NULL, &l) == 0 && l.count == 3 && l.items[0].kind == ITEM_SEARCH);
    plugins_list_free(&l);
    CHECK(search(yt, "pokemon", &l) == 0);
    for (int i = 0; i < l.count; i++) printf("    yt: %s | %s | %s\n", l.items[i].title, l.items[i].subtitle, l.items[i].id);
    CHECK(l.count == 4 && l.items[0].kind == ITEM_FOLDER && !strcmp(l.items[0].id, "chid:UCpoke"));
    CHECK(find_item(&l, "Pokemon Folge 1") == 1 && strstr(l.items[1].subtitle, "22:10") && strstr(l.items[1].subtitle, "Pokemon"));
    CHECK(find_item(&l, "Pokemon Folge 2") == 2 && strstr(l.items[2].subtitle, "21:55"));
    CHECK(l.items[1].thumb && !strcmp(l.items[1].thumb, "https://i.ytimg.com/vi/vid1/mqdefault.jpg"));
    CHECK(l.items[3].kind == ITEM_MORE);
    if (l.count == 4) {
        CHECK(resolve(yt, &l.items[1], &si) == 0 && strstr(si.url, "manifest.googlevideo.com") && strstr(si.headers, "User-Agent:"));
        CHECK(strstr(last_post, "\"VISIONOS\"") && strstr(last_post, "\"videoId\":\"vid1\""));
        CHECK(browse(yt, l.items[3].id, &l2) == 0 && l2.count == 1 && !strcmp(l2.items[0].title, "Seite 2"));
        plugins_list_free(&l2);
    }
    plugins_list_free(&l);
    /* offizieller Kanal: Handle -> Kanal -> Playlisten -> Folgen in Reihenfolge */
    CHECK(browse(yt, "official", &l) == 0 && l.count >= 5);
    int pk = find_item(&l, "Pokemon (offiziell)");
    char pkid[128]; snprintf(pkid, sizeof pkid, "%s", pk >= 0 ? l.items[pk].id : "");
    int lg = find_item(&l, "LEGO");
    char lgid[128]; snprintf(lgid, sizeof lgid, "%s", lg >= 0 ? l.items[lg].id : "");
    plugins_list_free(&l);
    CHECK(browse(yt, pkid, &l) == 0 && l.count == 2 && strstr(l.items[1].id, "chp:UCpoke"));
    char chp[64]; snprintf(chp, sizeof chp, "%s", l.count == 2 ? l.items[1].id : "");
    char chv[64]; snprintf(chv, sizeof chv, "%s", l.count == 2 ? l.items[0].id : "");
    plugins_list_free(&l);
    CHECK(browse(yt, chv, &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Neuestes Video"));
    plugins_list_free(&l);
    CHECK(browse(yt, chp, &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Staffel 1") && !strcmp(l.items[0].id, "pl:PLstaffel1"));
    plugins_list_free(&l);
    CHECK(browse(yt, "pl:PLstaffel1", &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "1. Pika!") && !strcmp(l.items[1].title, "2. Glurak"));
    plugins_list_free(&l);
    /* Handle nicht aufloesbar -> Suche nach dem Namen */
    CHECK(browse(yt, lgid, &l) == 0 && find_item(&l, "Pokemon Folge 1") >= 0);
    plugins_list_free(&l);
    /* ---------- Pluto TV auf Abruf ---------- */
    int pl = find_src("Pluto"); CHECK(pl >= 0);
    CHECK(browse(pl, NULL, &l) == 0 && l.count == 3);
    plugins_list_free(&l);
    CHECK(browse(pl, "genres:movies", &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Komoedie"));
    char gid[64]; snprintf(gid, sizeof gid, "%s", l.count ? l.items[0].id : "");
    plugins_list_free(&l);
    CHECK(browse(pl, gid, &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Ein Film") && strstr(l.items[0].subtitle, "1 Std. 30 Min."));
    if (l.count) {
        CHECK(resolve(pl, &l.items[0], &si) == 0);
        printf("  Pluto-Stream: %s\n", si.url);
        CHECK(!strcmp(si.url, "https://stitcher.pluto.example/v2/stitch/hls/episode/m1/master.m3u8?appName=web&deviceId=abc&jwt=PTOK&masterJWTPassthrough=true&includeExtendedEvents=true"));
    }
    plugins_list_free(&l);
    CHECK(browse(pl, "catpage:0", &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Top-Filme") && strstr(l.items[0].subtitle, "2+"));
    plugins_list_free(&l);
    CHECK(browse(pl, "genres:series", &l) == 0 && l.count == 1 && !strcmp(l.items[0].title, "Krimi"));
    plugins_list_free(&l);
    CHECK(browse(pl, "series:s1", &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "Staffel 1") && !strcmp(l.items[1].title, "Staffel 2"));
    plugins_list_free(&l);
    CHECK(browse(pl, "series:s1:1", &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "1. Erste") && !strcmp(l.items[1].title, "2. Zweite"));
    if (l.count == 2) {
        CHECK(resolve(pl, &l.items[0], &si) == 0 && strstr(si.url, "/v2/stitch/hls/episode/e11/master.m3u8?"));
    }
    plugins_list_free(&l);

    /* ---------- Audiothek ---------- */
    int au = find_src("Audiothek"); CHECK(au >= 0);
    CHECK(browse(au, NULL, &l) == 0 && l.count == 7 && l.items[0].kind == ITEM_SEARCH && l.items[1].kind == ITEM_SEARCH);
    plugins_list_free(&l);
    CHECK(browse(au, "rc:DE:0", &l) == 0 && l.count == 2);      /* OGG/Opus aussortiert, HLS bleibt */
    CHECK(strstr(last_url, "fi1.api.radio-browser.info") && strstr(last_url, "countrycode=DE") && strstr(last_url, "hidebroken=true"));
    if (l.count == 2) {
        CHECK(!strcmp(l.items[0].title, "1LIVE") && strstr(l.items[0].subtitle, "MP3 128 kbit/s") && l.items[0].thumb);
        CHECK(l.items[1].thumb == NULL);                          /* .ico kann die App nicht anzeigen */
        CHECK(resolve(au, &l.items[0], &si) == 0 && !strcmp(si.url, "http://wdr.example/1live.mp3"));
    }
    plugins_list_free(&l);
    CHECK(search_ctx(au, "wdr 2", "radiosearch", &l) == 0 && strstr(last_url, "name=wdr%202"));
    plugins_list_free(&l);
    CHECK(search_ctx(au, "tagesschau", "podsearch", &l) == 0 && l.count == 1 && !strcmp(l.items[0].id, "feed:https://feed.example/tagesschau.xml"));
    plugins_list_free(&l);
    CHECK(browse(au, "feed:https://feed.example/tagesschau.xml", &l) == 0 && l.count == 2);
    if (l.count == 2) {
        CHECK(!strcmp(l.items[0].title, "Folge & Eins") && strstr(l.items[0].subtitle, "12:34") && strstr(l.items[0].subtitle, "Fri, 02 Oct 2026"));
        CHECK(resolve(au, &l.items[0], &si) == 0 && !strcmp(si.url, "https://media.example/1.mp3?a=1&b=2"));
        CHECK(!strcmp(l.items[0].thumb, "https://img.example/kanal.jpg") && !strcmp(l.items[1].thumb, "https://img.example/f2.jpg"));
        CHECK(strstr(l.items[1].subtitle, "01:02:03") != NULL);
    }
    plugins_list_free(&l);
    CHECK(browse(au, "podcharts", &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "Hit 1"));   /* Reihenfolge der Charts */
    plugins_list_free(&l);

    CHECK(plugins_reload() == n);
    plugins_shutdown();
    printf(fails ? "%d Fehler\n" : "plugins: alle Tests ok\n", fails);
    return fails != 0;
}
