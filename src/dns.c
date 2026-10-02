#include "dns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef DNS_PORT
#define DNS_PORT 53   /* für Tests überschreibbar */
#endif

/* ---------- Cache ---------- */

#define CACHE_SIZE 64

typedef struct {
    char   host[128];
    char   ip[16];
    int    result;    /* DNS_OK oder DNS_BLOCKED/NXDOM (Negativ-Cache) */
    time_t expires;
} CacheEntry;

static CacheEntry s_cache[CACHE_SIZE];
static int        s_cache_next;

void dns_cache_clear(void) { memset(s_cache, 0, sizeof s_cache); s_cache_next = 0; }

static CacheEntry *cache_find(const char *host)
{
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_SIZE; i++)
        if (s_cache[i].host[0] && s_cache[i].expires > now && !strcmp(s_cache[i].host, host))
            return &s_cache[i];
    return NULL;
}

static void cache_put(const char *host, const char *ip, int result, unsigned ttl)
{
    if (strlen(host) >= sizeof s_cache[0].host) return;
    if (ttl < 30) ttl = 30;
    if (ttl > 3600) ttl = 3600;
    CacheEntry *e = &s_cache[s_cache_next];
    s_cache_next = (s_cache_next + 1) % CACHE_SIZE;
    strcpy(e->host, host);
    snprintf(e->ip, sizeof e->ip, "%s", ip ? ip : "");
    e->result = result;
    e->expires = time(NULL) + ttl;
}

/* ---------- Paketaufbau ---------- */

static int encode_name(const char *host, unsigned char *out, int max)
{
    int pos = 0;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len == 0 || len > 63 || pos + len + 2 > max) return -1;
        out[pos++] = (unsigned char)len;
        memcpy(out + pos, p, len);
        pos += len;
        p += len;
        if (*p == '.') p++;
    }
    if (pos + 1 > max) return -1;
    out[pos++] = 0;
    return pos;
}

/* Überspringt einen (ggf. komprimierten) Namen; gibt neue Position zurück. */
static int skip_name(const unsigned char *buf, int len, int pos)
{
    while (pos < len) {
        unsigned char c = buf[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return pos + 2;
        pos += c + 1;
    }
    return -1;
}

static int is_ipv4_literal(const char *h)
{
    struct in_addr a;
    return inet_pton(AF_INET, h, &a) == 1;
}

int dns_resolve(const char *server, const char *host_in, char *ip_out, int ip_len, int timeout_ms)
{
    char host[256];
    snprintf(host, sizeof host, "%s", host_in);
    for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);
    size_t hl = strlen(host);
    if (hl && host[hl - 1] == '.') host[--hl] = 0;

    if (is_ipv4_literal(host)) {
        snprintf(ip_out, ip_len, "%s", host);
        return DNS_OK;
    }

    CacheEntry *ce = cache_find(host);
    if (ce) {
        if (ce->result == DNS_OK) snprintf(ip_out, ip_len, "%s", ce->ip);
        return ce->result;
    }

    unsigned char pkt[512];
    unsigned short id = (unsigned short)(rand() & 0xFFFF);
    memset(pkt, 0, 12);
    pkt[0] = id >> 8; pkt[1] = id & 0xFF;
    pkt[2] = 0x01;              /* RD: Rekursion gewünscht */
    pkt[5] = 1;                 /* QDCOUNT = 1 */
    int n = encode_name(host, pkt + 12, sizeof pkt - 16);
    if (n < 0) return DNS_ERR_NXDOM;
    int qlen = 12 + n;
    pkt[qlen++] = 0; pkt[qlen++] = 1;   /* QTYPE  A  */
    pkt[qlen++] = 0; pkt[qlen++] = 1;   /* QCLASS IN */

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return DNS_ERR_NET;

    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(DNS_PORT);
    if (inet_pton(AF_INET, server, &sa.sin_addr) != 1) { close(s); return DNS_ERR_NET; }

    unsigned char resp[1500];
    int rlen = -1;
    for (int attempt = 0; attempt < 2 && rlen < 0; attempt++) {
        if (sendto(s, pkt, qlen, 0, (struct sockaddr *)&sa, sizeof sa) != qlen) continue;
        for (;;) {
            rlen = (int)recvfrom(s, resp, sizeof resp, 0, NULL, NULL);
            if (rlen < 0) break;                          /* Timeout */
            if (rlen >= 12 && resp[0] == pkt[0] && resp[1] == pkt[1]) break;
            rlen = -1;                                    /* fremde Antwort, weiter warten */
        }
    }
    close(s);
    if (rlen < 12) return DNS_ERR_NET;

    int rcode = resp[3] & 0x0F;
    if (rcode == 3) { cache_put(host, NULL, DNS_ERR_NXDOM, 300); return DNS_ERR_NXDOM; }
    if (rcode != 0) return DNS_ERR_NET;

    int qd = (resp[4] << 8) | resp[5];
    int an = (resp[6] << 8) | resp[7];
    int pos = 12;
    for (int i = 0; i < qd; i++) {
        pos = skip_name(resp, rlen, pos);
        if (pos < 0) return DNS_ERR_NET;
        pos += 4;
    }
    for (int i = 0; i < an && pos < rlen; i++) {
        pos = skip_name(resp, rlen, pos);
        if (pos < 0 || pos + 10 > rlen) return DNS_ERR_NET;
        int type = (resp[pos] << 8) | resp[pos + 1];
        unsigned ttl = ((unsigned)resp[pos + 4] << 24) | (resp[pos + 5] << 16) | (resp[pos + 6] << 8) | resp[pos + 7];
        int rdlen = (resp[pos + 8] << 8) | resp[pos + 9];
        pos += 10;
        if (pos + rdlen > rlen) return DNS_ERR_NET;
        if (type == 1 && rdlen == 4) {      /* A-Record (CNAMEs davor werden übersprungen) */
            char ip[16];
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", resp[pos], resp[pos + 1], resp[pos + 2], resp[pos + 3]);
            if (!strcmp(ip, "0.0.0.0")) {   /* Sperrantwort von AdGuard & Co. */
                cache_put(host, NULL, DNS_BLOCKED, ttl);
                return DNS_BLOCKED;
            }
            cache_put(host, ip, DNS_OK, ttl);
            snprintf(ip_out, ip_len, "%s", ip);
            return DNS_OK;
        }
        pos += rdlen;
    }
    return DNS_ERR_NOA;
}

int dns_resolve2(const char *primary, const char *secondary, const char *host,
                 char *ip_out, int ip_len, int timeout_ms)
{
    int r = dns_resolve(primary, host, ip_out, ip_len, timeout_ms);
    if (r == DNS_ERR_NET && secondary && *secondary)
        r = dns_resolve(secondary, host, ip_out, ip_len, timeout_ms);
    return r;
}
