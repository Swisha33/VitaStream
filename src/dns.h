#ifndef VS_DNS_H
#define VS_DNS_H

/* Minimaler DNS-Resolver (UDP, A-Records) mit Cache.
 * Wird genutzt, um Hostnamen über einen frei wählbaren Server
 * (z. B. AdGuard DNS 94.140.14.14) statt über den System-DNS aufzulösen. */

#define DNS_OK         0
#define DNS_ERR_NET   -1
#define DNS_ERR_NXDOM -2
#define DNS_ERR_NOA   -3   /* keine A-Records */
#define DNS_BLOCKED   -4   /* Server antwortete mit 0.0.0.0 (z. B. AdGuard-Sperre) */

/* Löst host über server (IPv4-Adresse als Text) auf.
 * ip_out bekommt die Adresse als Text ("1.2.3.4"). */
int  dns_resolve(const char *server, const char *host, char *ip_out, int ip_len, int timeout_ms);

/* Wie dns_resolve, probiert aber primary und danach secondary. */
int  dns_resolve2(const char *primary, const char *secondary, const char *host,
                  char *ip_out, int ip_len, int timeout_ms);

void dns_cache_clear(void);

#endif
