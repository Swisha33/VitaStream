/* Host-Test: Vorschaubilder laden, og:image finden, verkleinern */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/thumbfetch.h"
#include "../src/net.h"
#include "../src/config.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void one(const char *url, int ew, int eh, int r, int g, int b)
{
    uint8_t *px = NULL; int w = 0, h = 0;
    int res = thumb_fetch(url, NULL, 112, 63, &px, &w, &h);
    printf("  %-48s -> %d (%dx%d) Farbe %d,%d,%d\n", url, res, w, h, px ? px[0] : -1, px ? px[1] : -1, px ? px[2] : -1);
    CHECK(res == 0 && w == ew && h == eh);
    if (px) CHECK(abs(px[0] - r) < 12 && abs(px[1] - g) < 12 && abs(px[2] - b) < 12);
    free(px);
}

int main(int argc, char **argv)
{
    const char *B = argc > 1 ? argv[1] : "http://127.0.0.1:8767";
    config_load();
    g_cfg.custom_dns_enabled = 0;
    net_init();
    char u[256];
    snprintf(u, sizeof u, "%s/img/rot.png", B);          one(u, 112, 63, 200, 30, 30);
    snprintf(u, sizeof u, "%s/img/blau.jpg", B);         one(u, 112, 63, 30, 30, 200);
    snprintf(u, sizeof u, "%s/img/klein.png", B);        one(u, 63, 63, 30, 200, 30);
    snprintf(u, sizeof u, "og:%s/seite.html", B);        one(u, 112, 63, 30, 30, 200);   /* og:image hat Vorrang */

    char out[256];
    CHECK(thumb_find_og_image("<meta content=\"//cdn.x/a.jpg\" property=\"og:image\">", "https://site.x/p/q", out, sizeof out) == 0
          && !strcmp(out, "https://cdn.x/a.jpg"));
    CHECK(thumb_find_og_image("<link rel=\"image_src\" href=\"b.png\">", "https://site.x/p/q", out, sizeof out) == 0
          && !strcmp(out, "https://site.x/p/b.png"));
    CHECK(thumb_find_og_image("<p>nichts</p>", "https://site.x/", out, sizeof out) < 0);
    uint8_t *px; int w, h;
    snprintf(u, sizeof u, "%s/fehlt.png", B);
    CHECK(thumb_fetch(u, NULL, 112, 63, &px, &w, &h) < 0);
    snprintf(u, sizeof u, "%s/bbb.mp4", B);               /* kein Bild */
    CHECK(thumb_fetch(u, NULL, 112, 63, &px, &w, &h) < 0);
    printf(fails ? "%d Fehler\n" : "thumbs: alle Tests ok\n", fails);
    return fails != 0;
}
