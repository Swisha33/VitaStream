/* Download-Verwaltung (siehe dl.h) */
#include "dl.h"
#include "net.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <pthread.h>
#include <sys/stat.h>

static struct {
    pthread_t        t;
    int              started;
    volatile int     running, abort;
    volatile int64_t done, total;
    int              result;          /* NET_* nach Ende */
    char             title[128];
    char             path[512];
    char            *url, *headers;
    int              show_frames;     /* Ergebnis noch so lange anzeigen */
    char             err[160];        /* Fehlerdetail aus dem Download-Thread */
    char             subdir[32], fname[200];
    int              finished_unseen;
    char             status[200];
} D;

void dl_filename(const char *title, const char *url, char *out, int n)
{
    /* Endung aus der URL (ohne Query), sonst .mp4 */
    const char *ext = ".mp4";
    static const char *known[] = { ".mp4", ".m4v", ".mov", ".mp3", ".m4a", ".aac", ".ts" };
    const char *q = url + strcspn(url, "?#");
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++) {
        size_t l = strlen(known[i]);
        if (q - url >= (long)l && !strncasecmp(q - l, known[i], l)) { ext = known[i]; break; }
    }
    int j = 0, last_us = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p && j < n - 12; p++) {
        int ok = isalnum(*p) || *p >= 0x80 || *p == '-' || *p == '.' || *p == '(' || *p == ')';
        if (ok && !(j == 0 && *p == '.')) { out[j++] = (char)*p; last_us = 0; }
        else if (!last_us && j) { out[j++] = '_'; last_us = 1; }
    }
    while (j && (out[j - 1] == '_' || out[j - 1] == '.')) j--;
    if (!j) { memcpy(out, "video", 5); j = 5; }
    snprintf(out + j, n - j, "%s", ext);
}

static void *worker(void *arg)
{
    (void)arg;
    D.result = net_download(D.url, D.headers, D.path, &D.abort, &D.done, &D.total);
    snprintf(D.err, sizeof D.err, "%s", net_last_detail());   /* Detail ist pro Thread */
    if (D.result == NET_OK) D.finished_unseen = 1;
    D.running = 0;
    return NULL;
}

int dl_start_to(const char *url, const char *headers, const char *title, const char *subdir, const char *filename)
{
    if (D.running) return -1;
    if (D.started) { pthread_join(D.t, NULL); D.started = 0; }
    free(D.url); free(D.headers);
    if (!subdir || !*subdir || strstr(subdir, "..") || strchr(subdir, '/')) subdir = "downloads";
    char dir[300];
    snprintf(dir, sizeof dir, VS_DATA_DIR "/%s", subdir);
    mkdir(dir, 0777);
    char name[200];
    if (filename && *filename && !strstr(filename, "..") && !strchr(filename, '/')) snprintf(name, sizeof name, "%s", filename);
    else dl_filename(title, url, name, sizeof name);
    snprintf(D.path, sizeof D.path, "%s/%s", dir, name);
    snprintf(D.subdir, sizeof D.subdir, "%s", subdir);
    snprintf(D.fname, sizeof D.fname, "%s", name);
    snprintf(D.title, sizeof D.title, "%s", title);
    D.url = strdup(url);
    D.headers = headers && *headers ? strdup(headers) : NULL;
    D.abort = 0;
    D.done = D.total = 0;
    D.result = NET_ERR;
    D.finished_unseen = 0;
    D.running = 1;
    D.show_frames = 360;
    if (pthread_create(&D.t, NULL, worker, NULL) != 0) { D.running = 0; return -2; }
    D.started = 1;
    return 0;
}

int dl_start(const char *url, const char *headers, const char *title)
{
    return dl_start_to(url, headers, title, "downloads", NULL);
}

int dl_take_finished(char *subdir, int sn, char *name, int nn)
{
    if (D.running || !D.finished_unseen) return 0;
    D.finished_unseen = 0;
    snprintf(subdir, sn, "%s", D.subdir);
    snprintf(name, nn, "%s", D.fname);
    return 1;
}

int  dl_active(void) { return D.running; }
void dl_cancel(void) { if (D.running) D.abort = 1; }

const char *dl_status(void)
{
    if (D.running) {
        double mb = D.done / 1048576.0;
        if (D.total > 0)
            snprintf(D.status, sizeof D.status, "Download %d%%  %.0f/%.0f MB  -  %s",
                     (int)(D.done * 100 / D.total), mb, D.total / 1048576.0, D.title);
        else
            snprintf(D.status, sizeof D.status, "Download %.1f MB  -  %s", mb, D.title);
        D.show_frames = 360;   /* nach dem Ende ca. 6 s Ergebnis zeigen */
        return D.status;
    }
    if (D.started && D.show_frames > 0) {
        if (D.show_frames == 360) {
            if (D.result == NET_OK && !strcmp(D.subdir, "music")) snprintf(D.status, sizeof D.status, "Menuemusik geladen: %s", D.title);
            else if (D.result == NET_OK) snprintf(D.status, sizeof D.status, "Download fertig: %s (Quelle \"Downloads\")", D.title);
            else if (D.result == NET_ABORTED) snprintf(D.status, sizeof D.status, "Download abgebrochen");
            else snprintf(D.status, sizeof D.status, "Download fehlgeschlagen: %s", D.err[0] ? D.err : "Netzwerkfehler");
        }
        D.show_frames--;
        return D.status;
    }
    return "";
}

void dl_shutdown(void)
{
    if (D.started) {
        D.abort = 1;
        pthread_join(D.t, NULL);
        D.started = 0;
    }
    free(D.url); free(D.headers);
    D.url = D.headers = NULL;
}
