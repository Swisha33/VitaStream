/*
 * Media-Kern von VitaStream
 *
 *  Demux-Thread:  FFmpeg liest MP4 / MPEG-TS / fMP4 über eigene AVIO-Callbacks
 *                 (NetStream für Dateien per HTTP, Hls für .m3u8) und verteilt Pakete.
 *  Video-Thread:  H.264 (Annex-B) -> vdec_decode (Vita-Hardware) -> RGBA-Bildpuffer
 *  Audio-Thread:  AAC/MP3/AC3 -> FFmpeg-Decoder -> 16 Bit Stereo -> aout_write
 *                 Die Audioausgabe ist die Master-Uhr; ohne Audio läuft eine Systemuhr.
 *  Hauptthread:   media_current_frame wählt anhand der Uhr das anzuzeigende Bild.
 */
#include "media.h"
#include <strings.h>
#include "platform.h"
#include "net.h"
#include "hls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#define MAX_QUEUE_BYTES (20 * 1024 * 1024)
#define ENOUGH_PACKETS  150
#define IO_BUF_SIZE     (64 * 1024)
#define MAX_VIDEO_W     1920
#define MAX_VIDEO_H     1088

/* ================================================================ Decoder-Auswahl */

typedef struct {
    int  (*decode)(const uint8_t *, int, int64_t, const VdecTarget *, VdecResult *);
    void (*reset)(void);
    void (*close)(void);
    const char *(*error)(void);
    int  yuv;            /* Ausgabeformat: 0 = RGBA8888, 1 = YUV420 planar */
    const char *name;
} VDec;

static const VDec VD_HW = { vdec_decode, vdec_reset, vdec_close, vdec_last_error, 0, "Hardware" };
static const VDec VD_SW = { vsw_decode,  vsw_reset,  vsw_close,  vsw_last_error,  1, "Software" };

#define HW_MAX_W 1280
#define HW_MAX_H 720
#define SW_MAX_W 1024
#define SW_MAX_H 576

/* ================================================================ Paket-Warteschlange */

typedef struct PNode {
    AVPacket     *pkt;      /* NULL bei EOF-Markierung */
    int           serial;
    struct PNode *next;
} PNode;

typedef struct {
    PNode          *head, *tail;
    int             count;
    size_t          bytes;
    int             abort;
    pthread_mutex_t m;
    pthread_cond_t  cv;
} PQueue;

static void pq_init(PQueue *q)
{
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->cv, NULL);
}

static void pq_put(PQueue *q, AVPacket *pkt, int serial)
{
    PNode *n = calloc(1, sizeof *n);
    n->pkt = pkt;
    n->serial = serial;
    pthread_mutex_lock(&q->m);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    q->count++;
    q->bytes += pkt ? pkt->size : 0;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->m);
}

/* 1 = Paket, -1 = abgebrochen. Wartet höchstens timeout_ms (0 = nicht warten -> 0 bei leer). */
static int pq_get(PQueue *q, PNode *out, int timeout_ms)
{
    pthread_mutex_lock(&q->m);
    int waited = 0;
    while (!q->head && !q->abort) {
        if (waited >= timeout_ms) { pthread_mutex_unlock(&q->m); return 0; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&q->cv, &q->m, &ts);
        waited += 20;
    }
    if (q->abort) { pthread_mutex_unlock(&q->m); return -1; }
    PNode *n = q->head;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    q->bytes -= n->pkt ? n->pkt->size : 0;
    pthread_mutex_unlock(&q->m);
    *out = *n;
    free(n);
    return 1;
}

static void pq_flush(PQueue *q)
{
    pthread_mutex_lock(&q->m);
    PNode *n = q->head;
    while (n) {
        PNode *next = n->next;
        av_packet_free(&n->pkt);
        free(n);
        n = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    q->bytes = 0;
    pthread_mutex_unlock(&q->m);
}

static void pq_abort(PQueue *q)
{
    pthread_mutex_lock(&q->m);
    q->abort = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->m);
}

static int pq_count(PQueue *q)
{
    pthread_mutex_lock(&q->m);
    int c = q->count;
    pthread_mutex_unlock(&q->m);
    return c;
}

/* ================================================================ Zustand */

typedef enum { FS_FREE, FS_DECODING, FS_READY, FS_SHOWN, FS_RETIRED } FState;

typedef struct {
    void   *px;
    int     pitch, bw, bh;
    FState  st;
    int64_t pts;     /* µs */
    int     w, h;
    int     yuv;
} FSlot;

static struct {
    /* Eingabe */
    char           *url, *headers;
    volatile int    abort;
    NetStream      *ns;
    int64_t         ns_pos;
    NetLive        *nl;               /* endloser HTTP-Stream (Internetradio) */
    Hls            *hls;
    AVFormatContext *fmt;
    AVIOContext    *avio;
    /* separate Tonspur (HLS EXT-X-MEDIA) */
    Hls            *ahls;
    AVFormatContext *afmt;
    AVIOContext    *aavio;
    int             split;
    int             v_eof, a_eof;
    int64_t         last_vts, last_ats;   /* µs, zuletzt gelesen */

    /* Streams */
    int             vi, ai;
    AVRational      vtb, atb;
    AVBSFContext   *bsf;
    AVCodecContext *actx;
    SwrContext     *swr;
    int             swr_rate, swr_fmt;
    AVChannelLayout swr_layout;
    int             out_rate;
    int             aout_ok, audio_failed;
    int             vdec_ok;
    const VDec     *vd;
    int             vw, vh;           /* Videogröße laut Stream */
    int             sw_skip_to_key;

    /* Threads & Queues */
    pthread_t       demux_t, video_t, audio_t;
    int             video_started, audio_started, demux_started;
    PQueue          vq, aq;
    volatile int    serial;
    volatile int    eof;
    volatile int    seek_req;
    volatile int64_t seek_target;   /* µs relativ zum Anfang */

    /* Bilder */
    FSlot           slot[MEDIA_SLOTS];
    pthread_mutex_t fm;
    pthread_cond_t  fcv;
    int             shown;

    /* Uhr */
    pthread_mutex_t cm;
    int             clock_valid, clock_audio, paused;
    int64_t         clock_pts, clock_wall;

    /* Infos */
    volatile MediaState state;
    char            err[256];
    int64_t         origin;           /* µs, Zeitstempel des Anfangs */
    int64_t         duration;         /* µs */
    int64_t         last_pos;         /* µs */
    int64_t         pos_base, pos_anchor, pos_prev;   /* Anzeigeposition, robust gegen Zeitsprünge */
    int             live;
    char            info[160];
    int             frames_shown, frames_dropped, vdec_errors, vpackets;
    int64_t         wait_since;       /* seit wann Bilder auf den Audiostart warten */
    volatile int64_t v_offset;        /* Korrektur der Bild-Zeitachse gegenüber dem Ton (µs) */
    int             resyncs;
} M;

static void set_error(const char *fmt, const char *a)
{
    if (M.state == MS_ERROR) return;
    snprintf(M.err, sizeof M.err, fmt, a ? a : "");
    M.state = MS_ERROR;
}

/* ================================================================ Uhr */

static int64_t clock_get_locked(void)
{
    if (!M.clock_valid) return INT64_MIN;
    if (M.paused) return M.clock_pts;
    int64_t dt = plat_now_us() - M.clock_wall;
    if (M.clock_audio && dt > 300000) dt = 300000;   /* Audio stockt -> Uhr bleibt stehen */
    return M.clock_pts + dt;
}

static int64_t clock_get(void)
{
    pthread_mutex_lock(&M.cm);
    int64_t c = clock_get_locked();
    pthread_mutex_unlock(&M.cm);
    return c;
}

static void clock_set_serial(int64_t pts, int audio, int serial)
{
    pthread_mutex_lock(&M.cm);
    if (serial != M.serial) { pthread_mutex_unlock(&M.cm); return; }   /* veralteter Wert */
    M.clock_pts = pts;
    M.clock_wall = plat_now_us();
    M.clock_valid = 1;
    M.clock_audio = audio;
    pthread_mutex_unlock(&M.cm);
}

static void clock_set(int64_t pts, int audio)
{
    pthread_mutex_lock(&M.cm);
    M.clock_pts = pts;
    M.clock_wall = plat_now_us();
    M.clock_valid = 1;
    M.clock_audio = audio;
    pthread_mutex_unlock(&M.cm);
}

static void clock_invalidate(void)
{
    pthread_mutex_lock(&M.cm);
    M.clock_valid = 0;
    pthread_mutex_unlock(&M.cm);
}

/* Anzeigeposition aus Zeitstempeln: nach einem Sprung (Seek, Werbeblock, neu beginnende
   Zeitstempel nach dem Neuöffnen eines HLS-Streams) läuft sie an der bisherigen Stelle weiter.
   Aufruf mit M.fm gesperrt. */
static void pos_update(int64_t pts)
{
    if (M.pos_anchor == INT64_MIN) {
        M.pos_anchor = pts;
    } else if (pts - M.pos_prev > 5000000 || M.pos_prev - pts > 5000000) {
        M.pos_base += M.pos_prev - M.pos_anchor;
        M.pos_anchor = pts;
    }
    M.pos_prev = pts;
    M.last_pos = M.pos_base + (pts - M.pos_anchor);
}

static void pos_reset(int64_t base, int64_t anchor)
{
    pthread_mutex_lock(&M.fm);
    M.pos_base = base;
    M.pos_anchor = anchor;
    M.pos_prev = anchor;
    M.last_pos = base;
    pthread_mutex_unlock(&M.fm);
}

/* Alle dekodierten Bilder verwerfen (nach einem Sprung): sonst verankern die alten
   Zeitstempel die Positionsanzeige und sie springt auf 0, sobald die neuen Bilder kommen. */
static void frames_flush(void)
{
    pthread_mutex_lock(&M.fm);
    for (int i = 0; i < MEDIA_SLOTS; i++)
        if (M.slot[i].st != FS_DECODING) M.slot[i].st = FS_FREE;
    M.shown = -1;
    M.wait_since = 0;
    pthread_cond_broadcast(&M.fcv);
    pthread_mutex_unlock(&M.fm);
}

static int audio_is_master(void) { return M.ai >= 0 && !M.audio_failed; }

/* ================================================================ AVIO */

static int io_read(void *opaque, uint8_t *buf, int size)
{
    if (M.abort) return AVERROR_EXIT;
    Hls *hl = opaque ? (Hls *)opaque : M.hls;   /* opaque = Tonspur-Playlist */
    if (hl) {
        int n = hls_read(hl, buf, size);
        if (n == 0) return AVERROR_EOF;
        if (n < 0) return M.abort ? AVERROR_EXIT : AVERROR(EIO);
        return n;
    }
    if (M.nl) {
        int n = net_live_read(M.nl, buf, size);
        if (n == 0) return AVERROR_EOF;
        if (n < 0) return M.abort ? AVERROR_EXIT : AVERROR(EIO);
        return n;
    }
    uint64_t total = net_stream_size(M.ns);
    if ((uint64_t)M.ns_pos >= total) return AVERROR_EOF;
    int n = net_stream_read(M.ns, M.ns_pos, buf, size);
    if (n <= 0) return M.abort ? AVERROR_EXIT : AVERROR(EIO);
    M.ns_pos += n;
    return n;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
    (void)opaque;
    int64_t size = (int64_t)net_stream_size(M.ns);
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return size;
    case SEEK_SET:    M.ns_pos = off; break;
    case SEEK_CUR:    M.ns_pos += off; break;
    case SEEK_END:    M.ns_pos = size + off; break;
    default:          return -1;
    }
    if (M.ns_pos < 0) M.ns_pos = 0;
    return M.ns_pos;
}

static int io_interrupt(void *opaque) { (void)opaque; return M.abort; }

/* Dateiendungen, bei denen es sicher kein Endlos-Stream ist (spart eine Anfrage) */
static int has_file_ext(const char *url)
{
    static const char *ext[] = { ".mp4", ".m4v", ".mov", ".mkv", ".m4a", ".webm" };
    const char *q = url + strcspn(url, "?#");
    for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++) {
        size_t l = strlen(ext[i]);
        if (q - url >= (long)l && !strncasecmp(q - l, ext[i], l)) return 1;
    }
    return 0;
}

static int is_hls_url(const char *url)
{
    const char *q = url + strcspn(url, "?#");
    for (const char *p = url; p + 5 <= q; p++)
        if (!strncasecmp(p, ".m3u8", 5)) return 1;
    return 0;
}

/* Öffnet FFmpeg auf der aktuellen Quelle (NetStream, Hls oder lokale Datei). */
static int open_format(char *err, int errlen)
{
    int local = !M.ns && !M.hls && !M.nl;
    M.fmt = avformat_alloc_context();
    M.fmt->interrupt_callback.callback = io_interrupt;
    if (!local) {
        uint8_t *iobuf = av_malloc(IO_BUF_SIZE);
        M.avio = avio_alloc_context(iobuf, IO_BUF_SIZE, 0, NULL, io_read, NULL, M.ns ? io_seek : NULL);
        M.fmt->pb = M.avio;
        M.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (M.hls || M.nl) M.avio->seekable = 0;
    }
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "2000000", 0);
    av_dict_set(&opts, "analyzeduration", "3000000", 0);
    int r = avformat_open_input(&M.fmt, local ? M.url : NULL, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        char e[128];
        av_strerror(r, e, sizeof e);
        snprintf(err, errlen, "Stream nicht lesbar: %s", (M.hls && hls_error(M.hls)[0]) ? hls_error(M.hls) : e);
        return -1;
    }
    M.fmt->flags |= AVFMT_FLAG_GENPTS;
    if (avformat_find_stream_info(M.fmt, NULL) < 0) {
        snprintf(err, errlen, "Streaminformationen nicht lesbar");
        return -1;
    }
    return 0;
}

/* Separate Tonspur öffnen (eigener Demuxer auf eigener HLS-Playlist) */
static int open_audio_format(char *err, int errlen)
{
    M.afmt = avformat_alloc_context();
    M.afmt->interrupt_callback.callback = io_interrupt;
    uint8_t *iobuf = av_malloc(IO_BUF_SIZE);
    M.aavio = avio_alloc_context(iobuf, IO_BUF_SIZE, 0, M.ahls, io_read, NULL, NULL);
    M.aavio->seekable = 0;
    M.afmt->pb = M.aavio;
    M.afmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "500000", 0);
    av_dict_set(&opts, "analyzeduration", "2000000", 0);
    int r = avformat_open_input(&M.afmt, NULL, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0 || avformat_find_stream_info(M.afmt, NULL) < 0) {
        snprintf(err, errlen, "Tonspur nicht lesbar: %s", hls_error(M.ahls));
        return -1;
    }
    return 0;
}

static void close_audio_format(void)
{
    if (M.afmt) avformat_close_input(&M.afmt);
    if (M.aavio) {
        av_freep(&M.aavio->buffer);
        avio_context_free(&M.aavio);
    }
}

static int open_audio_decoder(AVStream *st)
{
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return -1;
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (avcodec_open2(ctx, c, NULL) < 0) { avcodec_free_context(&ctx); return -1; }
    avcodec_free_context(&M.actx);
    M.actx = ctx;
    M.atb = st->time_base;
    return 0;
}

static void close_format(void)
{
    if (M.fmt) avformat_close_input(&M.fmt);
    if (M.avio) {
        av_freep(&M.avio->buffer);
        avio_context_free(&M.avio);
    }
}

/* ================================================================ Bildpuffer */

static int slots_alloc(int w, int h)
{
    for (int i = 0; i < MEDIA_SLOTS; i++) {
        FSlot *s = &M.slot[i];
        s->px = fb_create(i, w, h, &s->pitch);
        if (!s->px) return -1;
        s->bw = w;
        s->bh = h;
        s->st = FS_FREE;
    }
    M.shown = -1;
    return 0;
}

int media_current_frame(int *w, int *h, int *yuv)
{
    if (!M.vdec_ok) return -1;
    pthread_mutex_lock(&M.fm);
    for (int i = 0; i < MEDIA_SLOTS; i++)
        if (M.slot[i].st == FS_RETIRED) M.slot[i].st = FS_FREE;

    int64_t clk = clock_get();
    int best = -1;
    int64_t min_pts = INT64_MAX;
    int min_i = -1;
    for (int i = 0; i < MEDIA_SLOTS; i++)
        if (M.slot[i].st == FS_READY && M.slot[i].pts < min_pts) { min_pts = M.slot[i].pts; min_i = i; }

    if (clk != INT64_MIN) M.wait_since = 0;
    if (clk == INT64_MIN) {
        if (min_i >= 0) {
            if (!audio_is_master()) {
                clock_set(M.slot[min_i].pts, 0);      /* Systemuhr startet mit dem ersten Bild */
                best = min_i;
            } else {
                int64_t now = plat_now_us();
                if (!M.wait_since) M.wait_since = now;
                if (now - M.wait_since > 1500000) {
                    clock_set(M.slot[min_i].pts, 0);  /* Ton startet nicht -> Bild trotzdem laufen lassen */
                    best = min_i;
                } else if (M.shown < 0) {
                    best = min_i;                     /* Vorschaubild, bis Audio läuft */
                }
            }
        }
    } else {
        for (int i = 0; i < MEDIA_SLOTS; i++) {
            FSlot *s = &M.slot[i];
            if (s->st == FS_READY && s->pts <= clk + 15000 && (best < 0 || s->pts > M.slot[best].pts)) best = i;
        }
        /* Zeitsprung (Werbeblock, Live-Diskontinuität, getrennte Spuren nach Sprung):
           alle Bilder liegen weit vor oder weit hinter der Uhr */
        if (best < 0 || (min_i >= 0 && audio_is_master())) {
            int64_t newest = INT64_MIN;
            for (int i = 0; i < MEDIA_SLOTS; i++)
                if (M.slot[i].st == FS_READY && M.slot[i].pts > newest) newest = M.slot[i].pts;
            int far = min_i >= 0 && (min_pts - clk > 2500000 || newest < clk - 2500000);
            if (far && audio_is_master()) {
                /* Ton führt: Bild-Zeitachse an den Ton anlehnen statt Bilder zu verwerfen */
                int64_t shift = clk - min_pts;
                M.v_offset += shift;
                for (int i = 0; i < MEDIA_SLOTS; i++)
                    if (M.slot[i].st == FS_READY) M.slot[i].pts += shift;
                best = min_i;
                M.resyncs++;
            } else if (far && best < 0) {
                clock_set(min_pts, 0);   /* Systemuhr springt mit */
                best = min_i;
            }
        }
        if (best >= 0) {
            for (int i = 0; i < MEDIA_SLOTS; i++)
                if (i != best && M.slot[i].st == FS_READY && M.slot[i].pts < M.slot[best].pts) {
                    M.slot[i].st = FS_FREE;
                    M.frames_dropped++;
                }
        }
    }
    if (best >= 0) {
        if (M.shown >= 0 && M.shown != best) M.slot[M.shown].st = FS_RETIRED;
        M.slot[best].st = FS_SHOWN;
        M.shown = best;
        pos_update(M.slot[best].pts);
        M.frames_shown++;
        pthread_cond_broadcast(&M.fcv);
    }
    int cur = M.shown;
    if (cur >= 0) {
        if (w) *w = M.slot[cur].w;
        if (h) *h = M.slot[cur].h;
        if (yuv) *yuv = M.slot[cur].yuv;
    }
    pthread_mutex_unlock(&M.fm);
    return cur;
}

/* ================================================================ Video-Thread */

static void *video_thread(void *arg)
{
    (void)arg;
    int serial = M.serial;
    while (!M.abort) {
        PNode n;
        int r = pq_get(&M.vq, &n, 100);
        if (r < 0) break;
        if (r == 0) continue;
        if (n.serial != serial) {
            serial = n.serial;
            M.vd->reset();
            pthread_mutex_lock(&M.fm);
            for (int i = 0; i < MEDIA_SLOTS; i++)
                if (M.slot[i].st == FS_READY) M.slot[i].st = FS_FREE;
            pthread_mutex_unlock(&M.fm);
        }
        if (!n.pkt) continue;       /* EOF-Markierung */
        if (n.serial != M.serial) { av_packet_free(&n.pkt); continue; }   /* veraltet */
        if (M.sw_skip_to_key && !(n.pkt->flags & AV_PKT_FLAG_KEY)) { av_packet_free(&n.pkt); continue; }
        M.sw_skip_to_key = 0;

        /* freien Puffer suchen */
        int slot = -1;
        pthread_mutex_lock(&M.fm);
        while (!M.abort && n.serial == M.serial) {
            for (int i = 0; i < MEDIA_SLOTS; i++)
                if (M.slot[i].st == FS_FREE) { slot = i; break; }
            if (slot >= 0) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&M.fcv, &M.fm, &ts);
        }
        if (slot >= 0) M.slot[slot].st = FS_DECODING;
        pthread_mutex_unlock(&M.fm);
        if (slot < 0) { av_packet_free(&n.pkt); continue; }

        int64_t ts = n.pkt->pts != AV_NOPTS_VALUE ? n.pkt->pts : n.pkt->dts;
        int64_t pts90k = ts != AV_NOPTS_VALUE ? av_rescale_q(ts, M.vtb, (AVRational){1, 90000}) : -1;
        VdecTarget dst = { M.slot[slot].px, M.slot[slot].pitch, M.slot[slot].bw, M.slot[slot].bh };
        VdecResult res = { -1, 0, 0 };
        int dr = M.vd->decode(n.pkt->data, n.pkt->size, pts90k, &dst, &res);
        M.vpackets++;

        pthread_mutex_lock(&M.fm);
        FSlot *s = &M.slot[slot];
        if (dr == 1 && n.serial == M.serial) {
            s->pts = (res.pts90k >= 0 ? res.pts90k * 100 / 9
                                      : av_rescale_q(ts, M.vtb, (AVRational){1, 1000000})) + M.v_offset;
            s->w = res.width > 0 ? res.width : s->bw;
            s->h = res.height > 0 ? res.height : s->bh;
            s->yuv = M.vd->yuv;
            s->st = FS_READY;
        } else {
            s->st = FS_FREE;
        }
        pthread_mutex_unlock(&M.fm);
        av_packet_free(&n.pkt);

        if (dr < 0) {
            M.vdec_errors++;
            /* Hardware lehnt den Stream ab (z. B. Halbbilder/Interlaced bei SD-TV): Software übernimmt */
            if (M.vd == &VD_HW && M.frames_shown == 0 && M.vdec_errors >= 6) {
                if (M.vw <= SW_MAX_W && M.vh <= SW_MAX_H && vsw_open() == 0) {
                    vdec_close();
                    M.vd = &VD_SW;
                    M.vdec_errors = 0;
                    M.sw_skip_to_key = 1;
                } else {
                    set_error("Hardware-Decoder lehnt den Stream ab (%s). Software-Dekodierung geht nur bis 1024x576.",
                              vdec_last_error());
                }
            } else if (M.frames_shown == 0 && M.vdec_errors >= 60) {
                set_error("Video nicht dekodierbar (%s)", M.vd->error());
            }
        }
    }
    return NULL;
}

/* ================================================================ Audio-Thread */

static int setup_swr(const AVFrame *f)
{
    if (M.swr && M.swr_rate == f->sample_rate && M.swr_fmt == f->format &&
        !av_channel_layout_compare(&M.swr_layout, &f->ch_layout))
        return 0;
    swr_free(&M.swr);
    av_channel_layout_uninit(&M.swr_layout);
    av_channel_layout_copy(&M.swr_layout, &f->ch_layout);
    M.swr_rate = f->sample_rate;
    M.swr_fmt = f->format;
    int out_rate = 48000;   /* MAIN-Port der Vita akzeptiert nur 48 kHz */
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&M.swr, &stereo, AV_SAMPLE_FMT_S16, out_rate,
                            &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(M.swr) < 0) {
        swr_free(&M.swr);
        return -1;
    }
    if (out_rate != M.out_rate || !M.aout_ok) {
        if (M.aout_ok) aout_close();
        M.aout_ok = aout_open(out_rate, 2) == 0;
        M.out_rate = out_rate;
        if (!M.aout_ok) {
            M.audio_failed = 1;           /* weiter ohne Ton, Systemuhr */
            clock_invalidate();
        }
    }
    return 0;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    AVFrame *f = av_frame_alloc();
    const int cap = 16384;
    int16_t *ring = malloc(cap * 2 * sizeof(int16_t));
    /* Die Hardware liest den übergebenen Puffer noch, während aout_write schon zurückkehrt:
       deshalb reihum eigene Ausgabepuffer statt des Ringpuffers selbst */
    static int16_t outbuf[3][AOUT_GRAIN * 2];
    int outidx = 0;
    int fill = 0;
    int64_t ring_pts = AV_NOPTS_VALUE;
    int serial = M.serial;

    while (!M.abort) {
        PNode n;
        int r = pq_get(&M.aq, &n, 100);
        if (r < 0) break;
        if (r == 0) continue;
        if (n.serial != serial) {
            serial = n.serial;
            avcodec_flush_buffers(M.actx);
            fill = 0;
            ring_pts = AV_NOPTS_VALUE;
        }
        if (!n.pkt || n.serial != M.serial) { av_packet_free(&n.pkt); continue; }

        if (avcodec_send_packet(M.actx, n.pkt) < 0) { av_packet_free(&n.pkt); continue; }
        av_packet_free(&n.pkt);

        while (!M.abort && avcodec_receive_frame(M.actx, f) == 0) {
            if (M.audio_failed || setup_swr(f) < 0 || !M.aout_ok) { av_frame_unref(f); continue; }

            int64_t fpts = f->best_effort_timestamp;
            if (fill == 0 && fpts != AV_NOPTS_VALUE)
                ring_pts = av_rescale_q(fpts, M.atb, (AVRational){1, 1000000});

            int max_out = swr_get_out_samples(M.swr, f->nb_samples);
            if (fill + max_out > cap) max_out = cap - fill;
            uint8_t *outp = (uint8_t *)(ring + fill * 2);
            int got = swr_convert(M.swr, &outp, max_out, (const uint8_t **)f->extended_data, f->nb_samples);
            av_frame_unref(f);
            if (got > 0) fill += got;

            while (fill >= AOUT_GRAIN && !M.abort && serial == M.serial) {
                while (M.paused && !M.abort && serial == M.serial) usleep(10000);
                if (serial != M.serial || M.abort) break;
                memcpy(outbuf[outidx], ring, sizeof outbuf[0]);
                aout_write(outbuf[outidx]);
                outidx = (outidx + 1) % 3;
                /* Während aout_write kann gesprungen worden sein: dann gehört dieser Puffer
                   zur alten Position und darf die Uhr nicht mehr setzen */
                if (serial != M.serial) break;
                if (ring_pts != AV_NOPTS_VALUE) {
                    /* aout_write kehrt zurück, wenn der Puffer übernommen wurde; hörbar ist etwa der vorige */
                    int64_t grain_us = (int64_t)AOUT_GRAIN * 1000000 / M.out_rate;
                    clock_set_serial(ring_pts - grain_us, 1, serial);
                    ring_pts += grain_us;
                }
                fill -= AOUT_GRAIN;
                memmove(ring, ring + AOUT_GRAIN * 2, fill * 2 * sizeof(int16_t));
            }
        }
    }
    if (M.aout_ok) aout_close();
    M.aout_ok = 0;
    free(ring);
    av_frame_free(&f);
    return NULL;
}

/* ================================================================ Spurwahl */

static char g_apref[48];          /* bevorzugte Tonspur: Sprachkürzel, Spurname oder "#<index>" */
static MediaTrack g_atracks[MEDIA_MAX_TRACKS], g_stracks[MEDIA_MAX_TRACKS];
static int g_natracks, g_cur_atrack = -1, g_nstracks;

void media_set_audio_pref(const char *pref)
{
    snprintf(g_apref, sizeof g_apref, "%s", pref ? pref : "");
    hls_set_audio_pref(g_apref[0] == '#' ? "" : g_apref);
}

/* ISO-639-2 -> zweistelliges Kürzel (für den Vergleich mit der Vorliebe) */
static const char *lang2(const char *l)
{
    static const char *map[][2] = { {"ger","de"},{"deu","de"},{"eng","en"},{"fre","fr"},{"fra","fr"},
        {"spa","es"},{"ita","it"},{"hrv","hr"},{"rus","ru"},{"jpn","ja"},{"por","pt"},{"pol","pl"},
        {"tur","tr"},{"nld","nl"},{"dut","nl"} };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) if (!strcasecmp(l, map[i][0])) return map[i][1];
    return l;
}

static const char *stream_lang(AVStream *st)
{
    AVDictionaryEntry *e = av_dict_get(st->metadata, "language", NULL, 0);
    return e && e->value ? e->value : "";
}

/* Tonspur im Container wählen: Vorliebe ("#<index>" oder Sprache) > FFmpeg-Wahl */
static int pick_audio(AVFormatContext *fmt, int related)
{
    if (g_apref[0] == '#') {
        int idx = atoi(g_apref + 1);
        if (idx >= 0 && idx < (int)fmt->nb_streams && fmt->streams[idx]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            return idx;
    } else if (g_apref[0]) {
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            if (fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            const char *l = lang2(stream_lang(fmt->streams[i]));
            if (l[0] && !strncasecmp(l, g_apref, 2)) return (int)i;
        }
    }
    return av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, related, NULL, 0);
}

static const char *lang_label(const char *l)
{
    static const char *names[][2] = { {"de","Deutsch"},{"en","Englisch"},{"fr","Franzoesisch"},{"es","Spanisch"},
        {"it","Italienisch"},{"hr","Kroatisch"},{"ru","Russisch"},{"ja","Japanisch"},{"pt","Portugiesisch"},
        {"pl","Polnisch"},{"tr","Tuerkisch"},{"nl","Niederlaendisch"},{"qaa","Originalton"},{"mis","Audiodeskription"} };
    const char *k = lang2(l);
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strncasecmp(k, names[i][0], 2) && strlen(k) <= 3) return names[i][1];
    return l;
}

/* Spurlisten nach dem Öffnen festhalten (der Demux-Thread öffnet beim Springen neu) */
static void collect_tracks(void)
{
    g_natracks = g_nstracks = 0;
    g_cur_atrack = -1;
    if (M.hls) {
        const HlsTrack *t; int cur;
        int n = hls_audio_tracks(M.hls, &t, &cur);
        for (int i = 0; i < n && g_natracks < MEDIA_MAX_TRACKS; i++) {
            MediaTrack *m = &g_atracks[g_natracks++];
            const char *ll = t[i].lang[0] ? lang_label(t[i].lang) : "";
            if (ll[0] && strcasecmp(ll, t[i].name)) snprintf(m->label, sizeof m->label, "%s (%s)", t[i].name, ll);
            else snprintf(m->label, sizeof m->label, "%s", t[i].name);
            snprintf(m->key, sizeof m->key, "%s", t[i].lang[0] ? t[i].lang : t[i].name);
        }
        g_cur_atrack = cur;
        n = hls_subtitle_tracks(M.hls, &t);
        for (int i = 0; i < n && g_nstracks < MEDIA_MAX_TRACKS; i++) {
            MediaTrack *m = &g_stracks[g_nstracks++];
            const char *ll = t[i].lang[0] ? lang_label(t[i].lang) : "";
            if (ll[0] && strcasecmp(ll, t[i].name)) snprintf(m->label, sizeof m->label, "%s (%s)", t[i].name, ll);
            else snprintf(m->label, sizeof m->label, "%s", t[i].name);
            snprintf(m->key, sizeof m->key, "%s", t[i].uri);
        }
    }
    if (g_natracks == 0 && M.fmt && !M.split) {
        for (unsigned i = 0; i < M.fmt->nb_streams && g_natracks < MEDIA_MAX_TRACKS; i++) {
            AVCodecParameters *cp = M.fmt->streams[i]->codecpar;
            if (cp->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            const char *l = stream_lang(M.fmt->streams[i]);
            MediaTrack *m = &g_atracks[g_natracks];
            snprintf(m->label, sizeof m->label, "%s%s%s", l[0] ? lang_label(l) : "Spur ", l[0] ? "  -  " : "",
                     avcodec_get_name(cp->codec_id));
            if (!l[0]) snprintf(m->label, sizeof m->label, "Spur %d  -  %s", g_natracks + 1, avcodec_get_name(cp->codec_id));
            snprintf(m->key, sizeof m->key, "#%u", i);
            if ((int)i == M.ai) g_cur_atrack = g_natracks;
            g_natracks++;
        }
    }
}

int media_audio_tracks(const MediaTrack **list, int *current)
{
    if (list) *list = g_atracks;
    if (current) *current = g_cur_atrack;
    return g_natracks;
}

int media_subtitle_tracks(const MediaTrack **list)
{
    if (list) *list = g_stracks;
    return g_nstracks;
}

int64_t media_origin_ms(void) { return M.origin / 1000; }

/* ================================================================ Demux-Thread */

static int setup_streams(char *err, int errlen)
{
    M.vi = av_find_best_stream(M.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    M.ai = pick_audio(M.fmt, M.vi >= 0 ? M.vi : -1);

    if (M.vi >= 0) {
        AVCodecParameters *cp = M.fmt->streams[M.vi]->codecpar;
        if (cp->codec_id != AV_CODEC_ID_H264) {
            if (M.ai < 0) {
                snprintf(err, errlen, "Videoformat %s wird nicht unterstuetzt (nur H.264)", avcodec_get_name(cp->codec_id));
                return -1;
            }
            M.vi = -1;    /* nur Ton abspielen */
        } else if (cp->width > MAX_VIDEO_W || cp->height > MAX_VIDEO_H) {
            snprintf(err, errlen, "Aufloesung %dx%d ist zu hoch fuer die Vita", cp->width, cp->height);
            return -1;
        }
    }
    if (M.vi < 0 && M.ai < 0) {
        snprintf(err, errlen, "Keine abspielbare Video- oder Tonspur gefunden");
        return -1;
    }
    for (unsigned i = 0; i < M.fmt->nb_streams; i++)
        if ((int)i != M.vi && (int)i != M.ai) M.fmt->streams[i]->discard = AVDISCARD_ALL;

    if (M.vi >= 0) {
        AVStream *st = M.fmt->streams[M.vi];
        M.vtb = st->time_base;
        if (!M.bsf) {
            const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
            if (av_bsf_alloc(f, &M.bsf) < 0) { snprintf(err, errlen, "BSF fehlt"); return -1; }
            avcodec_parameters_copy(M.bsf->par_in, st->codecpar);
            M.bsf->time_base_in = st->time_base;
            if (av_bsf_init(M.bsf) < 0) { snprintf(err, errlen, "H.264-Konvertierung fehlgeschlagen"); return -1; }
        }
    }
    if (M.ai >= 0 && !M.actx) {
        AVStream *st = M.fmt->streams[M.ai];
        M.atb = st->time_base;
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (!c) {
            if (M.vi < 0) {
                snprintf(err, errlen, "Tonformat %s wird nicht unterstuetzt", avcodec_get_name(st->codecpar->codec_id));
                return -1;
            }
            M.ai = -1;    /* ohne Ton weiter */
        } else {
            M.actx = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(M.actx, st->codecpar);
            M.actx->pkt_timebase = st->time_base;
            if (avcodec_open2(M.actx, c, NULL) < 0) {
                avcodec_free_context(&M.actx);
                M.ai = -1;
            }
        }
    } else if (M.ai >= 0) {
        M.atb = M.fmt->streams[M.ai]->time_base;
    }
    return 0;
}

static int open_source(char *err, int errlen)
{
    char live_url[1024];
    if (!strncmp(M.url, "ux0:", 4) || !strncmp(M.url, "uma0:", 5) || !strncmp(M.url, "file:", 5)) {
        /* lokale Datei: FFmpeg liest direkt */
    } else if (is_hls_url(M.url)) {
        M.hls = hls_open(M.url, M.headers, &M.abort, err, errlen);
        if (!M.hls) return -1;
    } else if (!has_file_ext(M.url) && (live_url[0] = 0, net_detect_live(M.url, M.headers, live_url, sizeof live_url) == 1)) {
        /* Internetradio & Co.: endloser Stream ohne Länge */
        M.nl = net_live_open(live_url[0] ? live_url : M.url, M.headers);
        if (!M.nl) {
            snprintf(err, errlen, "Verbindung fehlgeschlagen: %s", net_last_detail());
            return -1;
        }
    } else {
        M.ns = net_stream_open(M.url, M.headers);
        if (!M.ns) {
            snprintf(err, errlen, "Verbindung fehlgeschlagen: %s", net_last_detail());
            return -1;
        }
        uint8_t head[16] = {0};
        int n = net_stream_read(M.ns, 0, head, sizeof head - 1);
        const char *h = (const char *)head;
        if (n > 0 && (strstr(h, "#EXTM3U") || !strncmp(h + strspn(h, " \r\n\xEF\xBB\xBF"), "#EXT", 4))) {
            net_stream_close(M.ns);   /* Playlist ohne .m3u8-Endung */
            M.ns = NULL;
            M.hls = hls_open(M.url, M.headers, &M.abort, err, errlen);
            if (!M.hls) return -1;
        }
    }
    if (open_format(err, errlen) < 0) return -1;
    if (setup_streams(err, errlen) < 0) return -1;

    /* HLS mit separater Tonspur (z. B. Paramount/South Park): zweiten Demuxer öffnen */
    if (M.hls && hls_audio_url(M.hls)) {
        char aerr[200];
        M.ahls = hls_open(hls_audio_url(M.hls), M.headers, &M.abort, aerr, sizeof aerr);
        if (M.ahls && open_audio_format(aerr, sizeof aerr) == 0) {
            int ai = av_find_best_stream(M.afmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
            if (ai >= 0 && open_audio_decoder(M.afmt->streams[ai]) == 0) {
                M.ai = ai;
                M.split = 1;
                for (unsigned i = 0; i < M.fmt->nb_streams; i++)
                    if ((int)i != M.vi) M.fmt->streams[i]->discard = AVDISCARD_ALL;
                for (unsigned i = 0; i < M.afmt->nb_streams; i++)
                    if ((int)i != ai) M.afmt->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        if (!M.split) {
            close_audio_format();
            hls_close(M.ahls);
            M.ahls = NULL;
        }
    }

    collect_tracks();
    M.live = M.hls ? hls_is_live(M.hls) : (M.nl ? 1 : 0);
    M.duration = M.hls ? hls_duration_us(M.hls) : (M.nl ? 0 : (M.fmt->duration > 0 ? M.fmt->duration : 0));
    M.origin = M.fmt->start_time != AV_NOPTS_VALUE ? M.fmt->start_time : 0;
    pos_reset(0, M.origin);

    char vinfo[64] = "kein Video", ainfo[48] = "kein Ton";
    if (M.vi >= 0) {
        AVCodecParameters *cp = M.fmt->streams[M.vi]->codecpar;
        snprintf(vinfo, sizeof vinfo, "H.264 %dx%d", cp->width, cp->height);
    }
    if (M.ai >= 0) {
        AVCodecParameters *cp = (M.split ? M.afmt : M.fmt)->streams[M.ai]->codecpar;
        snprintf(ainfo, sizeof ainfo, "%s %d Hz%s%s", avcodec_get_name(cp->codec_id), cp->sample_rate,
                 M.split ? " (eigene Spur " : "", M.split ? (hls_audio_lang(M.hls)[0] ? hls_audio_lang(M.hls) : "?") : "");
        if (M.split) strncat(ainfo, ")", sizeof ainfo - strlen(ainfo) - 1);
    }
    snprintf(M.info, sizeof M.info, "%s | %s | %s", M.hls ? "HLS" : (M.ns ? "HTTP" : (M.nl ? "Live-HTTP" : "Datei")),
             vinfo, ainfo);
    if (M.hls && hls_info(M.hls)[0]) {
        size_t l = strlen(M.info);
        snprintf(M.info + l, sizeof M.info - l, " | %s", hls_info(M.hls));
    }

    if (M.vi >= 0) {
        AVCodecParameters *cp = M.fmt->streams[M.vi]->codecpar;
        M.vw = cp->width;
        M.vh = cp->height;
        int w = (cp->width + 15) & ~15, h = (cp->height + 15) & ~15;
        if (cp->width > HW_MAX_W || cp->height > HW_MAX_H) {
            /* manche Konsolen/Firmwares schaffen mehr - probieren, sonst klare Meldung */
            if (vdec_open(w, h) == 0) {
                M.vd = &VD_HW;
            } else {
                snprintf(err, errlen, "Video ist %dx%d - die Vita dekodiert per Hardware hoechstens 1280x720 (720p). "
                         "Bitte eine Quelle/Qualitaet mit max. 720p waehlen.", cp->width, cp->height);
                return -1;
            }
        } else if (vdec_open(HW_MAX_W, HW_MAX_H) == 0) {
            /* fest 720p: Auflösungswechsel im Stream (Werbung, adaptive Qualität) bleiben möglich */
            M.vd = &VD_HW;
            w = HW_MAX_W;
            h = HW_MAX_H;
        } else if (vdec_open(w, h) == 0) {
            M.vd = &VD_HW;
        } else if (cp->width <= SW_MAX_W && cp->height <= SW_MAX_H && vsw_open() == 0) {
            M.vd = &VD_SW;
        } else {
            snprintf(err, errlen, "Hardware-Decoder: %s", vdec_last_error());
            return -1;
        }
        M.vdec_ok = 1;
        if (slots_alloc(w, h) < 0) {
            snprintf(err, errlen, "Zu wenig Grafikspeicher fuer %dx%d", w, h);
            return -1;
        }
    }
    return 0;
}

static void do_seek(void)
{
    M.seek_req = 0;
    if (M.live) return;
    int64_t target = M.seek_target;
    if (target < 0) target = 0;
    if (M.duration && target > M.duration - 2000000) target = M.duration > 2000000 ? M.duration - 2000000 : 0;

    pthread_mutex_lock(&M.cm);
    M.serial++;
    M.clock_valid = 0;
    pthread_mutex_unlock(&M.cm);
    pq_flush(&M.vq);
    pq_flush(&M.aq);
    frames_flush();           /* dekodierte Bilder der alten Position verwerfen */
    M.eof = 0;
    if (M.bsf) av_bsf_flush(M.bsf);

    M.v_eof = M.a_eof = 0;
    M.last_vts = M.last_ats = 0;
    M.v_offset = 0;
    if (M.hls) {
        close_format();
        hls_seek(M.hls, target);
        char err[160];
        if (open_format(err, sizeof err) < 0) { set_error("%s", err); return; }
        /* Stream-Indizes nach dem Neuöffnen erneut bestimmen */
        int vi = av_find_best_stream(M.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (M.vi >= 0) { M.vi = vi; if (vi >= 0) M.vtb = M.fmt->streams[vi]->time_base; }
        if (M.split) {
            for (unsigned i = 0; i < M.fmt->nb_streams; i++)
                if ((int)i != M.vi) M.fmt->streams[i]->discard = AVDISCARD_ALL;
            close_audio_format();
            hls_seek(M.ahls, target);
            if (open_audio_format(err, sizeof err) < 0) { set_error("%s", err); return; }
            int ai = av_find_best_stream(M.afmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
            M.ai = ai;
            if (ai >= 0) M.atb = M.afmt->streams[ai]->time_base;
        } else {
            int ai = pick_audio(M.fmt, vi);
            if (M.ai >= 0) { M.ai = ai; if (ai >= 0) M.atb = M.fmt->streams[ai]->time_base; }
        }
    } else {
        av_seek_frame(M.fmt, -1, M.origin + target, AVSEEK_FLAG_BACKWARD);
    }
    pos_reset(target, INT64_MIN);
}

static void *demux_thread(void *arg)
{
    (void)arg;
    char err[256] = "";
    if (open_source(err, sizeof err) < 0) {
        if (!M.abort) set_error("%s", err[0] ? err : "Stream konnte nicht geoeffnet werden");
        return NULL;
    }
    if (M.vi >= 0) { pthread_create(&M.video_t, NULL, video_thread, NULL); M.video_started = 1; }
    if (M.ai >= 0) { pthread_create(&M.audio_t, NULL, audio_thread, NULL); M.audio_started = 1; }
    M.state = MS_PLAYING;

    AVPacket *pkt = av_packet_alloc();
    int read_errors = 0;
    while (!M.abort) {
        if (M.seek_req) do_seek();
        if (!M.fmt) { usleep(20000); continue; }

        size_t bytes = M.vq.bytes + M.aq.bytes;
        int v_enough = M.vi < 0 || pq_count(&M.vq) > ENOUGH_PACKETS;
        int a_enough = M.ai < 0 || pq_count(&M.aq) > ENOUGH_PACKETS;
        if (bytes > MAX_QUEUE_BYTES || (v_enough && a_enough) || M.eof) { usleep(10000); continue; }

        /* Bei separater Tonspur aus der Quelle lesen, die zeitlich zurückliegt */
        AVFormatContext *src = M.fmt;
        int from_audio = 0;
        if (M.split) {
            if (!M.afmt) { usleep(20000); continue; }
            int audio_full = pq_count(&M.aq) > 2 * ENOUGH_PACKETS;
            from_audio = !M.a_eof && (M.v_eof || (M.last_ats <= M.last_vts && !audio_full));
            src = from_audio ? M.afmt : M.fmt;
        }
        int r = av_read_frame(src, pkt);
        if (r < 0 && M.split) {
            if (M.abort) break;
            if (from_audio) M.a_eof = 1; else M.v_eof = 1;
            if (M.a_eof && M.v_eof) {
                pq_put(&M.vq, NULL, M.serial);
                pq_put(&M.aq, NULL, M.serial);
                M.eof = 1;
            }
            continue;
        }
        if (M.split && pkt->dts != AV_NOPTS_VALUE) {
            int64_t t = av_rescale_q(pkt->dts, src->streams[pkt->stream_index]->time_base, (AVRational){1, 1000000});
#ifndef __vita__
            if (getenv("VS_DEBUG") && ((from_audio && !M.last_ats) || (!from_audio && !M.last_vts)))
                fprintf(stderr, "[debug] erstes %s-Paket nach Start/Sprung: %.3fs\n", from_audio ? "Ton" : "Bild", t / 1e6);
#endif
            if (from_audio) M.last_ats = t; else M.last_vts = t;
        }
        if (from_audio) {
            if (pkt->stream_index == M.ai) {
                AVPacket *o = av_packet_alloc();
                av_packet_move_ref(o, pkt);
                pq_put(&M.aq, o, M.serial);
            } else {
                av_packet_unref(pkt);
            }
            continue;
        }
        if (r < 0) {
            if (M.abort) break;
            if (r == AVERROR_EOF || avio_feof(M.fmt->pb) || ++read_errors > 3) {
                if (r != AVERROR_EOF && M.frames_shown == 0 && M.vpackets == 0)
                    set_error("Lesefehler: %s", M.hls ? hls_error(M.hls) : net_last_detail());
                pq_put(&M.vq, NULL, M.serial);
                pq_put(&M.aq, NULL, M.serial);
                M.eof = 1;
            }
            continue;
        }
        read_errors = 0;
        if (pkt->stream_index == M.vi && M.bsf) {
            if (av_bsf_send_packet(M.bsf, pkt) == 0) {
                AVPacket *o = av_packet_alloc();
                while (av_bsf_receive_packet(M.bsf, o) == 0) {
                    pq_put(&M.vq, o, M.serial);
                    o = av_packet_alloc();
                }
                av_packet_free(&o);
            } else {
                av_packet_unref(pkt);
            }
        } else if (!M.split && pkt->stream_index == M.ai) {
            AVPacket *o = av_packet_alloc();
            av_packet_move_ref(o, pkt);
            pq_put(&M.aq, o, M.serial);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    return NULL;
}

/* ================================================================ API */

int media_open(const char *url, const char *headers)
{
    media_close();
    memset(&M, 0, sizeof M);
    M.url = strdup(url);
    M.headers = headers && *headers ? strdup(headers) : NULL;
    M.vi = M.ai = -1;
    M.shown = -1;
    pq_init(&M.vq);
    pq_init(&M.aq);
    pthread_mutex_init(&M.fm, NULL);
    pthread_cond_init(&M.fcv, NULL);
    pthread_mutex_init(&M.cm, NULL);
    av_log_set_level(AV_LOG_QUIET);
    M.state = MS_OPENING;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    if (pthread_create(&M.demux_t, &at, demux_thread, NULL) != 0) {
        set_error("Thread konnte nicht gestartet werden", NULL);
        pthread_attr_destroy(&at);
        return -1;
    }
    pthread_attr_destroy(&at);
    M.demux_started = 1;
    return 0;
}

void media_close(void)
{
    g_natracks = g_nstracks = 0;
    g_cur_atrack = -1;
    if (!M.demux_started) return;
    M.abort = 1;
    if (M.ns) net_stream_abort(M.ns);
    if (M.nl) net_live_abort(M.nl);
    pq_abort(&M.vq);
    pq_abort(&M.aq);
    pthread_cond_broadcast(&M.fcv);
    pthread_join(M.demux_t, NULL);
    if (M.video_started) pthread_join(M.video_t, NULL);
    if (M.audio_started) pthread_join(M.audio_t, NULL);

    pq_flush(&M.vq);
    pq_flush(&M.aq);
    close_format();
    close_audio_format();
    hls_close(M.ahls);
    av_bsf_free(&M.bsf);
    avcodec_free_context(&M.actx);
    swr_free(&M.swr);
    av_channel_layout_uninit(&M.swr_layout);
    if (M.vdec_ok && M.vd) M.vd->close();
    for (int i = 0; i < MEDIA_SLOTS; i++)
        if (M.slot[i].px) fb_destroy(i);
    hls_close(M.hls);
    net_stream_close(M.ns);
    net_live_close(M.nl);
    free(M.url);
    free(M.headers);
    pthread_mutex_destroy(&M.fm);
    pthread_cond_destroy(&M.fcv);
    pthread_mutex_destroy(&M.cm);
    memset(&M, 0, sizeof M);
    M.state = MS_IDLE;
}

MediaState media_state(void)
{
    if (M.state == MS_PLAYING && M.eof && pq_count(&M.vq) == 0 && pq_count(&M.aq) == 0) {
        int pending = 0;
        pthread_mutex_lock(&M.fm);
        for (int i = 0; i < MEDIA_SLOTS; i++)
            if (M.slot[i].st == FS_READY || M.slot[i].st == FS_DECODING) pending = 1;
        pthread_mutex_unlock(&M.fm);
        if (!pending) M.state = MS_ENDED;
    }
    return M.state;
}

const char *media_error(void) { return M.err; }

void media_toggle_pause(void)
{
    pthread_mutex_lock(&M.cm);
    if (!M.paused) {
        M.clock_pts = clock_get_locked();
        M.paused = 1;
    } else {
        M.clock_wall = plat_now_us();
        M.paused = 0;
    }
    pthread_mutex_unlock(&M.cm);
}

int media_paused(void) { return M.paused; }

void media_seek(int rel_seconds)
{
    if (M.state != MS_PLAYING || M.live) return;
    int64_t base = M.seek_req ? M.seek_target : media_position_ms() * 1000;
    M.seek_target = base + (int64_t)rel_seconds * 1000000;
    M.seek_req = 1;
}

int64_t media_position_ms(void)
{
    if (M.seek_req) return M.seek_target / 1000;
    if (M.vi < 0) {
        int64_t c = clock_get();
        if (c != INT64_MIN) { pthread_mutex_lock(&M.fm); pos_update(c); pthread_mutex_unlock(&M.fm); }
    }
    return M.last_pos > 0 ? M.last_pos / 1000 : 0;
}

int64_t media_duration_ms(void) { return M.duration / 1000; }
int     media_is_live(void)     { return M.live; }
int     media_has_video(void)   { return M.vi >= 0; }

int media_buffering(void)
{
    if (M.state == MS_OPENING) return 1;
    if (M.state != MS_PLAYING || M.paused || M.eof) return 0;
    if (M.seek_req) return 1;
    if (M.vi >= 0) {
        int ready = 0;
        pthread_mutex_lock(&M.fm);
        for (int i = 0; i < MEDIA_SLOTS; i++) if (M.slot[i].st == FS_READY) ready = 1;
        pthread_mutex_unlock(&M.fm);
        return !ready && pq_count(&M.vq) == 0;
    }
    return pq_count(&M.aq) == 0;
}

void media_debug(char *buf, int n)
{
    snprintf(buf, n, "%s\n%s-Decoder  Queue V:%d A:%d  Bilder:%d verw.:%d  Dec-Fehler:%d  Sync:%d (%+.1fs)",
             M.info[0] ? M.info : "-", M.vd ? M.vd->name : "-", pq_count(&M.vq), pq_count(&M.aq),
             M.frames_shown, M.frames_dropped, M.vdec_errors, M.resyncs, M.v_offset / 1e6);
}
