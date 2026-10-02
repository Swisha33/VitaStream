#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../src/dns.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)
/* Port 53 ist auf dem Testsystem nicht frei: dns.c wird mit umgebogenem htons kompiliert */
int main(void) {
    char ip[16];
    CHECK(dns_resolve("127.0.0.1", "www.example.com", ip, sizeof ip, 1000) == DNS_OK && !strcmp(ip, "93.184.216.34"));
    CHECK(dns_resolve("127.0.0.1", "WWW.Example.com.", ip, sizeof ip, 1000) == DNS_OK);   /* Cache, Groß/klein */
    CHECK(dns_resolve("127.0.0.1", "cname.test", ip, sizeof ip, 1000) == DNS_OK && !strcmp(ip, "10.0.0.7"));
    CHECK(dns_resolve("127.0.0.1", "ads.test", ip, sizeof ip, 1000) == DNS_BLOCKED);
    CHECK(dns_resolve("127.0.0.1", "ads.test", ip, sizeof ip, 1000) == DNS_BLOCKED);       /* Negativ-Cache */
    CHECK(dns_resolve("127.0.0.1", "nx.test", ip, sizeof ip, 1000) == DNS_ERR_NXDOM);
    CHECK(dns_resolve("127.0.0.1", "wrongid.test", ip, sizeof ip, 1000) == DNS_OK);
    CHECK(dns_resolve("127.0.0.1", "drop.test", ip, sizeof ip, 300) == DNS_ERR_NET);
    CHECK(dns_resolve("127.0.0.1", "1.2.3.4", ip, sizeof ip, 300) == DNS_OK && !strcmp(ip, "1.2.3.4"));
    CHECK(dns_resolve2("127.0.0.2", "127.0.0.1", "drop.test", ip, sizeof ip, 200) == DNS_ERR_NET);
    printf(fails ? "%d Fehler\n" : "dns: alle Tests ok\n", fails);
    return fails != 0;
}
