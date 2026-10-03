#ifndef VS_WATCHED_H
#define VS_WATCHED_H

/* Gesehen-Markierungen (ux0:data/VitaStream/watched.txt, ein Schlüssel pro Zeile).
 * Schlüssel: "<plugin-datei>|<eintrags-id>" */
void watched_load(const char *path);
int  watched_get(const char *key);
void watched_set(const char *key, int on);
int  watched_count(void);

#endif
