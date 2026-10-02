#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

VsConfig g_cfg;

static void set_defaults(void)
{
    g_cfg.adblock_enabled    = 1;
    g_cfg.custom_dns_enabled = 1;
    strcpy(g_cfg.dns_primary,   "94.140.14.14");
    strcpy(g_cfg.dns_secondary, "94.140.15.15");
    strcpy(g_cfg.user_agent,
           "Mozilla/5.0 (PlayStation Vita 3.74) AppleWebKit/537.73 (KHTML, like Gecko) VitaStream/0.1");
    g_cfg.timeout_sec = 20;
    g_cfg.ssl_verify  = 1;
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 0;
}

void config_install_defaults(void)
{
    static const char *files[] = {
        "config.ini", "blocklist.txt", "sites.txt", "playlists.txt",
        "plugins/json.lua", "plugins/direct.lua", "plugins/m3u.lua",
        "plugins/mediathek.lua", "plugins/website.lua",
    };
    mkdir("ux0:data", 0777);
    mkdir(VS_DATA_DIR, 0777);
    mkdir(VS_DATA_DIR "/plugins", 0777);

    char src[256], dst[256];
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(dst, sizeof dst, VS_DATA_DIR "/%s", files[i]);
        if (file_exists(dst)) continue; /* Nutzeränderungen nie überschreiben */
        snprintf(src, sizeof src, VS_APP_DATA "/%s", files[i]);
        copy_file(src, dst);
    }
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int config_load(void)
{
    set_defaults();
    FILE *f = fopen(VS_CONFIG_FILE, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);

        if      (!strcmp(k, "adblock"))       g_cfg.adblock_enabled = atoi(v) != 0;
        else if (!strcmp(k, "custom_dns"))    g_cfg.custom_dns_enabled = atoi(v) != 0;
        else if (!strcmp(k, "dns_primary"))   snprintf(g_cfg.dns_primary, sizeof g_cfg.dns_primary, "%s", v);
        else if (!strcmp(k, "dns_secondary")) snprintf(g_cfg.dns_secondary, sizeof g_cfg.dns_secondary, "%s", v);
        else if (!strcmp(k, "user_agent"))    snprintf(g_cfg.user_agent, sizeof g_cfg.user_agent, "%s", v);
        else if (!strcmp(k, "ssl_verify"))    g_cfg.ssl_verify = atoi(v) != 0;
        else if (!strcmp(k, "timeout"))       g_cfg.timeout_sec = atoi(v) > 0 ? atoi(v) : 20;
    }
    fclose(f);
    return 0;
}

int config_save(void)
{
    FILE *f = fopen(VS_CONFIG_FILE, "w");
    if (!f) return -1;
    fprintf(f,
        "# VitaStream Einstellungen\n"
        "# adblock: lokale Blockliste (blocklist.txt) fuer alle Anfragen der App\n"
        "adblock=%d\n"
        "# custom_dns: Namen ueber eigenen DNS aufloesen (z. B. AdGuard DNS)\n"
        "custom_dns=%d\n"
        "dns_primary=%s\n"
        "dns_secondary=%s\n"
        "timeout=%d\n"
        "# ssl_verify: HTTPS-Zertifikate pruefen (0 nur zur Fehlersuche)\n"
        "ssl_verify=%d\n"
        "user_agent=%s\n",
        g_cfg.adblock_enabled, g_cfg.custom_dns_enabled,
        g_cfg.dns_primary, g_cfg.dns_secondary,
        g_cfg.timeout_sec, g_cfg.ssl_verify, g_cfg.user_agent);
    fclose(f);
    return 0;
}
