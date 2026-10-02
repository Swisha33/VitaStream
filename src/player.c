/* Player-Oberfläche auf der Vita: zeigt die Bilder des Media-Kerns (media.c)
 * als vita2d-Texturen an und stellt Status (Laden/Puffern/Nur Ton) dar. */
#include "player.h"
#include "media.h"
#include "platform.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <vita2d.h>

static vita2d_texture *s_tex[MEDIA_SLOTS];
static int             s_tex_w[MEDIA_SLOTS], s_tex_h[MEDIA_SLOTS], s_tex_yuv[MEDIA_SLOTS];
static char            s_err[256];
static int             s_open;
static int             s_debug;

/* ---------- Bildpuffer für media.c ---------- */

void *fb_create(int slot, int w, int h, int *pitch)
{
    if (slot < 0 || slot >= MEDIA_SLOTS) return NULL;
    s_tex[slot] = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!s_tex[slot]) return NULL;
    vita2d_texture_set_filters(s_tex[slot], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    *pitch = vita2d_texture_get_stride(s_tex[slot]) / 4;
    s_tex_w[slot] = w;
    s_tex_h[slot] = h;
    s_tex_yuv[slot] = 0;
    void *px = vita2d_texture_get_datap(s_tex[slot]);
    memset(px, 0, (size_t)vita2d_texture_get_stride(s_tex[slot]) * h);
    return px;
}

void fb_destroy(int slot)
{
    if (slot < 0 || slot >= MEDIA_SLOTS || !s_tex[slot]) return;
    vita2d_free_texture(s_tex[slot]);
    s_tex[slot] = NULL;
}

/* ---------- API ---------- */

int player_open(const char *url, const char *headers)
{
    player_close();
    s_err[0] = 0;
    if (media_open(url, headers) < 0) {
        snprintf(s_err, sizeof s_err, "%s", media_error());
        return -1;
    }
    s_open = 1;
    return 0;
}

void player_close(void)
{
    if (!s_open) return;
    vita2d_wait_rendering_done();   /* Texturen werden evtl. noch gezeichnet */
    media_close();
    s_open = 0;
}

int player_active(void)
{
    if (!s_open) return 0;
    MediaState st = media_state();
    if (st == MS_ERROR) {
        snprintf(s_err, sizeof s_err, "%s", media_error());
        return 0;
    }
    if (st == MS_ENDED) { s_err[0] = 0; return 0; }
    return 1;
}

void player_toggle_debug(void) { s_debug = !s_debug; }

void player_draw(void)
{
    if (!s_open) return;
    int w = 0, h = 0, yuv = 0;
    int slot = media_current_frame(&w, &h, &yuv);
    if (slot >= 0 && s_tex[slot] && w > 0 && h > 0) {
        if (yuv != s_tex_yuv[slot]) {
            /* gleicher Speicher, andere Interpretation: RGBA vom Hardware-, YUV vom Software-Decoder */
            sceGxmTextureInitLinear(&s_tex[slot]->gxm_tex, vita2d_texture_get_datap(s_tex[slot]),
                                    yuv ? SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0 : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                    s_tex_w[slot], s_tex_h[slot], 0);
            vita2d_texture_set_filters(s_tex[slot], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            s_tex_yuv[slot] = yuv;
        }
        float sx = (float)SCREEN_W / w, sy = (float)SCREEN_H / h;
        float s = sx < sy ? sx : sy;
        float dw = w * s, dh = h * s;
        vita2d_draw_texture_part_scale(s_tex[slot], (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2,
                                       0, 0, w, h, s, s);
    }

    MediaState st = media_state();
    if (st == MS_OPENING) {
        ui_spinner("Stream wird geoeffnet...");
    } else if (media_buffering() && !media_paused()) {
        ui_spinner("Puffern...");
    } else if (st == MS_PLAYING && !media_has_video()) {
        const char *t = "Nur Ton";
        ui_text(SCREEN_W / 2 - ui_text_width(t) / 2, SCREEN_H / 2, COL_DIM, t);
    }

    if (s_debug) {
        char buf[320];
        media_debug(buf, sizeof buf);
        ui_rect(0, 0, SCREEN_W, 52, 0xC0000000);
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        ui_text_scaled(10, 20, COL_TEXT, 0.8f, buf);
        if (nl) ui_text_scaled(10, 42, COL_DIM, 0.8f, nl + 1);
    }
}

void     player_toggle_pause(void) { if (s_open) media_toggle_pause(); }
int      player_paused(void)       { return s_open && media_paused(); }
void     player_seek_rel(int sec)  { if (s_open) media_seek(sec); }
uint64_t player_position_ms(void)  { return s_open ? (uint64_t)media_position_ms() : 0; }
uint64_t player_duration_ms(void)  { return s_open ? (uint64_t)media_duration_ms() : 0; }
const char *player_error(void)     { return s_err; }
