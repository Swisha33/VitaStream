#ifndef VS_NET_H
#define VS_NET_H

#include <stddef.h>
#include <stdint.h>

#define NET_OK        0
#define NET_ERR      -1
#define NET_BLOCKED  -2   /* durch Blockliste oder DNS-Filter gesperrt */
#define NET_DNS_FAIL -3
#define NET_TLS      -4   /* Zertifikatsprüfung fehlgeschlagen */
#define NET_ABORTED  -5

typedef struct {
    char  *data;
    size_t len;
} NetBuf;

typedef struct {
    int  requests;
    int  blocked;
    char last_blocked[128];
} NetStats;

extern NetStats g_net_stats;

int  net_init(void);
void net_term(void);
int  net_online(void);

/* Einfache HTTP-Anfrage.
 * post_body == NULL -> GET, sonst POST.
 * headers: zusätzliche Header, durch '\n' getrennt (z. B. "Referer: https://..\nContent-Type: application/json")
 * Weiterleitungen werden selbst verfolgt, damit Blockliste und DNS für jedes Ziel gelten.
 * final_url (optional) erhält die URL nach allen Weiterleitungen. */
int  net_request(const char *url, const char *post_body, const char *headers,
                 NetBuf *out, long *status, char *final_url, int final_len);
void net_buf_free(NetBuf *b);

/* Wie net_request; bricht ab, sobald *abort_flag != 0 wird (z. B. Player geschlossen). */
int  net_request_ex(const char *url, const char *post_body, const char *headers,
                    NetBuf *out, long *status, char *final_url, int final_len,
                    const volatile int *abort_flag);

/* Genauere Beschreibung des letzten Fehlers im aufrufenden Thread (curl-Text, Host ...) */
const char *net_last_detail(void);

/* Prüft eine URL gegen Blockliste/DNS-Filter, ohne sie zu laden. */
int  net_check_url(const char *url);

/* ---- Bereichsweises Lesen für den Player (MP4 über eigenen Netzwerkstack) ---- */
typedef struct NetStream NetStream;

NetStream *net_stream_open(const char *url, const char *headers);
uint64_t   net_stream_size(NetStream *s);
int        net_stream_read(NetStream *s, uint64_t offset, void *buf, uint32_t len);
void       net_stream_close(NetStream *s);
void       net_stream_abort(NetStream *s);   /* laufende/künftige Lesevorgänge abbrechen */

const char *net_strerror(int code);

#endif
