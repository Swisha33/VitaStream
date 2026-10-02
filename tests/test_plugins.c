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
static char last_post[4096], last_headers[512];

static int reply(NetBuf *out, const char *s) { out->data = strdup(s); out->len = strlen(s); return NET_OK; }

int net_check_url(const char *url) {
    char h[256]; adblock_host_from_url(url, h, sizeof h);
    return (g_cfg.adblock_enabled && adblock_is_blocked(h)) ? NET_BLOCKED : NET_OK;
}
const char *net_strerror(int c) { return c == NET_BLOCKED ? "Durch AdBlock gesperrt" : "Netzwerkfehler"; }
void net_buf_free(NetBuf *b) { free(b->data); b->data = NULL; b->len = 0; }
const char *net_last_detail(void) { return ""; }

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
            "%s{\"channel\":\"ZDF\",\"topic\":\"%s\",\"title\":\"Folge %d\",\"timestamp\":1727900000,\"duration\":2700,"
            "\"url_video\":\"https://zdf.example/v%d.mp4\",\"url_website\":\"https://www.zdf.example/folge-%d.html\"}",
            i > off ? "," : "", i % 3 ? "Terra X" : "Die R\\u00f6mer", i, i, i);
    }
    len += snprintf(b + len, cap - len, "],\"queryInfo\":{\"totalResults\":%d}},\"err\":null}", total);
    out->data = b;
    out->len = len;
    return NET_OK;
}

int net_request(const char *url, const char *post, const char *hdr, NetBuf *out, long *status, char *final_url, int fl) {
    memset(out, 0, sizeof *out);
    if (status) *status = 200;
    if (final_url) snprintf(final_url, fl, "%s", url);
    snprintf(last_headers, sizeof last_headers, "%s", hdr ? hdr : "");
    if (net_check_url(url) == NET_BLOCKED) return NET_BLOCKED;
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
        return reply(out, "<html><title>Impressum</title>keine Videos</html>");
    if (!strcmp(url, "https://embed.example/p/9"))
        return reply(out, "<source src=\"https://cdn.embed.example/v9.mp4\" type=\"video/mp4\">");
    /* South Park */
    if (strstr(url, "southpark.de/seasons/south-park?json=true"))
        return reply(out, "{\"children\":[{\"type\":\"SeasonSelector\",\"props\":{\"items\":["
                          "{\"label\":\"Staffel 2\",\"url\":\"/seasons/south-park/b2/staffel-2\"},"
                          "{\"label\":\"Staffel 1\",\"url\":\"/seasons/south-park/a1/staffel-1\"}]}}]}");
    if (strstr(url, "southpark.de/seasons/south-park/a1/staffel-1?json=true"))
        return reply(out, "{\"children\":[{\"type\":\"LineList\",\"props\":{\"items\":["
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

    /* ---------- Mediathek: Sender -> Kategorien, Seiten, Vorschaubilder ---------- */
    int m = find_src("Mediathek"); CHECK(m >= 0);
    CHECK(browse(m, NULL, &l) == 0 && l.count > 10 && l.items[0].kind == ITEM_FOLDER);
    int zdf = find_item(&l, "ZDF");
    char zid[64]; snprintf(zid, sizeof zid, "%s", zdf >= 0 ? l.items[zdf].id : "");
    plugins_list_free(&l);
    CHECK(browse(m, zid, &l) == 0 && find_item(&l, "Neueste") == 0 && find_item(&l, "Filme") > 0 && find_item(&l, "Sendungen") == 1);
    char new_id[64], cat_id[64], top_id[64];
    snprintf(new_id, sizeof new_id, "%s", l.items[0].id);
    snprintf(top_id, sizeof top_id, "%s", l.items[1].id);
    snprintf(cat_id, sizeof cat_id, "%s", l.items[find_item(&l, "Filme")].id);
    plugins_list_free(&l);
    CHECK(browse(m, new_id, &l) == 0 && l.count == 51 && l.items[50].kind == ITEM_MORE);
    printf("  mediathek: %d Eintraege, letzter: %s | thumb=%s\n", l.count, l.items[50].title, l.items[0].thumb ? l.items[0].thumb : "-");
    CHECK(l.items[0].thumb && !strncmp(l.items[0].thumb, "og:https://www.zdf.example/", 27));
    /* Seite 2 und 3 anhängen */
    for (int page = 0; page < 2; page++) {
        int mi = l.count - 1;
        if (l.items[mi].kind != ITEM_MORE) break;
        CHECK(browse(m, l.items[mi].id, &l2) == 0);
        plugins_list_append(&l, &l2, mi);
    }
    printf("  mediathek nach 'Weitere laden': %d Eintraege\n", l.count);
    CHECK(l.count == 120 && l.items[119].kind == ITEM_VIDEO);
    plugins_list_free(&l);
    CHECK(browse(m, cat_id, &l) == 0 && l.count == 51);
    CHECK(strstr(last_post, "\"query\":\"film\"") && strstr(last_post, "\"duration_min\":4200") && strstr(last_post, "\"query\":\"ZDF\""));
    plugins_list_free(&l);
    CHECK(browse(m, top_id, &l) == 0 && l.count == 2 && l.items[0].kind == ITEM_FOLDER);
    if (l.count == 2) printf("  Sendungen: %s (%s), %s\n", l.items[0].title, l.items[0].subtitle, l.items[1].title);
    plugins_list_free(&l);
    CHECK(search(m, "Römer", &l) == 0 && l.count == 51);
    if (l.count) { CHECK(resolve(m, &l.items[0], &si) == 0 && !strcmp(si.url, "https://zdf.example/v0.mp4")); }
    plugins_list_free(&l);

    /* ---------- M3U-Playlists: Logos, Header ---------- */
    int p = find_src("M3U"); CHECK(p >= 0);
    CHECK(browse(p, NULL, &l) == 0 && l.count >= 2);
    int ti = find_item(&l, "Test-Liste");
    char tid[64]; snprintf(tid, sizeof tid, "%s", ti >= 0 ? l.items[ti].id : "");
    plugins_list_free(&l);
    CHECK(browse(p, tid, &l) == 0 && l.count == 3);
    if (l.count == 3) {
        CHECK(l.items[0].thumb && !strcmp(l.items[0].thumb, "https://logo.example/a.png"));
        CHECK(resolve(p, &l.items[0], &si) == 0 && !strcmp(si.url, "https://a.example/a.m3u8") && !strcmp(si.headers, "Referer: https://ref.example/"));
        CHECK(!strcmp(l.items[1].title, "Kanal B, mit Komma"));
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
    CHECK(l.count == 3);
    int fi = find_item(&l, "Folge 1 & mehr");
    CHECK(fi >= 0);
    if (fi >= 0) {
        CHECK(l.items[fi].thumb && !strcmp(l.items[fi].thumb, "https://scan.example/img/f1.jpg"));
        CHECK(resolve(d, &l.items[fi], &si) == 0 && !strcmp(si.url, "https://cdn.scan.example/f1/index.m3u8") &&
              strstr(si.headers, "Referer: https://scan.example/videos/folge-1"));
    }
    CHECK(find_item(&l, "(Player)") >= 0);
    plugins_list_free(&l);
    CHECK(browse(d, NULL, &l) == 0 && l.count >= 1 && l.items[0].kind == ITEM_FOLDER);   /* Verlauf */
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
    CHECK(browse(sp, root, &l) == 0 && l.count == 2 && !strcmp(l.items[0].title, "Staffel 1"));
    char season[160]; snprintf(season, sizeof season, "%s", l.count ? l.items[0].id : "");
    plugins_list_free(&l);
    CHECK(browse(sp, season, &l) == 0 && l.count == 2 && l.items[1].kind == ITEM_MORE);
    if (l.count == 2) {
        printf("  South Park: %s | %s | %s\n", l.items[0].title, l.items[0].subtitle ? l.items[0].subtitle : "-", l.items[0].thumb ? l.items[0].thumb : "-");
        CHECK(!strcmp(l.items[0].title, "Cartman") && l.items[0].thumb && strstr(l.items[0].thumb, "images.paramount.tech"));
        CHECK(resolve(sp, &l.items[0], &si) == 0 && !strcmp(si.url, "https://dai.example/sp/x1/master.m3u8"));
        CHECK(browse(sp, l.items[1].id, &l2) == 0 && l2.count == 1 && !strcmp(l2.items[0].title, "Weight Gain"));
        plugins_list_free(&l2);
    }
    plugins_list_free(&l);

    CHECK(plugins_reload() == n);
    plugins_shutdown();
    printf(fails ? "%d Fehler\n" : "plugins: alle Tests ok\n", fails);
    return fails != 0;
}
