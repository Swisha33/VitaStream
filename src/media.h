#ifndef VS_MEDIA_H
#define VS_MEDIA_H

#include <stdint.h>

/* Media-Kern: Demuxing (FFmpeg), HLS, Hardware-Video (vdec_*), Audio (aout_*), A/V-Sync.
 * Alle Funktionen werden vom Hauptthread aufgerufen; die Arbeit läuft in eigenen Threads. */

typedef enum { MS_IDLE, MS_OPENING, MS_PLAYING, MS_ENDED, MS_ERROR } MediaState;

#define MEDIA_SLOTS 4

int        media_open(const char *url, const char *headers);
void       media_close(void);
MediaState media_state(void);
const char *media_error(void);

/* Liefert den Bildpuffer (Slot), der jetzt angezeigt werden soll, oder -1.
 * yuv: 0 = RGBA8888, 1 = YUV420 planar (Y, dann U, dann V; Breite/Höhe des Puffers) */
int        media_current_frame(int *w, int *h, int *yuv);

void       media_toggle_pause(void);
int        media_paused(void);
void       media_seek(int rel_seconds);

int64_t    media_position_ms(void);
int64_t    media_duration_ms(void);   /* 0 = Live/unbekannt */
int        media_buffering(void);
int        media_has_video(void);
int        media_is_live(void);

/* Kurzinfo für die Debug-Anzeige */
void       media_debug(char *buf, int n);

#endif
