/* Programmführer (XMLTV) - siehe epg.h */
#include "epg.h"
#include "net.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <zlib.h>
#ifdef __vita__
#include <psp2/rtc.h>
#endif

#define WINDOW_BEFORE  3600
#define WINDOW_AFTER   (12 * 3600)
#define MAX_SHOWS      40000
#define MAX_CHANNELS   4000
#define CACHE_SECONDS  (6 * 3600)
#define CHUNK          65536

typedef struct { char *id, *name, *norm; } EpgChan;
typedef struct { int ch; int64_t start, stop; char *title; } EpgProg;

static struct {
    pthread_mutex_t m;
    pthread_t       t;
    int             started, loading, ready;
    char            url[1024];
    char            pending[1024];
    int64_t         fail_time;
    int64_t         loaded_at;              /* Zeitfenster gilt ab hier 12 h */              /* letzter Fehlschlag für url */          /* während des Ladens angefragte andere Adresse */
    char            status[160];
    /* fertige Daten (vom Lade-Thread vorbereitet, dann getauscht) */
    EpgChan        *ch;  int nch;
    EpgProg        *pr;  int npr;
} E = { .m = PTHREAD_MUTEX_INITIALIZER };

/* ---------------------------------------------------------------- Zeit */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t epg_parse_time(const char *s)
{
    int Y, M, D, h = 0, mi = 0, se = 0;
    if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &Y, &M, &D, &h, &mi, &se) < 3) return 0;
    int64_t t = days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + se;
    const char *z = s;
    while (*z && *z != ' ' && *z != '+' && *z != '-') z++;
    while (*z == ' ') z++;
    if (*z == '+' || *z == '-') {
        int off = atoi(z + 1);                  /* hhmm */
        int sec = (off / 100) * 3600 + (off % 100) * 60;
        t += (*z == '+') ? -sec : sec;          /* Ortszeit -> UTC */
    }
    return t;
}

int64_t epg_time_now(void) { return (int64_t)time(NULL); }

int epg_tz_offset(void)
{
#ifdef __vita__
    SceRtcTick utc, local;
    if (sceRtcGetCurrentTick(&utc) < 0) return 0;
    if (sceRtcConvertUtcToLocalTime(&utc, &local) < 0) return 0;
    return (int)(((int64_t)local.tick - (int64_t)utc.tick) / 1000000);
#else
    time_t now = time(NULL);
    struct tm lt = *localtime(&now), gt = *gmtime(&now);
    return (int)(days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60
               - (days_from_civil(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday) * 86400 + gt.tm_hour * 3600 + gt.tm_min * 60));
#endif
}

void epg_fmt_hm(int64_t t, char *out, int n)
{
    int64_t l = t + epg_tz_offset();
    int64_t sod = ((l % 86400) + 86400) % 86400;
    snprintf(out, n, "%02d:%02d", (int)(sod / 3600), (int)(sod / 60 % 60));
}

/* ---------------------------------------------------------------- Parser */

static void norm_name(const char *in, char *out, int n)
{
    /* angehängte Qualitätswörter ("HD", "FHD", "UHD", "4K", "SD") entfernen, dann nur Buchstaben/Ziffern */
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s", in);
    for (;;) {
        int l = (int)strlen(tmp);
        while (l > 0 && (isspace((unsigned char)tmp[l - 1]) || tmp[l - 1] == ')' || tmp[l - 1] == ']')) tmp[--l] = 0;
        int w = l;
        while (w > 0 && !isspace((unsigned char)tmp[w - 1]) && tmp[w - 1] != '(' && tmp[w - 1] != '[') w--;
        if (w == 0) break;
        const char *word = tmp + w;
        if (strcasecmp(word, "hd") && strcasecmp(word, "fhd") && strcasecmp(word, "uhd") && strcasecmp(word, "4k") && strcasecmp(word, "sd")) break;
        tmp[w] = 0;
        while (w > 0 && (tmp[w - 1] == '(' || tmp[w - 1] == '[')) tmp[--w] = 0;
    }
    int j = 0;
    for (const unsigned char *p = (const unsigned char *)tmp; *p && j < n - 1; p++)
        if (isalnum(*p)) out[j++] = (char)tolower(*p);
    out[j] = 0;
}

static int attr(const char *tag, const char *end, const char *name, char *out, int n)
{
    size_t nl = strlen(name);
    for (const char *p = tag + 1; p + nl + 2 < end; p++) {
        if (isspace((unsigned char)p[-1]) && !strncmp(p, name, nl) && p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            char q = p[nl + 1];
            const char *s = p + nl + 2, *e = memchr(s, q, (size_t)(end - s));
            if (!e) return 0;
            snprintf(out, n, "%.*s", (int)(e - s), s);
            return 1;
        }
    }
    return 0;
}

static void unescape(char *s)
{
    static const struct { const char *e; char c; } ent[] = { {"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},{"&apos;",'\''},{"&#39;",'\''} };
    char *o = s;
    for (char *p = s; *p; ) {
        int hit = 0;
        if (*p == '&')
            for (unsigned k = 0; k < sizeof ent / sizeof *ent; k++) {
                size_t l = strlen(ent[k].e);
                if (!strncmp(p, ent[k].e, l)) { *o++ = ent[k].c; p += l; hit = 1; break; }
            }
        if (!hit) *o++ = *p++;
    }
    *o = 0;
}

/* Text des ersten <tag ...>...</tag> innerhalb [s, e) */
static int inner(const char *s, const char *e, const char *tag, char *out, int n)
{
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = s;
    size_t ol = strlen(open);
    while ((a = strstr(a, open)) && a < e) {
        if (a[ol] == '>' || isspace((unsigned char)a[ol])) break;
        a += ol;
    }
    if (!a || a >= e) return 0;
    const char *gt = memchr(a, '>', (size_t)(e - a));
    if (!gt) return 0;
    const char *c = strstr(gt, close);
    if (!c || c > e) return 0;
    int len = (int)(c - gt - 1);
    if (len >= n) len = n - 1;
    memcpy(out, gt + 1, len);
    out[len] = 0;
    /* CDATA entfernen */
    if (len >= 9 && !strncmp(out, "<![CDATA[", 9)) {
        int k = 0;
        for (int i = 9; i <= len; i++) out[k++] = out[i];
        char *x = strstr(out, "]]>");
        if (x) *x = 0;
    }
    unescape(out);
    return 1;
}

typedef struct {
    EpgChan *ch; int nch, cch;
    EpgProg *pr; int npr, cpr;
    int64_t from, to;
} Build;

static int find_or_add_channel(Build *b, const char *id)
{
    for (int i = 0; i < b->nch; i++) if (!strcasecmp(b->ch[i].id, id)) return i;
    if (b->nch >= MAX_CHANNELS) return -1;
    if (b->nch == b->cch) {
        int nc = b->cch ? b->cch * 2 : 256;
        EpgChan *p = realloc(b->ch, nc * sizeof *p);
        if (!p) return -1;
        b->ch = p; b->cch = nc;
    }
    b->ch[b->nch].id = strdup(id);
    b->ch[b->nch].name = NULL;
    b->ch[b->nch].norm = NULL;
    return b->nch++;
}

static void handle_element(Build *b, const char *s, const char *e)
{
    char buf[256];
    if (!strncmp(s, "<channel", 8)) {
        char id[160];
        if (!attr(s, e, "id", id, sizeof id)) return;
        int c = find_or_add_channel(b, id);
        if (c < 0) return;
        if (inner(s, e, "display-name", buf, sizeof buf) && !b->ch[c].name) {
            b->ch[c].name = strdup(buf);
            char nn[160];
            norm_name(buf, nn, sizeof nn);
            b->ch[c].norm = strdup(nn);
        }
    } else if (!strncmp(s, "<programme", 10)) {
        char st[48], sp[48], chid[160];
        if (!attr(s, e, "start", st, sizeof st) || !attr(s, e, "channel", chid, sizeof chid)) return;
        int64_t a = epg_parse_time(st);
        int64_t z = attr(s, e, "stop", sp, sizeof sp) ? epg_parse_time(sp) : a + 1800;
        if (z < b->from || a > b->to || b->npr >= MAX_SHOWS) return;
        if (!inner(s, e, "title", buf, sizeof buf)) return;
        int c = find_or_add_channel(b, chid);
        if (c < 0) return;
        if (b->npr == b->cpr) {
            int nc = b->cpr ? b->cpr * 2 : 1024;
            EpgProg *p = realloc(b->pr, nc * sizeof *p);
            if (!p) return;
            b->pr = p; b->cpr = nc;
        }
        b->pr[b->npr].ch = c;
        b->pr[b->npr].start = a;
        b->pr[b->npr].stop = z;
        b->pr[b->npr].title = strdup(buf);
        b->npr++;
    }
}

static void free_data(EpgChan *ch, int nch, EpgProg *pr, int npr)
{
    for (int i = 0; i < nch; i++) { free(ch[i].id); free(ch[i].name); free(ch[i].norm); }
    for (int i = 0; i < npr; i++) free(pr[i].title);
    free(ch);
    free(pr);
}

static int cmp_prog(const void *x, const void *y)
{
    const EpgProg *a = x, *b = y;
    if (a->ch != b->ch) return a->ch - b->ch;
    return a->start < b->start ? -1 : a->start > b->start;
}

int epg_parse_file(const char *path, int64_t now)
{
    gzFile gz = gzopen(path, "rb");             /* liest gzip und Klartext */
    if (!gz) return -1;
    Build b;
    memset(&b, 0, sizeof b);
    b.from = now - WINDOW_BEFORE;
    b.to = now + WINDOW_AFTER;
    char *buf = malloc(CHUNK * 2 + 1);
    if (!buf) { gzclose(gz); return -1; }
    int have = 0;
    for (;;) {
        int r = gzread(gz, buf + have, CHUNK * 2 - have);
        if (r > 0) have += r;
        buf[have] = 0;
        char *p = buf;
        for (;;) {
            char *pc = strstr(p, "<channel"), *pp = strstr(p, "<programme");
            char *s = (pc && (!pp || pc < pp)) ? pc : pp;
            if (!s) { p = buf + (have > 16 ? have - 16 : 0); break; }        /* Rest behalten (Tag-Anfang) */
            const char *close = (s == pc) ? "</channel>" : "</programme>";
            char *e = strstr(s, close);
            if (!e) {
                char *sc = strchr(s, '>');
                if (sc && sc[-1] == '/') { handle_element(&b, s, sc + 1); p = sc + 1; continue; }   /* <channel ... /> */
                p = s;
                break;
            }
            e += strlen(close);
            handle_element(&b, s, e);
            p = e;
        }
        int rest = have - (int)(p - buf);
        if (rest >= CHUNK * 2 - 1) rest = 0;    /* einzelnes Element zu groß: verwerfen */
        memmove(buf, p, rest);
        have = rest;
        if (r <= 0) break;
    }
    free(buf);
    gzclose(gz);
    qsort(b.pr, b.npr, sizeof *b.pr, cmp_prog);
    pthread_mutex_lock(&E.m);
    free_data(E.ch, E.nch, E.pr, E.npr);
    E.ch = b.ch; E.nch = b.nch;
    E.pr = b.pr; E.npr = b.npr;
    E.ready = 1;
    E.loaded_at = now;
    snprintf(E.status, sizeof E.status, "%d Sendungen, %d Sender", b.npr, b.nch);
    pthread_mutex_unlock(&E.m);
    return b.npr;
}

/* ---------------------------------------------------------------- Laden */

static void set_status(const char *s)
{
    pthread_mutex_lock(&E.m);
    snprintf(E.status, sizeof E.status, "%s", s);
    pthread_mutex_unlock(&E.m);
}

static volatile int s_abort;
static void load_one(void);

static void *loader(void *arg)
{
    (void)arg;
    for (;;) {
        load_one();
        pthread_mutex_lock(&E.m);
        int again = E.pending[0] && strcmp(E.pending, E.url);
        if (again) snprintf(E.url, sizeof E.url, "%s", E.pending);
        E.pending[0] = 0;
        if (!again) E.loading = 0;
        pthread_mutex_unlock(&E.m);
        if (!again) break;
    }
    return NULL;
}

static void load_one(void)
{
    /* je Adresse eine Cache-Datei */
    uint32_t h = 2166136261u;
    for (const char *c = E.url; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    char path[300], meta_path[300];
    snprintf(path, sizeof path, VS_DATA_DIR "/epg_%08x.bin", (unsigned)h);
    snprintf(meta_path, sizeof meta_path, VS_DATA_DIR "/epg_%08x.url", (unsigned)h);
    E.fail_time = 0;
    /* Cache benutzen, wenn gleiche URL und jünger als 6 Stunden */
    int fresh = 0;
    struct stat st;
    FILE *mf = fopen(meta_path, "r");
    if (mf) {
        char u[1024] = "";
        if (fgets(u, sizeof u, mf)) {
            u[strcspn(u, "\r\n")] = 0;
            if (!strcmp(u, E.url) && stat(path, &st) == 0 && (int64_t)time(NULL) - (int64_t)st.st_mtime < CACHE_SECONDS) fresh = 1;
        }
        fclose(mf);
    }
    if (!fresh) {
        set_status("Programmfuehrer wird geladen ...");
        volatile int64_t done = 0, total = 0;
        int r = net_download(E.url, NULL, path, &s_abort, &done, &total);
        if (r != NET_OK) {
            char m[160];
            snprintf(m, sizeof m, "Programmfuehrer nicht ladbar: %s", net_last_detail());
            set_status(m);
            E.fail_time = epg_time_now();
            return;
        }
        mf = fopen(meta_path, "w");
        if (mf) { fprintf(mf, "%s\n", E.url); fclose(mf); }
    }
    set_status("Programmfuehrer wird gelesen ...");
    if (epg_parse_file(path, epg_time_now()) < 0) { set_status("Programmfuehrer nicht lesbar"); E.fail_time = epg_time_now(); }
}

static char s_default[1024];
void epg_set_default(const char *url) { snprintf(s_default, sizeof s_default, "%s", url ? url : ""); }

void epg_load(const char *url)
{
    if (!url || !*url) url = s_default;
    if (!*url) return;
    pthread_mutex_lock(&E.m);
    int same = !strcmp(E.url, url) && (E.loading || (E.ready && epg_time_now() - E.loaded_at < 6 * 3600) || (E.fail_time && epg_time_now() - E.fail_time < 600));
    int busy = E.loading;
    if (busy && !same) snprintf(E.pending, sizeof E.pending, "%s", url);   /* nach dem aktuellen laden */
    if (busy && same) E.pending[0] = 0;
    pthread_mutex_unlock(&E.m);
    if (same || busy) return;
    if (E.started) { pthread_join(E.t, NULL); E.started = 0; }
    snprintf(E.url, sizeof E.url, "%s", url);
    E.loading = 1;
    if (pthread_create(&E.t, NULL, loader, NULL) == 0) E.started = 1;
    else E.loading = 0;
}

const char *epg_status(void) { return E.status; }
int epg_ready(void) { return E.ready; }

int epg_now(const char *tvg_id, const char *name, int64_t now, EpgShow out[2])
{
    int n = 0;
    pthread_mutex_lock(&E.m);
    int c = -1;
    if (tvg_id && *tvg_id)
        for (int i = 0; i < E.nch; i++) if (!strcasecmp(E.ch[i].id, tvg_id)) { c = i; break; }
    if (c < 0 && tvg_id && *tvg_id) {
        /* iptv-org: "DasErste.de@HD" <-> "DasErste.de" */
        char a[160];
        snprintf(a, sizeof a, "%s", tvg_id);
        char *at = strchr(a, '@');
        if (at) *at = 0;
        size_t al = strlen(a);
        for (int i = 0; i < E.nch && al; i++) {
            const char *b = E.ch[i].id;
            if (!strncasecmp(b, a, al) && (b[al] == 0 || b[al] == '@')) { c = i; break; }
        }
    }
    if (c < 0 && name && *name) {
        char nn[160];
        norm_name(name, nn, sizeof nn);
        if (nn[0])
            for (int i = 0; i < E.nch; i++) if (E.ch[i].norm && !strcmp(E.ch[i].norm, nn)) { c = i; break; }
    }
    if (c >= 0) {
        /* Sendungen sind nach Sender und Beginn sortiert: binäre Suche des Senders */
        int lo = 0, hi = E.npr;
        while (lo < hi) { int mid = (lo + hi) / 2; if (E.pr[mid].ch < c) lo = mid + 1; else hi = mid; }
        for (int i = lo; i < E.npr && E.pr[i].ch == c && n < 2; i++) {
            if (E.pr[i].stop <= now) continue;
            if (n == 0 && E.pr[i].start > now) {
                /* gerade nichts: "jetzt" leer lassen, nächste als "danach" */
                out[0].title[0] = 0; out[0].start = out[0].stop = 0;
                n = 1;
            }
            snprintf(out[n].title, sizeof out[n].title, "%s", E.pr[i].title);
            out[n].start = E.pr[i].start;
            out[n].stop = E.pr[i].stop;
            n++;
        }
    }
    pthread_mutex_unlock(&E.m);
    return n;
}

void epg_shutdown(void)
{
    s_abort = 1;
    if (E.started) { pthread_join(E.t, NULL); E.started = 0; }
    pthread_mutex_lock(&E.m);
    free_data(E.ch, E.nch, E.pr, E.npr);
    E.ch = NULL; E.pr = NULL; E.nch = E.npr = 0; E.ready = 0;
    pthread_mutex_unlock(&E.m);
}
