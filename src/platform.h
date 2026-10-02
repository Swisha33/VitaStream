#ifndef VS_PLATFORM_H
#define VS_PLATFORM_H

/* Plattformfunktionen, die der Media-Kern (media.c) benötigt.
 * Vita: vdec_vita.c, aout_vita.c, player.c   Host-Tests: tests/platform_host.c */

#include <stdint.h>

int64_t plat_now_us(void);

/* ---------- Hardware-H.264-Decoder (Ausgabe RGBA8888) ---------- */

typedef struct {
    void *pixels;     /* Zielpuffer RGBA8888 */
    int   pitch;      /* in Pixeln */
    int   width;      /* Puffergröße */
    int   height;
} VdecTarget;

typedef struct {
    int64_t pts90k;   /* -1, wenn unbekannt */
    int     width;    /* sichtbare Größe nach Cropping */
    int     height;
} VdecResult;

int  vdec_open(int coded_w, int coded_h);
/* Dekodiert eine Access Unit (Annex-B). Rückgabe: 1 = Bild in dst, 0 = (noch) kein Bild, <0 Fehler */
int  vdec_decode(const uint8_t *au, int len, int64_t pts90k, const VdecTarget *dst, VdecResult *res);
void vdec_reset(void);      /* nach Sprüngen: interne Referenzbilder verwerfen */
void vdec_close(void);
const char *vdec_last_error(void);

/* ---------- Bildpuffer (vom Player als Textur angezeigt) ---------- */

/* Legt Puffer Nummer slot an (RGBA8888, mindestens w x h); liefert Zeiger und pitch (Pixel). */
void *fb_create(int slot, int w, int h, int *pitch);
void  fb_destroy(int slot);

/* ---------- Audioausgabe (16 Bit, interleaved) ---------- */

#define AOUT_GRAIN 1024   /* Samples pro Kanal und aout_write-Aufruf */

int  aout_open(int rate, int channels);
int  aout_write(const int16_t *pcm);      /* blockiert, bis der Puffer übernommen wurde */
void aout_close(void);

#endif
