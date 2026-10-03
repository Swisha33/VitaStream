/* Host-Test: Download-Verwaltung gegen den lokalen Testserver */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/dl.h"
#include "../src/net.h"
#include "../src/config.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "http://127.0.0.1:8769";
    char name[200];
    dl_filename("Folge 1: Der Anfang / Teil?", "https://x/a/video.MP4?token=1", name, sizeof name);
    printf("  Dateiname: %s\n", name);
    CHECK(!strcmp(name, "Folge_1_Der_Anfang_Teil.mp4"));
    dl_filename("...", "https://x/podcast.mp3", name, sizeof name);
    CHECK(!strcmp(name, "video.mp3"));
    dl_filename("Radio", "https://x/stream", name, sizeof name);
    CHECK(!strcmp(name, "Radio.mp4"));

    config_load();
    g_cfg.custom_dns_enabled = 0;
    net_init();
    mkdir("ux0:data", 0777); mkdir("ux0:data/VitaStream", 0777);
    char url[256];
    snprintf(url, sizeof url, "%s/redirect.mp4", base);       /* mit Weiterleitung */
    CHECK(dl_start(url, NULL, "Testfilm") == 0);
    CHECK(dl_start(url, NULL, "Zweiter") == -1);              /* nur einer zur Zeit */
    int seen_progress = 0;
    for (int i = 0; i < 600 && dl_active(); i++) { if (strstr(dl_status(), "%")) seen_progress = 1; usleep(10000); }
    const char *st = dl_status();
    printf("  Status: %s\n", st);
    CHECK(strstr(st, "fertig") != NULL);
    struct stat s1, s2;
    CHECK(stat(VS_DATA_DIR "/downloads/Testfilm.mp4", &s1) == 0);
    CHECK(stat("/tmp/claude-0/-home-claude/34e8d20f-a6be-5c20-8891-4b0a96d4c8f2/scratchpad/www/bbb.mp4", &s2) == 0 && s1.st_size == s2.st_size);
    CHECK(stat(VS_DATA_DIR "/downloads/Testfilm.mp4.part", &s1) != 0);
    (void)seen_progress;
    /* Fehlerfall */
    snprintf(url, sizeof url, "%s/gibtsnicht.mp4", base);
    CHECK(dl_start(url, NULL, "Fehlt") == 0);
    for (int i = 0; i < 300 && dl_active(); i++) usleep(10000);
    st = dl_status();
    printf("  Status: %s\n", st);
    CHECK(strstr(st, "fehlgeschlagen") && strstr(st, "404"));
    CHECK(stat(VS_DATA_DIR "/downloads/Fehlt.mp4", &s1) != 0 && stat(VS_DATA_DIR "/downloads/Fehlt.mp4.part", &s1) != 0);
    /* Abbruch */
    snprintf(url, sizeof url, "%s/icy.aac", base);            /* endlos */
    CHECK(dl_start(url, NULL, "Radio") == 0);
    usleep(500000);
    dl_cancel();
    for (int i = 0; i < 300 && dl_active(); i++) usleep(10000);
    st = dl_status();
    printf("  Status: %s\n", st);
    CHECK(strstr(st, "abgebrochen") != NULL);
    dl_shutdown();
    printf(fails ? "%d Fehler\n" : "dl: alle Tests ok\n", fails);
    return fails != 0;
}
