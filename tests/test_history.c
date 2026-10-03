/* Host-Test: Fortschritt und Verlauf */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/history.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    mkdir("hist_run", 0777);
    remove("hist_run/progress.txt");
    remove("hist_run/history.txt");
    history_load("hist_run");

    progress_set("youtube.lua|v:1", 65000, 600000);
    progress_set("m3u.lua|x", 5000, 100000);
    int64_t p, d;
    CHECK(progress_get("youtube.lua|v:1", &p, &d) && p == 65000 && d == 600000);
    CHECK(progress_percent("youtube.lua|v:1") == 10);
    CHECK(progress_percent("gibt|es nicht") == 0);
    progress_set("youtube.lua|v:1", 120000, 600000);      /* aktualisieren */
    progress_clear("m3u.lua|x");

    HistEntry e;
    memset(&e, 0, sizeof e);
    strcpy(e.src, "pluto.lua"); strcpy(e.id, "ep:1"); strcpy(e.key, "pluto.lua|ep:1");
    strcpy(e.title, "Titel mit\tTab und \\ Backslash"); strcpy(e.url, "https://x/1.m3u8");
    strcpy(e.headers, "Referer: https://a/\nUser-Agent: Test"); e.save_ref = 1;
    history_add(&e);
    strcpy(e.id, "ep:2"); strcpy(e.title, "Zweiter"); e.save_ref = 0;
    history_add(&e);
    strcpy(e.id, "ep:1"); strcpy(e.title, "Erster erneut");
    history_add(&e);                                         /* nach vorn, kein Duplikat */
    CHECK(history_count() == 2 && !strcmp(history_get(0)->id, "ep:1") && !strcmp(history_get(1)->id, "ep:2"));

    /* neu laden */
    history_load("hist_run");
    CHECK(progress_get("youtube.lua|v:1", &p, &d) && p == 120000);
    CHECK(!progress_get("m3u.lua|x", NULL, NULL));
    CHECK(history_count() == 2);
    const HistEntry *h = history_get(1);
    CHECK(h && !strcmp(h->title, "Zweiter") && !strcmp(h->headers, "Referer: https://a/\nUser-Agent: Test"));
    CHECK(!strcmp(history_get(0)->title, "Erster erneut") && history_get(0)->save_ref == 0);
    history_get(0);
    history_remove(0);
    CHECK(history_count() == 1 && !strcmp(history_get(0)->id, "ep:2"));
    /* Sonderzeichen überleben */
    strcpy(e.id, "ep:3"); strcpy(e.title, "Tab\there \\n kein Umbruch");
    history_add(&e);
    history_load("hist_run");
    CHECK(!strcmp(history_get(0)->title, "Tab\there \\n kein Umbruch"));
    /* Obergrenze */
    for (int i = 0; i < 60; i++) { snprintf(e.id, sizeof e.id, "n:%d", i); history_add(&e); }
    CHECK(history_count() == HISTORY_MAX && !strcmp(history_get(0)->id, "n:59"));
    history_clear();
    history_load("hist_run");
    CHECK(history_count() == 0);

    printf(fails ? "%d Fehler\n" : "history: alle Tests ok\n", fails);
    return fails != 0;
}
