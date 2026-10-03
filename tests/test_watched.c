/* Gesehen-Markierungen: setzen, entfernen, speichern, laden, viele Einträge */
#include <stdio.h>
#include <string.h>
#include "../src/watched.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
    remove("w_test.txt");
    watched_load("w_test.txt");
    CHECK(!watched_get("a|1"));
    watched_set("a|1", 1); watched_set("b|https://x/y?z=1", 1); watched_set("a|1", 1);
    CHECK(watched_get("a|1") && watched_get("b|https://x/y?z=1") && watched_count() == 2);
    char k[64];
    for (int i = 0; i < 3000; i++) { snprintf(k, sizeof k, "mediathek.lua|v%d", i); watched_set(k, 1); }
    for (int i = 0; i < 3000; i += 2) { snprintf(k, sizeof k, "mediathek.lua|v%d", i); watched_set(k, 0); }
    CHECK(watched_count() == 1502);
    watched_set("a|1", 0);
    CHECK(!watched_get("a|1") && watched_get("mediathek.lua|v2999") && !watched_get("mediathek.lua|v2998"));
    /* neu laden (wie nach App-Neustart) */
    watched_load("w_test.txt");
    CHECK(watched_count() == 1501 && watched_get("b|https://x/y?z=1") && watched_get("mediathek.lua|v1"));
    remove("w_test.txt");
    printf(fails ? "%d Fehler\n" : "watched: alle Tests ok\n", fails);
    return fails != 0;
}
