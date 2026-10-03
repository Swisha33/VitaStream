#ifndef VS_UI_H
#define VS_UI_H

#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Farben (ABGR) - per Thema umschaltbar, siehe ui_set_theme */
extern uint32_t COL_BG, COL_PANEL, COL_SEL, COL_ACCENT, COL_TEXT, COL_DIM, COL_OK, COL_BAD;

int         ui_theme_count(void);
const char *ui_theme_name(int i);
void        ui_set_theme(int i);

/* Tasten (nach Region: Kreuz/Kreis vertauscht, falls nötig) */
typedef struct {
    uint32_t pressed;   /* gerade neu gedrückt (mit Auto-Repeat für Steuerkreuz) */
    uint32_t held;
} Input;

extern uint32_t BTN_ACCEPT, BTN_CANCEL;

void ui_init(void);
void ui_term(void);
void ui_poll(Input *in);

void ui_begin(void);
void ui_end(void);

void ui_text(int x, int y, uint32_t col, const char *s);
void ui_text_scaled(int x, int y, uint32_t col, float scale, const char *s);
int  ui_text_width(const char *s);
void ui_text_clipped(int x, int y, int max_w, uint32_t col, const char *s);
void ui_rect(int x, int y, int w, int h, uint32_t col);

void ui_header(const char *title, const char *right);
void ui_footer(const char *hints);

/* Zeichnet eine scrollende Liste; get_label liefert Titel/Untertitel für Index i. */
typedef void (*ListLabelFn)(void *ctx, int i, const char **title, const char **sub);
void ui_list(int count, int cursor, int *scroll, ListLabelFn fn, void *ctx);

/* Liste mit Vorschaubild links (thumb = URL oder NULL -> Platzhalter mit Initialen) */
#define LIST_FLAG_WATCHED 1
typedef void (*ListThumbFn)(void *ctx, int i, const char **title, const char **sub, const char **thumb, int *flags);
void ui_list_thumbs(int count, int cursor, int *scroll, ListThumbFn fn, void *ctx);
int  ui_list_thumbs_visible(void);

/* Auswahlmenü (blockierend). Rückgabe: Index oder -1 bei Abbruch. */
int  ui_menu(const char *title, const char **options, int count);

/* Bildschirmtastatur (blockierend). Rückgabe 1 bei Bestätigung. */
int  ui_input_text(const char *title, const char *initial, char *out, int outlen);

void ui_spinner(const char *msg);
void ui_message(const char *title, const char *msg);   /* blockierend, wartet auf Taste */

#endif
