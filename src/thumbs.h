#ifndef VS_THUMBS_H
#define VS_THUMBS_H

#include <vita2d.h>

/* Vorschaubild-Cache: lädt im Hintergrund, zeigt sofort Platzhalter.
 * Aus dem Hauptthread aufrufen. */
void            thumbs_init(void);
void            thumbs_shutdown(void);
/* Liefert die Textur, wenn schon geladen; stößt sonst das Laden an (NULL). */
vita2d_texture *thumbs_get(const char *url);
/* Noch nicht begonnene Ladeaufträge verwerfen (z. B. beim Listenwechsel). */
void            thumbs_drop_pending(void);
/* Einmal pro Bild aufrufen: fertige Bilder hochladen, Cache begrenzen. */
void            thumbs_tick(void);

#define THUMB_W 112
#define THUMB_H 63

#endif
