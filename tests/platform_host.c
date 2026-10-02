/* Host-Ersatz für die Vita-Plattform: Software-H.264 statt sceAvcdec,
 * zeitgesteuerte "Audioausgabe", Bildpuffer im RAM. VS_SPEED beschleunigt die Zeit. */
#include "../src/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>

int g_vdec_frames, g_vdec_errors, g_vdec_opened, g_aout_chunks, g_aout_rate;
static double speed(void) { const char *s = getenv("VS_SPEED"); return s ? atof(s) : 1.0; }

int64_t plat_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)((ts.tv_sec * 1000000LL + ts.tv_nsec / 1000) * speed());
}

static AVCodecContext *s_ctx;
static AVFrame *s_frame;
static AVPacket *s_pkt;
static char s_err[64];
const char *vdec_last_error(void) { return s_err; }

int vdec_open(int w, int h)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    s_ctx = avcodec_alloc_context3(c);
    s_ctx->thread_count = 1;
    if (avcodec_open2(s_ctx, c, NULL) < 0) return -1;
    s_frame = av_frame_alloc();
    s_pkt = av_packet_alloc();
    g_vdec_opened = w * 10000 + h;
    return 0;
}

int vdec_decode(const uint8_t *au, int len, int64_t pts90k, const VdecTarget *dst, VdecResult *res)
{
    if (getenv("VS_HW_FAIL")) { snprintf(s_err, sizeof s_err, "Decode 0x80620010 (simuliert)"); g_vdec_errors++; return -1; }
    /* wie die Hardware: reine Annex-B-Daten, keine Extradata */
    if (len < 4 || !(au[0] == 0 && au[1] == 0 && (au[2] == 1 || (au[2] == 0 && au[3] == 1)))) {
        snprintf(s_err, sizeof s_err, "kein Annex-B");
        g_vdec_errors++;
        return -1;
    }
    av_new_packet(s_pkt, len);
    memcpy(s_pkt->data, au, len);
    s_pkt->pts = pts90k >= 0 ? pts90k : AV_NOPTS_VALUE;
    int r = avcodec_send_packet(s_ctx, s_pkt);
    av_packet_unref(s_pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) { g_vdec_errors++; snprintf(s_err, sizeof s_err, "decode %d", r); return -1; }
    if (avcodec_receive_frame(s_ctx, s_frame) != 0) return 0;
    if (s_frame->width > dst->width || s_frame->height > dst->height) { g_vdec_errors++; return -1; }
    res->pts90k = s_frame->pts;
    res->width = s_frame->width;
    res->height = s_frame->height;
    ((uint8_t *)dst->pixels)[0] = s_frame->data[0][0];   /* "Bild schreiben" */
    av_frame_unref(s_frame);
    g_vdec_frames++;
    return 1;
}

void vdec_reset(void) { if (s_ctx) avcodec_flush_buffers(s_ctx); }
void vdec_close(void) { avcodec_free_context(&s_ctx); av_frame_free(&s_frame); av_packet_free(&s_pkt); }

static void *s_fb[8];
void *fb_create(int slot, int w, int h, int *pitch) { *pitch = w; return s_fb[slot] = calloc((size_t)w * h, 4); }
void fb_destroy(int slot) { free(s_fb[slot]); s_fb[slot] = NULL; }

int aout_open(int rate, int ch) { (void)ch; g_aout_rate = rate; return 0; }
int aout_write(const int16_t *pcm)
{
    (void)pcm;
    g_aout_chunks++;
    usleep((useconds_t)(AOUT_GRAIN * 1000000.0 / g_aout_rate / speed()));
    return 0;
}
void aout_close(void) {}

