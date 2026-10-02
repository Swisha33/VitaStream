#include <stdio.h>
#include <string.h>
#include "../src/adblock.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(void) {
    FILE *f = fopen("bl.txt", "w");
    fputs("# kommentar\n! auch\nads.example.com\n0.0.0.0 tracker.net\n127.0.0.1 evil.org # x\n"
          "||adnet.io^\n||pathrule.com/foo^\n||opt.com^$third-party\n@@||good.adnet.io^\n"
          "0.0.0.0 localhost\nUPPER.Example.ORG\n\n", f);
    fclose(f);
    int n = adblock_load("bl.txt");
    printf("geladen: %d Regeln, %d Block\n", n, adblock_rule_count());
    CHECK(adblock_is_blocked("ads.example.com"));
    CHECK(adblock_is_blocked("x.y.ads.example.com"));
    CHECK(!adblock_is_blocked("example.com"));
    CHECK(!adblock_is_blocked("notads.example.com"));
    CHECK(adblock_is_blocked("tracker.net"));
    CHECK(adblock_is_blocked("cdn.tracker.net."));
    CHECK(adblock_is_blocked("evil.org"));
    CHECK(adblock_is_blocked("adnet.io"));
    CHECK(adblock_is_blocked("s.adnet.io"));
    CHECK(!adblock_is_blocked("good.adnet.io"));
    CHECK(!adblock_is_blocked("a.good.adnet.io"));
    CHECK(!adblock_is_blocked("pathrule.com"));
    CHECK(!adblock_is_blocked("opt.com"));
    CHECK(!adblock_is_blocked("localhost"));
    CHECK(adblock_is_blocked("upper.example.org"));
    CHECK(adblock_is_blocked("UPPER.EXAMPLE.ORG"));
    char h[256];
    adblock_host_from_url("https://user:pw@Sub.Ads.Example.com:8443/p?q=1", h, sizeof h);
    CHECK(!strcmp(h, "sub.ads.example.com"));
    adblock_host_from_url("http://[::1]:80/x", h, sizeof h);
    CHECK(!strcmp(h, "::1"));
    adblock_host_from_url("example.org/path", h, sizeof h);
    CHECK(!strcmp(h, "example.org"));
    /* Realistische Liste aus dem Projekt */
    adblock_clear();
    printf("Projekt-Blockliste: %d\n", adblock_load("../data/blocklist.txt"));
    CHECK(adblock_is_blocked("securepubads.g.doubleclick.net"));
    CHECK(!adblock_is_blocked("www.zdf.de"));
    printf(fails ? "%d Fehler\n" : "adblock: alle Tests ok\n", fails);
    return fails != 0;
}
