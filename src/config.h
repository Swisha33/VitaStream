#ifndef VS_CONFIG_H
#define VS_CONFIG_H

#define VS_DATA_DIR    "ux0:data/VitaStream"
#define VS_APP_DATA    "app0:data"
#define VS_CONFIG_FILE VS_DATA_DIR "/config.ini"

typedef struct {
    int  adblock_enabled;     /* lokale Blockliste aktiv */
    int  custom_dns_enabled;  /* eigenen DNS-Server statt System-DNS nutzen */
    char dns_primary[64];     /* z. B. 94.140.14.14 (AdGuard) */
    char dns_secondary[64];   /* z. B. 94.140.15.15 */
    char user_agent[160];
    int  timeout_sec;
} VsConfig;

extern VsConfig g_cfg;

/* Kopiert mitgelieferte Standarddateien nach ux0:data/VitaStream,
   sofern dort noch nicht vorhanden. */
void config_install_defaults(void);
int  config_load(void);
int  config_save(void);

#endif
