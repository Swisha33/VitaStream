/* Host-Test: XMLTV-Programmführer */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include "../src/epg.h"
#include "../src/net.h"

/* Stubs */
int net_download(const char *u, const char *h, const char *p, volatile int *a, volatile int64_t *d, volatile int64_t *t)
{ (void)u; (void)h; (void)p; (void)a; (void)d; (void)t; return -1; }
const char *net_last_detail(void) { return "stub"; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char *XML =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tv generator-info-name=\"x\">\n"
"<channel id=\"DasErste.de\"><display-name lang=\"de\">Das Erste HD</display-name></channel>\n"
"<channel id=\"zdf.de\">\n  <display-name>ZDF</display-name>\n</channel>\n"
"<channel id=\"leer.de\"/>\n"
"<programme start=\"20261006180000 +0200\" stop=\"20261006200000 +0200\" channel=\"DasErste.de\"><title lang=\"de\">Alt &amp; vorbei</title></programme>\n"
"<programme start=\"20261006200000 +0200\" stop=\"20261006201500 +0200\" channel=\"DasErste.de\"><title lang=\"de\">Tagesschau</title><desc>x</desc></programme>\n"
"<programme start=\"20261006201500 +0200\" stop=\"20261006214500 +0200\" channel=\"DasErste.de\"><title><![CDATA[Tatort]]></title></programme>\n"
"<programme start=\"20261007180000 +0200\" stop=\"20261007200000 +0200\" channel=\"DasErste.de\"><title>Zu spaet</title></programme>\n"
"<programme start=\"20261006183000 +0000\" stop=\"20261006190000 +0000\" channel=\"zdf.de\"><title>heute journal</title></programme>\n"
"</tv>\n";

int main(void)
{
    /* Zeit */
    int64_t t = epg_parse_time("20261006200500 +0200");
    CHECK(t == epg_parse_time("20261006180500 +0000"));
    CHECK(t == epg_parse_time("20261006180500"));
    CHECK(epg_parse_time("20261006133500 -0430") == epg_parse_time("20261006180500 +0000"));
    CHECK(epg_parse_time("19700101000000 +0000") == 0);
    CHECK(epg_parse_time("20000101000000 +0000") == 946684800);

    FILE *f = fopen("epg_plain.xml", "w"); fputs(XML, f); fclose(f);
    gzFile g = gzopen("epg.xml.gz", "wb");
    /* viele Füllsender, damit Elemente über Puffergrenzen laufen */
    for (int i = 0; i < 3000; i++) gzprintf(g, "<channel id=\"fill%d\"><display-name>Fuell %d</display-name></channel>\n", i, i);
    gzputs(g, XML); gzclose(g);

    for (int round = 0; round < 2; round++) {
        int n = epg_parse_file(round ? "epg.xml.gz" : "epg_plain.xml", t);
        CHECK(n == 4);       /* "Zu spaet" liegt außerhalb des Fensters */
        CHECK(epg_ready());
        EpgShow s[2];
        int k = epg_now("daserste.de", NULL, t, s);
        CHECK(k == 2 && !strcmp(s[0].title, "Tagesschau") && !strcmp(s[1].title, "Tatort"));
        CHECK(s[0].start == epg_parse_time("20261006180000 +0000"));
        k = epg_now("DasErste.de@SD", NULL, t, s);  /* iptv-org-Kennung mit Feed */
        CHECK(k == 2 && !strcmp(s[0].title, "Tagesschau"));
        k = epg_now(NULL, "Das Erste", t, s);       /* über Namen (ohne HD) */
        CHECK(k == 2 && !strcmp(s[0].title, "Tagesschau"));
        k = epg_now("unbekannt", "ZDF HD", epg_parse_time("20261006184000 +0000"), s);
        CHECK(k == 1 && !strcmp(s[0].title, "heute journal"));
        k = epg_now("zdf.de", NULL, epg_parse_time("20261006180000 +0000"), s);   /* Lücke: jetzt leer, danach */
        CHECK(k == 2 && s[0].title[0] == 0 && !strcmp(s[1].title, "heute journal"));
        k = epg_now("leer.de", NULL, t, s);
        CHECK(k == 0);
        CHECK(epg_now("nix", "nix", t, s) == 0);
    }
    char hm[16];
    epg_fmt_hm(epg_parse_time("20261006181500 +0000") - epg_tz_offset() + 0, hm, sizeof hm);
    CHECK(!strcmp(hm, "18:15"));
    epg_shutdown();
    CHECK(!epg_ready());
    printf(fails ? "test_epg: %d FEHLER\n" : "test_epg: alles ok\n", fails);
    return fails != 0;
}
