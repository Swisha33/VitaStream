/*
 * VitaStream – generischer Stream-Player mit Quellen-Plugins für PS Vita
 *
 * Bildschirme: Quellen -> Liste (verschachtelt) -> Player, plus Einstellungen.
 * Plugins laufen in einem Worker-Thread, die UI zeigt währenddessen einen Spinner.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/shellutil.h>

#include "ui.h"
#include "config.h"
#include "adblock.h"
#include "dns.h"
#include "net.h"
#include "plugins.h"
#include "player.h"
#include "thumbs.h"

int _newlib_heap_size_user = 192 * 1024 * 1024;

#define BLOCKLIST_FILE VS_DATA_DIR "/blocklist.txt"
#define PLAYLIST_FILE  VS_DATA_DIR "/playlists.txt"
#define MAX_DEPTH      16

typedef enum { SCR_SOURCES, SCR_LIST, SCR_LOADING, SCR_PLAYER, SCR_SETTINGS } Screen;
typedef enum { PEND_NONE, PEND_PUSH_LIST, PEND_PLAY, PEND_APPEND, PEND_SAVE } Pending;

typedef struct {
    PluginList list;
    char       title[128];
    int        cursor, scroll;
} Level;

static Screen  scr = SCR_SOURCES, scr_before_loading = SCR_SOURCES;
static int     src_cursor, src_scroll;
static int     cur_src = -1;
static Level   stack[MAX_DEPTH];
static int     depth;
static Pending pending;
static char    pending_title[128];
static char    last_query[256];
static int     set_cursor, set_scroll;
static int     append_index;            /* Position des "Weitere laden"-Eintrags */

/* Speichern in eine Playlist: Einträge nacheinander auflösen */
static struct {
    char  file[128], name[64];
    int  *idx;
    int   n, pos, ok, fail;
} save_job;

/* ---------------- DNS-Presets ---------------- */

typedef struct { const char *name, *p, *s; } DnsPreset;
static const DnsPreset presets[] = {
    { "AdGuard (Werbung/Tracker)", "94.140.14.14",  "94.140.15.15"    },
    { "AdGuard Familie",           "94.140.14.15",  "94.140.15.16"    },
    { "AdGuard ungefiltert",       "94.140.14.140", "94.140.14.141"   },
    { "Cloudflare",                "1.1.1.1",       "1.0.0.1"         },
    { "Quad9",                     "9.9.9.9",       "149.112.112.112" },
};
#define NPRESETS (int)(sizeof presets / sizeof *presets)

static int current_preset(void)
{
    for (int i = 0; i < NPRESETS; i++)
        if (!strcmp(g_cfg.dns_primary, presets[i].p)) return i;
    return -1;
}

/* ---------------- Hilfen ---------------- */

static void stack_clear(void)
{
    while (depth > 0) plugins_list_free(&stack[--depth].list);
}

static void reload_blocklist(void)
{
    adblock_clear();
    adblock_load(BLOCKLIST_FILE);
}

static void start_job_screen(Pending p, const char *title)
{
    pending = p;
    snprintf(pending_title, sizeof pending_title, "%s", title ? title : "");
    scr_before_loading = scr;
    scr = SCR_LOADING;
}

static void open_source(int idx)
{
    Source *s = plugins_source(idx);
    if (!s) return;
    stack_clear();
    cur_src = idx;
    if (s->has_browse) {
        if (plugins_start_browse(idx, NULL) == 0) start_job_screen(PEND_PUSH_LIST, s->name);
    } else if (s->has_search) {
        char q[256];
        if (ui_input_text(s->name, last_query, q, sizeof q) && q[0]) {
            snprintf(last_query, sizeof last_query, "%s", q);
            char t[160];
            snprintf(t, sizeof t, "%s: %s", s->name, q);
            if (plugins_start_search(idx, q) == 0) start_job_screen(PEND_PUSH_LIST, t);
        }
    }
}

static void search_in_current(void)
{
    Source *s = plugins_source(cur_src);
    if (!s || !s->has_search) return;
    char q[256];
    if (ui_input_text("Suchen", last_query, q, sizeof q) && q[0]) {
        snprintf(last_query, sizeof last_query, "%s", q);
        char t[160];
        snprintf(t, sizeof t, "Suche: %s", q);
        if (plugins_start_search(cur_src, q) == 0) start_job_screen(PEND_PUSH_LIST, t);
    }
}

static void fmt_time(uint64_t ms, char *out, int n)
{
    unsigned s = (unsigned)(ms / 1000);
    if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else           snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

/* ---------------- Listen-Beschriftungen ---------------- */

static void label_sources(void *ctx, int i, const char **t, const char **sub)
{
    (void)ctx;
    Source *s = plugins_source(i);
    *t = s->name;
    *sub = s->description;
}

static void label_items(void *ctx, int i, const char **t, const char **sub, const char **thumb)
{
    PluginList *l = ctx;
    static char buf[300];
    PluginItem *it = &l->items[i];
    if (it->kind == ITEM_FOLDER) {
        snprintf(buf, sizeof buf, "[+] %s", it->title);
        *t = buf;
    } else if (it->kind == ITEM_MORE) {
        snprintf(buf, sizeof buf, ">> %s", it->title);
        *t = buf;
    } else {
        *t = it->title;
    }
    *sub = it->subtitle;
    *thumb = it->thumb;
}

static void label_sources_thumb(void *ctx, int i, const char **t, const char **sub, const char **thumb)
{
    label_sources(ctx, i, t, sub);
    *thumb = NULL;
}

/* ---------------- Playlists / Favoriten ---------------- */

typedef struct { char name[64]; char file[128]; } LocalPl;

/* Lokale Playlists aus playlists.txt ("Name|file:datei.m3u") */
static int local_playlists(LocalPl *out, int max)
{
    FILE *f = fopen(PLAYLIST_FILE, "r");
    if (!f) return 0;
    char line[1100];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        char *u = bar + 1;
        u[strcspn(u, "\r\n")] = 0;
        if (strncmp(u, "file:", 5)) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", line);
        snprintf(out[n].file, sizeof out[n].file, "%s", u + 5);
        n++;
    }
    fclose(f);
    return n;
}

/* Legt die Playlist an (Datei + Eintrag in playlists.txt), falls neu */
static void ensure_playlist(const char *name, const char *file)
{
    LocalPl pl[64];
    int n = local_playlists(pl, 64);
    for (int i = 0; i < n; i++) if (!strcmp(pl[i].file, file)) return;
    FILE *f = fopen(PLAYLIST_FILE, "a");
    if (f) { fprintf(f, "\n%s|file:%s\n", name, file); fclose(f); }
    char path[256];
    snprintf(path, sizeof path, VS_DATA_DIR "/%s", file);
    FILE *m = fopen(path, "r");
    if (m) { fclose(m); return; }
    m = fopen(path, "w");
    if (m) { fputs("#EXTM3U\n", m); fclose(m); }
}

static void header_value(const char *headers, const char *key, char *out, int n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = headers; p && *p; ) {
        if (!strncasecmp(p, key, kl) && p[kl] == ':') {
            p += kl + 1;
            while (*p == ' ') p++;
            int i = 0;
            while (*p && *p != '\n' && i < n - 1) out[i++] = *p++;
            out[i] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

static void append_entry(const char *file, const PluginItem *it, const StreamInfo *si)
{
    char path[256], v[512];
    snprintf(path, sizeof path, VS_DATA_DIR "/%s", file);
    FILE *f = fopen(path, "a");
    if (!f) return;
    char title[256];
    snprintf(title, sizeof title, "%s", it->title);
    for (char *c = title; *c; c++) if (*c == ',' || *c == '\n') *c = ' ';
    fprintf(f, "#EXTINF:-1 tvg-logo=\"%s\" group-title=\"VitaStream\",%s\n",
            it->thumb && strncmp(it->thumb, "og:", 3) ? it->thumb : "", title);
    header_value(si->headers, "Referer", v, sizeof v);
    if (v[0]) fprintf(f, "#EXTVLCOPT:http-referrer=%s\n", v);
    header_value(si->headers, "User-Agent", v, sizeof v);
    if (v[0]) fprintf(f, "#EXTVLCOPT:http-user-agent=%s\n", v);
    fprintf(f, "%s\n", si->url);
    fclose(f);
}

static void make_filename(const char *name, char *out, int n)
{
    int j = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && j < n - 5; p++) {
        if (isalnum(*p)) out[j++] = (char)tolower(*p);
        else if (j && out[j - 1] != '_') out[j++] = '_';
    }
    if (!j) out[j++] = 'p';
    strcpy(out + j, ".m3u");
}

/* Ziel wählen: Favoriten / bestehende Playlist / neue. 1 = gewählt */
static int choose_playlist(int favorites_only)
{
    if (favorites_only) {
        snprintf(save_job.name, sizeof save_job.name, "Favoriten");
        snprintf(save_job.file, sizeof save_job.file, "favoriten.m3u");
        return 1;
    }
    LocalPl pl[32];
    int n = local_playlists(pl, 31);
    const char *opts[34];
    int k = 0;
    opts[k++] = "Favoriten";
    for (int i = 0; i < n; i++) if (strcmp(pl[i].file, "favoriten.m3u")) opts[k++] = pl[i].name;
    opts[k++] = "Neue Playlist...";
    int c = ui_menu("In welche Playlist?", opts, k);
    if (c < 0) return 0;
    if (c == 0) return choose_playlist(1);
    if (c == k - 1) {
        char name[64];
        if (!ui_input_text("Name der neuen Playlist", "", name, sizeof name) || !name[0]) return 0;
        snprintf(save_job.name, sizeof save_job.name, "%s", name);
        make_filename(name, save_job.file, sizeof save_job.file);
        return 1;
    }
    for (int i = 0; i < n; i++)
        if (!strcmp(pl[i].name, opts[c])) {
            snprintf(save_job.name, sizeof save_job.name, "%s", pl[i].name);
            snprintf(save_job.file, sizeof save_job.file, "%s", pl[i].file);
            return 1;
        }
    return 0;
}

static void start_save_next(void);

static void start_save(int *idx, int n)
{
    ensure_playlist(save_job.name, save_job.file);
    free(save_job.idx);
    save_job.idx = idx;
    save_job.n = n;
    save_job.pos = save_job.ok = save_job.fail = 0;
    start_save_next();
}

static void start_save_next(void)
{
    Level *lv = &stack[depth - 1];
    while (save_job.pos < save_job.n) {
        PluginItem *it = &lv->list.items[save_job.idx[save_job.pos]];
        char t[96];
        snprintf(t, sizeof t, "Speichere %d/%d ...", save_job.pos + 1, save_job.n);
        if (plugins_start_resolve(cur_src, it) == 0) {
            pending = PEND_SAVE;
            snprintf(pending_title, sizeof pending_title, "%s", t);
            if (scr != SCR_LOADING) scr_before_loading = scr;
            scr = SCR_LOADING;
            return;
        }
        save_job.pos++;
        save_job.fail++;
    }
    /* fertig */
    char msg[256];
    snprintf(msg, sizeof msg, "%d Eintrag/Eintraege in \"%s\" gespeichert%s. Zu finden unter \"M3U-Playlists\".",
             save_job.ok, save_job.name, save_job.fail ? " (einige nicht aufloesbar)" : "");
    pending = PEND_NONE;
    scr = SCR_LIST;
    free(save_job.idx);
    save_job.idx = NULL;
    ui_message("Playlist", msg);
}

/* Quadrat-Menü für den aktuellen Eintrag */
static void item_menu(void)
{
    Level *lv = &stack[depth - 1];
    if (!lv->list.count) return;
    PluginItem *it = &lv->list.items[lv->cursor];
    const char *opts[3];
    int k = 0, a_fav = -1, a_pl = -1, a_all = -1;
    if (it->kind == ITEM_VIDEO) {
        a_fav = k; opts[k++] = "Zu Favoriten hinzufuegen";
        a_pl  = k; opts[k++] = "Zu Playlist hinzufuegen...";
    }
    a_all = k; opts[k++] = "Ganze Liste als Playlist speichern...";
    int c = ui_menu(it->title, opts, k);
    if (c < 0) return;
    if (c == a_fav || c == a_pl) {
        if (!choose_playlist(c == a_fav)) return;
        int *idx = malloc(sizeof(int));
        idx[0] = lv->cursor;
        start_save(idx, 1);
    } else if (c == a_all) {
        if (!choose_playlist(0)) return;
        int *idx = malloc(sizeof(int) * lv->list.count), n = 0;
        for (int i = 0; i < lv->list.count; i++)
            if (lv->list.items[i].kind == ITEM_VIDEO) idx[n++] = i;
        if (!n) { free(idx); ui_message("Playlist", "Diese Liste enthaelt keine abspielbaren Eintraege."); return; }
        start_save(idx, n);
    }
}

enum {
    SET_ADBLOCK, SET_DNS, SET_PRESET, SET_CUSTOM_DNS, SET_RELOAD_BL,
    SET_ADD_PLAYLIST, SET_RELOAD_PLUGINS, SET_STATS, SET_COUNT
};

static void label_settings(void *ctx, int i, const char **t, const char **sub)
{
    (void)ctx;
    static char tb[160], sb[200];
    tb[0] = sb[0] = 0;
    switch (i) {
    case SET_ADBLOCK:
        snprintf(tb, sizeof tb, "AdBlock (lokale Blockliste): %s", g_cfg.adblock_enabled ? "AN" : "AUS");
        snprintf(sb, sizeof sb, "%d Domains in blocklist.txt", adblock_rule_count());
        break;
    case SET_DNS:
        snprintf(tb, sizeof tb, "Eigener DNS-Server: %s", g_cfg.custom_dns_enabled ? "AN" : "AUS");
        snprintf(sb, sizeof sb, "AUS = System-DNS der Vita");
        break;
    case SET_PRESET: {
        int p = current_preset();
        snprintf(tb, sizeof tb, "DNS: %s", p >= 0 ? presets[p].name : "Benutzerdefiniert");
        snprintf(sb, sizeof sb, "%s / %s   (Links/Rechts zum Wechseln)", g_cfg.dns_primary, g_cfg.dns_secondary);
        break;
    }
    case SET_CUSTOM_DNS:
        snprintf(tb, sizeof tb, "Eigene DNS-Adresse eingeben...");
        snprintf(sb, sizeof sb, "IPv4, z. B. Pi-hole oder AdGuard Home im Heimnetz");
        break;
    case SET_RELOAD_BL:
        snprintf(tb, sizeof tb, "Blockliste neu laden");
        break;
    case SET_ADD_PLAYLIST:
        snprintf(tb, sizeof tb, "M3U-Playlist hinzufuegen...");
        snprintf(sb, sizeof sb, "Name und URL eingeben, landet in playlists.txt");
        break;
    case SET_RELOAD_PLUGINS:
        snprintf(tb, sizeof tb, "Plugins neu laden");
        snprintf(sb, sizeof sb, "%d Quellen geladen%s%s", plugins_source_count(),
                 plugins_last_log()[0] ? "  |  " : "", plugins_last_log());
        break;
    case SET_STATS:
        snprintf(tb, sizeof tb, "Anfragen: %d   Gesperrt: %d", g_net_stats.requests, g_net_stats.blocked);
        snprintf(sb, sizeof sb, "Zuletzt gesperrt: %s",
                 g_net_stats.last_blocked[0] ? g_net_stats.last_blocked : "-");
        break;
    }
    *t = tb;
    *sub = sb;
}

static void settings_action(int i, int dir)
{
    switch (i) {
    case SET_ADBLOCK:
        g_cfg.adblock_enabled = !g_cfg.adblock_enabled;
        break;
    case SET_DNS:
        g_cfg.custom_dns_enabled = !g_cfg.custom_dns_enabled;
        dns_cache_clear();
        break;
    case SET_PRESET: {
        int p = current_preset();
        p = (p < 0) ? 0 : (p + (dir < 0 ? NPRESETS - 1 : 1)) % NPRESETS;
        strcpy(g_cfg.dns_primary, presets[p].p);
        strcpy(g_cfg.dns_secondary, presets[p].s);
        dns_cache_clear();
        break;
    }
    case SET_CUSTOM_DNS: {
        char ip[64];
        if (ui_input_text("Primaerer DNS (IPv4)", g_cfg.dns_primary, ip, sizeof ip) && ip[0]) {
            snprintf(g_cfg.dns_primary, sizeof g_cfg.dns_primary, "%s", ip);
            if (ui_input_text("Sekundaerer DNS (leer = keiner)", "", ip, sizeof ip))
                snprintf(g_cfg.dns_secondary, sizeof g_cfg.dns_secondary, "%s", ip);
            dns_cache_clear();
        }
        break;
    }
    case SET_RELOAD_BL:
        reload_blocklist();
        break;
    case SET_ADD_PLAYLIST: {
        char name[64], url[1024];
        if (!ui_input_text("Name der Playlist", "", name, sizeof name) || !name[0]) break;
        if (!ui_input_text("URL der M3U-Datei", "https://", url, sizeof url) || strlen(url) < 10) break;
        FILE *f = fopen(PLAYLIST_FILE, "a");
        if (f) {
            fprintf(f, "\n%s|%s\n", name, url);
            fclose(f);
            stack_clear();
            plugins_reload();
            ui_message("Playlist", "Hinzugefuegt. Du findest sie in der Quelle \"M3U-Playlists\".");
        }
        break;
    }
    case SET_RELOAD_PLUGINS:
        stack_clear();
        plugins_reload();
        src_cursor = 0;
        break;
    }
    config_save();
}

/* ---------------- Main ---------------- */

int main(void)
{
    sceShellUtilInitEvents(0);
    /* volle Taktrate: hilft beim Software-Decoder und beim Parsen großer Listen */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    ui_init();
    config_install_defaults();
    config_load();
    reload_blocklist();
    net_init();
    plugins_init();
    thumbs_init();

    if (!net_online())
        ui_message("Keine Verbindung", "Die Vita ist nicht mit dem Internet verbunden. "
                   "Lokale Quellen funktionieren trotzdem.");

    Input in;
    int running = 1;
    int osd_timer = 0;

    while (running) {
        ui_poll(&in);

        switch (scr) {
        /* ---------- Quellenliste ---------- */
        case SCR_SOURCES: {
            int n = plugins_source_count();
            if (in.pressed & SCE_CTRL_UP)   src_cursor = src_cursor > 0 ? src_cursor - 1 : (n ? n - 1 : 0);
            if (in.pressed & SCE_CTRL_DOWN) src_cursor = n ? (src_cursor + 1) % n : 0;
            if ((in.pressed & BTN_ACCEPT) && n) open_source(src_cursor);
            if (in.pressed & SCE_CTRL_TRIANGLE) scr = SCR_SETTINGS;
            if (in.pressed & SCE_CTRL_START) running = 0;

            if (scr != SCR_SOURCES) break;
            ui_begin();
            char right[64];
            snprintf(right, sizeof right, "AdBlock %s  DNS %s",
                     g_cfg.adblock_enabled ? "AN" : "AUS", g_cfg.custom_dns_enabled ? "AN" : "AUS");
            ui_header("VitaStream - Quellen", right);
            ui_list_thumbs(n, src_cursor, &src_scroll, label_sources_thumb, NULL);
            if (n == 0) ui_text(40, 180, COL_DIM, "Keine Plugins gefunden in " VS_DATA_DIR "/plugins");
            ui_footer("Bestaetigen: Oeffnen   Dreieck: Einstellungen   START: Beenden");
            ui_end();
            break;
        }

        /* ---------- Einträge einer Quelle ---------- */
        case SCR_LIST: {
            Level *lv = &stack[depth - 1];
            int n = lv->list.count;
            Source *s = plugins_source(cur_src);
            if (in.pressed & SCE_CTRL_UP)   lv->cursor = lv->cursor > 0 ? lv->cursor - 1 : (n ? n - 1 : 0);
            if (in.pressed & SCE_CTRL_DOWN) lv->cursor = n ? (lv->cursor + 1) % n : 0;
            int page = ui_list_thumbs_visible();
            if (in.pressed & SCE_CTRL_LTRIGGER) lv->cursor = lv->cursor > page ? lv->cursor - page : 0;
            if (in.pressed & SCE_CTRL_RTRIGGER) lv->cursor = lv->cursor + page < n ? lv->cursor + page : (n ? n - 1 : 0);

            if ((in.pressed & BTN_ACCEPT) && n) {
                PluginItem *it = &lv->list.items[lv->cursor];
                if (it->kind == ITEM_FOLDER) {
                    thumbs_drop_pending();
                    if (plugins_start_browse(cur_src, it->id) == 0) start_job_screen(PEND_PUSH_LIST, it->title);
                } else if (it->kind == ITEM_MORE) {
                    append_index = lv->cursor;
                    if (plugins_start_browse(cur_src, it->id) == 0) start_job_screen(PEND_APPEND, lv->title);
                } else {
                    if (plugins_start_resolve(cur_src, it) == 0) start_job_screen(PEND_PLAY, it->title);
                }
            } else if (in.pressed & SCE_CTRL_TRIANGLE) {
                search_in_current();
            } else if (in.pressed & SCE_CTRL_SQUARE) {
                item_menu();
            } else if (in.pressed & BTN_CANCEL) {
                thumbs_drop_pending();
                plugins_list_free(&stack[--depth].list);
                if (depth == 0) scr = SCR_SOURCES;
            }

            if (scr != SCR_LIST || depth == 0) break;
            lv = &stack[depth - 1];
            ui_begin();
            char right[32];
            snprintf(right, sizeof right, "%d Eintraege", lv->list.count);
            ui_header(lv->title, right);
            ui_list_thumbs(lv->list.count, lv->cursor, &lv->scroll, label_items, &lv->list);
            ui_footer(s && s->has_search
                      ? "Bestaetigen: Oeffnen  Zurueck  Dreieck: Suchen  Quadrat: Playlist/Favoriten  L/R: Seite"
                      : "Bestaetigen: Oeffnen  Zurueck  Quadrat: Playlist/Favoriten  L/R: Seite");
            ui_end();
            break;
        }

        /* ---------- Plugin arbeitet ---------- */
        case SCR_LOADING: {
            JobState st = plugins_job_state();
            if (st == JOB_DONE) {
                if (pending == PEND_PUSH_LIST) {
                    if (depth < MAX_DEPTH) {
                        Level *lv = &stack[depth++];
                        memset(lv, 0, sizeof *lv);
                        plugins_take_list(&lv->list);
                        snprintf(lv->title, sizeof lv->title, "%s", pending_title);
                        scr = SCR_LIST;
                    } else {
                        plugins_job_reset();
                        scr = scr_before_loading;
                    }
                } else if (pending == PEND_APPEND) {
                    PluginList more;
                    plugins_take_list(&more);
                    if (depth > 0) {
                        Level *lv = &stack[depth - 1];
                        plugins_list_append(&lv->list, &more, append_index);
                        if (lv->cursor >= lv->list.count) lv->cursor = lv->list.count ? lv->list.count - 1 : 0;
                    } else {
                        plugins_list_free(&more);
                    }
                    scr = SCR_LIST;
                } else if (pending == PEND_SAVE) {
                    StreamInfo si;
                    plugins_take_stream(&si);
                    append_entry(save_job.file, &stack[depth - 1].list.items[save_job.idx[save_job.pos]], &si);
                    save_job.ok++;
                    save_job.pos++;
                    start_save_next();
                    break;
                } else if (pending == PEND_PLAY) {
                    StreamInfo si;
                    plugins_take_stream(&si);
                    if (player_open(si.url, si.headers) == 0) {
                        scr = SCR_PLAYER;
                        osd_timer = 180;
                    } else {
                        ui_message("Wiedergabe fehlgeschlagen", player_error());
                        scr = scr_before_loading;
                    }
                }
                pending = PEND_NONE;
            } else if (st == JOB_ERROR && pending == PEND_SAVE) {
                plugins_job_reset();      /* einzelner Eintrag nicht auflösbar: weiter mit dem nächsten */
                save_job.fail++;
                save_job.pos++;
                start_save_next();
            } else if (st == JOB_ERROR) {
                char msg[300];
                snprintf(msg, sizeof msg, "%s", plugins_job_error());
                plugins_job_reset();
                pending = PEND_NONE;
                scr = scr_before_loading;
                ui_message("Fehler", msg);
            } else {
                ui_begin();
                ui_header(pending_title, NULL);
                ui_spinner(pending == PEND_PLAY ? "Stream wird ermittelt..." :
                           pending == PEND_SAVE ? "Wird gespeichert..." : "Lade...");
                /* Fortschritt des Plugins (vs.log), z. B. beim Durchsuchen einer Website */
                const char *lg = plugins_last_log();
                if (lg[0] && pending != PEND_SAVE) {
                    int w = ui_text_width(lg);
                    ui_text_clipped(w < SCREEN_W - 40 ? (SCREEN_W - w) / 2 : 20, SCREEN_H / 2 + 100, SCREEN_W - 40, COL_DIM, lg);
                }
                ui_end();
            }
            break;
        }

        /* ---------- Player ---------- */
        case SCR_PLAYER: {
            if (in.pressed) osd_timer = 180;
            if (in.pressed & BTN_ACCEPT)          player_toggle_pause();
            if (in.pressed & SCE_CTRL_LEFT)       player_seek_rel(-10);
            if (in.pressed & SCE_CTRL_RIGHT)      player_seek_rel(10);
            if (in.pressed & SCE_CTRL_LTRIGGER)   player_seek_rel(-60);
            if (in.pressed & SCE_CTRL_RTRIGGER)   player_seek_rel(60);
            if (in.pressed & SCE_CTRL_SELECT)     player_toggle_debug();

            int quit = (in.pressed & BTN_CANCEL) != 0;
            if (!quit && !player_active()) {
                const char *e = player_error();
                player_close();
                if (e[0]) ui_message("Wiedergabe beendet", e);
                quit = 2;
            }
            if (quit) {
                if (quit == 1) player_close();
                scr = depth ? SCR_LIST : SCR_SOURCES;
                break;
            }

            /* Bildschirm während der Wiedergabe nicht abdunkeln */
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

            ui_begin();
            player_draw();
            if (osd_timer > 0 || player_paused()) {
                if (osd_timer > 0) osd_timer--;
                uint64_t pos = player_position_ms(), dur = player_duration_ms();
                char a[32], b[32], line[96];
                fmt_time(pos, a, sizeof a);
                if (dur) { fmt_time(dur, b, sizeof b); snprintf(line, sizeof line, "%s / %s", a, b); }
                else     snprintf(line, sizeof line, "%s  (Live)", a);
                /* Fehlerdetails bleiben nach Ende sichtbar, siehe player_error() */

                ui_rect(0, SCREEN_H - 70, SCREEN_W, 70, 0xB0000000);
                if (dur) {
                    int w = (int)((SCREEN_W - 40) * (double)pos / (double)dur);
                    ui_rect(20, SCREEN_H - 60, SCREEN_W - 40, 6, 0x60FFFFFF);
                    ui_rect(20, SCREEN_H - 60, w, 6, COL_ACCENT);
                }
                ui_text(20, SCREEN_H - 22, COL_TEXT, line);
                ui_text_scaled(SCREEN_W - 650, SCREEN_H - 22, COL_DIM, 0.8f,
                    player_paused() ? "PAUSE   Links/Rechts: 10 s   L/R: 60 s   SELECT: Infos"
                                    : "Bestaetigen: Pause   Links/Rechts: 10 s   L/R: 60 s   SELECT: Infos");
            }
            ui_end();
            break;
        }

        /* ---------- Einstellungen ---------- */
        case SCR_SETTINGS: {
            if (in.pressed & SCE_CTRL_UP)   set_cursor = (set_cursor + SET_COUNT - 1) % SET_COUNT;
            if (in.pressed & SCE_CTRL_DOWN) set_cursor = (set_cursor + 1) % SET_COUNT;
            if (in.pressed & BTN_ACCEPT)    settings_action(set_cursor, 1);
            if (set_cursor == SET_PRESET) {
                if (in.pressed & SCE_CTRL_LEFT)  settings_action(SET_PRESET, -1);
                if (in.pressed & SCE_CTRL_RIGHT) settings_action(SET_PRESET, 1);
            }
            if (in.pressed & (BTN_CANCEL | SCE_CTRL_TRIANGLE)) {
                config_save();
                scr = SCR_SOURCES;
                if (src_cursor >= plugins_source_count()) src_cursor = 0;
            }
            if (scr != SCR_SETTINGS) break;
            ui_begin();
            ui_header("Einstellungen", "Netzwerk & AdBlock");
            ui_list(SET_COUNT, set_cursor, &set_scroll, label_settings, NULL);
            ui_footer("Bestaetigen: Umschalten/Ausfuehren   Zurueck: Speichern & Schliessen");
            ui_end();
            break;
        }
        }
    }

    player_close();
    stack_clear();
    thumbs_shutdown();
    plugins_shutdown();
    net_term();
    config_save();
    ui_term();
    sceKernelExitProcess(0);
    return 0;
}
