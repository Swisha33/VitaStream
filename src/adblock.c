#include "adblock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char **v;
    int    n, cap;
    int    sorted;
} StrSet;

static StrSet s_block, s_allow;

static void set_add(StrSet *s, const char *str)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->v = realloc(s->v, sizeof(char *) * s->cap);
    }
    s->v[s->n++] = strdup(str);
    s->sorted = 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void set_sort(StrSet *s)
{
    if (s->sorted || s->n == 0) { s->sorted = 1; return; }
    qsort(s->v, s->n, sizeof(char *), cmp_str);
    /* Duplikate entfernen */
    int w = 1;
    for (int r = 1; r < s->n; r++) {
        if (strcmp(s->v[r], s->v[w - 1])) s->v[w++] = s->v[r];
        else free(s->v[r]);
    }
    s->n = w;
    s->sorted = 1;
}

static int set_has(StrSet *s, const char *str)
{
    if (s->n == 0) return 0;
    if (!s->sorted) set_sort(s);
    return bsearch(&str, s->v, s->n, sizeof(char *), cmp_str) != NULL;
}

static void set_free(StrSet *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof *s);
}

void adblock_clear(void)
{
    set_free(&s_block);
    set_free(&s_allow);
}

int adblock_rule_count(void) { return s_block.n; }

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static int valid_domain(const char *d)
{
    if (!*d || !strchr(d, '.')) return 0;
    for (; *d; d++)
        if (!(isalnum((unsigned char)*d) || *d == '.' || *d == '-' || *d == '_')) return 0;
    return 1;
}

static int parse_line(char *line, int *is_allow, char *out, int outlen)
{
    char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (!*p || *p == '#' || *p == '!' || *p == '[') return 0;

    *is_allow = 0;
    if (!strncmp(p, "@@", 2)) { *is_allow = 1; p += 2; }

    if (!strncmp(p, "||", 2)) {
        p += 2;
        char *end = p;
        while (*end && *end != '^' && *end != '/' && *end != '$' && !isspace((unsigned char)*end)) end++;
        /* Regeln mit Optionen ($third-party usw.) oder Pfaden nicht als Domainregel werten */
        if (*end == '$' || *end == '/') return 0;
        if (*end == '^' && end[1] && end[1] != '\n' && end[1] != '\r' && !isspace((unsigned char)end[1])) return 0;
        *end = 0;
    } else {
        /* hosts-Format: "IP domain" */
        char *first = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p++ = 0;
            while (isspace((unsigned char)*p)) p++;
            if (!*p || *p == '#') {
                /* nur eine Domain in der Zeile */
            } else if (!strcmp(first, "0.0.0.0") || !strcmp(first, "127.0.0.1") || !strcmp(first, "::")
                || !strcmp(first, "::1")) {
                first = p;
                while (*p && !isspace((unsigned char)*p) && *p != '#') p++;
                *p = 0;
            } else {
                return 0;
            }
        }
        p = first;
    }

    if (*p == '.') p++;
    if ((int)strlen(p) >= outlen) return 0;
    strcpy(out, p);
    lower(out);
    if (!strcmp(out, "localhost") || !strcmp(out, "0.0.0.0")) return 0;
    return valid_domain(out);
}

int adblock_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512], dom[256];
    int count = 0, allow;
    while (fgets(line, sizeof line, f)) {
        if (parse_line(line, &allow, dom, sizeof dom)) {
            set_add(allow ? &s_allow : &s_block, dom);
            count++;
        }
    }
    fclose(f);
    set_sort(&s_block);
    set_sort(&s_allow);
    return count;
}

/* Prüft host und alle übergeordneten Domains: a.b.c.com -> b.c.com -> c.com -> com */
static int match_suffix(StrSet *s, const char *host)
{
    for (const char *h = host; h && *h; ) {
        if (set_has(s, h)) return 1;
        h = strchr(h, '.');
        if (h) h++;
    }
    return 0;
}

int adblock_is_blocked(const char *host_in)
{
    if (!host_in || !*host_in || s_block.n == 0) return 0;
    char host[256];
    snprintf(host, sizeof host, "%s", host_in);
    lower(host);
    size_t L = strlen(host);
    if (L && host[L - 1] == '.') host[L - 1] = 0;
    if (match_suffix(&s_allow, host)) return 0;
    return match_suffix(&s_block, host);
}

int adblock_host_from_url(const char *url, char *out, int outlen)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = NULL;
    for (const char *q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++)
        if (*q == '@') at = q;
    if (at) p = at + 1;

    int n = 0;
    if (*p == '[') { /* IPv6-Literal */
        p++;
        while (*p && *p != ']' && n < outlen - 1) out[n++] = *p++;
    } else {
        while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#' && n < outlen - 1)
            out[n++] = (char)tolower((unsigned char)*p++);
    }
    out[n] = 0;
    return n;
}
