#include "watched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char   **s_tab;       /* offene Adressierung */
static size_t   s_cap, s_n;
static char     s_path[256];

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static size_t find(const char *key)
{
    size_t i = hash(key) & (s_cap - 1);
    while (s_tab[i] && strcmp(s_tab[i], key)) i = (i + 1) & (s_cap - 1);
    return i;
}

static void grow(void)
{
    size_t old_cap = s_cap;
    char **old = s_tab;
    s_cap = s_cap ? s_cap * 2 : 1024;
    s_tab = calloc(s_cap, sizeof(char *));
    for (size_t i = 0; i < old_cap; i++)
        if (old[i]) s_tab[find(old[i])] = old[i];
    free(old);
}

static void insert(const char *key)
{
    if ((s_n + 1) * 2 > s_cap) grow();
    size_t i = find(key);
    if (!s_tab[i]) { s_tab[i] = strdup(key); s_n++; }
}

static void save(void)
{
    if (!s_path[0]) return;
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    for (size_t i = 0; i < s_cap; i++) if (s_tab[i]) fprintf(f, "%s\n", s_tab[i]);
    fclose(f);
}

void watched_load(const char *path)
{
    snprintf(s_path, sizeof s_path, "%s", path);
    if (!s_cap) grow();
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2304];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) insert(line);
    }
    fclose(f);
}

int watched_get(const char *key)
{
    if (!s_cap || !key || !*key) return 0;
    return s_tab[find(key)] != NULL;
}

void watched_set(const char *key, int on)
{
    if (!key || !*key) return;
    if (!s_cap) grow();
    if (on) {
        if (watched_get(key)) return;
        insert(key);
    } else {
        size_t i = find(key);
        if (!s_tab[i]) return;
        /* Löschen bei offener Adressierung: Tabelle neu aufbauen */
        free(s_tab[i]);
        s_tab[i] = NULL;
        s_n--;
        char **old = s_tab;
        size_t cap = s_cap;
        s_tab = calloc(s_cap, sizeof(char *));
        for (size_t k = 0; k < cap; k++) if (old[k]) s_tab[find(old[k])] = old[k];
        free(old);
    }
    save();
}

int watched_count(void) { return (int)s_n; }
