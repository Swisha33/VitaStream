#include "plugins.h"
#include "net.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define MAX_SOURCES 64
#define PLUGIN_DIR  VS_DATA_DIR "/plugins"

static lua_State      *L;
static pthread_mutex_t s_lua_lock = PTHREAD_MUTEX_INITIALIZER;
static Source          s_sources[MAX_SOURCES];
static int             s_nsources;
static char            s_log[256];

/* ======================= Lua-API: vs.* ======================= */

static int l_http(lua_State *ls, int is_post)
{
    const char *url = luaL_checkstring(ls, 1);
    const char *body = is_post ? luaL_checkstring(ls, 2) : NULL;
    const char *hdr = luaL_optstring(ls, is_post ? 3 : 2, NULL);

    NetBuf b;
    long status = 0;
    char final_url[1024];
    int r = net_request(url, body, hdr, &b, &status, final_url, sizeof final_url);
    if (r != NET_OK) {
        lua_pushnil(ls);
        const char *d = net_last_detail();
        if (d && *d) lua_pushfstring(ls, "%s: %s", net_strerror(r), d);
        else         lua_pushstring(ls, net_strerror(r));
        return 2;
    }
    lua_pushlstring(ls, b.data ? b.data : "", b.len);
    lua_pushinteger(ls, status);
    lua_pushstring(ls, final_url);
    net_buf_free(&b);
    return 3;
}

static int l_http_get(lua_State *ls)  { return l_http(ls, 0); }
static int l_http_post(lua_State *ls) { return l_http(ls, 1); }

static int l_is_blocked(lua_State *ls)
{
    lua_pushboolean(ls, net_check_url(luaL_checkstring(ls, 1)) == NET_BLOCKED);
    return 1;
}

static int l_urlencode(lua_State *ls)
{
    size_t n;
    const char *s = luaL_checklstring(ls, 1, &n);
    luaL_Buffer b;
    luaL_buffinit(ls, &b);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            luaL_addchar(&b, (char)c);
        } else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            luaL_addstring(&b, hex);
        }
    }
    luaL_pushresult(&b);
    return 1;
}

static void add_utf8(luaL_Buffer *b, unsigned long cp)
{
    char o[4];
    int n;
    if (cp < 0x80)        { o[0] = (char)cp; n = 1; }
    else if (cp < 0x800)  { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000){ o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            o[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                  { o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    luaL_addlstring(b, o, n);
}

static int l_html_unescape(lua_State *ls)
{
    size_t n;
    const char *s = luaL_checklstring(ls, 1, &n);
    static const struct { const char *e; const char *r; } ents[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
        {"&nbsp;", " "}, {"&auml;", "\xC3\xA4"}, {"&ouml;", "\xC3\xB6"}, {"&uuml;", "\xC3\xBC"},
        {"&Auml;", "\xC3\x84"}, {"&Ouml;", "\xC3\x96"}, {"&Uuml;", "\xC3\x9C"}, {"&szlig;", "\xC3\x9F"},
    };
    luaL_Buffer b;
    luaL_buffinit(ls, &b);
    for (size_t i = 0; i < n; ) {
        if (s[i] == '&') {
            if (i + 2 < n && s[i + 1] == '#') {
                char *end;
                int hex = (s[i + 2] == 'x' || s[i + 2] == 'X');
                unsigned long cp = strtoul(s + i + 2 + hex, &end, hex ? 16 : 10);
                if (*end == ';' && end > s + i + 2 + hex) {
                    add_utf8(&b, cp);
                    i = (size_t)(end - s) + 1;
                    continue;
                }
            }
            int hit = 0;
            for (size_t k = 0; k < sizeof ents / sizeof *ents; k++) {
                size_t el = strlen(ents[k].e);
                if (i + el <= n && !strncmp(s + i, ents[k].e, el)) {
                    luaL_addstring(&b, ents[k].r);
                    i += el;
                    hit = 1;
                    break;
                }
            }
            if (hit) continue;
        }
        luaL_addchar(&b, s[i++]);
    }
    luaL_pushresult(&b);
    return 1;
}

static int l_read_file(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    if (strstr(name, "..") || strchr(name, ':')) return luaL_error(ls, "ungueltiger Dateiname");
    char path[256];
    snprintf(path, sizeof path, VS_DATA_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { lua_pushnil(ls); return 1; }
    luaL_Buffer b;
    luaL_buffinit(ls, &b);
    char tmp[1024];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) luaL_addlstring(&b, tmp, r);
    fclose(f);
    luaL_pushresult(&b);
    return 1;
}

/* vs.write_file(name, text [, append]) – nur innerhalb von ux0:data/VitaStream,
   Plugins und config.ini sind tabu. */
static int l_write_file(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    size_t n;
    const char *data = luaL_checklstring(ls, 2, &n);
    int append = lua_toboolean(ls, 3);
    if (strstr(name, "..") || strchr(name, ':') || strchr(name, '/') || strchr(name, '\\')
        || !strcmp(name, "config.ini"))
        return luaL_error(ls, "ungueltiger Dateiname");
    char path[256];
    snprintf(path, sizeof path, VS_DATA_DIR "/%s", name);
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) { lua_pushboolean(ls, 0); return 1; }
    fwrite(data, 1, n, f);
    fclose(f);
    lua_pushboolean(ls, 1);
    return 1;
}

static int l_log(lua_State *ls)
{
    snprintf(s_log, sizeof s_log, "%s", luaL_checkstring(ls, 1));
#ifndef __vita__
    fprintf(stderr, "[plugin] %s\n", s_log);
#endif
    return 0;
}

static const luaL_Reg vs_funcs[] = {
    {"http_get",      l_http_get},
    {"http_post",     l_http_post},
    {"is_blocked",    l_is_blocked},
    {"urlencode",     l_urlencode},
    {"html_unescape", l_html_unescape},
    {"read_file",     l_read_file},
    {"write_file",    l_write_file},
    {"log",           l_log},
    {NULL, NULL}
};

/* ======================= Laden ======================= */

static void open_safe_libs(lua_State *ls)
{
    static const luaL_Reg libs[] = {
        {"_G",      luaopen_base},
        {"package", luaopen_package},
        {"string",  luaopen_string},
        {"table",   luaopen_table},
        {"math",    luaopen_math},
        {"utf8",    luaopen_utf8},
        {NULL, NULL}
    };
    for (const luaL_Reg *l = libs; l->func; l++) {
        luaL_requiref(ls, l->name, l->func, 1);
        lua_pop(ls, 1);
    }
    /* Keine Dateisystem-/Codeladefunktionen außer require aus dem Plugin-Ordner */
    lua_pushnil(ls); lua_setglobal(ls, "dofile");
    lua_pushnil(ls); lua_setglobal(ls, "loadfile");

    lua_getglobal(ls, "package");
    lua_pushstring(ls, PLUGIN_DIR "/?.lua");
    lua_setfield(ls, -2, "path");
    lua_pushstring(ls, "");
    lua_setfield(ls, -2, "cpath");
    lua_pushnil(ls);
    lua_setfield(ls, -2, "loadlib");
    lua_pop(ls, 1);

    luaL_newlib(ls, vs_funcs);
    lua_pushstring(ls, VS_DATA_DIR);
    lua_setfield(ls, -2, "data_dir");
    lua_pushstring(ls, "0.1");
    lua_setfield(ls, -2, "version");
    lua_setglobal(ls, "vs");
}

static void copy_field(lua_State *ls, int idx, const char *key, char *dst, size_t n, const char *def)
{
    lua_getfield(ls, idx, key);
    const char *v = lua_isstring(ls, -1) ? lua_tostring(ls, -1) : def;
    snprintf(dst, n, "%s", v ? v : "");
    lua_pop(ls, 1);
}

static int has_func(lua_State *ls, int idx, const char *key)
{
    lua_getfield(ls, idx, key);
    int r = lua_isfunction(ls, -1);
    lua_pop(ls, 1);
    return r;
}

/* Erwartet eine Quellentabelle oben auf dem Stack; nimmt sie herunter. */
static void register_source(const char *file)
{
    int t = lua_gettop(L);
    if (s_nsources >= MAX_SOURCES || !has_func(L, t, "resolve")) { lua_pop(L, 1); return; }
    Source *s = &s_sources[s_nsources];
    memset(s, 0, sizeof *s);
    copy_field(L, t, "name", s->name, sizeof s->name, file);
    copy_field(L, t, "description", s->description, sizeof s->description, "");
    snprintf(s->file, sizeof s->file, "%s", file);
    s->has_search = has_func(L, t, "search");
    s->has_browse = has_func(L, t, "browse");
    s->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_nsources++;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_plugin_file(const char *fname)
{
    char path[256];
    snprintf(path, sizeof path, PLUGIN_DIR "/%s", fname);
    if (luaL_loadfile(L, path) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        snprintf(s_log, sizeof s_log, "%s: %s", fname, lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; } /* Bibliotheken wie json.lua */

    lua_getfield(L, -1, "resolve");
    int single = !lua_isnil(L, -1);
    lua_pop(L, 1);

    if (single) {
        register_source(fname);
    } else {
        int list = lua_gettop(L);
        int n = (int)lua_rawlen(L, list);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, list, i);
            if (lua_istable(L, -1)) register_source(fname);
            else lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

static int load_all(void)
{
    L = luaL_newstate();
    if (!L) return -1;
    open_safe_libs(L);
    s_nsources = 0;

    DIR *d = opendir(PLUGIN_DIR);
    if (!d) return 0;
    char *names[128];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 128) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".lua")) names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof(char *), cmp_name);
    for (int i = 0; i < nn; i++) {
        load_plugin_file(names[i]);
        free(names[i]);
    }
    return s_nsources;
}

/* ======================= Worker ======================= */

typedef enum { OP_SEARCH, OP_BROWSE, OP_RESOLVE } Op;

static struct {
    pthread_t       thread;
    pthread_mutex_t m;
    pthread_cond_t  cv;
    int             quit;
    int             pending;
    volatile JobState state;
    Op              op;
    int             src;
    char           *arg;
    int             item_ref;
    PluginList      list;
    StreamInfo      stream;
    char            err[256];
} W;

static void set_error(const char *msg)
{
    snprintf(W.err, sizeof W.err, "%s", msg ? msg : "Unbekannter Fehler");
}

/* Liest eine Liste von Items von Stack-Index idx. */
static void read_items(int idx, PluginList *out)
{
    memset(out, 0, sizeof *out);
    if (!lua_istable(L, idx)) return;
    int n = (int)lua_rawlen(L, idx);
    if (n <= 0) return;
    out->items = calloc(n, sizeof(PluginItem));
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        int t = lua_gettop(L);
        if (!lua_istable(L, t)) { lua_pop(L, 1); continue; }
        PluginItem *it = &out->items[out->count];
        char buf[512];
        copy_field(L, t, "title", buf, sizeof buf, "(ohne Titel)");    it->title = strdup(buf);
        copy_field(L, t, "subtitle", buf, sizeof buf, "");             it->subtitle = strdup(buf);
        copy_field(L, t, "id", buf, sizeof buf, "");                   it->id = strdup(buf);
        copy_field(L, t, "kind", buf, sizeof buf, "video");
        it->kind = !strcmp(buf, "folder") ? ITEM_FOLDER : ITEM_VIDEO;
        it->ref = luaL_ref(L, LUA_REGISTRYINDEX);                      /* pop */
        out->count++;
    }
}

static void read_headers_table(int idx, char *out, size_t n)
{
    out[0] = 0;
    if (!lua_istable(L, idx)) return;
    size_t used = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
            int w = snprintf(out + used, n - used, "%s%s: %s",
                             used ? "\n" : "", lua_tostring(L, -2), lua_tostring(L, -1));
            if (w > 0 && (size_t)w < n - used) used += w;
        }
        lua_pop(L, 1);
    }
}

static void run_job(void)
{
    pthread_mutex_lock(&s_lua_lock);
    int top = lua_gettop(L);
    const char *fname = W.op == OP_SEARCH ? "search" : W.op == OP_BROWSE ? "browse" : "resolve";

    lua_rawgeti(L, LUA_REGISTRYINDEX, s_sources[W.src].ref);
    lua_getfield(L, -1, fname);
    if (!lua_isfunction(L, -1)) {
        set_error("Funktion vom Plugin nicht unterstuetzt");
        W.state = JOB_ERROR;
        goto out;
    }
    if (W.op == OP_RESOLVE) lua_rawgeti(L, LUA_REGISTRYINDEX, W.item_ref);
    else if (W.arg)          lua_pushstring(L, W.arg);
    else                     lua_pushnil(L);

    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
        set_error(lua_tostring(L, -1));
        W.state = JOB_ERROR;
        goto out;
    }
    int res = lua_gettop(L) - 1;
    if (lua_isnil(L, res)) {
        set_error(lua_isstring(L, res + 1) ? lua_tostring(L, res + 1) : "Keine Ergebnisse");
        W.state = JOB_ERROR;
        goto out;
    }

    if (W.op == OP_RESOLVE) {
        memset(&W.stream, 0, sizeof W.stream);
        if (lua_isstring(L, res)) {
            snprintf(W.stream.url, sizeof W.stream.url, "%s", lua_tostring(L, res));
        } else if (lua_istable(L, res)) {
            copy_field(L, res, "url", W.stream.url, sizeof W.stream.url, "");
            lua_getfield(L, res, "headers");
            read_headers_table(lua_gettop(L), W.stream.headers, sizeof W.stream.headers);
            lua_pop(L, 1);
        }
        if (!W.stream.url[0]) { set_error("Plugin lieferte keine Stream-URL"); W.state = JOB_ERROR; goto out; }
        if (net_check_url(W.stream.url) == NET_BLOCKED) {
            set_error("Stream-Host ist durch AdBlock gesperrt");
            W.state = JOB_ERROR;
            goto out;
        }
    } else {
        read_items(res, &W.list);
    }
    W.state = JOB_DONE;
out:
    lua_settop(L, top);
    pthread_mutex_unlock(&s_lua_lock);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&W.m);
    for (;;) {
        while (!W.pending && !W.quit) pthread_cond_wait(&W.cv, &W.m);
        if (W.quit) break;
        W.pending = 0;
        pthread_mutex_unlock(&W.m);
        run_job();
        pthread_mutex_lock(&W.m);
    }
    pthread_mutex_unlock(&W.m);
    return NULL;
}

static int start(Op op, int src, const char *arg, int item_ref)
{
    if (W.state == JOB_RUNNING || src < 0 || src >= s_nsources) return -1;
    pthread_mutex_lock(&W.m);
    free(W.arg);
    W.arg = arg ? strdup(arg) : NULL;
    W.op = op;
    W.src = src;
    W.item_ref = item_ref;
    W.err[0] = 0;
    W.state = JOB_RUNNING;
    W.pending = 1;
    pthread_cond_signal(&W.cv);
    pthread_mutex_unlock(&W.m);
    return 0;
}

int plugins_start_search(int src, const char *q)  { return start(OP_SEARCH, src, q, LUA_NOREF); }
int plugins_start_browse(int src, const char *id) { return start(OP_BROWSE, src, id, LUA_NOREF); }
int plugins_start_resolve(int src, const PluginItem *it) { return start(OP_RESOLVE, src, NULL, it->ref); }

JobState    plugins_job_state(void) { return W.state; }
const char *plugins_job_error(void) { return W.err; }
const char *plugins_last_log(void)  { return s_log; }

void plugins_job_reset(void)
{
    if (W.state == JOB_RUNNING) return;
    plugins_list_free(&W.list);
    W.state = JOB_IDLE;
}

int plugins_take_list(PluginList *out)
{
    if (W.state != JOB_DONE) return -1;
    *out = W.list;
    memset(&W.list, 0, sizeof W.list);
    W.state = JOB_IDLE;
    return 0;
}

int plugins_take_stream(StreamInfo *out)
{
    if (W.state != JOB_DONE) return -1;
    *out = W.stream;
    W.state = JOB_IDLE;
    return 0;
}

void plugins_list_free(PluginList *l)
{
    if (!l->items) return;
    pthread_mutex_lock(&s_lua_lock);
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].title);
        free(l->items[i].subtitle);
        free(l->items[i].id);
        if (L) luaL_unref(L, LUA_REGISTRYINDEX, l->items[i].ref);
    }
    pthread_mutex_unlock(&s_lua_lock);
    free(l->items);
    memset(l, 0, sizeof *l);
}

/* ======================= Lebenszyklus ======================= */

int plugins_init(void)
{
    memset(&W, 0, sizeof W);
    pthread_mutex_init(&W.m, NULL);
    pthread_cond_init(&W.cv, NULL);
    int n = load_all();
    pthread_create(&W.thread, NULL, worker, NULL);
    return n;
}

void plugins_shutdown(void)
{
    pthread_mutex_lock(&W.m);
    W.quit = 1;
    pthread_cond_signal(&W.cv);
    pthread_mutex_unlock(&W.m);
    pthread_join(W.thread, NULL);
    plugins_list_free(&W.list);
    free(W.arg);
    if (L) lua_close(L);
    L = NULL;
}

/* Hinweis: Vor dem Aufruf müssen alle PluginLists freigegeben sein. */
int plugins_reload(void)
{
    if (W.state == JOB_RUNNING) return -1;
    pthread_mutex_lock(&s_lua_lock);
    if (L) lua_close(L);
    L = NULL;
    s_log[0] = 0;
    int n = load_all();
    pthread_mutex_unlock(&s_lua_lock);
    return n;
}

int     plugins_source_count(void) { return s_nsources; }
Source *plugins_source(int i)      { return (i >= 0 && i < s_nsources) ? &s_sources[i] : NULL; }
