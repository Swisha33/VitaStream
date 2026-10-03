#ifndef VS_SECURE_H
#define VS_SECURE_H

/* Geheimnisse (z. B. Anmelde-Token) verschlüsselt ablegen: AES-128-CTR mit einem Schlüssel,
 * der aus der konsolenspezifischen OpenPSID abgeleitet wird. Die Dateien liegen in
 * ux0:data/VitaStream/.secure/ und sind auf einem anderen Gerät oder per FTP unlesbar.
 * Passwörter selbst werden nie gespeichert. */

/* name: nur Buchstaben, Ziffern, '_' (max. 40 Zeichen). value NULL = löschen. 0 = ok */
int secure_put(const char *name, const char *value);
/* Liefert den Wert (malloc, vom Aufrufer freizugeben) oder NULL */
char *secure_get(const char *name);

/* intern/Tests: anderes Verzeichnis und feste Geräte-ID */
void secure_set_dir(const char *dir);
void secure_set_device_id(const unsigned char id[16]);

#endif
