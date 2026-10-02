#ifndef VS_THUMBFETCH_H
#define VS_THUMBFETCH_H

#include <stdint.h>

/* Lädt ein Vorschaubild und verkleinert es auf höchstens max_w x max_h (Seitenverhältnis bleibt).
 * url: direkte Bild-URL (PNG/JPEG/GIF/BMP) oder "og:<Seiten-URL>" - dann wird das
 *      og:image/twitter:image der Seite verwendet (nur der Seitenanfang wird geladen).
 * Ergebnis: RGBA8888, *rgba mit free() freigeben. Rückgabe 0 = ok. */
int thumb_fetch(const char *url, const volatile int *abort_flag, int max_w, int max_h,
                uint8_t **rgba, int *w, int *h);

/* Sucht og:image & Co. in HTML; Ergebnis absolut. 0 = gefunden. (für Tests sichtbar) */
int thumb_find_og_image(const char *html, const char *page_url, char *out, int outlen);

#endif
