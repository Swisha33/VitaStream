#ifndef VS_HLS_H
#define VS_HLS_H

#include <stdint.h>

/* Einfacher HLS-Client: lädt Master-/Media-Playlist, wählt eine Qualität
 * (max. 1280x720, H.264), lädt Segmente nacheinander über den eigenen
 * Netzwerkstack und liefert sie als durchgehenden Datenstrom (MPEG-TS oder fMP4). */

typedef struct Hls Hls;

/* abort_flag: Abbruch von außen (Player schließen). err erhält eine Fehlermeldung. */
Hls    *hls_open(const char *url, const char *headers, const volatile int *abort_flag,
                 char *err, int errlen);
void    hls_close(Hls *h);

/* Liest bis zu size Bytes. Rückgabe: >0 Bytes, 0 = Ende (VOD), <0 Fehler.
 * Bei Live-Streams wird blockierend auf neue Segmente gewartet. */
int     hls_read(Hls *h, uint8_t *buf, int size);

int     hls_is_live(const Hls *h);
int64_t hls_duration_us(const Hls *h);          /* 0 bei Live */
/* Springt (nur VOD) zum Segment, das time_us enthält; liefert dessen Startzeit. */
int64_t hls_seek(Hls *h, int64_t time_us);
const char *hls_info(const Hls *h);             /* z. B. "1280x720, 2.5 Mbit/s" */
const char *hls_error(const Hls *h);
/* Separate Tonspur der gewählten Qualität (Media-Playlist-URL) oder NULL, wenn der Ton im Videostream steckt */
const char *hls_audio_url(const Hls *h);
const char *hls_audio_lang(const Hls *h);

/* --- intern, für Tests sichtbar --- */
void    hls_join_url(const char *base, const char *ref, char *out, int outlen);

#endif
