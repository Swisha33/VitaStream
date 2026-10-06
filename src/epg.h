#ifndef VS_EPG_H
#define VS_EPG_H

#include <stdint.h>

/* Programmführer aus XMLTV (auch .gz): lädt im Hintergrund, hält nur ein Zeitfenster
 * (jetzt -1 h bis +12 h) im Speicher und liefert "jetzt"/"danach" pro Sender. */

void        epg_load(const char *url);          /* startet Laden (nichts, wenn dieselbe URL schon geladen/lädt); NULL/"" = Standard */
void        epg_set_default(const char *url);   /* Adresse aus den Einstellungen */
const char *epg_status(void);                   /* "", "lädt ...", "1234 Sendungen" oder Fehler */
int         epg_ready(void);

typedef struct {
    char    title[160];
    int64_t start, stop;                        /* Unix-Zeit (UTC) */
} EpgShow;

/* Sender per tvg-id oder (Ersatz) Namen suchen. Rückgabe: Anzahl gefüllter Einträge (0..2): jetzt, danach */
int         epg_now(const char *tvg_id, const char *name, int64_t now, EpgShow out[2]);

int64_t     epg_time_now(void);                 /* Unix-Zeit (UTC) */
int         epg_tz_offset(void);                /* Sekunden Ortszeit - UTC */
void        epg_fmt_hm(int64_t t, char *out, int n);   /* "20:15" in Ortszeit */

/* intern/Tests: XMLTV-Datei (auch gzip) direkt einlesen */
int         epg_parse_file(const char *path, int64_t now);
int64_t     epg_parse_time(const char *s);      /* "20261006181500 +0200" -> UTC */
void        epg_shutdown(void);

#endif
