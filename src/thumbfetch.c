#define _GNU_SOURCE
#include "thumbfetch.h"
#include "net.h"
#include "hls.h"   /* hls_join_url */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

#define MAX_IMAGE_BYTES (6 * 1024 * 1024)
#define PAGE_HEAD_BYTES (192 * 1024)

/* Wert eines HTML-Attributs ab p (nach name=) lesen */
static int attr_value(const char *tag_start, const char *tag_end, const char *name, char *out, int outlen)
{
    size_t nl = strlen(name);
    for (const char *p = tag_start; p + nl < tag_end; p++) {
        if (strncasecmp(p, name, nl) || p[nl] != '=') continue;
        if (p > tag_start && !isspace((unsigned char)p[-1])) continue;
        const char *v = p + nl + 1;
        char q = (*v == '"' || *v == '\'') ? *v++ : 0;
        int n = 0;
        while (v < tag_end && *v && (q ? *v != q : !isspace((unsigned char)*v) && *v != '>') && n < outlen - 1)
            out[n++] = *v++;
        out[n] = 0;
        return n > 0;
    }
    return 0;
}

static void html_unescape_amp(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; }
        else *w++ = *r++;
    }
    *w = 0;
}

int thumb_find_og_image(const char *html, const char *page_url, char *out, int outlen)
{
    static const char *keys[] = { "og:image:secure_url", "og:image", "twitter:image", "twitter:image:src" };
    char best[2048] = "";
    int best_rank = 99;
    for (const char *p = html; (p = strcasestr(p, "<meta")); p++) {
        const char *end = strchr(p, '>');
        if (!end) break;
        char prop[64] = "", content[2048] = "";
        if (!attr_value(p, end, "property", prop, sizeof prop)) attr_value(p, end, "name", prop, sizeof prop);
        if (prop[0] && attr_value(p, end, "content", content, sizeof content)) {
            for (int k = 0; k < 4; k++)
                if (!strcasecmp(prop, keys[k]) && k < best_rank) {
                    best_rank = k;
                    snprintf(best, sizeof best, "%s", content);
                }
        }
    }
    if (!best[0]) {
        for (const char *p = html; (p = strcasestr(p, "<link")); p++) {
            const char *end = strchr(p, '>');
            if (!end) break;
            char rel[32], href[2048];
            if (attr_value(p, end, "rel", rel, sizeof rel) && !strcasecmp(rel, "image_src") &&
                attr_value(p, end, "href", href, sizeof href)) {
                snprintf(best, sizeof best, "%s", href);
                break;
            }
        }
    }
    if (!best[0]) return -1;
    html_unescape_amp(best);
    hls_join_url(page_url, best, out, outlen);
    return 0;
}

static int download(const char *url, const char *extra_headers, const volatile int *abort_flag, NetBuf *b)
{
    long status = 0;
    int r = net_request_ex(url, NULL, extra_headers, b, &status, NULL, 0, abort_flag);
    if (r != NET_OK || status >= 400 || !b->data) { net_buf_free(b); return -1; }
    return 0;
}

/* Box-Filter-Verkleinerung RGBA */
static uint8_t *downscale(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0}, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = src + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    n++;
                }
            }
            uint8_t *d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(acc[c] / n);
        }
    }
    return dst;
}

int thumb_fetch(const char *url, const volatile int *abort_flag, int max_w, int max_h,
                uint8_t **rgba, int *w, int *h)
{
    *rgba = NULL;
    char img_url[2048];
    if (!strncmp(url, "og:", 3)) {
        const char *page = url + 3;
        char hdr[64];
        snprintf(hdr, sizeof hdr, "Range: bytes=0-%d", PAGE_HEAD_BYTES - 1);
        NetBuf pb;
        if (download(page, hdr, abort_flag, &pb) < 0) return -1;
        int r = thumb_find_og_image(pb.data, page, img_url, sizeof img_url);
        net_buf_free(&pb);
        if (r < 0) return -1;
    } else {
        snprintf(img_url, sizeof img_url, "%s", url);
    }

    NetBuf ib;
    if (download(img_url, NULL, abort_flag, &ib) < 0) return -1;
    if (ib.len > MAX_IMAGE_BYTES) { net_buf_free(&ib); return -1; }

    int iw = 0, ih = 0, ch = 0;
    uint8_t *px = stbi_load_from_memory((const uint8_t *)ib.data, (int)ib.len, &iw, &ih, &ch, 4);
    net_buf_free(&ib);
    if (!px || iw <= 0 || ih <= 0) { stbi_image_free(px); return -1; }

    int dw = iw, dh = ih;
    if (dw > max_w) { dh = dh * max_w / dw; dw = max_w; }
    if (dh > max_h) { dw = dw * max_h / dh; dh = max_h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint8_t *out;
    if (dw == iw && dh == ih) {
        out = malloc((size_t)iw * ih * 4);
        if (out) memcpy(out, px, (size_t)iw * ih * 4);
    } else {
        out = downscale(px, iw, ih, dw, dh);
    }
    stbi_image_free(px);
    if (!out) return -1;
    *rgba = out;
    *w = dw;
    *h = dh;
    return 0;
}
