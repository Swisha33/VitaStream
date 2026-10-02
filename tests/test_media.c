/* Host-Test des Media-Kerns: spielt eine URL "ab" (simulierte Uhr) und prüft Ablauf und Sync.
 * Aufruf: test_media URL SEKUNDEN [SPRUNG_SEK] [erwartung: ok|error|audio]  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/media.h"
#include "../src/net.h"
#include "../src/config.h"
#include "../src/adblock.h"
#include "../src/platform.h"

extern int g_vdec_frames, g_vdec_errors, g_vdec_opened, g_aout_chunks;

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "test_media URL SEK [SPRUNG] [ok|error|audio]\n"); return 2; }
    const char *url = argv[1];
    double secs = atof(argv[2]);
    int seek = argc > 3 ? atoi(argv[3]) : 0;
    const char *expect = argc > 4 ? argv[4] : "ok";
    double speed = getenv("VS_SPEED") ? atof(getenv("VS_SPEED")) : 1.0;

    config_load();               /* setzt Standardwerte */
    g_cfg.custom_dns_enabled = 0;
    if (getenv("VS_BLOCK")) { FILE *f = fopen("bl_media.txt", "w"); fputs(getenv("VS_BLOCK"), f); fclose(f); adblock_load("bl_media.txt"); }
    net_init();

    media_open(url, getenv("VS_HEADERS"));
    int64_t t0 = plat_now_us(), last_pos = -1, pos_before = 0;
    int seek_done = 0, backwards = 0;
    int frames_changed = 0, last_slot = -2;
    MediaState st;
    while ((st = media_state()) == MS_OPENING || st == MS_PLAYING) {
        int64_t t = plat_now_us() - t0;
        if (t > secs * 1e6) break;
        int w, h;
        int yuv;
        int slot = media_current_frame(&w, &h, &yuv);
        if (slot != last_slot) { frames_changed++; last_slot = slot; }
        int64_t pos = media_position_ms();
        if (st == MS_PLAYING && last_pos >= 0 && pos + 50 < last_pos && !seek_done && !getenv("VS_JUMPS")) backwards++;
        last_pos = pos;
        if (seek && !seek_done && t > secs * 1e6 / 2 && st == MS_PLAYING) {
            pos_before = pos;
            media_seek(seek);
            seek_done = 1;
        }
        usleep((useconds_t)(16667 / speed));
    }
    char dbg[320];
    media_debug(dbg, sizeof dbg);
    for (char *p = dbg; *p; p++) if (*p == '\n') *p = ' ';
    st = media_state();
    int64_t pos = media_position_ms(), dur = media_duration_ms();
    printf("  Zustand=%d Fehler=\"%s\"\n  %s\n  Position=%.1fs Dauer=%.1fs Live=%d Video=%d  HW-Bilder=%d Fehler=%d Audio-Chunks=%d Anzeigewechsel=%d Rueckspruenge=%d\n",
           st, media_error(), dbg, pos / 1000.0, dur / 1000.0, media_is_live(), media_has_video(),
           g_vdec_frames, g_vdec_errors, g_aout_chunks, frames_changed, backwards);
    if (seek) printf("  Sprung %+ds: vorher %.1fs, danach %.1fs\n", seek, pos_before / 1000.0, pos / 1000.0);

    int ok;
    if (!strcmp(expect, "error")) ok = st == MS_ERROR;
    else if (!strcmp(expect, "audio")) ok = st != MS_ERROR && !media_has_video() && g_aout_chunks > 50;
    else {
        double played = secs - (seek_done ? 0 : 0);
        int allowed_errors = getenv("VS_HW_FAIL") ? 6 : 0;   /* bis zum Wechsel auf Software */
        ok = st != MS_ERROR && g_vdec_errors <= allowed_errors && backwards == 0 && g_aout_chunks > 50 &&
             frames_changed > played * 10 && pos > 1000;
        if (seek && seek_done) {
            double expect_pos = pos_before / 1000.0 + seek + (secs / 2);   /* grob */
            (void)expect_pos;
            if (pos / 1000.0 < pos_before / 1000.0 + seek - 1) ok = 0;
        }
    }
    media_close();
    printf("  => %s\n", ok ? "OK" : "FEHLGESCHLAGEN");
    return ok ? 0 : 1;
}
