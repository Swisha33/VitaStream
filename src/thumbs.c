/* Vorschaubild-Cache für die Listen (Vita). Ein Hintergrund-Thread lädt und verkleinert,
 * der Hauptthread erzeugt daraus vita2d-Texturen. Zuletzt angefragte Bilder zuerst. */
#include "thumbs.h"
#include "thumbfetch.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_ENTRIES 96
#define MAX_TEXTURES 64

typedef enum { T_PENDING, T_LOADING, T_DECODED, T_DONE, T_FAILED } TState;

typedef struct {
    char           *url;
    TState          st;
    unsigned        last_used;
    unsigned        requested;
    uint8_t        *rgba;
    int             w, h;
    vita2d_texture *tex;
} Entry;

static Entry           s_e[MAX_ENTRIES];
static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static pthread_t       s_thread;
static volatile int    s_abort;
static int             s_started;
static unsigned        s_clock;

static void entry_free(Entry *e)
{
    free(e->url);
    free(e->rgba);
    if (e->tex) vita2d_free_texture(e->tex);
    memset(e, 0, sizeof *e);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_m);
        Entry *job = NULL;
        while (!s_abort) {
            unsigned best = 0;
            for (int i = 0; i < MAX_ENTRIES; i++)
                if (s_e[i].url && s_e[i].st == T_PENDING && s_e[i].requested >= best) { best = s_e[i].requested; job = &s_e[i]; }
            if (job) break;
            pthread_cond_wait(&s_cv, &s_m);
        }
        if (s_abort) { pthread_mutex_unlock(&s_m); break; }
        job->st = T_LOADING;
        char *url = strdup(job->url);
        pthread_mutex_unlock(&s_m);

        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        int r = thumb_fetch(url, &s_abort, THUMB_W, THUMB_H, &rgba, &w, &h);

        pthread_mutex_lock(&s_m);
        /* Eintrag kann inzwischen verworfen worden sein */
        if (job->url && !strcmp(job->url, url) && job->st == T_LOADING) {
            if (r == 0) { job->rgba = rgba; job->w = w; job->h = h; job->st = T_DECODED; rgba = NULL; }
            else job->st = T_FAILED;
        }
        pthread_mutex_unlock(&s_m);
        free(rgba);
        free(url);
    }
    return NULL;
}

void thumbs_init(void)
{
    if (s_started) return;
    s_abort = 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    s_started = pthread_create(&s_thread, &at, worker, NULL) == 0;
    pthread_attr_destroy(&at);
}

void thumbs_shutdown(void)
{
    if (!s_started) return;
    s_abort = 1;
    pthread_mutex_lock(&s_m);
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_m);
    pthread_join(s_thread, NULL);
    s_started = 0;
    vita2d_wait_rendering_done();
    for (int i = 0; i < MAX_ENTRIES; i++) entry_free(&s_e[i]);
}

vita2d_texture *thumbs_get(const char *url)
{
    if (!url || !*url || !s_started) return NULL;
    s_clock++;
    pthread_mutex_lock(&s_m);
    Entry *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        Entry *e = &s_e[i];
        if (!e->url) { if (!free_slot) free_slot = e; continue; }
        if (!strcmp(e->url, url)) {
            e->last_used = s_clock;
            if (e->st == T_PENDING) e->requested = s_clock;   /* sichtbar -> bevorzugen */
            vita2d_texture *t = e->st == T_DONE ? e->tex : NULL;
            pthread_mutex_unlock(&s_m);
            return t;
        }
        if (e->st != T_LOADING && (!oldest || e->last_used < oldest->last_used)) oldest = e;
    }
    Entry *e = free_slot;
    if (!e && oldest) {
        if (oldest->tex) vita2d_wait_rendering_done();
        entry_free(oldest);
        e = oldest;
    }
    if (e) {
        e->url = strdup(url);
        e->st = T_PENDING;
        e->last_used = e->requested = s_clock;
        pthread_cond_signal(&s_cv);
    }
    pthread_mutex_unlock(&s_m);
    return NULL;
}

void thumbs_drop_pending(void)
{
    pthread_mutex_lock(&s_m);
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (s_e[i].url && s_e[i].st == T_PENDING) entry_free(&s_e[i]);
    pthread_mutex_unlock(&s_m);
}

void thumbs_tick(void)
{
    pthread_mutex_lock(&s_m);
    int textures = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        Entry *e = &s_e[i];
        if (e->st == T_DECODED) {
            e->tex = vita2d_create_empty_texture(e->w, e->h);
            if (e->tex) {
                uint8_t *dst = vita2d_texture_get_datap(e->tex);
                int stride = vita2d_texture_get_stride(e->tex);
                for (int y = 0; y < e->h; y++) memcpy(dst + (size_t)y * stride, e->rgba + (size_t)y * e->w * 4, e->w * 4);
                vita2d_texture_set_filters(e->tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
                e->st = T_DONE;
            } else {
                e->st = T_FAILED;
            }
            free(e->rgba);
            e->rgba = NULL;
        }
        if (e->tex) textures++;
    }
    /* zu viele Texturen: die am längsten unbenutzten freigeben */
    if (textures > MAX_TEXTURES) {
        vita2d_wait_rendering_done();
        while (textures > MAX_TEXTURES - 8) {
            Entry *oldest = NULL;
            for (int i = 0; i < MAX_ENTRIES; i++)
                if (s_e[i].tex && (!oldest || s_e[i].last_used < oldest->last_used)) oldest = &s_e[i];
            if (!oldest) break;
            entry_free(oldest);
            textures--;
        }
    }
    pthread_mutex_unlock(&s_m);
}
