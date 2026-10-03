#ifndef VS_SUB_H
#define VS_SUB_H

#include <stdint.h>

/* Untertitel: lädt im Hintergrund WebVTT (Datei oder HLS-Untertitel-Playlist mit
 * VTT-Segmenten), SRT oder TTML (ARD/ZDF) über den eigenen Netzwerkstack und liefert
 * den Text zur aktuellen Abspielposition. */

/* origin_ms: Zeitstempel des Stream-Anfangs (media_origin_ms), für X-TIMESTAMP-MAP */
void        sub_open(const char *url, const char *headers, int64_t origin_ms);
void        sub_close(void);
int         sub_active(void);
/* Text (Zeilen mit '\n' getrennt) zur Position oder NULL */
const char *sub_text_at(int64_t pos_ms);
/* "", "Untertitel werden geladen ..." oder Fehlermeldung */
const char *sub_status(void);

/* --- intern, für Tests --- */
typedef struct { int64_t start, end; char *text; } SubCue;
/* Parst einen Text (VTT/SRT/TTML) und hängt die Einträge an. Rückgabe: Anzahl neuer Einträge */
int  sub_parse_into(const char *text, int64_t origin_ms, SubCue **cues, int *n, int *cap);
void sub_free_cues(SubCue *cues, int n);

#endif
