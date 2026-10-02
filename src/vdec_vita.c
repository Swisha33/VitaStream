/* Hardware-H.264-Decoder der Vita (sceAvcdec), Ausgabe direkt als RGBA8888.
 * Vorgehen wie bei Moonlight-Vita: Decoder-Arbeitsspeicher in PHYCONT-Speicher,
 * Annex-B-Access-Units rein, RGBA-Bild in einen Textur-Puffer raus. */
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <psp2/videodec.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>

static SceAvcdecCtrl             s_dec;
static SceAvcdecQueryDecoderInfo s_query;
static SceUID                    s_block = -1;
static int                       s_lib, s_created;
static char                      s_err[96];

int64_t plat_now_us(void) { return (int64_t)sceKernelGetProcessTimeWide(); }

const char *vdec_last_error(void) { return s_err; }

static void release(void)
{
    if (s_created) sceAvcdecDeleteDecoder(&s_dec);
    s_created = 0;
    if (s_block >= 0) sceKernelFreeMemBlock(s_block);
    s_block = -1;
    if (s_lib) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    s_lib = 0;
}

int vdec_open(int w, int h)
{
    static const int refs_try[] = { 16, 8, 5, 4 };
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    s_err[0] = 0;

    for (unsigned i = 0; i < sizeof refs_try / sizeof *refs_try; i++) {
        release();
        int refs = refs_try[i];

        SceVideodecQueryInitInfoHwAvcdec init;
        memset(&init, 0, sizeof init);
        init.size = sizeof init;
        init.horizontal = w;
        init.vertical = h;
        init.numOfRefFrames = refs;
        init.numOfStreams = 1;
        int r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
        if (r < 0) { snprintf(s_err, sizeof s_err, "InitLibrary 0x%08X (%dx%d, %d Ref.)", r, w, h, refs); continue; }
        s_lib = 1;

        memset(&s_query, 0, sizeof s_query);
        s_query.horizontal = w;
        s_query.vertical = h;
        s_query.numOfRefFrames = refs;
        SceAvcdecDecoderInfo info;
        memset(&info, 0, sizeof info);
        r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &s_query, &info);
        if (r < 0) { snprintf(s_err, sizeof s_err, "QueryMemSize 0x%08X", r); continue; }

        SceSize sz = (info.frameMemSize + 0xFFFFF) & ~0xFFFFF;
        s_block = sceKernelAllocMemBlock("vs_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
        if (s_block < 0) { snprintf(s_err, sizeof s_err, "Kein Decoder-Speicher (%u KB)", (unsigned)(sz / 1024)); continue; }

        memset(&s_dec, 0, sizeof s_dec);
        sceKernelGetMemBlockBase(s_block, &s_dec.frameBuf.pBuf);
        s_dec.frameBuf.size = sz;
        r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &s_dec, &s_query);
        if (r < 0) { snprintf(s_err, sizeof s_err, "CreateDecoder 0x%08X", r); continue; }
        s_created = 1;
        return 0;
    }
    release();
    return -1;
}

void vdec_reset(void)
{
    if (!s_created) return;
    sceAvcdecDeleteDecoder(&s_dec);
    s_created = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &s_dec, &s_query) >= 0;
}

int vdec_decode(const uint8_t *au_data, int len, int64_t pts90k, const VdecTarget *dst, VdecResult *res)
{
    if (!s_created) { snprintf(s_err, sizeof s_err, "Decoder nicht bereit"); return -1; }

    SceAvcdecAu au;
    memset(&au, 0, sizeof au);
    au.es.pBuf = (void *)au_data;
    au.es.size = len;
    au.dts.upper = au.dts.lower = 0xFFFFFFFF;
    if (pts90k >= 0) {
        au.pts.upper = (uint32_t)((uint64_t)pts90k >> 32);
        au.pts.lower = (uint32_t)((uint64_t)pts90k & 0xFFFFFFFF);
    } else {
        au.pts.upper = au.pts.lower = 0xFFFFFFFF;
    }

    SceAvcdecPicture pic;
    memset(&pic, 0, sizeof pic);
    SceAvcdecPicture *pp = &pic;
    SceAvcdecArrayPicture arr;
    memset(&arr, 0, sizeof arr);
    arr.numOfElm = 1;
    arr.pPicture = &pp;

    pic.size = sizeof pic;
    pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    pic.frame.framePitch = dst->pitch;
    pic.frame.frameWidth = dst->width;
    pic.frame.frameHeight = dst->height;
    pic.frame.pPicture[0] = dst->pixels;

    int r = sceAvcdecDecode(&s_dec, &au, &arr);
    if (r < 0) {
        snprintf(s_err, sizeof s_err, "Decode 0x%08X", r);
        return -1;
    }
    if (arr.numOfOutput < 1) return 0;

    if (pic.info.pts.upper == 0xFFFFFFFF && pic.info.pts.lower == 0xFFFFFFFF)
        res->pts90k = -1;
    else
        res->pts90k = ((int64_t)pic.info.pts.upper << 32) | pic.info.pts.lower;
    int w = pic.frame.horizontalSize, h = pic.frame.verticalSize;
    w -= pic.frame.frameCropLeftOffset + pic.frame.frameCropRightOffset;
    h -= pic.frame.frameCropTopOffset + pic.frame.frameCropBottomOffset;
    res->width  = (w > 0 && w <= dst->width)  ? w : dst->width;
    res->height = (h > 0 && h <= dst->height) ? h : dst->height;
    return 1;
}

void vdec_close(void) { release(); }
