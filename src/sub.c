/* Untertitel: WebVTT / SRT / TTML laden und zur Position liefern */
#include "sub.h"
#include "net.h"
#include "hls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <pthread.h>

static struct {
    pthread_mutex_t m;
    pthread_t       t;
    int             started;
    volatile int    abort;
    SubCue         *cues;
    int             n;
    char            status[160];
    char           *url, *headers;
    int64_t         origin;
    char            cur[512];
} S = { .m = PTHREAD_MUTEX_INITIALIZER };

/* ---------------------------------------------------------------- Text */

static const char *find_in(const char *p, const char *end, const char *needle)
{
    size_t nl = strlen(needle);
    for (; p + nl <= end; p++) if (!memcmp(p, needle, nl)) return p;
    return NULL;
}

static void add_cue(SubCue **cues, int *n, int *cap, int64_t a, int64_t b, const char *txt, size_t len)
{
    if (b <= a || len == 0) return;
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 256;
        SubCue *p = realloc(*cues, nc * sizeof *p);
        if (!p) return;
        *cues = p;
        *cap = nc;
    }
    /* Tags entfernen, <br> -> Zeilenumbruch, Entities */
    char *o = malloc(len + 1);
    if (!o) return;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (txt[i] == '<') {
            const char *e = memchr(txt + i, '>', len - i);
            if (!e) break;
            if (!strncasecmp(txt + i, "<br", 3) && j && o[j - 1] != '\n') o[j++] = '\n';
            i = e - txt;
            continue;
        }
        if (txt[i] == '&') {
            static const struct { const char *e; char c; } ent[] = {
                {"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},{"&apos;",'\''},{"&#39;",'\''},{"&nbsp;",' '} };
            int hit = 0;
            for (unsigned k = 0; k < sizeof ent / sizeof *ent; k++) {
                size_t el = strlen(ent[k].e);
                if (i + el <= len && !strncmp(txt + i, ent[k].e, el)) { o[j++] = ent[k].c; i += el - 1; hit = 1; break; }
            }
            if (hit) continue;
        }
        char c = txt[i];
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c == '\n' && (j == 0 || o[j - 1] == '\n')) continue;
        if (c == ' ' && (j == 0 || o[j - 1] == ' ' || o[j - 1] == '\n')) continue;
        o[j++] = c;
    }
    while (j && (o[j - 1] == '\n' || o[j - 1] == ' ')) j--;
    o[j] = 0;
    if (!j) { free(o); return; }
    (*cues)[*n].start = a;
    (*cues)[*n].end = b;
    (*cues)[*n].text = o;
    (*n)++;
}

/* "01:02:03.456", "02:03.456", "01:02:03,456" (SRT) */
static int parse_clock(const char *p, int64_t *ms, const char **endp)
{
    int h = 0, m = 0, s = 0, f = 0, fd = 0;
    const char *q = p;
    int parts[3], np = 0;
    while (np < 3 && isdigit((unsigned char)*q)) {
        int v = 0;
        while (isdigit((unsigned char)*q)) v = v * 10 + (*q++ - '0');
        parts[np++] = v;
        if (*q == ':') q++; else break;
    }
    if (np < 2) return 0;
    if (*q == '.' || *q == ',') {
        q++;
        while (isdigit((unsigned char)*q)) { if (fd < 3) { f = f * 10 + (*q - '0'); fd++; } q++; }
        while (fd < 3) { f *= 10; fd++; }
    }
    if (np == 3) { h = parts[0]; m = parts[1]; s = parts[2]; } else { m = parts[0]; s = parts[1]; }
    *ms = ((int64_t)h * 3600 + m * 60 + s) * 1000 + f;
    if (endp) *endp = q;
    return 1;
}

static int parse_vtt(const char *text, int64_t origin_ms, SubCue **cues, int *n, int *cap)
{
    int64_t offset = 0;
    const char *map = strstr(text, "X-TIMESTAMP-MAP=");
    if (map) {
        const char *mp = strstr(map, "MPEGTS:"), *lo = strstr(map, "LOCAL:");
        int64_t local = 0;
        if (lo) parse_clock(lo + 6, &local, NULL);
        if (mp) offset = strtoll(mp + 7, NULL, 10) / 90 - local - origin_ms;
    }
    int before = *n;
    const char *p = text;
    while (*p) {
        const char *eol = p + strcspn(p, "\n");
        const char *arrow = find_in(p, eol, "-->");
        if (arrow) {
            int64_t a, b;
            const char *s = p;
            while (s < arrow && !isdigit((unsigned char)*s)) s++;
            const char *t = arrow + 3;
            while (*t == ' ') t++;
            if (parse_clock(s, &a, NULL) && parse_clock(t, &b, NULL)) {
                /* Text bis zur Leerzeile */
                const char *body = *eol ? eol + 1 : eol;
                const char *end = body;
                while (*end) {
                    const char *le = end + strcspn(end, "\n");
                    const char *x = end;
                    while (x < le && (*x == ' ' || *x == '\r')) x++;
                    if (x == le) break;
                    end = *le ? le + 1 : le;
                }
                add_cue(cues, n, cap, a + offset, b + offset, body, end - body);
                p = end;
                continue;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    return *n - before;
}

/* TTML-Zeit: "00:00:01.000", "00:00:01:12" (Frames, 25 fps), "12.5s", "1500ms", "123t" */
static int64_t ttml_time(const char *v, double tick_rate)
{
    size_t l = strlen(v);
    if (l > 1 && v[l - 1] == 't') return (int64_t)(atof(v) * 1000.0 / tick_rate);
    if (l > 2 && !strcmp(v + l - 2, "ms")) return (int64_t)atof(v);
    if (l > 1 && v[l - 1] == 's' && !strchr(v, ':')) return (int64_t)(atof(v) * 1000.0);
    int h = 0, m = 0, s = 0, fr = 0;
    double sec = 0;
    if (sscanf(v, "%d:%d:%d:%d", &h, &m, &s, &fr) == 4) return ((int64_t)h * 3600 + m * 60 + s) * 1000 + fr * 40;
    if (sscanf(v, "%d:%d:%lf", &h, &m, &sec) == 3) return ((int64_t)h * 3600 + m * 60) * 1000 + (int64_t)(sec * 1000 + 0.5);
    return -1;
}

static int attr_val(const char *tag, size_t tl, const char *name, char *out, int on)
{
    size_t nl = strlen(name);
    for (const char *p = tag; p + nl + 2 < tag + tl; p++) {
        if ((p == tag || isspace((unsigned char)p[-1]) || p[-1] == ':') && !strncmp(p, name, nl) && p[nl] == '=') {
            char q = p[nl + 1];
            if (q != '"' && q != '\'') continue;
            const char *s = p + nl + 2, *e = memchr(s, q, tag + tl - s);
            if (!e) return 0;
            snprintf(out, on, "%.*s", (int)(e - s), s);
            return 1;
        }
    }
    return 0;
}

static int parse_ttml(const char *text, SubCue **cues, int *n, int *cap)
{
    double tick_rate = 10000000.0;
    const char *tr = strstr(text, "tickRate=\"");
    if (tr) tick_rate = atof(tr + 10);
    if (tick_rate <= 0) tick_rate = 10000000.0;
    int before = *n;
    const char *p = text;
    while ((p = strstr(p, "<p")) != NULL) {
        if (p[2] != ' ' && p[2] != '>' && p[2] != '\n' && p[2] != '\t') { p += 2; continue; }
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char b[48] = "", e[48] = "", d[48] = "";
        attr_val(p, gt - p, "begin", b, sizeof b);
        attr_val(p, gt - p, "end", e, sizeof e);
        attr_val(p, gt - p, "dur", d, sizeof d);
        const char *close = strstr(gt, "</p>");
        if (!close) break;
        if (b[0] && (e[0] || d[0])) {
            int64_t a = ttml_time(b, tick_rate);
            int64_t z = e[0] ? ttml_time(e, tick_rate) : a + ttml_time(d, tick_rate);
            if (a >= 0 && z > a) add_cue(cues, n, cap, a, z, gt + 1, close - gt - 1);
        }
        p = close + 4;
    }
    /* ZDF & Co. beginnen bei 10:00:00 - auf 0 bringen */
    int64_t minv = INT64_MAX;
    for (int i = before; i < *n; i++) if ((*cues)[i].start < minv) minv = (*cues)[i].start;
    if (*n > before && minv >= 36000000) {
        for (int i = before; i < *n; i++) { (*cues)[i].start -= 36000000; (*cues)[i].end -= 36000000; }
    }
    return *n - before;
}

int sub_parse_into(const char *text, int64_t origin_ms, SubCue **cues, int *n, int *cap)
{
    const char *t = text + strspn(text, " \r\n\t\xEF\xBB\xBF");
    if (strstr(t, "<tt") && (strstr(t, "<p ") || strstr(t, "<p>"))) return parse_ttml(t, cues, n, cap);
    return parse_vtt(t, origin_ms, cues, n, cap);   /* WebVTT und SRT */
}

void sub_free_cues(SubCue *cues, int n)
{
    for (int i = 0; i < n; i++) free(cues[i].text);
    free(cues);
}

static int cmp_cue(const void *a, const void *b)
{
    int64_t x = ((const SubCue *)a)->start, y = ((const SubCue *)b)->start;
    return x < y ? -1 : x > y;
}

/* ---------------------------------------------------------------- Laden */

static void set_status(const char *s)
{
    pthread_mutex_lock(&S.m);
    snprintf(S.status, sizeof S.status, "%s", s);
    pthread_mutex_unlock(&S.m);
}

static char *fetch(const char *url, char *final_url, int fl)
{
    NetBuf b;
    long status = 0;
    if (net_request(url, NULL, S.headers, &b, &status, final_url, fl) != NET_OK) return NULL;
    if (status >= 400 || !b.data) { net_buf_free(&b); return NULL; }
    char *t = malloc(b.len + 1);
    if (t) { memcpy(t, b.data, b.len); t[b.len] = 0; }
    net_buf_free(&b);
    return t;
}

static void *loader(void *arg)
{
    (void)arg;
    SubCue *cues = NULL;
    int n = 0, cap = 0;
    char final[1024];
    char *text = fetch(S.url, final, sizeof final);
    if (!text) { set_status("Untertitel konnten nicht geladen werden"); return NULL; }

    if (!strncmp(text + strspn(text, " \r\n\xEF\xBB\xBF"), "#EXTM3U", 7)) {
        /* HLS-Untertitel: alle Segmente laden */
        int segs = 0, total = 0;
        for (const char *p = text; *p; ) {
            const char *e = p + strcspn(p, "\r\n");
            if (e > p && *p != '#') total++;
            p = e + strspn(e, "\r\n");
        }
        for (const char *p = text; *p && !S.abort; ) {
            const char *e = p + strcspn(p, "\r\n");
            if (e > p && *p != '#') {
                char ref[1024], url[1024];
                snprintf(ref, sizeof ref, "%.*s", (int)(e - p), p);
                hls_join_url(final, ref, url, sizeof url);
                char segfinal[1024];
                char *seg = fetch(url, segfinal, sizeof segfinal);
                if (seg) { sub_parse_into(seg, S.origin, &cues, &n, &cap); free(seg); }
                segs++;
                if (segs % 10 == 0) {
                    char st[80];
                    snprintf(st, sizeof st, "Untertitel werden geladen ... %d/%d", segs, total);
                    set_status(st);
                }
            }
            p = e + strspn(e, "\r\n");
        }
    } else {
        sub_parse_into(text, 0, &cues, &n, &cap);
    }
    free(text);
    if (S.abort) { sub_free_cues(cues, n); return NULL; }
    if (n > 1) qsort(cues, n, sizeof *cues, cmp_cue);
    pthread_mutex_lock(&S.m);
    S.cues = cues;
    S.n = n;
    snprintf(S.status, sizeof S.status, "%s", n ? "" : "Keine Untertitel im Format gefunden");
    pthread_mutex_unlock(&S.m);
    return NULL;
}

void sub_open(const char *url, const char *headers, int64_t origin_ms)
{
    sub_close();
    S.abort = 0;
    S.url = strdup(url);
    S.headers = headers ? strdup(headers) : NULL;
    S.origin = origin_ms;
    snprintf(S.status, sizeof S.status, "Untertitel werden geladen ...");
    if (pthread_create(&S.t, NULL, loader, NULL) == 0) S.started = 1;
    else snprintf(S.status, sizeof S.status, "Untertitel: Thread-Fehler");
}

void sub_close(void)
{
    if (S.started) {
        S.abort = 1;
        pthread_join(S.t, NULL);
        S.started = 0;
    }
    pthread_mutex_lock(&S.m);
    sub_free_cues(S.cues, S.n);
    S.cues = NULL;
    S.n = 0;
    S.status[0] = 0;
    pthread_mutex_unlock(&S.m);
    free(S.url); S.url = NULL;
    free(S.headers); S.headers = NULL;
}

int sub_active(void) { return S.started; }

const char *sub_status(void) { return S.status; }

const char *sub_text_at(int64_t pos)
{
    const char *res = NULL;
    pthread_mutex_lock(&S.m);
    if (S.n) {
        /* erster Eintrag mit start > pos, dann rückwärts nach überlappenden suchen */
        int lo = 0, hi = S.n;
        while (lo < hi) { int mid = (lo + hi) / 2; if (S.cues[mid].start <= pos) lo = mid + 1; else hi = mid; }
        S.cur[0] = 0;
        int found = 0;
        for (int i = lo - 1; i >= 0 && i >= lo - 8; i--) {
            if (S.cues[i].start <= pos && pos < S.cues[i].end) {
                size_t l = strlen(S.cur);
                snprintf(S.cur + l, sizeof S.cur - l, "%s%s", found ? "\n" : "", S.cues[i].text);
                if (++found == 2) break;
            }
        }
        if (found) res = S.cur;
    }
    pthread_mutex_unlock(&S.m);
    return res;
}
