#ifndef VS_PLUGINS_H
#define VS_PLUGINS_H

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
 * item = { title="...", subtitle="...", id="...", kind="video"|"folder" }
 * folder-Einträge werden mit browse(id) geöffnet, video-Einträge mit resolve(item). */

#define ITEM_VIDEO  0
#define ITEM_FOLDER 1

typedef struct {
    char *title;
    char *subtitle;
    char *id;
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
} Source;

typedef struct {
    char url[2048];
    char headers[1024];  /* durch '\n' getrennt */
} StreamInfo;

int     plugins_init(void);      /* lädt alle Plugins, gibt Anzahl Quellen zurück */
void    plugins_shutdown(void);
int     plugins_reload(void);

int     plugins_source_count(void);
Source *plugins_source(int idx);

void    plugins_list_free(PluginList *l);

/* ---- asynchrone Aufrufe (Lua läuft in einem Worker-Thread) ---- */
typedef enum { JOB_IDLE, JOB_RUNNING, JOB_DONE, JOB_ERROR } JobState;

int      plugins_start_search(int src, const char *query);
int      plugins_start_browse(int src, const char *id);       /* id darf NULL sein */
int      plugins_start_resolve(int src, const PluginItem *it);

JobState plugins_job_state(void);
/* Ergebnis abholen; danach ist der Job wieder JOB_IDLE. */
int      plugins_take_list(PluginList *out);
int      plugins_take_stream(StreamInfo *out);
const char *plugins_job_error(void);
void     plugins_job_reset(void);

/* Log der letzten Plugin-Meldungen (vs.log) */
const char *plugins_last_log(void);

#endif
