#include "harbor/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libssh2.h>
#include <sodium.h>

const char *hb_strerror(hb_err e)
{
    switch (e) {
    case HB_OK:            return "ok";
    case HB_ERR_NOMEM:     return "out of memory";
    case HB_ERR_IO:        return "i/o error";
    case HB_ERR_DB:        return "database error";
    case HB_ERR_CRYPTO:    return "crypto error";
    case HB_ERR_NOT_FOUND: return "not found";
    case HB_ERR_INVALID:   return "invalid argument";
    case HB_ERR_EXISTS:    return "already exists";
    case HB_ERR_PASSPHRASE:return "wrong passphrase";
    }
    return "unknown error";
}

int hb_core_init(void)
{
    static int inited;
    if (inited)
        return 0;
    if (sodium_init() < 0)
        return -1;
    if (libssh2_init(0) != 0)
        return -1;
    inited = 1;
    return 0;
}

void hb_uuid(char out[37])
{
    uint8_t b[16];
    if (hb_core_init() != 0) {
        memset(out, '0', 36);
        out[36] = 0;
        return;
    }
    randombytes_buf(b, sizeof b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

int64_t hb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

char *hb_secret_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    if (hb_core_init() != 0)
        return NULL;
    n = strlen(s) + 1;
    p = sodium_malloc(n);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    return p;
}

void hb_secret_free(char *s)
{
    if (!s)
        return;
    sodium_free(s);
}

void hb_strfreev(char **v, size_t n)
{
    if (!v)
        return;
    for (size_t i = 0; i < n; i++)
        free(v[i]);
    free(v);
}
