#ifndef VS_ADBLOCK_H
#define VS_ADBLOCK_H

/* Lädt eine Blockliste. Unterstützte Zeilenformate:
 *   example.com
 *   0.0.0.0 example.com      (hosts-Datei)
 *   127.0.0.1 example.com
 *   ||example.com^           (AdGuard/uBlock-Domainregel)
 *   @@||example.com^         (Ausnahme / Allowlist)
 *   # oder ! Kommentar
 * Gesperrt wird die Domain inklusive aller Subdomains.
 * Mehrfacher Aufruf hängt Listen an. Rückgabe: Anzahl geladener Regeln. */
int  adblock_load(const char *path);
void adblock_clear(void);

/* 1, wenn der Host gesperrt ist (unabhängig vom Ein/Aus-Schalter). */
int  adblock_is_blocked(const char *host);

/* Extrahiert den Host aus einer URL in out (klein geschrieben). */
int  adblock_host_from_url(const char *url, char *out, int outlen);

int  adblock_rule_count(void);

#endif
