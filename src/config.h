#ifndef VS_CONFIG_H
#define VS_CONFIG_H

#define VS_DATA_DIR    "ux0:data/VitaStream"
#define VS_APP_DATA    "app0:data"
#define VS_CONFIG_FILE VS_DATA_DIR "/config.ini"
#define VS_APP_VERSION "0.9"

typedef struct {
    int  adblock_enabled;     /* lokale Blockliste aktiv */
    int  custom_dns_enabled;  /* eigenen DNS-Server statt System-DNS nutzen */
    char dns_primary[64];     /* z. B. 94.140.14.14 (AdGuard) */
    char dns_secondary[64];   /* z. B. 94.140.15.15 */
    char user_agent[160];
    int  timeout_sec;
    int  ssl_verify;          /* HTTPS-Zertifikate prüfen */
    char proxy[192];          /* z. B. socks5h://host:1080, leer = kein Proxy */
    char audio_lang[48];      /* bevorzugte Tonspur (z. B. "en"), leer = automatisch (Deutsch) */
    int  theme;               /* Farbthema der Oberfläche (ui_theme_*) */
    char custom_colors[64];   /* eigenes Thema: 6x RRGGBB, durch Komma getrennt */
    char menu_music[256];     /* Menümusik: Datei in ux0:data/VitaStream/music/, leer = aus */
    char epg_url[512];        /* Programmführer (XMLTV, auch .gz) für Listen ohne eigenen, leer = keiner */
} VsConfig;

extern VsConfig g_cfg;

/* Kopiert mitgelieferte Standarddateien nach ux0:data/VitaStream,
   sofern dort noch nicht vorhanden. */
void config_install_defaults(void);
int  config_load(void);
int  config_save(void);

#endif
