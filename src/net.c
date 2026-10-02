#include "net.h"
#include "config.h"
#include "adblock.h"
#include "dns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <curl/curl.h>

#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/stat.h>
#endif

NetStats g_net_stats;

/* Fehlerdetails pro Thread (Plugin-Worker und Player laufen parallel) */
static pthread_key_t  s_err_key;
static pthread_once_t s_err_once = PTHREAD_ONCE_INIT;
static void err_key_init(void) { pthread_key_create(&s_err_key, free); }

static void set_detail(const char *fmt, const char *a, long b)
{
    pthread_once(&s_err_once, err_key_init);
    char *buf = pthread_getspecific(s_err_key);
    if (!buf) { buf = malloc(200); pthread_setspecific(s_err_key, buf); }
    if (buf) snprintf(buf, 200, fmt, a, b);
}

const char *net_last_detail(void)
{
    pthread_once(&s_err_once, err_key_init);
    const char *buf = pthread_getspecific(s_err_key);
    return buf ? buf : "";
}

#define CA_FILE   VS_DATA_DIR "/cacert.pem"
#define MAX_REDIR 8

static int  s_have_ca;
static void *s_net_mem;

int net_init(void)
{
#ifdef __vita__
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    int r = sceNetShowNetstat();
    if (r == (int)SCE_NET_ERROR_ENOTINIT) {
        const int size = 1 * 1024 * 1024;
        s_net_mem = malloc(size);
        SceNetInitParam p = { s_net_mem, size, 0 };
        if (sceNetInit(&p) < 0) return -1;
    }
    sceNetCtlInit();
    SceIoStat st;
    s_have_ca = sceIoGetstat(CA_FILE, &st) >= 0;
#else
    FILE *f = fopen(CA_FILE, "r");
    if (f) { s_have_ca = 1; fclose(f); }
#endif
    curl_global_init(CURL_GLOBAL_ALL);
    return 0;
}

void net_term(void)
{
    curl_global_cleanup();
#ifdef __vita__
    sceNetCtlTerm();
    sceNetTerm();
    free(s_net_mem);
    s_net_mem = NULL;
#endif
}

int net_online(void)
{
#ifdef __vita__
    int state = 0;
    sceNetCtlInetGetState(&state);
    return state == SCE_NETCTL_STATE_CONNECTED;
#else
    return 1;
#endif
}

const char *net_strerror(int code)
{
    switch (code) {
    case NET_OK:       return "OK";
    case NET_BLOCKED:  return "Durch AdBlock gesperrt";
    case NET_DNS_FAIL: return "DNS-Aufloesung fehlgeschlagen";
    case NET_TLS:      return "TLS-Zertifikat ungueltig (cacert.pem ablegen oder ssl_verify=0)";
    case NET_ABORTED:  return "Abgebrochen";
    default:           return "Netzwerkfehler";
    }
}

void net_buf_free(NetBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
}

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    NetBuf *b = ud;
    size_t n = sz * nm;
    char *d = realloc(b->data, b->len + n + 1);
    if (!d) return 0;
    b->data = d;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

static int xferinfo_cb(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    const volatile int *abort_flag = ud;
    return (abort_flag && *abort_flag) ? 1 : 0;
}

static void set_abort(CURL *c, const volatile int *abort_flag)
{
    if (!abort_flag) return;
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)abort_flag);
}

static int url_port(const char *url)
{
    int def = !strncmp(url, "https://", 8) ? 443 : 80;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    if (*p == '[') { p = strchr(p, ']'); if (!p) return def; p++; }
    else while (*p && *p != ':' && *p != '/' && *p != '?') p++;
    if (*p == ':') return atoi(p + 1) ? atoi(p + 1) : def;
    return def;
}

static void note_blocked(const char *host)
{
    g_net_stats.blocked++;
    snprintf(g_net_stats.last_blocked, sizeof g_net_stats.last_blocked, "%.120s", host);
}

/* Prüft Blockliste und löst bei Bedarf über eigenen DNS auf.
 * resolve_entry bekommt "host:port:ip" für CURLOPT_RESOLVE (leer, wenn System-DNS). */
static int prepare_host(const char *url, char *resolve_entry, int len)
{
    char host[256];
    resolve_entry[0] = 0;
    if (!adblock_host_from_url(url, host, sizeof host)) return NET_ERR;

    if (g_cfg.adblock_enabled && adblock_is_blocked(host)) {
        set_detail("%s (Blockliste)", host, 0);
        note_blocked(host);
        return NET_BLOCKED;
    }
    if (g_cfg.custom_dns_enabled) {
        char ip[16];
        int r = dns_resolve2(g_cfg.dns_primary, g_cfg.dns_secondary, host, ip, sizeof ip, 3000);
        if (r == DNS_BLOCKED) { set_detail("%s (DNS-Filter)", host, 0); note_blocked(host); return NET_BLOCKED; }
        if (r == DNS_OK) {
            if (strcmp(ip, host)) snprintf(resolve_entry, len, "%s:%d:%s", host, url_port(url), ip);
        } else if (r == DNS_ERR_NXDOM || r == DNS_ERR_NOA) {
            set_detail("%s nicht gefunden", host, 0);
            return NET_DNS_FAIL;
        }
        /* DNS_ERR_NET: eigener DNS nicht erreichbar -> System-DNS als Rückfall */
    }
    return NET_OK;
}

int net_check_url(const char *url)
{
    char tmp[300];
    return prepare_host(url, tmp, sizeof tmp);
}

static struct curl_slist *build_headers(const char *headers)
{
    struct curl_slist *list = NULL;
    if (!headers) return NULL;
    char *copy = strdup(headers), *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\r') line++;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        if (*line) list = curl_slist_append(list, line);
    }
    free(copy);
    return list;
}

static void common_opts(CURL *c, const char *url)
{
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, g_cfg.user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);   /* Weiterleitungen selbst prüfen */
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)g_cfg.timeout_sec);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* Standard: Zertifikatsliste der Vita (vs0:data/external/cert/CA_LIST.cer, im
       curl-Paket voreingestellt). Eine eigene cacert.pem hat Vorrang. */
    if (s_have_ca) curl_easy_setopt(c, CURLOPT_CAINFO, CA_FILE);
    if (!g_cfg.ssl_verify) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

int net_request(const char *url_in, const char *post_body, const char *headers,
                NetBuf *out, long *status, char *final_url, int final_len)
{
    return net_request_ex(url_in, post_body, headers, out, status, final_url, final_len, NULL);
}

int net_request_ex(const char *url_in, const char *post_body, const char *headers,
                   NetBuf *out, long *status, char *final_url, int final_len,
                   const volatile int *abort_flag)
{
    memset(out, 0, sizeof *out);
    if (status) *status = 0;

    char *url = strdup(url_in);
    int result = NET_ERR;

    for (int hop = 0; hop <= MAX_REDIR; hop++) {
        char resolve[300];
        int pr = prepare_host(url, resolve, sizeof resolve);
        if (pr != NET_OK) { result = pr; break; }

        CURL *c = curl_easy_init();
        if (!c) break;
        common_opts(c, url);

        struct curl_slist *rl = NULL, *hl = build_headers(headers);
        if (resolve[0]) {
            rl = curl_slist_append(NULL, resolve);
            curl_easy_setopt(c, CURLOPT_RESOLVE, rl);
        }
        if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
        if (post_body && hop == 0) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
        }

        net_buf_free(out);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
        set_abort(c, abort_flag);

        g_net_stats.requests++;
        CURLcode cr = curl_easy_perform(c);
        long code = 0;
        char *redir = NULL;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &redir);
        char *next = (cr == CURLE_OK && code >= 300 && code < 400 && redir) ? strdup(redir) : NULL;

        curl_slist_free_all(rl);
        curl_slist_free_all(hl);
        curl_easy_cleanup(c);

        if (cr != CURLE_OK) set_detail("%s (curl %ld)", curl_easy_strerror(cr), (long)cr);
        if (cr == CURLE_ABORTED_BY_CALLBACK) { result = NET_ABORTED; break; }
        if (cr == CURLE_PEER_FAILED_VERIFICATION || cr == CURLE_SSL_CACERT_BADFILE) { result = NET_TLS; break; }
        if (cr != CURLE_OK) { result = NET_ERR; break; }
        if (status) *status = code;
        if (next) {
            free(url);
            url = next;
            post_body = NULL;   /* nach 30x wird per GET weitergemacht */
            continue;
        }
        result = NET_OK;
        break;
    }

    if (final_url) snprintf(final_url, final_len, "%s", url);
    free(url);
    if (result != NET_OK) net_buf_free(out);
    return result;
}

/* ---------------- Bereichs-Stream ---------------- */

#define WINDOW (1024 * 1024)

struct NetStream {
    char           *url;
    char           *headers;
    char            resolve[300];
    uint64_t        size;
    uint64_t        win_off;
    NetBuf          win;
    CURL           *curl;
    pthread_mutex_t lock;
    volatile int    abort;
};

void net_stream_abort(NetStream *s) { if (s) s->abort = 1; }

typedef struct { uint64_t total; } HdrInfo;

static size_t header_cb(char *b, size_t sz, size_t nm, void *ud)
{
    size_t n = sz * nm;
    HdrInfo *hi = ud;
    if (n > 14 && !strncasecmp(b, "Content-Range:", 14)) {
        const char *slash = memchr(b, '/', n);
        if (slash && slash[1] != '*') hi->total = strtoull(slash + 1, NULL, 10);
    }
    return n;
}

static int stream_fetch(NetStream *s, uint64_t off, uint64_t want, NetBuf *out, HdrInfo *hi)
{
    char range[64];
    snprintf(range, sizeof range, "%llu-%llu",
             (unsigned long long)off, (unsigned long long)(off + want - 1));

    CURL *c = s->curl;
    curl_easy_reset(c);
    common_opts(c, s->url);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, NULL);   /* Byte-Offsets müssen roh sein */
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    set_abort(c, &s->abort);

    struct curl_slist *rl = NULL, *hl = build_headers(s->headers);
    if (s->resolve[0]) { rl = curl_slist_append(NULL, s->resolve); curl_easy_setopt(c, CURLOPT_RESOLVE, rl); }
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);

    memset(out, 0, sizeof *out);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    if (hi) {
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, hi);
    }
    CURLcode cr = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(rl);
    curl_slist_free_all(hl);
    g_net_stats.requests++;

    if (cr != CURLE_OK || (code != 206 && code != 200)) {
        if (cr != CURLE_OK) set_detail("%s (curl %ld)", curl_easy_strerror(cr), (long)cr);
        else                set_detail("HTTP %s%ld", "", code);
        net_buf_free(out);
        return NET_ERR;
    }
    if (code == 200 && off > 0) { net_buf_free(out); return NET_ERR; } /* Server kann keine Ranges */
    return NET_OK;
}

NetStream *net_stream_open(const char *url_in, const char *headers)
{
    /* Weiterleitungen vorab auflösen, damit Range-Anfragen direkt ans Ziel gehen */
    char final_url[1024];
    NetBuf probe;
    long st;
    char hdr2[1024];
    snprintf(hdr2, sizeof hdr2, "%s%sRange: bytes=0-0", headers ? headers : "", headers ? "\n" : "");
    int r = net_request(url_in, NULL, hdr2, &probe, &st, final_url, sizeof final_url);
    net_buf_free(&probe);
    if (r != NET_OK) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s", net_strerror(r));
        if (!*net_last_detail()) set_detail("%s", msg, 0);
        return NULL;
    }
    if (st >= 400) { set_detail("HTTP %s%ld", "", st); return NULL; }

    NetStream *s = calloc(1, sizeof *s);
    s->url = strdup(final_url);
    s->headers = headers ? strdup(headers) : NULL;
    if (prepare_host(s->url, s->resolve, sizeof s->resolve) != NET_OK) goto fail;
    s->curl = curl_easy_init();
    if (!s->curl) goto fail;
    pthread_mutex_init(&s->lock, NULL);

    HdrInfo hi = { 0 };
    if (stream_fetch(s, 0, WINDOW, &s->win, &hi) != NET_OK) goto fail_lock;
    s->win_off = 0;
    s->size = hi.total ? hi.total : s->win.len;   /* 200 ohne Range: ganze Datei geladen */
    return s;

fail_lock:
    pthread_mutex_destroy(&s->lock);
fail:
    if (s->curl) curl_easy_cleanup(s->curl);
    free(s->url);
    free(s->headers);
    free(s);
    return NULL;
}

uint64_t net_stream_size(NetStream *s) { return s ? s->size : 0; }

int net_stream_read(NetStream *s, uint64_t offset, void *buf, uint32_t len)
{
    if (!s || offset >= s->size) return 0;
    if (offset + len > s->size) len = (uint32_t)(s->size - offset);

    pthread_mutex_lock(&s->lock);
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        if (pos >= s->win_off && pos < s->win_off + s->win.len) {
            uint64_t avail = s->win_off + s->win.len - pos;
            uint32_t n = (uint32_t)((len - done) < avail ? (len - done) : avail);
            memcpy((char *)buf + done, s->win.data + (pos - s->win_off), n);
            done += n;
            continue;
        }
        net_buf_free(&s->win);
        uint64_t want = (len - done) > WINDOW ? (len - done) : WINDOW;
        if (pos + want > s->size) want = s->size - pos;
        int ok = NET_ERR;
        for (int attempt = 0; attempt < 3 && ok != NET_OK && !s->abort; attempt++)
            ok = stream_fetch(s, pos, want, &s->win, NULL);
        if (ok != NET_OK || s->win.len == 0) break;
        s->win_off = pos;
    }
    pthread_mutex_unlock(&s->lock);
    return (int)done;
}

void net_stream_close(NetStream *s)
{
    if (!s) return;
    curl_easy_cleanup(s->curl);
    net_buf_free(&s->win);
    pthread_mutex_destroy(&s->lock);
    free(s->url);
    free(s->headers);
    free(s);
}
