/* Wiedergabe-Fortschritt und Verlauf "Zuletzt gesehen" */
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRESS_MAX 500

typedef struct { char *key; int64_t pos, dur; } Prog;

static Prog      s_prog[PROGRESS_MAX];
static int       s_nprog;
static HistEntry *s_hist;          /* HISTORY_MAX Einträge */
static int       s_nhist;
static char      s_prog_path[256], s_hist_path[256];

/* ---------------------------------------------------------------- Dateien */

/* Tabs/Zeilenumbrüche in Feldern: als \t bzw. \n schreiben */
static void put_field(FILE *f, const char *s, int last)
{
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", f);
        else if (*s == '\t') fputs("\\t", f);
        else if (*s == '\\') fputs("\\\\", f);
        else if (*s != '\r') fputc(*s, f);
    }
    fputc(last ? '\n' : '\t', f);
}

/* nächstes Feld aus *p lesen (bis Tab/Zeilenende), Escapes auflösen */
static void get_field(char **p, char *out, size_t n)
{
    size_t j = 0;
    char *s = *p;
    while (*s && *s != '\t' && *s != '\n' && *s != '\r') {
        char c = *s++;
        if (c == '\\' && *s) {
            char e = *s++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
        if (j + 1 < n) out[j++] = c;
    }
    out[j] = 0;
    if (*s == '\t') s++;
    *p = s;
}

static void save_progress(void)
{
    FILE *f = fopen(s_prog_path, "w");
    if (!f) return;
    for (int i = 0; i < s_nprog; i++)
        fprintf(f, "%lld\t%lld\t%s\n", (long long)s_prog[i].pos, (long long)s_prog[i].dur, s_prog[i].key);
    fclose(f);
}

static void save_history(void)
{
    FILE *f = fopen(s_hist_path, "w");
    if (!f) return;
    for (int i = 0; i < s_nhist; i++) {
        HistEntry *e = &s_hist[i];
        put_field(f, e->src, 0); put_field(f, e->id, 0); put_field(f, e->key, 0);
        put_field(f, e->title, 0); put_field(f, e->thumb, 0); put_field(f, e->url, 0);
        put_field(f, e->headers, 0);
        fprintf(f, "%d\n", e->save_ref);
    }
    fclose(f);
}

void history_load(const char *dir)
{
    snprintf(s_prog_path, sizeof s_prog_path, "%s/progress.txt", dir);
    snprintf(s_hist_path, sizeof s_hist_path, "%s/history.txt", dir);
    if (!s_hist) s_hist = calloc(HISTORY_MAX, sizeof *s_hist);
    for (int i = 0; i < s_nprog; i++) free(s_prog[i].key);
    s_nprog = s_nhist = 0;

    char *line = malloc(8192);
    if (!line) return;
    FILE *f = fopen(s_prog_path, "r");
    if (f) {
        while (s_nprog < PROGRESS_MAX && fgets(line, 8192, f)) {
            long long pos, dur;
            int off = 0;
            if (sscanf(line, "%lld\t%lld\t%n", &pos, &dur, &off) < 2 || !off) continue;
            char *k = line + off;
            k[strcspn(k, "\r\n")] = 0;
            if (!*k) continue;
            s_prog[s_nprog].key = strdup(k);
            s_prog[s_nprog].pos = pos;
            s_prog[s_nprog].dur = dur;
            s_nprog++;
        }
        fclose(f);
    }
    f = s_hist ? fopen(s_hist_path, "r") : NULL;
    if (f) {
        while (s_nhist < HISTORY_MAX && fgets(line, 8192, f)) {
            HistEntry *e = &s_hist[s_nhist];
            memset(e, 0, sizeof *e);
            char *p = line, num[8];
            get_field(&p, e->src, sizeof e->src);
            get_field(&p, e->id, sizeof e->id);
            get_field(&p, e->key, sizeof e->key);
            get_field(&p, e->title, sizeof e->title);
            get_field(&p, e->thumb, sizeof e->thumb);
            get_field(&p, e->url, sizeof e->url);
            get_field(&p, e->headers, sizeof e->headers);
            get_field(&p, num, sizeof num);
            e->save_ref = atoi(num);
            if (e->src[0] && (e->url[0] || e->id[0])) s_nhist++;
        }
        fclose(f);
    }
    free(line);
}

/* ---------------------------------------------------------------- Fortschritt */

static int find_prog(const char *key)
{
    for (int i = 0; i < s_nprog; i++) if (!strcmp(s_prog[i].key, key)) return i;
    return -1;
}

int progress_get(const char *key, int64_t *pos, int64_t *dur)
{
    if (!key || !*key) return 0;
    int i = find_prog(key);
    if (i < 0) return 0;
    if (pos) *pos = s_prog[i].pos;
    if (dur) *dur = s_prog[i].dur;
    return 1;
}

void progress_set(const char *key, int64_t pos, int64_t dur)
{
    if (!key || !*key) return;
    int i = find_prog(key);
    if (i < 0) {
        if (s_nprog == PROGRESS_MAX) {          /* ältesten (vorn) verwerfen */
            free(s_prog[0].key);
            memmove(s_prog, s_prog + 1, (PROGRESS_MAX - 1) * sizeof *s_prog);
            s_nprog--;
        }
        i = s_nprog++;
        s_prog[i].key = strdup(key);
    } else if (i != s_nprog - 1) {              /* zuletzt benutzte ans Ende */
        Prog p = s_prog[i];
        memmove(s_prog + i, s_prog + i + 1, (s_nprog - i - 1) * sizeof *s_prog);
        i = s_nprog - 1;
        s_prog[i] = p;
    }
    s_prog[i].pos = pos;
    s_prog[i].dur = dur;
    save_progress();
}

void progress_clear(const char *key)
{
    int i = key ? find_prog(key) : -1;
    if (i < 0) return;
    free(s_prog[i].key);
    memmove(s_prog + i, s_prog + i + 1, (s_nprog - i - 1) * sizeof *s_prog);
    s_nprog--;
    save_progress();
}

int progress_percent(const char *key)
{
    int64_t pos, dur;
    if (!progress_get(key, &pos, &dur) || dur <= 0 || pos <= 0) return 0;
    int p = (int)(pos * 100 / dur);
    return p < 1 ? 1 : p > 99 ? 99 : p;
}

/* ---------------------------------------------------------------- Verlauf */

void history_add(const HistEntry *e)
{
    if (!s_hist) s_hist = calloc(HISTORY_MAX, sizeof *s_hist);
    if (!s_hist) return;
    HistEntry copy = *e;
    for (int i = 0; i < s_nhist; i++) {
        if (!strcmp(s_hist[i].src, e->src) && !strcmp(s_hist[i].id, e->id)) {
            memmove(s_hist + i, s_hist + i + 1, (s_nhist - i - 1) * sizeof *s_hist);
            s_nhist--;
            break;
        }
    }
    if (s_nhist == HISTORY_MAX) s_nhist--;
    memmove(s_hist + 1, s_hist, s_nhist * sizeof *s_hist);
    s_hist[0] = copy;
    s_nhist++;
    save_history();
}

int history_count(void) { return s_nhist; }
const HistEntry *history_get(int i) { return (i >= 0 && i < s_nhist) ? &s_hist[i] : NULL; }

void history_remove(int i)
{
    if (i < 0 || i >= s_nhist) return;
    memmove(s_hist + i, s_hist + i + 1, (s_nhist - i - 1) * sizeof *s_hist);
    s_nhist--;
    save_history();
}

void history_clear(void)
{
    s_nhist = 0;
    save_history();
}
