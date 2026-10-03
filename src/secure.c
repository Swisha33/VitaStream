/* Verschlüsselte Ablage für Anmelde-Token (siehe secure.h) */
#include "secure.h"
#include "config.h"
#include "../third_party/aes/aes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/kernel/openpsid.h>
#include <psp2/kernel/processmgr.h>
#endif

#define MAGIC "VSEC1:"

static char s_dir[256] = VS_DATA_DIR "/.secure";
static unsigned char s_devid[16];
static int s_have_id;

void secure_set_dir(const char *dir) { snprintf(s_dir, sizeof s_dir, "%s", dir); }
void secure_set_device_id(const unsigned char id[16]) { memcpy(s_devid, id, 16); s_have_id = 1; }

static void device_key(uint8_t key[16])
{
    if (!s_have_id) {
#ifdef __vita__
        SceKernelOpenPsId id;
        memset(&id, 0, sizeof id);
        sceKernelGetOpenPsId(&id);
        memcpy(s_devid, &id, 16);
#else
        memcpy(s_devid, "host-test-device", 16);
#endif
        s_have_id = 1;
    }
    /* Schlüssel = AES(Geräte-ID, fester Block): nie die ID selbst als Schlüssel verwenden */
    struct AES_ctx c;
    AES_init_ctx(&c, s_devid);
    memcpy(key, "VitaStream-Key-1", 16);
    AES_ECB_encrypt(&c, key);
}

static int valid_name(const char *n)
{
    size_t l = strlen(n);
    if (!l || l > 40) return 0;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') || *n == '_')) return 0;
    return 1;
}

static void path_for(const char *name, char *out, size_t n) { snprintf(out, n, "%s/%s.bin", s_dir, name); }

static void make_iv(uint8_t iv[16])
{
    static unsigned counter;
    uint64_t t;
#ifdef __vita__
    t = sceKernelGetProcessTimeWide();
#else
    t = (uint64_t)time(NULL) * 1000003u + (uint64_t)clock();
#endif
    t ^= (uint64_t)(uintptr_t)iv;
    for (int i = 0; i < 16; i++) {
        t = t * 6364136223846793005ULL + 1442695040888963407ULL + (++counter);
        iv[i] = (uint8_t)(t >> 56);
    }
}

int secure_put(const char *name, const char *value)
{
    if (!valid_name(name)) return -1;
    char path[300];
    path_for(name, path, sizeof path);
    if (!value) { remove(path); return 0; }
    mkdir(s_dir, 0777);

    size_t vl = strlen(value), ml = strlen(MAGIC);
    uint8_t *buf = malloc(ml + vl);
    if (!buf) return -1;
    memcpy(buf, MAGIC, ml);
    memcpy(buf + ml, value, vl);
    uint8_t key[16], iv[16];
    device_key(key);
    make_iv(iv);
    struct AES_ctx c;
    AES_init_ctx_iv(&c, key, iv);
    AES_CTR_xcrypt_buffer(&c, buf, ml + vl);

    FILE *f = fopen(path, "w");
    if (!f) { free(buf); return -1; }
    for (int i = 0; i < 16; i++) fprintf(f, "%02x", iv[i]);
    for (size_t i = 0; i < ml + vl; i++) fprintf(f, "%02x", buf[i]);
    fputc('\n', f);
    fclose(f);
    memset(buf, 0, ml + vl);
    free(buf);
    memset(key, 0, sizeof key);
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *secure_get(const char *name)
{
    if (!valid_name(name)) return NULL;
    char path[300];
    path_for(name, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 34 || sz > 65536) { fclose(f); return NULL; }
    char *hex = malloc(sz + 1);
    size_t n = hex ? fread(hex, 1, sz, f) : 0;
    fclose(f);
    if (!hex) return NULL;
    while (n && (hex[n - 1] == '\n' || hex[n - 1] == '\r')) n--;
    if (n < 34 || n % 2) { free(hex); return NULL; }
    size_t bl = n / 2;
    uint8_t *bin = malloc(bl + 1);
    if (!bin) { free(hex); return NULL; }
    for (size_t i = 0; i < bl; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) { free(hex); free(bin); return NULL; }
        bin[i] = (uint8_t)(a * 16 + b);
    }
    free(hex);
    uint8_t key[16];
    device_key(key);
    struct AES_ctx c;
    AES_init_ctx_iv(&c, key, bin);
    AES_CTR_xcrypt_buffer(&c, bin + 16, bl - 16);
    memset(key, 0, sizeof key);
    size_t ml = strlen(MAGIC);
    if (bl - 16 < ml || memcmp(bin + 16, MAGIC, ml)) { free(bin); return NULL; }   /* anderes Gerät/kaputt */
    size_t vl = bl - 16 - ml;
    char *out = malloc(vl + 1);
    if (out) { memcpy(out, bin + 16 + ml, vl); out[vl] = 0; }
    memset(bin, 0, bl);
    free(bin);
    return out;
}
