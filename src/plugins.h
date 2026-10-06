#ifndef VS_PLUGINS_H
#define VS_PLUGINS_H

#include "net.h"

/* Quellen-Plugins in Lua (ux0:data/VitaStream/plugins/, Endung .lua).
 * Ein Plugin gibt eine Quelle (Tabelle) oder eine Liste von Quellen zurück:
 *
 *   return {
 *     name = "Meine Quelle",
 *     description = "Kurzbeschreibung",
 *     search  = function(query) return { item, ... } end,   -- optional
 *     browse  = function(id)    return { item, ... } end,   -- optional, id == nil -> Startseite
 *     resolve = function(item)  return "https://...m3u8" end -- oder { url=..., headers=... }
 *   }
 *
 * item = { title="...", subtitle="...", id="...", kind="video"|"folder"|"more", thumb="URL" }
 * folder-Einträge werden mit browse(id) geöffnet, video-Einträge mit resolve(item).
 * "more" = "Weitere laden": browse(id) liefert die nächste Seite, die an die Liste angehängt wird.
 * thumb: Bild-URL (PNG/JPEG) oder "og:<Seiten-URL>" (Vorschaubild der Webseite). */

#define ITEM_VIDEO  0
#define ITEM_FOLDER 1
#define ITEM_MORE   2
#define ITEM_SEARCH 3   /* "search": öffnet die Tastatur und startet search(text) */

typedef struct {
    char *title;
    char *subtitle;
    char *id;
    char *thumb;     /* NULL = Platzhalter */
    int   kind;
    int   ref;       /* Lua-Registry-Referenz auf die Original-Tabelle */
} PluginItem;

typedef struct {
    PluginItem *items;
    int         count;
} PluginList;

typedef struct {
    char name[64];
    char description[160];
    char file[64];
    int  ref;          /* Registry-Referenz auf die Quellentabelle */
    int  has_search;
    int  has_browse;
    int  save_ref;     /* Streams laufen ab: beim Speichern Verweis statt Stream-URL ablegen */
} Source;

#define MAX_STREAM_SUBS 4
typedef struct {
    char url[VS_URL_MAX];
    char headers[2048];  /* durch '\n' getrennt */
    /* vom Plugin gelieferte Untertitel: resolve -> { url=..., subtitles = { {label=..., url=...}, ... } } */
    int  nsubs;
    char sub_label[MAX_STREAM_SUBS][48];
    char sub_url[MAX_STREAM_SUBS][4096];
} StreamInfo;

int     plugins_init(void);      /* lädt alle Plugins, gibt Anzahl Quellen zurück */
void    plugins_shutdown(void);
int     plugins_reload(void);

int     plugins_source_count(void);
Source *plugins_source(int idx);
int     plugins_find_source(const char *file);          /* Index der Quelle zu "youtube.lua" oder -1 */
/* wie Abspielen, nutzt aber download(item) des Plugins, falls vorhanden (Ergebnis wie resolve) */
int     plugins_start_download(int src, const PluginItem *it);
/* Plugin hat vs.menu_music(...) aufgerufen: 1 + Daten (einmalig) */
int     plugins_take_music_request(char *url, int ul, char *name, int nl, char *title, int tl);
/* Eintrag nur anhand seiner id auflösen (gespeicherte Verweise "vsplugin://datei/id") */
int     plugins_start_resolve_id(int src, const char *id, const char *title);

void    plugins_list_free(PluginList *l);
/* Hängt src an dst an (src ist danach leer); entfernt vorher dst->items[remove_index] (-1 = nichts) */
void    plugins_list_append(PluginList *dst, PluginList *src, int remove_index);

/* ---- asynchrone Aufrufe (Lua läuft in einem Worker-Thread) ---- */
typedef enum { JOB_IDLE, JOB_RUNNING, JOB_DONE, JOB_ERROR } JobState;

int      plugins_start_search(int src, const char *query);
/* wie oben, mit Kontext (id des Such-Eintrags) als 2. Argument: search(text, kontext) */
int      plugins_start_search_ctx(int src, const char *query, const char *ctx);
int      plugins_start_browse(int src, const char *id);       /* id darf NULL sein */
int      plugins_start_resolve(int src, const PluginItem *it);

JobState plugins_job_state(void);
/* Ergebnis abholen; danach ist der Job wieder JOB_IDLE. */
int      plugins_take_list(PluginList *out);
int      plugins_take_stream(StreamInfo *out);
const char *plugins_job_error(void);
void     plugins_job_reset(void);

/* ---- Aktionen (Quadrat-Menü), vom Plugin über actions(item)/action(item, id, input) ----
 *   actions = function(item) return { { id="del", label="Löschen", confirm=true },
 *                                      { id="ren", label="Umbenennen", input="Neuer Name", default=item.title } } end
 *   action  = function(item, id, input) return { message="...", refresh=true } end */
typedef struct {
    char id[32];
    char label[96];
    char input[96];    /* nicht leer: vorher Text abfragen (Titel der Tastatur) */
    char def[256];     /* Vorgabetext */
    int  confirm;      /* vorher "Wirklich?" fragen */
} PluginAction;

int  plugins_item_actions(int src, const PluginItem *it, PluginAction *out, int max);
/* Zusatzinfo (Plugin-Funktion info(item), z. B. "Jetzt: ... · Danach: ..."); 0 = keine */
int  plugins_item_info(int src, const PluginItem *it, char *out, int n);
int  plugins_start_action(int src, const PluginItem *it, const char *action_id, const char *input);
int  plugins_take_action_result(char *msg, int msglen, int *refresh);

/* Log der letzten Plugin-Meldungen (vs.log) */
const char *plugins_last_log(void);

#endif
