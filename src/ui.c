#include "ui.h"
#include "thumbs.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>

/* ---------------------------------------------------------------- Themen */

uint32_t COL_BG, COL_PANEL, COL_SEL, COL_ACCENT, COL_TEXT, COL_DIM, COL_OK, COL_BAD;

typedef struct { const char *name; uint32_t bg, panel, sel, accent, text, dim; } Theme;
static const Theme s_themes[] = {
    /* name                       bg          panel       sel         accent      text        dim */
    { "Standard (VitaStream)",   0xFF1E1A16, 0xFF2C2620, 0xFF6A4A2A, 0xFF33B5FF, 0xFFF0F0F0, 0xFF9A9A9A },
    { "PS1 (Grau, Logo-Gelb)",   0xFF262626, 0xFF3A3A3A, 0xFF686868, 0xFF00C8FF, 0xFFEDEDED, 0xFFA0A0A0 },
    { "PS2 (Tiefblau)",          0xFF200A00, 0xFF381804, 0xFF7A4210, 0xFFFFD080, 0xFFF4F0EA, 0xFFB09A8A },
    { "PS3 (XMB-Blau)",          0xFF40200C, 0xFF542E14, 0xFF9A6232, 0xFFFFF0D8, 0xFFFFFFFF, 0xFFD0B8A8 },
    { "PSP (Silber)",            0xFF343434, 0xFF4A4A4A, 0xFF7C7C7C, 0xFFF0F0F0, 0xFFFFFFFF, 0xFFB4B4B4 },
    { "Vita (LiveArea-Blau)",    0xFF4A2810, 0xFF603618, 0xFFB06C30, 0xFFF0C840, 0xFFFFFFFF, 0xFFD8C0A8 },
    { "Dunkel (OLED-Schwarz)",   0xFF000000, 0xFF141414, 0xFF3A3A3A, 0xFF33B5FF, 0xFFE8E8E8, 0xFF8A8A8A },
    { "Hell",                    0xFFEFEDEA, 0xFFFFFFFF, 0xFFE8C8A0, 0xFFB05A10, 0xFF202020, 0xFF707070 },
};
#define NTHEMES (int)(sizeof s_themes / sizeof *s_themes)
static int s_theme;
/* eigenes Thema (Themen-Editor): letzter Eintrag der Liste */
static Theme s_custom = { "Eigenes Thema", 0xFF1E1A16, 0xFF2C2620, 0xFF6A4A2A, 0xFF33B5FF, 0xFFF0F0F0, 0xFF9A9A9A };

int ui_theme_count(void) { return NTHEMES + 1; }
const char *ui_theme_name(int i) { return (i >= 0 && i < NTHEMES) ? s_themes[i].name : (i == NTHEMES ? s_custom.name : "?"); }
int ui_theme_custom_index(void) { return NTHEMES; }

void ui_get_custom(uint32_t c[6])
{
    c[0] = s_custom.bg; c[1] = s_custom.panel; c[2] = s_custom.sel;
    c[3] = s_custom.accent; c[4] = s_custom.text; c[5] = s_custom.dim;
}

void ui_set_custom(const uint32_t c[6])
{
    s_custom.bg = c[0]; s_custom.panel = c[1]; s_custom.sel = c[2];
    s_custom.accent = c[3]; s_custom.text = c[4]; s_custom.dim = c[5];
    if (s_theme == NTHEMES) ui_set_theme(NTHEMES);
}

void ui_set_theme(int i)
{
    if (i < 0 || i > NTHEMES) i = 0;
    s_theme = i;
    const Theme *t = i == NTHEMES ? &s_custom : &s_themes[i];
    COL_BG = t->bg; COL_PANEL = t->panel; COL_SEL = t->sel; COL_ACCENT = t->accent;
    COL_TEXT = t->text; COL_DIM = t->dim;
    COL_OK = 0xFF66CC66;
    COL_BAD = 0xFF5555EE;
    vita2d_set_clear_color(COL_BG);
}

static vita2d_pgf *s_font;
uint32_t BTN_ACCEPT = SCE_CTRL_CROSS, BTN_CANCEL = SCE_CTRL_CIRCLE;

#define ROW_H       52
#define LIST_TOP    56
#define LIST_BOTTOM (SCREEN_H - 40)

void ui_init(void)
{
    vita2d_init();
    ui_set_theme(s_theme);
    vita2d_set_vblank_wait(1);
    s_font = vita2d_load_default_pgf();

    SceAppUtilInitParam ip; SceAppUtilBootParam bp;
    memset(&ip, 0, sizeof ip); memset(&bp, 0, sizeof bp);
    sceAppUtilInit(&ip, &bp);
    int enter = 1;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    if (enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
        BTN_ACCEPT = SCE_CTRL_CIRCLE;
        BTN_CANCEL = SCE_CTRL_CROSS;
    }
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
}

void ui_term(void)
{
    vita2d_fini();
    vita2d_free_pgf(s_font);
    sceAppUtilShutdown();
}

void ui_poll(Input *in)
{
    static uint32_t prev;
    static int repeat_timer;
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);

    uint32_t b = pad.buttons;
    /* Linker Stick als Steuerkreuz */
    if (pad.ly < 40)  b |= SCE_CTRL_UP;
    if (pad.ly > 215) b |= SCE_CTRL_DOWN;
    if (pad.lx < 40)  b |= SCE_CTRL_LEFT;
    if (pad.lx > 215) b |= SCE_CTRL_RIGHT;

    in->held = b;
    in->pressed = b & ~prev;

    const uint32_t rep = SCE_CTRL_UP | SCE_CTRL_DOWN;
    if (b & rep) {
        if (in->pressed & rep) repeat_timer = 22;
        else if (--repeat_timer <= 0) { in->pressed |= b & rep; repeat_timer = 5; }
    }
    prev = b;
}

void ui_begin(void) { vita2d_start_drawing(); vita2d_clear_screen(); }

void ui_end(void)
{
    thumbs_tick();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

void ui_text_scaled(int x, int y, uint32_t col, float scale, const char *s)
{
    vita2d_pgf_draw_text(s_font, x, y, col, scale, s);
}

void ui_text(int x, int y, uint32_t col, const char *s) { ui_text_scaled(x, y, col, 1.0f, s); }

int ui_text_width(const char *s) { return vita2d_pgf_text_width(s_font, 1.0f, s); }

void ui_text_clipped(int x, int y, int max_w, uint32_t col, const char *s)
{
    if (ui_text_width(s) <= max_w) { ui_text(x, y, col, s); return; }
    char buf[512];
    int n = (int)strlen(s);
    if (n > (int)sizeof buf - 4) n = sizeof buf - 4;
    while (n > 0) {
        /* nur an UTF-8-Zeichengrenzen kürzen */
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        memcpy(buf, s, n);
        strcpy(buf + n, "...");
        if (ui_text_width(buf) <= max_w) break;
        n--;
    }
    ui_text(x, y, col, buf);
}

void ui_rect(int x, int y, int w, int h, uint32_t col) { vita2d_draw_rectangle(x, y, w, h, col); }

void ui_header(const char *title, const char *right)
{
    ui_rect(0, 0, SCREEN_W, 46, COL_PANEL);
    ui_rect(0, 46, SCREEN_W, 2, COL_ACCENT);
    ui_text_clipped(18, 32, 640, COL_TEXT, title);
    if (right) ui_text(SCREEN_W - 18 - ui_text_width(right), 32, COL_DIM, right);
}

static char s_status[256];
void ui_set_status(const char *s) { snprintf(s_status, sizeof s_status, "%s", s ? s : ""); }

void ui_footer(const char *hints)
{
    if (s_status[0]) {
        /* Statuszeile (Download, Hintergrundwiedergabe) über der Fußzeile */
        ui_rect(0, SCREEN_H - 60, SCREEN_W, 26, 0xE0000000);
        ui_text_clipped(18, SCREEN_H - 41, SCREEN_W - 36, COL_ACCENT, s_status);
    }
    ui_rect(0, SCREEN_H - 34, SCREEN_W, 34, COL_PANEL);
    ui_text_scaled(18, SCREEN_H - 11, COL_DIM, 0.85f, hints);
}

void ui_list(int count, int cursor, int *scroll, ListLabelFn fn, void *ctx)
{
    int visible = (LIST_BOTTOM - LIST_TOP) / ROW_H;
    if (cursor < *scroll) *scroll = cursor;
    if (cursor >= *scroll + visible) *scroll = cursor - visible + 1;
    if (*scroll < 0) *scroll = 0;

    if (count == 0) {
        ui_text(40, LIST_TOP + 60, COL_DIM, "Keine Eintraege");
        return;
    }
    for (int r = 0; r < visible && *scroll + r < count; r++) {
        int i = *scroll + r;
        int y = LIST_TOP + r * ROW_H;
        if (i == cursor) {
            ui_rect(10, y, SCREEN_W - 20, ROW_H - 4, COL_SEL);
            ui_rect(10, y, 4, ROW_H - 4, COL_ACCENT);
        }
        const char *t = "", *sub = NULL;
        fn(ctx, i, &t, &sub);
        if (sub && *sub) {
            ui_text_clipped(28, y + 22, SCREEN_W - 70, COL_TEXT, t);
            vita2d_pgf_draw_text(s_font, 28, y + 42, COL_DIM, 0.8f, sub);
        } else {
            ui_text_clipped(28, y + 31, SCREEN_W - 70, COL_TEXT, t);
        }
    }
    /* Scrollbalken */
    if (count > visible) {
        int track = LIST_BOTTOM - LIST_TOP;
        int h = track * visible / count;
        if (h < 20) h = 20;
        int y = LIST_TOP + (track - h) * *scroll / (count - visible);
        ui_rect(SCREEN_W - 8, y, 4, h, COL_DIM);
    }
}

/* ---------- Liste mit Vorschaubildern ---------- */

#define TROW_H 74

int ui_list_thumbs_visible(void) { return (LIST_BOTTOM - LIST_TOP) / TROW_H; }

static uint32_t placeholder_color(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    static const uint32_t pal[] = { 0xFF7A4A2E, 0xFF2E7A4A, 0xFF4A2E7A, 0xFF7A2E5E, 0xFF2E5E7A, 0xFF5E7A2E, 0xFF3A3A8A, 0xFF8A5A2A };
    return pal[h % (sizeof pal / sizeof *pal)];
}

static void draw_placeholder(int x, int y, int w, int h, const char *title)
{
    ui_rect(x, y, w, h, placeholder_color(title));
    /* Initialen: erste zwei Buchstaben/Ziffern */
    char ini[8] = {0};
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p && n < 2; p++) {
        if (*p < 0x80 && isalnum(*p)) ini[n++] = (char)toupper(*p);
        else if (*p >= 0xC0) {                    /* UTF-8-Zeichen komplett übernehmen */
            int len = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : 2;
            if (n + len > 6) break;
            memcpy(ini + n, p, len);
            n += len;
            p += len - 1;
            break;
        }
    }
    int tw = vita2d_pgf_text_width(s_font, 1.2f, ini);
    vita2d_pgf_draw_text(s_font, x + (w - tw) / 2, y + h / 2 + 9, 0xDDFFFFFF, 1.2f, ini);
}

void ui_list_thumbs(int count, int cursor, int *scroll, ListThumbFn fn, void *ctx)
{
    int visible = ui_list_thumbs_visible();
    if (cursor < *scroll) *scroll = cursor;
    if (cursor >= *scroll + visible) *scroll = cursor - visible + 1;
    if (*scroll < 0) *scroll = 0;
    if (count == 0) {
        ui_text(40, LIST_TOP + 60, COL_DIM, "Keine Eintraege");
        return;
    }
    for (int r = 0; r < visible && *scroll + r < count; r++) {
        int i = *scroll + r;
        int y = LIST_TOP + r * TROW_H;
        if (i == cursor) {
            ui_rect(10, y, SCREEN_W - 20, TROW_H - 4, COL_SEL);
            ui_rect(10, y, 4, TROW_H - 4, COL_ACCENT);
        }
        const char *t = "", *sub = NULL, *thumb = NULL;
        int flags = 0;
        fn(ctx, i, &t, &sub, &thumb, &flags);

        int bx = 22, by = y + (TROW_H - 4 - THUMB_H) / 2;
        vita2d_texture *tex = thumb ? thumbs_get(thumb) : NULL;
        if (tex) {
            int tw = vita2d_texture_get_width(tex), th = vita2d_texture_get_height(tex);
            ui_rect(bx, by, THUMB_W, THUMB_H, 0xFF101010);
            vita2d_draw_texture(tex, bx + (THUMB_W - tw) / 2, by + (THUMB_H - th) / 2);
        } else {
            draw_placeholder(bx, by, THUMB_W, THUMB_H, t);
        }
        if (flags & LIST_FLAG_WATCHED) {
            /* grünes Band "Gesehen" über dem Vorschaubild */
            ui_rect(bx, by + THUMB_H - 16, THUMB_W, 16, 0xD0309030);
            const char *g = "Gesehen";
            int gw = vita2d_pgf_text_width(s_font, 0.7f, g);
            vita2d_pgf_draw_text(s_font, bx + (THUMB_W - gw) / 2, by + THUMB_H - 3, 0xFFFFFFFF, 0.7f, g);
        }
        if ((flags & LIST_FLAG_STARTED) && !(flags & LIST_FLAG_WATCHED)) {
            /* angefangen: Band + Fortschrittsbalken */
            int pct = (flags >> 8) & 0xFF;
            ui_rect(bx, by + THUMB_H - 20, THUMB_W, 16, 0xD02060C0);
            const char *g = "Angefangen";
            int gw = vita2d_pgf_text_width(s_font, 0.7f, g);
            vita2d_pgf_draw_text(s_font, bx + (THUMB_W - gw) / 2, by + THUMB_H - 7, 0xFFFFFFFF, 0.7f, g);
            ui_rect(bx, by + THUMB_H - 4, THUMB_W, 4, 0xC0303030);
            ui_rect(bx, by + THUMB_H - 4, THUMB_W * pct / 100, 4, 0xFF20A0FF);
        }
        uint32_t tcol = (flags & LIST_FLAG_WATCHED) ? COL_DIM : COL_TEXT;

        int tx = bx + THUMB_W + 14, tmax = SCREEN_W - tx - 30;
        if (sub && *sub) {
            ui_text_clipped(tx, y + 30, tmax, tcol, t);
            char buf[256];
            snprintf(buf, sizeof buf, "%s", sub);
            /* Untertitel ebenfalls kürzen */
            if (vita2d_pgf_text_width(s_font, 0.8f, buf) > tmax) {
                int n = (int)strlen(buf);
                while (n > 0 && vita2d_pgf_text_width(s_font, 0.8f, buf) > tmax - 20) {
                    do { n--; } while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
                    strcpy(buf + n, "...");
                }
            }
            vita2d_pgf_draw_text(s_font, tx, y + 54, COL_DIM, 0.8f, buf);
        } else {
            ui_text_clipped(tx, y + 42, tmax, tcol, t);
        }
    }
    if (count > visible) {
        int track = LIST_BOTTOM - LIST_TOP;
        int h = track * visible / count;
        if (h < 20) h = 20;
        int y = LIST_TOP + (track - h) * *scroll / (count - visible);
        ui_rect(SCREEN_W - 8, y, 4, h, COL_DIM);
    }
}

/* ---------- Auswahlmenü ---------- */

typedef struct { const char **opt; } MenuCtx;
static void menu_label(void *ctx, int i, const char **t, const char **sub)
{
    *t = ((MenuCtx *)ctx)->opt[i];
    *sub = NULL;
}

int ui_menu(const char *title, const char **options, int count)
{
    MenuCtx mc = { options };
    int cursor = 0, scroll = 0;
    Input in;
    for (;;) {
        ui_poll(&in);
        if (in.pressed & SCE_CTRL_UP)   cursor = (cursor + count - 1) % count;
        if (in.pressed & SCE_CTRL_DOWN) cursor = (cursor + 1) % count;
        if (in.pressed & BTN_ACCEPT) return cursor;
        if (in.pressed & (BTN_CANCEL | SCE_CTRL_SQUARE)) return -1;
        ui_begin();
        ui_header(title, NULL);
        ui_list(count, cursor, &scroll, menu_label, &mc);
        ui_footer("Bestaetigen: Auswaehlen   Zurueck: Abbrechen");
        ui_end();
    }
}

/* ---------- UTF-8 <-> UTF-16 ---------- */

static void utf8_to_utf16(const char *in, uint16_t *out, int max)
{
    const unsigned char *s = (const unsigned char *)in;
    int n = 0;
    while (*s && n < max - 1) {
        uint32_t cp;
        if (*s < 0x80)              { cp = *s++; }
        else if ((*s & 0xE0) == 0xC0) { cp = (*s++ & 0x1F) << 6; if (*s) cp |= *s++ & 0x3F; }
        else if ((*s & 0xF0) == 0xE0) { cp = (*s++ & 0x0F) << 12; if (*s) cp |= (*s++ & 0x3F) << 6; if (*s) cp |= *s++ & 0x3F; }
        else { s++; cp = '?'; }
        out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
}

static void utf16_to_utf8(const uint16_t *in, char *out, int max)
{
    int n = 0;
    for (; *in; in++) {
        uint32_t c = *in;
        if (c < 0x80)       { if (n + 1 >= max) break; out[n++] = (char)c; }
        else if (c < 0x800) { if (n + 2 >= max) break; out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else                { if (n + 3 >= max) break; out[n++] = (char)(0xE0 | (c >> 12));
                              out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = 0;
}

int ui_input_text(const char *title, const char *initial, char *out, int outlen)
{
    static uint16_t t16[128], init16[512], buf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
    utf8_to_utf16(title, t16, 128);
    int is_secret = title && (strstr(title, "Passwort") || strstr(title, "passwort") || strstr(title, "Password"));
    utf8_to_utf16((initial && !is_secret) ? initial : "", init16, 512);
    memset(buf16, 0, sizeof buf16);

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;  /* Systemsprache */
    p.languagesForced    = SCE_FALSE;
    p.type               = SCE_IME_TYPE_DEFAULT;
    p.option             = 0;
    /* Passwortfelder verdeckt eingeben */
    int secret = title && (strstr(title, "Passwort") || strstr(title, "passwort") || strstr(title, "Password"));
    p.textBoxMode        = secret ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title              = t16;
    p.maxTextLength      = 500;
    p.initialText        = init16;
    p.inputTextBuffer    = buf16;
    if (sceImeDialogInit(&p) < 0) return 0;

    int ok = 0;
    for (;;) {
        ui_begin();
        ui_header(title, NULL);
        ui_end();
        SceCommonDialogStatus st = sceImeDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult r;
            memset(&r, 0, sizeof r);
            sceImeDialogGetResult(&r);
            ok = (r.button == SCE_IME_DIALOG_BUTTON_ENTER);
            break;
        }
    }
    sceImeDialogTerm();
    if (ok) utf16_to_utf8(buf16, out, outlen);
    return ok;
}

void ui_spinner(const char *msg)
{
    static int frame;
    frame++;
    const int n = 12;
    int cx = SCREEN_W / 2, cy = SCREEN_H / 2 - 20;
    for (int i = 0; i < n; i++) {
        int on = (frame / 4) % n;
        int d = (i - on + n) % n;
        uint32_t a = 0xFF - d * 18;
        static const int ox[12] = { 0, 15, 26, 30, 26, 15, 0, -15, -26, -30, -26, -15 };
        static const int oy[12] = { -30, -26, -15, 0, 15, 26, 30, 26, 15, 0, -15, -26 };
        vita2d_draw_fill_circle(cx + ox[i], cy + oy[i], 5, (a << 24) | (COL_ACCENT & 0xFFFFFF));
    }
    ui_text(cx - ui_text_width(msg) / 2, cy + 70, COL_TEXT, msg);
}

void ui_message(const char *title, const char *msg)
{
    Input in;
    for (;;) {
        ui_poll(&in);
        if (in.pressed & (BTN_ACCEPT | BTN_CANCEL)) break;
        ui_begin();
        ui_header(title, NULL);
        /* einfacher Zeilenumbruch */
        char line[256];
        const char *p = msg;
        int y = 110;
        while (*p && y < SCREEN_H - 60) {
            int n = 0, last_space = -1;
            while (p[n] && p[n] != '\n' && n < (int)sizeof line - 1) {
                line[n] = p[n];
                line[n + 1] = 0;
                if (p[n] == ' ') last_space = n;
                if (ui_text_width(line) > SCREEN_W - 80) {
                    if (last_space > 0) n = last_space;
                    else while (n > 0 && ((unsigned char)p[n] & 0xC0) == 0x80) n--;
                    if (n == 0) n = 1;
                    break;
                }
                n++;
            }
            line[n] = 0;
            ui_text(40, y, COL_TEXT, line);
            p += n;
            while (*p == ' ' || *p == '\n') p++;
            y += 30;
        }
        ui_footer("Weiter mit Bestaetigen");
        ui_end();
    }
}
