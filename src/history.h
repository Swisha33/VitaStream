#ifndef VS_HISTORY_H
#define VS_HISTORY_H

#include <stdint.h>

/* Wiedergabe-Fortschritt (progress.txt) und "Zuletzt gesehen" (history.txt).
 * Schlüssel wie bei watched: "<plugin-datei>|<eintrags-id>" */

void history_load(const char *dir);

/* Fortschritt: 1 = vorhanden */
int  progress_get(const char *key, int64_t *pos_ms, int64_t *dur_ms);
void progress_set(const char *key, int64_t pos_ms, int64_t dur_ms);
void progress_clear(const char *key);
/* Prozent (1..99) für die Anzeige "angefangen", 0 = keiner */
int  progress_percent(const char *key);

typedef struct {
    char src[64];          /* Plugin-Datei, z. B. "youtube.lua" */
    char id[1024];         /* Eintrags-ID in der Quelle */
    char key[1100];        /* Fortschritts-/Gesehen-Schlüssel */
    char title[256];
    char thumb[512];
    char url[2048];        /* aufgelöster Stream (bei save_ref-Quellen neu auflösen) */
    char headers[1024];
    int  save_ref;
} HistEntry;

#define HISTORY_MAX 50
void history_add(const HistEntry *e);       /* nach vorn, doppelte ersetzen */
int  history_count(void);
const HistEntry *history_get(int i);
void history_remove(int i);
void history_clear(void);

#endif
