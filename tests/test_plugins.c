/* Host-Test für plugins.c mit echtem Lua 5.4 und simuliertem Netzwerk */
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
static char last_post[2048], last_headers[512];

static int reply(NetBuf *out, const char *s) { out->data = strdup(s); out->len = strlen(s); return NET_OK; }

int net_check_url(const char *url) {
    char h[256]; adblock_host_from_url(url, h, sizeof h);
    return (g_cfg.adblock_enabled && adblock_is_blocked(h)) ? NET_BLOCKED : NET_OK;
}
const char *net_strerror(int c) { return c == NET_BLOCKED ? "Durch AdBlock gesperrt" : "Netzwerkfehler"; }
void net_buf_free(NetBuf *b) { free(b->data); b->data = NULL; b->len = 0; }
const char *net_last_detail(void) { return ""; }

int net_request(const char *url, const char *post, const char *hdr, NetBuf *out, long *status, char *final_url, int fl) {
    memset(out, 0, sizeof *out);
    if (status) *status = 200;
    if (final_url) snprintf(final_url, fl, "%s", url);
    snprintf(last_headers, sizeof last_headers, "%s", hdr ? hdr : "");
    if (net_check_url(url) == NET_BLOCKED) return NET_BLOCKED;
    if (strstr(url, "mediathekviewweb")) {
        snprintf(last_post, sizeof last_post, "%s", post ? post : "");
        return reply(out, "{\"result\":{\"results\":[{\"channel\":\"ZDF\",\"topic\":\"Terra X\",\"title\":\"Die R\\u00f6mer\","
            "\"timestamp\":1727900000,\"duration\":2700,\"url_video\":\"https://zdf.example/v.mp4\",\"url_video_low\":\"\",\"url_video_hd\":\"https://zdf.example/hd.mp4\"},"
            "{\"channel\":\"ARD\",\"topic\":\"Tagesschau\",\"title\":\"Tagesschau\",\"timestamp\":1727910000,\"duration\":900,\"url_video\":\"https://ard.example/t.m3u8\"}],"
            "\"queryInfo\":{\"totalResults\":2}},\"err\":null}");
    }
    if (strstr(url, "liste.m3u"))
        return reply(out, "#EXTM3U\r\n#EXTINF:-1 tvg-id=\"a\" group-title=\"News\",Kanal A\r\n#EXTVLCOPT:http-referrer=https://ref.example/\r\nhttps://a.example/a.m3u8\r\n"
                          "#EXTINF:-1 group-title=\"Sport\",Kanal B, mit Komma\nhttps://b.example/b.m3u8\n#EXTINF:-1 group-title=\"News\",Kanal C\nhttps://c.example/c.mp4\n");
    if (strstr(url, "example.org/neu") || strstr(url, "example.org/suche"))
        return reply(out, "<html><a class=\"video\" href=\"/video/1\">Film &amp; Eins</a> <a class=\"video\" href=\"/video/2\"><b>Zwei</b></a>"
                          "<a class=\"video\" href=\"/video/1\">Film &amp; Eins</a></html>");
    if (strstr(url, "example.org/video/1"))
        return reply(out, "<div><iframe width=1 src=\"https://player.example.net/e/42\"></iframe></div>");
    if (strstr(url, "example.org/video/2"))
        return reply(out, "<iframe src=\"https://ads.adnet.example/e/1\"></iframe>");
    if (strstr(url, "player.example.net/e/42"))
        return reply(out, "<script>var src=\"https:\\/\\/cdn.example.net\\/hls\\/master.m3u8?t=1\";</script>");
    return NET_ERR;
}

static int wait_job(void) {
    for (int i = 0; i < 500 && plugins_job_state() == JOB_RUNNING; i++) usleep(2000);
    return plugins_job_state();
}
static int find_src(const char *name) {
    for (int i = 0; i < plugins_source_count(); i++) if (strstr(plugins_source(i)->name, name)) return i;
    return -1;
}

int main(void) {
    g_cfg.adblock_enabled = 1;
    adblock_load(VS_DATA_DIR "/blocklist.txt");
    int n = plugins_init();
    printf("Quellen: %d  (Log: %s)\n", n, plugins_last_log());
    for (int i = 0; i < n; i++) printf("  - %s [%s] search=%d browse=%d\n", plugins_source(i)->name, plugins_source(i)->file,
                                       plugins_source(i)->has_search, plugins_source(i)->has_browse);
    PluginList l; StreamInfo si;

    /* Mediathek */
    int m = find_src("Mediathek"); CHECK(m >= 0);
    plugins_start_search(m, "Römer"); CHECK(wait_job() == JOB_DONE);
    plugins_take_list(&l);
    CHECK(l.count == 2);
    if (l.count == 2) {
        printf("  mediathek[0]: %s | %s\n", l.items[0].title, l.items[0].subtitle);
        CHECK(!strcmp(l.items[0].title, "Terra X - Die R\xC3\xB6mer"));
        CHECK(strstr(l.items[0].subtitle, "02.10.2024") && strstr(l.items[0].subtitle, "45:00"));
        plugins_start_resolve(m, &l.items[1]); CHECK(wait_job() == JOB_DONE);
        plugins_take_stream(&si); CHECK(!strcmp(si.url, "https://ard.example/t.m3u8"));
    }
    printf("  POST-Body: %s\n  Header: %s\n", last_post, last_headers);
    CHECK(strstr(last_post, "\"query\":\"R\xC3\xB6mer\"") != NULL);
    plugins_list_free(&l);
    plugins_start_browse(m, NULL); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count > 5 && l.items[0].kind == ITEM_FOLDER); plugins_list_free(&l);

    /* M3U */
    int p = find_src("M3U"); CHECK(p >= 0);
    plugins_start_browse(p, NULL); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 2);
    char pl_id[32]; snprintf(pl_id, sizeof pl_id, "%s", l.count == 2 ? l.items[1].id : "");
    plugins_list_free(&l);
    plugins_start_browse(p, pl_id); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 2);  /* Gruppen News, Sport */
    char grp[32]; snprintf(grp, sizeof grp, "%s", l.count ? l.items[0].id : "");
    if (l.count) printf("  Gruppe: %s (%s)\n", l.items[0].title, l.items[0].subtitle);
    plugins_list_free(&l);
    plugins_start_browse(p, grp); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 2 && !strcmp(l.items[0].title, "Kanal A"));
    if (l.count) { plugins_start_resolve(p, &l.items[0]); CHECK(wait_job() == JOB_DONE); plugins_take_stream(&si);
        printf("  m3u stream: %s  headers: [%s]\n", si.url, si.headers);
        CHECK(!strcmp(si.url, "https://a.example/a.m3u8") && !strcmp(si.headers, "Referer: https://ref.example/")); }
    plugins_list_free(&l);
    plugins_start_search(p, "komma"); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 1 && !strcmp(l.items[0].title, "Kanal B, mit Komma")); plugins_list_free(&l);

    /* Website */
    int w = find_src("Beispielseite"); CHECK(w >= 0);
    plugins_start_search(w, "a b&c"); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 2);
    if (l.count == 2) {
        printf("  site: %s -> %s | %s\n", l.items[0].title, l.items[0].id, l.items[1].title);
        CHECK(!strcmp(l.items[0].title, "Film & Eins") && !strcmp(l.items[1].title, "Zwei"));
        plugins_start_resolve(w, &l.items[0]); if (wait_job() != JOB_DONE) { printf("FAIL resolve: %s\n", plugins_job_error()); fails++; plugins_job_reset(); } else plugins_take_stream(&si);
        printf("  site stream: %s  headers: [%s]\n", si.url, si.headers);
        CHECK(!strcmp(si.url, "https://cdn.example.net/hls/master.m3u8?t=1"));
        plugins_start_resolve(w, &l.items[1]); CHECK(wait_job() == JOB_ERROR);
        printf("  gesperrtes iframe -> %s\n", plugins_job_error()); plugins_job_reset();
    }
    plugins_list_free(&l);

    /* Direkt + Fehlerfälle */
    int d = find_src("Direkte"); CHECK(d >= 0);
    plugins_start_search(d, "kein link"); CHECK(wait_job() == JOB_ERROR); printf("  direkt-fehler: %s\n", plugins_job_error()); plugins_job_reset();
    plugins_start_search(d, " https://x.example/a.mp4 "); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 1);
    if (l.count) { plugins_start_resolve(d, &l.items[0]); CHECK(wait_job() == JOB_DONE); plugins_take_stream(&si); }
    plugins_list_free(&l);
    plugins_start_browse(d, NULL); CHECK(wait_job() == JOB_DONE); plugins_take_list(&l);
    CHECK(l.count == 1); plugins_list_free(&l);   /* Verlauf gespeichert */

    /* Gesperrte Stream-URL wird abgefangen */
    plugins_start_search(d, "https://doubleclick.net/x.mp4"); wait_job(); plugins_take_list(&l);
    if (l.count) { plugins_start_resolve(d, &l.items[0]); CHECK(wait_job() == JOB_ERROR); printf("  gesperrter stream -> %s\n", plugins_job_error()); plugins_job_reset(); }
    plugins_list_free(&l);

    CHECK(plugins_reload() == n);
    plugins_shutdown();
    printf(fails ? "%d Fehler\n" : "plugins: alle Tests ok\n", fails);
    return fails != 0;
}
