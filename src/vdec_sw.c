/* Software-H.264 über FFmpeg als Ersatz für den Hardware-Decoder.
 * Gedacht für SD-Streams (z. B. Interlaced-TV), die sceAvcdec ablehnt.
 * Ausgabe: YUV420 planar direkt im Texturpuffer (die GPU wandelt in RGB). */
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>

static AVCodecContext *s_ctx;
static AVFrame        *s_frame;
static AVPacket       *s_pkt;
static char            s_err[96];

const char *vsw_last_error(void) { return s_err; }

int vsw_open(void)
{
    vsw_close();
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) { snprintf(s_err, sizeof s_err, "kein Software-Decoder vorhanden"); return -1; }
    s_ctx = avcodec_alloc_context3(c);
    s_ctx->thread_count = 2;                       /* Vita: 3 Kerne, einer für UI/Ton */
    s_ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
    s_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    s_ctx->skip_loop_filter = AVDISCARD_NONREF;     /* spart viel Rechenzeit */
    if (avcodec_open2(s_ctx, c, NULL) < 0) {
        snprintf(s_err, sizeof s_err, "Software-Decoder startet nicht");
        avcodec_free_context(&s_ctx);
        return -1;
    }
    s_frame = av_frame_alloc();
    s_pkt = av_packet_alloc();
    return 0;
}

static void copy_plane(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride,
                       int w, int h, int interlaced)
{
    for (int y = 0; y < h; y++) {
        /* Halbbilder: nur das obere Feld verwenden und Zeilen verdoppeln (kein Kamm-Effekt) */
        int sy = interlaced ? (y & ~1) : y;
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)sy * src_stride, w);
    }
}

int vsw_decode(const uint8_t *au, int len, int64_t pts90k, const VdecTarget *dst, VdecResult *res)
{
    if (!s_ctx) return -1;
    if (av_new_packet(s_pkt, len) < 0) return -1;
    memcpy(s_pkt->data, au, len);
    s_pkt->pts = pts90k >= 0 ? pts90k : AV_NOPTS_VALUE;
    int r = avcodec_send_packet(s_ctx, s_pkt);
    av_packet_unref(s_pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        snprintf(s_err, sizeof s_err, "Software-Dekodierfehler %d", r);
        return -1;
    }
    if (avcodec_receive_frame(s_ctx, s_frame) != 0) return 0;

    AVFrame *f = s_frame;
    int ok_fmt = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
    if (!ok_fmt || f->width > dst->width || f->height > dst->height) {
        snprintf(s_err, sizeof s_err, "Format %s %dx%d nicht darstellbar",
                 av_get_pix_fmt_name(f->format) ? av_get_pix_fmt_name(f->format) : "?", f->width, f->height);
        av_frame_unref(f);
        return -1;
    }
    int interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    uint8_t *y = dst->pixels;
    uint8_t *u = y + (size_t)dst->width * dst->height;
    uint8_t *v = u + (size_t)(dst->width / 2) * (dst->height / 2);
    copy_plane(y, dst->width, f->data[0], f->linesize[0], f->width, f->height, interlaced);
    copy_plane(u, dst->width / 2, f->data[1], f->linesize[1], (f->width + 1) / 2, (f->height + 1) / 2, interlaced);
    copy_plane(v, dst->width / 2, f->data[2], f->linesize[2], (f->width + 1) / 2, (f->height + 1) / 2, interlaced);

    res->pts90k = f->pts != AV_NOPTS_VALUE ? f->pts : -1;
    res->width = f->width;
    res->height = f->height;
    av_frame_unref(f);
    return 1;
}

void vsw_reset(void) { if (s_ctx) avcodec_flush_buffers(s_ctx); }

void vsw_close(void)
{
    avcodec_free_context(&s_ctx);
    av_frame_free(&s_frame);
    av_packet_free(&s_pkt);
}
