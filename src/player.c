#include "player.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

/* In SceAvPlayer_stub vorhanden, fehlt aber in den vita-headers */
int sceAvPlayerStreamCount(SceAvPlayerHandle handle);

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static SceAvPlayerHandle s_h;
static int               s_active;
static int               s_paused;
static NetStream        *s_stream;
static char              s_url[2048];
static char              s_headers[1024];
static char              s_err[128];
static vita2d_texture    s_tex;        /* nur gxm_tex wird genutzt, nie mit vita2d freigeben */
static int               s_have_frame;
static int               s_fw, s_fh;

static SceUID            s_audio_thread = -1;
static volatile int      s_audio_run;

/* ---------- Speicher-Callbacks ---------- */

static void *mem_alloc(void *p, uint32_t align, uint32_t size)
{
    (void)p;
    return memalign(align < 16 ? 16 : align, size);
}

static void mem_free(void *p, void *ptr) { (void)p; free(ptr); }

static void *gpu_alloc(void *p, uint32_t align, uint32_t size)
{
    (void)p;
    (void)align;
    size = ALIGN(size, 256 * 1024);
    SceUID uid = sceKernelAllocMemBlock("vs_avp_tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (uid < 0) return NULL;
    void *base = NULL;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    return base;
}

static void gpu_free(void *p, void *ptr)
{
    (void)p;
    if (!ptr) return;
    SceUID uid = sceKernelFindMemBlockByAddr(ptr, 0);
    if (uid < 0) return;
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(uid);
}

/* ---------- Datei-Callbacks (MP4 über eigenen Netzwerkstack) ---------- */

static int f_open(void *p, const char *name)
{
    (void)p; (void)name;
    if (s_stream) return 0;
    s_stream = net_stream_open(s_url, s_headers[0] ? s_headers : NULL);
    return s_stream ? 0 : -1;
}

static int f_close(void *p)
{
    (void)p;
    net_stream_close(s_stream);
    s_stream = NULL;
    return 0;
}

static int f_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len)
{
    (void)p;
    return net_stream_read(s_stream, pos, buf, len);
}

static uint64_t f_size(void *p)
{
    (void)p;
    return net_stream_size(s_stream);
}

/* ---------- Audio ---------- */

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) return 0;
    int rate = 48000, ch = 2;
    SceAvPlayerFrameInfo fr;

    while (s_audio_run) {
        if (s_active && !s_paused && sceAvPlayerIsActive(s_h)) {
            memset(&fr, 0, sizeof fr);
            if (sceAvPlayerGetAudioData(s_h, &fr)) {
                int r = fr.details.audio.sampleRate, c = fr.details.audio.channelCount;
                if (r != rate || c != ch) {
                    rate = r; ch = c;
                    sceAudioOutSetConfig(port, 1024, rate,
                        ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
                }
                sceAudioOutOutput(port, fr.pData);
                continue;
            }
        }
        sceKernelDelayThread(2000);
    }
    sceAudioOutReleasePort(port);
    return 0;
}

/* ---------- Öffentliche API ---------- */

static int is_hls(const char *url)
{
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    return n >= 5 && !strncasecmp(url + n - 5, ".m3u8", 5);
}

int player_open(const char *url, const char *headers)
{
    player_close();
    s_err[0] = 0;
    snprintf(s_url, sizeof s_url, "%s", url);
    snprintf(s_headers, sizeof s_headers, "%s", headers ? headers : "");

    if (net_check_url(url) == NET_BLOCKED) {
        snprintf(s_err, sizeof s_err, "Stream durch AdBlock gesperrt");
        return -1;
    }

    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof init);
    init.memoryReplacement.allocate          = mem_alloc;
    init.memoryReplacement.deallocate        = mem_free;
    init.memoryReplacement.allocateTexture   = gpu_alloc;
    init.memoryReplacement.deallocateTexture = gpu_free;
    init.basePriority               = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart                  = 1;
    init.debugLevel                 = 0;

    const char *source = url;
    int hls = is_hls(url) || !strncmp(url, "ux0:", 4);
    if (!hls) {
        init.fileReplacement.open       = f_open;
        init.fileReplacement.close      = f_close;
        init.fileReplacement.readOffset = f_read;
        init.fileReplacement.size       = f_size;
        source = "vitastream.mp4";
    }

    s_h = sceAvPlayerInit(&init);
    if (!s_h) {
        snprintf(s_err, sizeof s_err, "sceAvPlayerInit fehlgeschlagen");
        return -1;
    }
    if (sceAvPlayerAddSource(s_h, source) < 0) {
        snprintf(s_err, sizeof s_err, "Quelle kann nicht geoeffnet werden");
        sceAvPlayerClose(s_h);
        s_h = 0;
        return -1;
    }

    s_active = 1;
    s_paused = 0;
    s_have_frame = 0;

    s_audio_run = 1;
    s_audio_thread = sceKernelCreateThread("vs_audio", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (s_audio_thread >= 0) sceKernelStartThread(s_audio_thread, 0, NULL);
    return 0;
}

void player_close(void)
{
    if (s_audio_thread >= 0) {
        s_audio_run = 0;
        sceKernelWaitThreadEnd(s_audio_thread, NULL, NULL);
        sceKernelDeleteThread(s_audio_thread);
        s_audio_thread = -1;
    }
    if (s_h) {
        sceAvPlayerStop(s_h);
        sceAvPlayerClose(s_h);
        s_h = 0;
    }
    if (s_stream) { net_stream_close(s_stream); s_stream = NULL; }
    s_active = 0;
    s_have_frame = 0;
}

int player_active(void)
{
    if (!s_active) return 0;
    if (!sceAvPlayerIsActive(s_h)) {
        if (!s_have_frame && !s_err[0])
            snprintf(s_err, sizeof s_err, "Format nicht unterstuetzt oder Verbindung abgebrochen");
        return 0;
    }
    return 1;
}

void player_draw(void)
{
    if (s_active && !s_paused) {
        SceAvPlayerFrameInfo fr;
        memset(&fr, 0, sizeof fr);
        if (sceAvPlayerGetVideoData(s_h, &fr)) {
            s_fw = fr.details.video.width;
            s_fh = fr.details.video.height;
            sceGxmTextureInitLinear(&s_tex.gxm_tex, fr.pData,
                                    SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, s_fw, s_fh, 0);
            sceGxmTextureSetMinFilter(&s_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(&s_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            s_have_frame = 1;
        }
    }
    if (!s_have_frame || s_fw <= 0 || s_fh <= 0) return;

    float sx = 960.0f / s_fw, sy = 544.0f / s_fh;
    float s = sx < sy ? sx : sy;
    float w = s_fw * s, h = s_fh * s;
    vita2d_draw_texture_scale(&s_tex, (960.0f - w) / 2.0f, (544.0f - h) / 2.0f, s, s);
}

void player_toggle_pause(void)
{
    if (!s_active) return;
    if (s_paused) sceAvPlayerResume(s_h);
    else          sceAvPlayerPause(s_h);
    s_paused = !s_paused;
}

int player_paused(void) { return s_paused; }

uint64_t player_position_ms(void) { return s_active ? sceAvPlayerCurrentTime(s_h) : 0; }

uint64_t player_duration_ms(void)
{
    if (!s_active) return 0;
    int n = sceAvPlayerStreamCount(s_h);
    for (int i = 0; i < n; i++) {
        SceAvPlayerStreamInfo info;
        memset(&info, 0, sizeof info);
        if (sceAvPlayerGetStreamInfo(s_h, i, &info) == 0 && info.type == SCE_AVPLAYER_VIDEO)
            return info.duration;
    }
    return 0;
}

void player_seek_rel(int seconds)
{
    if (!s_active) return;
    int64_t t = (int64_t)player_position_ms() + (int64_t)seconds * 1000;
    uint64_t dur = player_duration_ms();
    if (t < 0) t = 0;
    if (dur && (uint64_t)t > dur - 1000) t = dur > 1000 ? dur - 1000 : 0;
    sceAvPlayerJumpToTime(s_h, (uint64_t)t);
}

const char *player_error(void) { return s_err; }
