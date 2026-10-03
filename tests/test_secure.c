/* Host-Test: verschlüsselte Ablage */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/secure.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    mkdir("sec_run", 0777);
    secure_set_dir("sec_run/.secure");
    secure_set_device_id((const unsigned char *)"GeraetA-12345678");
    CHECK(secure_put("jellyfin", "server=http://192.168.1.5:8096\ntoken=ABCDEF0123456789\nuid=u1") == 0);
    char *v = secure_get("jellyfin");
    CHECK(v && !strcmp(v, "server=http://192.168.1.5:8096\ntoken=ABCDEF0123456789\nuid=u1"));
    free(v);
    /* Klartext nicht in der Datei */
    FILE *f = fopen("sec_run/.secure/jellyfin.bin", "r");
    char raw[4096] = ""; if (f) { fread(raw, 1, sizeof raw - 1, f); fclose(f); }
    CHECK(raw[0] && !strstr(raw, "token") && !strstr(raw, "ABCDEF"));
    /* zweimal speichern -> anderer Inhalt (neuer IV) */
    secure_put("jellyfin", "x");
    f = fopen("sec_run/.secure/jellyfin.bin", "r");
    char raw2[4096] = ""; if (f) { fread(raw2, 1, sizeof raw2 - 1, f); fclose(f); }
    secure_put("jellyfin", "x");
    f = fopen("sec_run/.secure/jellyfin.bin", "r");
    char raw3[4096] = ""; if (f) { fread(raw3, 1, sizeof raw3 - 1, f); fclose(f); }
    CHECK(strcmp(raw2, raw3) != 0);
    /* anderes Gerät kann nicht entschlüsseln */
    secure_set_device_id((const unsigned char *)"GeraetB-87654321");
    CHECK(secure_get("jellyfin") == NULL);
    secure_set_device_id((const unsigned char *)"GeraetA-12345678");
    v = secure_get("jellyfin");
    CHECK(v && !strcmp(v, "x"));
    free(v);
    /* ungültige Namen, Löschen */
    CHECK(secure_put("../boese", "x") != 0 && secure_get("a/b") == NULL);
    CHECK(secure_put("jellyfin", NULL) == 0 && secure_get("jellyfin") == NULL);
    printf(fails ? "%d Fehler\n" : "secure: alle Tests ok\n", fails);
    return fails != 0;
}
