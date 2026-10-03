#ifndef VS_DL_H
#define VS_DL_H

/* Download im Hintergrund nach ux0:data/VitaStream/downloads/ (eine Datei zur Zeit). */

/* 0 = gestartet, -1 = es läuft schon einer, -2 = Ordner/Thread-Fehler */
int  dl_start(const char *url, const char *headers, const char *title);
int  dl_active(void);
void dl_cancel(void);
/* Kurztext für die Statuszeile ("" = nichts anzuzeigen). Pro Frame aufrufen. */
const char *dl_status(void);
void dl_shutdown(void);

/* intern/Tests: Dateiname aus Titel und URL */
void dl_filename(const char *title, const char *url, char *out, int n);

#endif
