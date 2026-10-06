/* Host-Test des Untertitel-Parsers (WebVTT, SRT, TTML) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/sub.h"
#include "../src/net.h"

/* Stubs: der Parser-Test braucht kein Netzwerk */
int net_request(const char *u, const char *p, const char *h, NetBuf *o, long *s, char *f, int fl)
{ (void)u; (void)p; (void)h; (void)o; (void)s; (void)f; (void)fl; return NET_ERR; }
void net_buf_free(NetBuf *b) { free(b->data); b->data = NULL; }
const char *net_strerror(int c) { (void)c; return "Netzwerkfehler"; }
void hls_join_url(const char *base, const char *ref, char *out, int n) { (void)base; snprintf(out, n, "%s", ref); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char *at(SubCue *c, int n, int64_t t)
{
    for (int i = 0; i < n; i++) if (c[i].start <= t && t < c[i].end) return c[i].text;
    return NULL;
}

int main(void)
{
    SubCue *c = NULL; int n = 0, cap = 0;

    /* WebVTT-Segment aus HLS: MPEGTS 900000 (=10 s) bei LOCAL 0, Stream-Anfang 10 s -> kein Versatz */
    const char *vtt =
        "WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:900000,LOCAL:00:00:00.000\n\n"
        "1\n00:00:01.500 --> 00:00:03.000 line:90%\n<c.yellow>Hallo &amp; willkommen</c>\nZweite Zeile\n\n"
        "00:05.000 --> 00:06.000\nKurzform\n";
    CHECK(sub_parse_into(vtt, 10000, &c, &n, &cap) == 2);
    CHECK(at(c, n, 2000) && !strcmp(at(c, n, 2000), "Hallo & willkommen\nZweite Zeile"));
    CHECK(at(c, n, 5500) && !strcmp(at(c, n, 5500), "Kurzform"));
    CHECK(at(c, n, 4000) == NULL);
    sub_free_cues(c, n); c = NULL; n = cap = 0;

    /* Versatz: MPEGTS 1800000 (=20 s), Anfang 10 s -> +10 s */
    CHECK(sub_parse_into("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:1800000\n\n00:00:01.000 --> 00:00:02.000\nSpaeter\n",
                         10000, &c, &n, &cap) == 1);
    CHECK(n == 1 && c[0].start == 11000 && c[0].end == 12000);
    sub_free_cues(c, n); c = NULL; n = cap = 0;

    /* SRT mit Windows-Zeilenenden */
    CHECK(sub_parse_into("1\r\n00:00:01,000 --> 00:00:02,500\r\nSRT <i>Text</i>\r\n\r\n2\r\n00:01:00,000 --> 00:01:01,000\r\nZwei\r\n",
                         0, &c, &n, &cap) == 2);
    CHECK(at(c, n, 1200) && !strcmp(at(c, n, 1200), "SRT Text"));
    CHECK(at(c, n, 60500) && !strcmp(at(c, n, 60500), "Zwei"));
    sub_free_cues(c, n); c = NULL; n = cap = 0;

    /* TTML wie bei ZDF: Beginn 10:00:00, <br/>, <span> */
    const char *ttml =
        "<?xml version=\"1.0\"?><tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div>"
        "<p begin=\"10:00:03.000\" end=\"10:00:05.000\" region=\"r1\"><span style=\"s1\">Erste</span><br/>Zeile</p>\n"
        "<p xml:id=\"x\" begin=\"10:00:06.000\" dur=\"00:00:01.000\">Dur-Angabe</p>"
        "</div></body></tt>";
    CHECK(sub_parse_into(ttml, 0, &c, &n, &cap) == 2);
    CHECK(at(c, n, 4000) && !strcmp(at(c, n, 4000), "Erste\nZeile"));
    CHECK(at(c, n, 6500) && !strcmp(at(c, n, 6500), "Dur-Angabe"));
    sub_free_cues(c, n); c = NULL; n = cap = 0;

    /* EBU-TT der ARD: Namensraum-Praefix tt:, verschachtelte Spans, tt:br */
    const char *ebu =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tt:tt xmlns:tt=\"http://www.w3.org/ns/ttml\" ttp:timeBase=\"media\">"
        "<tt:body><tt:div>\n"
        "<tt:p xml:id=\"sub0\" region=\"bottom\" begin=\"00:00:02.000\" end=\"00:00:04.500\">\n"
        "  <tt:span style=\"textWhite\">Guten Abend,</tt:span><tt:br/>\n  <tt:span style=\"textYellow\">meine Damen &amp; Herren.</tt:span>\n"
        "</tt:p>\n<tt:p begin=\"00:00:05.000\" end=\"00:00:06.000\"><tt:span>Zweiter</tt:span></tt:p></tt:div></tt:body></tt:tt>";
    CHECK(sub_parse_into(ebu, 0, &c, &n, &cap) == 2);
    CHECK(at(c, n, 3000) && !strcmp(at(c, n, 3000), "Guten Abend,\nmeine Damen & Herren."));
    CHECK(at(c, n, 5500) && !strcmp(at(c, n, 5500), "Zweiter"));
    sub_free_cues(c, n); c = NULL; n = cap = 0;

    /* TTML mit Ticks (ARD) */
    CHECK(sub_parse_into("<tt ttp:tickRate=\"10000000\"><body><p begin=\"20000000t\" end=\"35000000t\">Ticks</p></body></tt>",
                         0, &c, &n, &cap) == 1);
    CHECK(n == 1 && c[0].start == 2000 && c[0].end == 3500);
    sub_free_cues(c, n);

    printf(fails ? "%d Fehler\n" : "sub: alle Tests ok\n", fails);
    return fails != 0;
}
