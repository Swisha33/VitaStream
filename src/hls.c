#include "hls.h"
#include "net.h"
#include "../third_party/aes/aes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define MAX_W 1280
#define MAX_H 720

typedef struct {
    char   *uri;
    double  duration;
    int64_t seq;
    int64_t br_off, br_len;   /* EXT-X-BYTERANGE, br_len < 0 = ganzes Segment */
    char   *key_uri;          /* AES-128-Schlüssel (NULL = unverschlüsselt) */
    uint8_t iv[16];
    int     has_iv;           /* sonst IV = Sequenznummer */
} Segment;

struct Hls {
    char     *headers;
    const volatile int *abort_flag;
    char      media_url[2048];
    Segment  *seg;
    int       nseg, capseg;
    double    target_duration;
    int       endlist;
    char     *map_uri;           /* fMP4-Init-Segment */
    int64_t   map_off, map_len;
    int       need_map;
    int       cur;               /* Index des nächsten zu ladenden Segments */
    int64_t   last_seq;          /* zuletzt geladene Sequenznummer (Live) */
    NetBuf    buf;
    size_t    pos;
    char      info[96];
    char     *audio_url;         /* separate Tonspur (EXT-X-MEDIA TYPE=AUDIO) */
    char      audio_lang[16];
    HlsTrack  atr[HLS_MAX_TRACKS];   /* alle Tonspuren der gewählten Qualität */
    int       natr, cur_atr;
    HlsTrack  str[HLS_MAX_TRACKS];   /* Untertitel (EXT-X-MEDIA TYPE=SUBTITLES) */
    int       nstr;
    char      err[160];
    char     *cur_key_uri;       /* beim Parsen gültiger Schlüssel */
    uint8_t   cur_iv[16];
    int       cur_has_iv;
    char     *key_cache_uri;     /* zuletzt geladener Schlüssel */
    uint8_t   key_cache[16];
};

/* bevorzugte Tonspur (Sprache oder Name), gilt beim nächsten Öffnen */
static char g_audio_pref[48];
void hls_set_audio_pref(const char *pref) { snprintf(g_audio_pref, sizeof g_audio_pref, "%s", pref ? pref : ""); }

/* ---------------------------------------------------------------- Hilfen */

static void sleep_ms(int ms) { usleep(ms * 1000); }

static int aborted(const Hls *h) { return h->abort_flag && *h->abort_flag; }

void hls_join_url(const char *base, const char *ref, char *out, int outlen)
{
    if (strstr(ref, "://")) { snprintf(out, outlen, "%s", ref); return; }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) { snprintf(out, outlen, "%s", ref); return; }
    if (ref[0] == '/' && ref[1] == '/') {          /* protokollrelativ */
        snprintf(out, outlen, "%.*s:%s", (int)(scheme_end - base), base, ref);
        return;
    }
    const char *host_start = scheme_end + 3;
    const char *path_start = host_start + strcspn(host_start, "/?#");
    if (ref[0] == '/') {
        snprintf(out, outlen, "%.*s%s", (int)(path_start - base), base, ref);
        return;
    }
    /* relativ zum Verzeichnis der Basis (Query der Basis ignorieren) */
    const char *q = base + strcspn(base, "?#");
    const char *slash = q;
    while (slash > path_start && slash[-1] != '/') slash--;
    if (slash <= path_start) snprintf(out, outlen, "%.*s/%s", (int)(path_start - base), base, ref);
    else                     snprintf(out, outlen, "%.*s%s", (int)(slash - base), base, ref);

    /* "./" und "../" auflösen */
    char *p;
    while ((p = strstr(out, "/./"))) memmove(p, p + 2, strlen(p + 2) + 1);
    while ((p = strstr(out, "/../"))) {
        char *prev = p;
        char *min = strstr(out, "://");
        min = min ? strchr(min + 3, '/') : out;
        if (!min || p <= min) break;
        do { prev--; } while (prev > min && *prev != '/');
        memmove(prev, p + 3, strlen(p + 3) + 1);
    }
}

static const char *attr(const char *line, const char *key, char *out, int outlen)
{
    /* Attribute-Liste: KEY=VALUE,KEY="VALUE",... */
    size_t kl = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key))) {
        if ((p == line || p[-1] == ',' || p[-1] == ':') && p[kl] == '=') {
            p += kl + 1;
            int n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && n < outlen - 1) out[n++] = *p++;
            } else {
                while (*p && *p != ',' && *p != '\r' && *p != '\n' && n < outlen - 1) out[n++] = *p++;
            }
            out[n] = 0;
            return out;
        }
        p += kl;
    }
    return NULL;
}

static char *fetch(Hls *h, const char *url, long *status_out, char *final_url, int final_len)
{
    NetBuf b;
    long status = 0;
    int r = net_request_ex(url, NULL, h->headers, &b, &status, final_url, final_len, h->abort_flag);
    if (status_out) *status_out = status;
    if (r != NET_OK) {
        snprintf(h->err, sizeof h->err, "%s: %s", net_strerror(r), net_last_detail());
        return NULL;
    }
    if (status >= 400) {
        snprintf(h->err, sizeof h->err, "HTTP %ld beim Laden der Playlist", status);
        net_buf_free(&b);
        return NULL;
    }
    return b.data ? b.data : strdup("");
}

static void free_segments(Hls *h)
{
    for (int i = 0; i < h->nseg; i++) { free(h->seg[i].uri); free(h->seg[i].key_uri); }
    h->nseg = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "0x00112233..." -> 16 Bytes (kürzere Werte werden links mit 0 aufgefüllt) */
static int parse_iv(const char *v, uint8_t out[16])
{
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
    size_t n = strlen(v);
    if (n == 0 || n > 32) return -1;
    memset(out, 0, 16);
    for (size_t i = 0; i < n; i++) {
        int d = hexval(v[n - 1 - i]);
        if (d < 0) return -1;
        out[15 - i / 2] |= (uint8_t)(i % 2 ? d << 4 : d);
    }
    return 0;
}

/* ---------------------------------------------------------------- Master */

typedef struct { char uri[2048]; long bw; int w, h; int ok; char audio[64], subs[64]; } Variant;

static int pref_matches(const char *lang, const char *name)
{
    if (!g_audio_pref[0]) return 0;
    if (lang[0] && !strcasecmp(lang, g_audio_pref)) return 1;
    if (name[0] && !strcasecmp(name, g_audio_pref)) return 1;
    /* "de" passt auch auf "de-DE", "deu", "ger" */
    if (lang[0] && strlen(g_audio_pref) == 2 && !strncasecmp(lang, g_audio_pref, 2)) return 1;
    return 0;
}

/* Tonspuren (AUDIO) und Untertitel (SUBTITLES) einer Gruppe sammeln; Tonspur wählen:
   Vorliebe des Nutzers > Deutsch > DEFAULT=YES > erste */
static void choose_audio(Hls *h, const char *text, const char *base, const char *group, const char *sgroup)
{
    int best_score = -1;
    char best_uri[2048] = "", best_lang[16] = "";
    h->natr = h->nstr = 0;
    h->cur_atr = -1;
    const char *p = text;
    while (*p) {
        const char *eol = p + strcspn(p, "\r\n");
        if (!strncmp(p, "#EXT-X-MEDIA:", 13)) {
            char line[2048], v[2048], gid[64] = "", type[16] = "";
            snprintf(line, sizeof line, "%.*s", (int)(eol - p), p + 13);
            attr(line, "TYPE", type, sizeof type);
            attr(line, "GROUP-ID", gid, sizeof gid);
            char lang[16] = "", name[48] = "";
            attr(line, "LANGUAGE", lang, sizeof lang);
            attr(line, "NAME", name, sizeof name);
            for (char *c = lang; *c; c++) *c = (char)tolower((unsigned char)*c);
            int has_uri = attr(line, "URI", v, sizeof v) != NULL;
            char uri[2048] = "";
            if (has_uri) hls_join_url(base, v, uri, sizeof uri);

            if (group && !strcmp(type, "AUDIO") && !strcmp(gid, group)) {
                if (h->natr < HLS_MAX_TRACKS) {
                    HlsTrack *t = &h->atr[h->natr];
                    snprintf(t->lang, sizeof t->lang, "%s", lang);
                    snprintf(t->name, sizeof t->name, "%s", name[0] ? name : (lang[0] ? lang : "Ton"));
                    snprintf(t->uri, sizeof t->uri, "%s", uri);
                    h->natr++;
                }
                int score = 0;
                if (pref_matches(lang, name)) score += 8;
                if (!strncmp(lang, "de", 2) || !strcmp(lang, "ger")) score += 4;
                if (attr(line, "DEFAULT", v, sizeof v) && !strcmp(v, "YES")) score += 2;
                if (score > best_score) {
                    best_score = score;
                    snprintf(best_uri, sizeof best_uri, "%s", uri);   /* leer: Ton steckt im Videostream */
                    snprintf(best_lang, sizeof best_lang, "%s", lang);
                    h->cur_atr = h->natr - 1;
                }
            } else if (sgroup && !strcmp(type, "SUBTITLES") && !strcmp(gid, sgroup) && uri[0]) {
                if (h->nstr < HLS_MAX_TRACKS) {
                    HlsTrack *t = &h->str[h->nstr];
                    snprintf(t->lang, sizeof t->lang, "%s", lang);
                    snprintf(t->name, sizeof t->name, "%s", name[0] ? name : (lang[0] ? lang : "Untertitel"));
                    snprintf(t->uri, sizeof t->uri, "%s", uri);
                    h->nstr++;
                }
            }
        }
        p = eol + strspn(eol, "\r\n");
    }
    free(h->audio_url);
    h->audio_url = best_uri[0] ? strdup(best_uri) : NULL;
    snprintf(h->audio_lang, sizeof h->audio_lang, "%s", best_lang);
}

static int choose_variant(Hls *h, const char *text, const char *base, char *out, int outlen)
{
    Variant best = {0}, fallback = {0};
    int found = 0;
    const char *p = text;
    while (*p) {
        const char *eol = p + strcspn(p, "\r\n");
        if (!strncmp(p, "#EXT-X-STREAM-INF:", 18)) {
            char line[1024], v[256];
            snprintf(line, sizeof line, "%.*s", (int)(eol - p), p);
            Variant cand = {0};
            cand.bw = attr(line, "BANDWIDTH", v, sizeof v) ? atol(v) : 0;
            if (attr(line, "RESOLUTION", v, sizeof v)) sscanf(v, "%dx%d", &cand.w, &cand.h);
            cand.ok = 1;
            if (attr(line, "AUDIO", v, sizeof v)) snprintf(cand.audio, sizeof cand.audio, "%s", v);
            if (attr(line, "SUBTITLES", v, sizeof v)) snprintf(cand.subs, sizeof cand.subs, "%s", v);
            if (attr(line, "CODECS", v, sizeof v)) {
                for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
                if (strstr(v, "hvc1") || strstr(v, "hev1") || strstr(v, "av01") || strstr(v, "vp09"))
                    cand.ok = 0;      /* Codec kann die Vita nicht */
            }
            /* nächste URI-Zeile */
            const char *q = eol;
            while (*q) {
                q += strspn(q, "\r\n");
                const char *e = q + strcspn(q, "\r\n");
                if (e > q && *q != '#') {
                    char ref[2048];
                    snprintf(ref, sizeof ref, "%.*s", (int)(e - q), q);
                    hls_join_url(base, ref, cand.uri, sizeof cand.uri);
                    break;
                }
                q = e;
            }
            if (cand.uri[0] && cand.ok) {
                found = 1;
                int fits = cand.h ? (cand.w <= MAX_W && cand.h <= MAX_H) : (cand.bw <= 4000000);
                if (fits) {
                    if (!best.uri[0] || cand.bw > best.bw) best = cand;
                } else if (!fallback.uri[0] || (cand.h && cand.h < fallback.h) || (!cand.h && cand.bw < fallback.bw)) {
                    fallback = cand;
                }
            }
        }
        p = eol + strspn(eol, "\r\n");
    }
    if (!found) {
        snprintf(h->err, sizeof h->err, "Keine abspielbare Qualitaet (nur HEVC/AV1?)");
        return -1;
    }
    Variant *v = best.uri[0] ? &best : &fallback;
    snprintf(out, outlen, "%s", v->uri);
    if (v->audio[0] || v->subs[0]) choose_audio(h, text, base, v->audio[0] ? v->audio : NULL, v->subs[0] ? v->subs : NULL);
    if (v->h) snprintf(h->info, sizeof h->info, "%dx%d, %.1f Mbit/s", v->w, v->h, v->bw / 1e6);
    else      snprintf(h->info, sizeof h->info, "%.1f Mbit/s", v->bw / 1e6);
    return 0;
}

/* ---------------------------------------------------------------- Media-Playlist */

static int parse_media(Hls *h, const char *text, const char *base)
{
    free_segments(h);
    free(h->cur_key_uri);
    h->cur_key_uri = NULL;
    h->cur_has_iv = 0;
    h->endlist = 0;
    h->target_duration = 6;
    int64_t seq = 0;
    double dur = 0;
    int64_t br_off = 0, br_len = -1, next_off = 0;
    int have_br = 0;
    const char *p = text;
    while (*p) {
        const char *eol = p + strcspn(p, "\r\n");
        char line[2048];
        snprintf(line, sizeof line, "%.*s", (int)(eol - p), p);
        if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
            h->target_duration = atof(line + 22);
        } else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            seq = atoll(line + 22);
        } else if (!strncmp(line, "#EXTINF:", 8)) {
            dur = atof(line + 8);
        } else if (!strncmp(line, "#EXT-X-BYTERANGE:", 17)) {
            br_len = atoll(line + 17);
            const char *at = strchr(line + 17, '@');
            br_off = at ? atoll(at + 1) : next_off;
            have_br = 1;
        } else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) {
            h->endlist = 1;
        } else if (!strncmp(line, "#EXT-X-KEY:", 11)) {
            char v[2048], abs_uri[2048];
            if (!attr(line + 11, "METHOD", v, sizeof v) || !strcmp(v, "NONE")) {
                free(h->cur_key_uri);
                h->cur_key_uri = NULL;
            } else if (!strcmp(v, "AES-128")) {
                if (!attr(line + 11, "URI", v, sizeof v)) {
                    snprintf(h->err, sizeof h->err, "AES-128 ohne Schluessel-URI");
                    return -1;
                }
                hls_join_url(base, v, abs_uri, sizeof abs_uri);
                free(h->cur_key_uri);
                h->cur_key_uri = strdup(abs_uri);
                h->cur_has_iv = attr(line + 11, "IV", v, sizeof v) && parse_iv(v, h->cur_iv) == 0;
            } else {
                /* SAMPLE-AES, Widevine, FairPlay ... = echtes DRM */
                snprintf(h->err, sizeof h->err, "Stream ist DRM-geschuetzt (%s) - nicht abspielbar", v);
                return -1;
            }
        } else if (!strncmp(line, "#EXT-X-MAP:", 11)) {
            char v[2048], abs_uri[2048];
            if (attr(line + 11, "URI", v, sizeof v)) {
                hls_join_url(base, v, abs_uri, sizeof abs_uri);
                if (!h->map_uri || strcmp(h->map_uri, abs_uri)) {
                    free(h->map_uri);
                    h->map_uri = strdup(abs_uri);
                    h->need_map = 1;
                }
                h->map_len = -1;
                if (attr(line + 11, "BYTERANGE", v, sizeof v)) {
                    h->map_len = atoll(v);
                    const char *at = strchr(v, '@');
                    h->map_off = at ? atoll(at + 1) : 0;
                }
            }
        } else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            return 1;   /* doch eine Master-Playlist */
        } else if (line[0] && line[0] != '#') {
            if (h->nseg == h->capseg) {
                h->capseg = h->capseg ? h->capseg * 2 : 64;
                h->seg = realloc(h->seg, sizeof(Segment) * h->capseg);
            }
            Segment *s = &h->seg[h->nseg++];
            char abs_uri[2048];
            hls_join_url(base, line, abs_uri, sizeof abs_uri);
            s->uri = strdup(abs_uri);
            s->duration = dur > 0 ? dur : h->target_duration;
            s->seq = seq++;
            s->br_len = have_br ? br_len : -1;
            s->br_off = have_br ? br_off : 0;
            s->key_uri = h->cur_key_uri ? strdup(h->cur_key_uri) : NULL;
            s->has_iv = h->cur_has_iv;
            memcpy(s->iv, h->cur_iv, 16);
            if (have_br) next_off = br_off + br_len;
            have_br = 0;
            dur = 0;
        }
        p = eol + strspn(eol, "\r\n");
    }
    if (h->nseg == 0) {
        snprintf(h->err, sizeof h->err, "Playlist enthaelt keine Segmente");
        return -1;
    }
    return 0;
}

static int load_media(Hls *h)
{
    char final_url[2048];
    char *text = fetch(h, h->media_url, NULL, final_url, sizeof final_url);
    if (!text) return -1;
    int r = parse_media(h, text, final_url);
    free(text);
    return r;
}

/* ---------------------------------------------------------------- API */

Hls *hls_open(const char *url, const char *headers, const volatile int *abort_flag, char *err, int errlen)
{
    Hls *h = calloc(1, sizeof *h);
    h->headers = headers && *headers ? strdup(headers) : NULL;
    h->abort_flag = abort_flag;
    snprintf(h->media_url, sizeof h->media_url, "%s", url);

    char final_url[2048];
    char *text = fetch(h, url, NULL, final_url, sizeof final_url);
    if (!text) goto fail;
    if (strncmp(text + strspn(text, " \t\r\n\xEF\xBB\xBF"), "#EXTM3U", 7)) {
        snprintf(h->err, sizeof h->err, "Keine HLS-Playlist");
        free(text);
        goto fail;
    }

    int r = parse_media(h, text, final_url);
    if (r == 1) {   /* Master-Playlist -> Variante wählen */
        h->err[0] = 0;
        if (choose_variant(h, text, final_url, h->media_url, sizeof h->media_url) < 0) { free(text); goto fail; }
        free(text);
        if (load_media(h) != 0) goto fail;
    } else {
        free(text);
        if (r < 0) goto fail;
        snprintf(h->media_url, sizeof h->media_url, "%s", final_url);
    }

    if (!h->endlist) {
        /* Live: ein paar Segmente vor dem Ende beginnen */
        h->cur = h->nseg > 3 ? h->nseg - 3 : 0;
    }
    h->last_seq = h->seg[h->cur].seq - 1;
    return h;

fail:
    if (err) snprintf(err, errlen, "%s", h->err[0] ? h->err : "HLS-Fehler");
    hls_close(h);
    return NULL;
}

void hls_close(Hls *h)
{
    if (!h) return;
    free_segments(h);
    free(h->seg);
    free(h->map_uri);
    free(h->headers);
    free(h->cur_key_uri);
    free(h->key_cache_uri);
    free(h->audio_url);
    net_buf_free(&h->buf);
    free(h);
}

static int download(Hls *h, const char *uri, int64_t off, int64_t len)
{
    char hdr[1400];
    const char *headers = h->headers;
    if (len >= 0) {
        snprintf(hdr, sizeof hdr, "%s%sRange: bytes=%lld-%lld", h->headers ? h->headers : "",
                 h->headers ? "\n" : "", (long long)off, (long long)(off + len - 1));
        headers = hdr;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (aborted(h)) return -1;
        net_buf_free(&h->buf);
        long status = 0;
        int r = net_request_ex(uri, NULL, headers, &h->buf, &status, NULL, 0, h->abort_flag);
        if (r == NET_OK && status < 400) { h->pos = 0; return 0; }
        if (r == NET_OK) snprintf(h->err, sizeof h->err, "HTTP %ld bei Segment", status);
        else             snprintf(h->err, sizeof h->err, "%s: %s", net_strerror(r), net_last_detail());
        if (r == NET_BLOCKED || r == NET_ABORTED || status == 404 || status == 403) break;
        sleep_ms(500);
    }
    net_buf_free(&h->buf);
    return -1;
}

/* AES-128-CBC: Schlüssel laden (gecacht) und h->buf entschlüsseln, PKCS#7-Padding entfernen */
static int decrypt_segment(Hls *h, const Segment *s)
{
    if (!h->key_cache_uri || strcmp(h->key_cache_uri, s->key_uri)) {
        NetBuf kb;
        long status = 0;
        int r = net_request_ex(s->key_uri, NULL, h->headers, &kb, &status, NULL, 0, h->abort_flag);
        if (r != NET_OK || status >= 400 || kb.len != 16) {
            snprintf(h->err, sizeof h->err, "AES-Schluessel nicht ladbar (%s)",
                     r != NET_OK ? net_last_detail() : (kb.len != 16 ? "falsche Laenge" : "HTTP-Fehler"));
            net_buf_free(&kb);
            return -1;
        }
        memcpy(h->key_cache, kb.data, 16);
        net_buf_free(&kb);
        free(h->key_cache_uri);
        h->key_cache_uri = strdup(s->key_uri);
    }
    if (h->buf.len == 0 || h->buf.len % 16) {
        snprintf(h->err, sizeof h->err, "Verschluesseltes Segment hat ungueltige Laenge");
        return -1;
    }
    uint8_t iv[16];
    if (s->has_iv) {
        memcpy(iv, s->iv, 16);
    } else {
        memset(iv, 0, 16);
        for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)((uint64_t)s->seq >> (8 * i));
    }
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, h->key_cache, iv);
    AES_CBC_decrypt_buffer(&ctx, (uint8_t *)h->buf.data, h->buf.len);
    uint8_t pad = (uint8_t)h->buf.data[h->buf.len - 1];
    if (pad >= 1 && pad <= 16 && pad <= h->buf.len) h->buf.len -= pad;
    return 0;
}

/* Lädt das nächste Segment (oder das Init-Segment) in h->buf. 0 = ok, 1 = Ende, -1 = Fehler */
static int next_chunk(Hls *h)
{
    if (h->need_map && h->map_uri) {
        h->need_map = 0;
        return download(h, h->map_uri, h->map_off, h->map_len) == 0 ? 0 : -1;
    }
    for (;;) {
        if (aborted(h)) return -1;
        if (h->cur < h->nseg) {
            Segment *s = &h->seg[h->cur++];
            h->last_seq = s->seq;
            if (download(h, s->uri, s->br_off, s->br_len) == 0) {
                if (!s->key_uri || decrypt_segment(h, s) == 0) return 0;
                net_buf_free(&h->buf);
                if (!h->endlist) continue;
                return -1;
            }
            if (!h->endlist) continue;      /* Live: fehlendes Segment überspringen */
            return -1;
        }
        if (h->endlist) return 1;

        /* Live: Playlist neu laden, bis neue Segmente da sind */
        int wait_ms = (int)(h->target_duration * 500);
        if (wait_ms < 1000) wait_ms = 1000;
        for (int t = 0; t < wait_ms && !aborted(h); t += 100) sleep_ms(100);
        if (aborted(h)) return -1;
        int64_t want = h->last_seq + 1;
        int failures = 0;
        while (load_media(h) != 0) {
            if (aborted(h) || ++failures >= 5) return -1;
            sleep_ms(1000);
        }
        h->cur = h->nseg;
        for (int i = 0; i < h->nseg; i++)
            if (h->seg[i].seq >= want) { h->cur = i; break; }
        /* Zu weit zurückgefallen: Sequenz nicht mehr in der Liste -> am Ende weitermachen */
        if (h->nseg && h->seg[0].seq > want) h->cur = h->nseg > 3 ? h->nseg - 3 : 0;
    }
}

int hls_read(Hls *h, uint8_t *dst, int size)
{
    while (!h->buf.data || h->pos >= h->buf.len) {
        int r = next_chunk(h);
        if (r == 1) return 0;
        if (r < 0) return -1;
    }
    size_t n = h->buf.len - h->pos;
    if (n > (size_t)size) n = size;
    memcpy(dst, h->buf.data + h->pos, n);
    h->pos += n;
    return (int)n;
}

int hls_is_live(const Hls *h) { return !h->endlist; }

int64_t hls_duration_us(const Hls *h)
{
    if (!h->endlist) return 0;
    double d = 0;
    for (int i = 0; i < h->nseg; i++) d += h->seg[i].duration;
    return (int64_t)(d * 1e6);
}

int64_t hls_seek(Hls *h, int64_t time_us)
{
    if (!h->endlist) return -1;
    double t = 0, target = time_us / 1e6;
    int idx = 0;
    for (; idx < h->nseg - 1; idx++) {
        if (t + h->seg[idx].duration > target) break;
        t += h->seg[idx].duration;
    }
    h->cur = idx;
    h->need_map = h->map_uri != NULL;
    net_buf_free(&h->buf);
    h->pos = 0;
    return (int64_t)(t * 1e6);
}

const char *hls_info(const Hls *h)  { return h->info; }
const char *hls_audio_url(const Hls *h) { return h->audio_url; }
const char *hls_audio_lang(const Hls *h) { return h->audio_lang; }
int  hls_audio_tracks(const Hls *h, const HlsTrack **list, int *current)
{
    if (list) *list = h->atr;
    if (current) *current = h->cur_atr;
    return h->natr;
}
int  hls_subtitle_tracks(const Hls *h, const HlsTrack **list)
{
    if (list) *list = h->str;
    return h->nstr;
}
const char *hls_error(const Hls *h) { return h->err; }
