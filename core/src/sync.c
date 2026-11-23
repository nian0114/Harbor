#include "harbor/sync.h"

#include "obj_priv.h"
#include "vault_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

/* bip39_words.c（bitcoin/bips bip-0039/english.txt，
 * SHA256 2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda） */
extern const char *const hb__bip39_words[2048];

/* ================================ BIP-39 ================================ */

/* 264 位流（E||SHA256(E)[0]，大端）第 bit 位取值/置位。 */
static int bit_at(const uint8_t *b, int bit)
{
    return (b[bit / 8] >> (7 - bit % 8)) & 1;
}

static void bit_set(uint8_t *b, int bit, int v)
{
    if (v)
        b[bit / 8] |= (uint8_t)(1 << (7 - bit % 8));
}

int hb_bip39_encode(const uint8_t ent[32], char *out, size_t out_sz)
{
    uint8_t buf[33], h[crypto_hash_sha256_BYTES];
    size_t pos = 0;
    int w;

    crypto_hash_sha256(h, ent, 32);
    memcpy(buf, ent, 32);
    buf[32] = h[0];
    sodium_memzero(h, sizeof h);
    for (w = 0; w < 24; w++) {
        unsigned idx = 0;
        size_t n;
        int i;
        for (i = 0; i < 11; i++)
            idx = (idx << 1) | (unsigned)bit_at(buf, w * 11 + i);
        n = strlen(hb__bip39_words[idx]);
        if (pos + n + 2 > out_sz)
            return -1;
        if (pos)
            out[pos++] = ' ';
        memcpy(out + pos, hb__bip39_words[idx], n);
        pos += n;
    }
    out[pos] = 0;
    sodium_memzero(buf, sizeof buf);
    return 0;
}

/* 词表二分查找（BIP-39 英文表按字典序排列）。 */
static int word_index(const char *w, size_t len)
{
    int lo = 0, hi = 2047;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(w, hb__bip39_words[mid], len);
        if (c == 0 && hb__bip39_words[mid][len] != 0)
            c = -1;
        if (c < 0)
            hi = mid - 1;
        else if (c > 0)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

int hb_bip39_decode(const char *words, uint8_t ent[32])
{
    uint8_t buf[33], h[crypto_hash_sha256_BYTES];
    int w = 0;
    const char *p = words;

    memset(buf, 0, sizeof buf);
    while (*p) {
        const char *start;
        size_t len, i;
        char low[16];
        int idx, b;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        len = (size_t)(p - start);
        if (len >= sizeof low || w >= 24)
            return -1;
        for (i = 0; i < len; i++)
            low[i] = (char)tolower((unsigned char)start[i]);
        low[len] = 0;
        idx = word_index(low, len);
        if (idx < 0)
            return -1;
        for (i = 0, b = w * 11; i < 11; i++)
            bit_set(buf, b + (int)i, (idx >> (10 - i)) & 1);
        w++;
    }
    if (w != 24)
        return -1;
    memcpy(ent, buf, 32);
    crypto_hash_sha256(h, ent, 32);
    if (h[0] != buf[32]) {
        sodium_memzero(h, sizeof h);
        sodium_memzero(buf, sizeof buf);
        sodium_memzero(ent, 32);
        return -1;
    }
    sodium_memzero(h, sizeof h);
    sodium_memzero(buf, sizeof buf);
    return 0;
}

/* ============================ base64url ============================ */

static char *b64_enc(const uint8_t *bin, size_t n)
{
    size_t blen = sodium_base64_encoded_len(n, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char *s = malloc(blen);
    if (!s)
        return NULL;
    sodium_bin2base64(s, blen, bin, n, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return s;
}

static uint8_t *b64_dec(const char *s, size_t *n)
{
    size_t len = strlen(s);
    uint8_t *bin = sodium_malloc(len / 4 * 3 + 4);
    size_t bin_len = 0;
    if (!bin)
        return NULL;
    if (sodium_base642bin(bin, len / 4 * 3 + 4, s, len, NULL, &bin_len, NULL,
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        sodium_free(bin);
        return NULL;
    }
    *n = bin_len;
    return bin;
}

/* ============================ seal / open ============================ */

uint8_t *hb_sync_seal(const uint8_t key[32], const uint8_t *pt, size_t pt_len,
                      const uint8_t *ad, size_t ad_len, size_t *out_len)
{
    size_t n = 24 + pt_len + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    uint8_t *buf = sodium_malloc(n);
    if (!buf)
        return NULL;
    randombytes_buf(buf, 24);
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        buf + 24, NULL, pt, pt_len, ad, ad_len, NULL, buf, key);
    *out_len = n;
    return buf;
}

uint8_t *hb_sync_open(const uint8_t key[32], const uint8_t *blob, size_t n,
                      const uint8_t *ad, size_t ad_len, size_t *pt_len)
{
    uint8_t *pt;
    unsigned long long pt_n = 0;
    if (n < 24 + crypto_aead_xchacha20poly1305_ietf_ABYTES)
        return NULL;
    pt = sodium_malloc(n - 24 - crypto_aead_xchacha20poly1305_ietf_ABYTES + 1);
    if (!pt)
        return NULL;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            pt, &pt_n, NULL, blob + 24,
            n - 24, ad, ad_len, blob, key) != 0) {
        sodium_free(pt);
        return NULL;
    }
    *pt_len = (size_t)pt_n;
    return pt;
}

/* ============================ KDF 派生 ============================ */

hb_err hb_sync_pwhash(const char *password, const uint8_t salt[16],
                      uint64_t ops, uint64_t mem, uint8_t mk[32])
{
    if (crypto_pwhash(mk, 32, password, strlen(password), salt,
                      ops, (size_t)mem, crypto_pwhash_ALG_ARGON2ID13) != 0)
        return HB_ERR_CRYPTO;
    return HB_OK;
}

void hb_sync_derive_mk(const uint8_t mk[32], uint8_t auth_key[32], uint8_t wrap_key[32])
{
    crypto_kdf_derive_from_key(auth_key, 32, 1, "hbauth01", mk);
    crypto_kdf_derive_from_key(wrap_key, 32, 2, "hbwrap01", mk);
}

void hb_sync_derive_rc(const uint8_t E[32], uint8_t rc_wrap[32], uint8_t rc_auth[32])
{
    crypto_kdf_derive_from_key(rc_wrap, 32, 1, "hbrcwrap", E);
    crypto_kdf_derive_from_key(rc_auth, 32, 2, "hbrcauth", E);
}

/* ============================ JSON 小工具 ============================ */

static char *json_print_str(cJSON *j)
{
    return cJSON_PrintUnformatted(j);
}

static cJSON *json_obj(void)
{
    return cJSON_CreateObject();
}

static void json_add_device(cJSON *j, const char *name, const char *platform)
{
    cJSON *d = json_obj();
    cJSON_AddStringToObject(d, "name", name ? name : "");
    cJSON_AddStringToObject(d, "platform", platform ? platform : "");
    cJSON_AddItemToObject(j, "device", d);
}

/* VK 封装 AD */
#define AD_VK_PW "hb:vk:pw:v1"
#define AD_VK_RC "hb:vk:rc:v1"

static hb_err seal_to_b64(const uint8_t key[32], const uint8_t *pt, size_t n,
                          const char *ad, char **b64)
{
    size_t sl;
    uint8_t *sealed = hb_sync_seal(key, pt, n, (const uint8_t *)ad, strlen(ad), &sl);
    if (!sealed)
        return HB_ERR_NOMEM;
    *b64 = b64_enc(sealed, sl);
    sodium_free(sealed);
    return *b64 ? HB_OK : HB_ERR_NOMEM;
}

static hb_err unseal_b64(const uint8_t key[32], const char *b64,
                         const char *ad, uint8_t **pt, size_t *pt_len)
{
    size_t blen = 0;
    uint8_t *bin = b64_dec(b64, &blen);
    if (!bin)
        return HB_ERR_CRYPTO;
    *pt = hb_sync_open(key, bin, blen, (const uint8_t *)ad, strlen(ad), pt_len);
    sodium_free(bin);
    return *pt ? HB_OK : HB_ERR_CRYPTO;
}

/* ============================ 账号体 ============================ */

static uint64_t kdf_ops_or_def(uint64_t ops)
{
    return ops ? ops : crypto_pwhash_OPSLIMIT_MODERATE;
}

static uint64_t kdf_mem_or_def(uint64_t mem)
{
    return mem ? mem : crypto_pwhash_MEMLIMIT_MODERATE;
}

hb_err hb_sync_prelogin_body(const char *email, char **out)
{
    cJSON *j = json_obj();
    *out = NULL;
    if (!j)
        return HB_ERR_NOMEM;
    cJSON_AddStringToObject(j, "email", email);
    *out = json_print_str(j);
    cJSON_Delete(j);
    return *out ? HB_OK : HB_ERR_NOMEM;
}

hb_err hb_sync_prelogin_parse(const char *resp, uint8_t salt[16],
                              uint64_t *ops, uint64_t *mem)
{
    cJSON *j = cJSON_Parse(resp), *k, *s, *o, *m;
    uint8_t *bin = NULL;
    size_t blen = 0;
    hb_err e = HB_ERR_INVALID;

    if (!j)
        return HB_ERR_INVALID;
    k = cJSON_GetObjectItemCaseSensitive(j, "kdf");
    s = k ? cJSON_GetObjectItemCaseSensitive(k, "salt") : NULL;
    o = k ? cJSON_GetObjectItemCaseSensitive(k, "ops") : NULL;
    m = k ? cJSON_GetObjectItemCaseSensitive(k, "mem") : NULL;
    if (!cJSON_IsString(s) || !cJSON_IsNumber(o) || !cJSON_IsNumber(m))
        goto out;
    bin = b64_dec(s->valuestring, &blen);
    if (!bin || blen != HB_SYNC_SALT_LEN)
        goto out;
    if (o->valuedouble < 2 || m->valuedouble < 64.0 * 1024 * 1024) {
        e = HB_ERR_CRYPTO; /* 拒绝低于协议下限的 KDF 参数 */
        goto out;
    }
    memcpy(salt, bin, HB_SYNC_SALT_LEN);
    *ops = (uint64_t)o->valuedouble;
    *mem = (uint64_t)m->valuedouble;
    e = HB_OK;
out:
    if (bin) {
        sodium_memzero(bin, blen);
        sodium_free(bin);
    }
    cJSON_Delete(j);
    return e;
}

hb_err hb_sync_register_body(const char *email, const char *password,
                             const char *dev_name, const char *dev_platform,
                             uint64_t kdf_ops, uint64_t kdf_mem,
                             char **body_out, char **words_out,
                             uint8_t vk[32])
{
    uint8_t salt[HB_SYNC_SALT_LEN], mk[32], auth[32], wrap[32];
    uint8_t E[32], rc_wrap[32], rc_auth[32];
    char *s_b64 = NULL, *a_b64 = NULL, *wp_b64 = NULL, *ra_b64 = NULL, *wr_b64 = NULL;
    char words[256];
    cJSON *j = NULL, *kdf;
    hb_err e = HB_ERR_NOMEM;

    *body_out = NULL;
    *words_out = NULL;
    kdf_ops = kdf_ops_or_def(kdf_ops);
    kdf_mem = kdf_mem_or_def(kdf_mem);
    randombytes_buf(salt, sizeof salt);
    randombytes_buf(vk, 32);
    randombytes_buf(E, sizeof E);

    e = hb_sync_pwhash(password, salt, kdf_ops, kdf_mem, mk);
    if (e != HB_OK)
        goto out;
    hb_sync_derive_mk(mk, auth, wrap);
    hb_sync_derive_rc(E, rc_wrap, rc_auth);
    if (hb_bip39_encode(E, words, sizeof words) != 0) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    s_b64 = b64_enc(salt, sizeof salt);
    a_b64 = b64_enc(auth, sizeof auth);
    ra_b64 = b64_enc(rc_auth, sizeof rc_auth);
    e = seal_to_b64(wrap, vk, 32, AD_VK_PW, &wp_b64);
    if (e != HB_OK)
        goto out;
    e = seal_to_b64(rc_wrap, vk, 32, AD_VK_RC, &wr_b64);
    if (e != HB_OK)
        goto out;
    if (!s_b64 || !a_b64 || !ra_b64) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    j = json_obj();
    if (!j)
        goto out;
    cJSON_AddStringToObject(j, "email", email);
    cJSON_AddStringToObject(j, "auth_key", a_b64);
    kdf = json_obj();
    cJSON_AddStringToObject(kdf, "salt", s_b64);
    cJSON_AddNumberToObject(kdf, "ops", (double)kdf_ops);
    cJSON_AddNumberToObject(kdf, "mem", (double)kdf_mem);
    cJSON_AddItemToObject(j, "kdf", kdf);
    cJSON_AddStringToObject(j, "wrapped_vk_pw", wp_b64);
    cJSON_AddStringToObject(j, "rc_auth", ra_b64);
    cJSON_AddStringToObject(j, "wrapped_vk_rc", wr_b64);
    json_add_device(j, dev_name, dev_platform);
    *body_out = json_print_str(j);
    *words_out = strdup(words);
    e = (*body_out && *words_out) ? HB_OK : HB_ERR_NOMEM;
out:
    cJSON_Delete(j);
    free(s_b64); free(a_b64); free(wp_b64); free(ra_b64); free(wr_b64);
    sodium_memzero(mk, sizeof mk);
    sodium_memzero(auth, sizeof auth);
    sodium_memzero(wrap, sizeof wrap);
    sodium_memzero(E, sizeof E);
    sodium_memzero(rc_wrap, sizeof rc_wrap);
    sodium_memzero(rc_auth, sizeof rc_auth);
    sodium_memzero(words, sizeof words);
    sodium_memzero(salt, sizeof salt);
    return e;
}

hb_err hb_sync_login_body(const char *email, const char *password,
                          const uint8_t salt[16], uint64_t ops, uint64_t mem,
                          const char *totp, const char *dev_name,
                          const char *dev_platform,
                          char **body_out, uint8_t wrap_key[32])
{
    uint8_t mk[32], auth[32];
    char *a_b64;
    cJSON *j;
    hb_err e;

    *body_out = NULL;
    if (ops < 2 || mem < 64ULL * 1024 * 1024)
        return HB_ERR_CRYPTO;
    e = hb_sync_pwhash(password, salt, ops, mem, mk);
    if (e != HB_OK) {
        sodium_memzero(mk, sizeof mk);
        return e;
    }
    hb_sync_derive_mk(mk, auth, wrap_key);
    sodium_memzero(mk, sizeof mk);
    a_b64 = b64_enc(auth, sizeof auth);
    sodium_memzero(auth, sizeof auth);
    j = json_obj();
    if (!a_b64 || !j) {
        free(a_b64);
        cJSON_Delete(j);
        return HB_ERR_NOMEM;
    }
    cJSON_AddStringToObject(j, "email", email);
    cJSON_AddStringToObject(j, "auth_key", a_b64);
    free(a_b64);
    if (totp)
        cJSON_AddStringToObject(j, "totp", totp);
    json_add_device(j, dev_name, dev_platform);
    *body_out = json_print_str(j);
    cJSON_Delete(j);
    return *body_out ? HB_OK : HB_ERR_NOMEM;
}

static char *json_dup_str(cJSON *j, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsString(it))
        return NULL;
    return strdup(it->valuestring);
}

hb_err hb_sync_login_parse(const char *resp, const uint8_t wrap_key[32],
                           uint8_t vk[32], char **account_id,
                           char **device_id, char **token, bool *totp_enabled)
{
    cJSON *j = cJSON_Parse(resp);
    char *wp;
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    hb_err e = HB_ERR_INVALID;

    if (!j)
        return HB_ERR_INVALID;
    if (account_id) *account_id = json_dup_str(j, "account_id");
    if (device_id)  *device_id = json_dup_str(j, "device_id");
    if (token)      *token = json_dup_str(j, "token");
    if (totp_enabled) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "totp_enabled");
        *totp_enabled = cJSON_IsTrue(t);
    }
    wp = json_dup_str(j, "wrapped_vk_pw");
    if (!wp) {
        cJSON_Delete(j);
        return HB_ERR_INVALID;
    }
    e = unseal_b64(wrap_key, wp, AD_VK_PW, &pt, &pt_len);
    free(wp);
    if (e == HB_OK && pt_len != 32)
        e = HB_ERR_CRYPTO;
    if (e == HB_OK)
        memcpy(vk, pt, 32);
    if (pt) {
        sodium_memzero(pt, pt_len);
        sodium_free(pt);
    }
    cJSON_Delete(j);
    return e;
}

hb_err hb_sync_auth_resp_parse(const char *resp, char **account_id,
                               char **device_id, char **token)
{
    cJSON *j = cJSON_Parse(resp);
    if (!j)
        return HB_ERR_INVALID;
    if (account_id) *account_id = json_dup_str(j, "account_id");
    if (device_id)  *device_id = json_dup_str(j, "device_id");
    if (token)      *token = json_dup_str(j, "token");
    cJSON_Delete(j);
    return HB_OK;
}

hb_err hb_sync_recover_start_body(const char *email, const char *words,
                                  char **body_out)
{
    uint8_t E[32], rc_wrap[32], rc_auth[32];
    char *ra_b64;
    cJSON *j;
    hb_err e = HB_ERR_NOMEM;

    *body_out = NULL;
    if (hb_bip39_decode(words, E) != 0)
        return HB_ERR_CRYPTO;
    hb_sync_derive_rc(E, rc_wrap, rc_auth);
    sodium_memzero(E, sizeof E);
    sodium_memzero(rc_wrap, sizeof rc_wrap);
    ra_b64 = b64_enc(rc_auth, sizeof rc_auth);
    sodium_memzero(rc_auth, sizeof rc_auth);
    j = json_obj();
    if (!ra_b64 || !j)
        goto out;
    cJSON_AddStringToObject(j, "email", email);
    cJSON_AddStringToObject(j, "rc_auth", ra_b64);
    *body_out = json_print_str(j);
    e = *body_out ? HB_OK : HB_ERR_NOMEM;
out:
    free(ra_b64);
    cJSON_Delete(j);
    return e;
}

hb_err hb_sync_recover_start_parse(const char *resp, const char *words,
                                   uint8_t vk[32], char **recovery_token)
{
    cJSON *j = cJSON_Parse(resp);
    uint8_t E[32], rc_wrap[32], rc_auth[32];
    char *wr;
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    hb_err e = HB_ERR_INVALID;

    *recovery_token = NULL;
    if (!j)
        return HB_ERR_INVALID;
    if (hb_bip39_decode(words, E) != 0) {
        cJSON_Delete(j);
        return HB_ERR_CRYPTO;
    }
    hb_sync_derive_rc(E, rc_wrap, rc_auth);
    sodium_memzero(E, sizeof E);
    sodium_memzero(rc_auth, sizeof rc_auth);
    *recovery_token = json_dup_str(j, "recovery_token");
    wr = json_dup_str(j, "wrapped_vk_rc");
    if (*recovery_token && wr)
        e = unseal_b64(rc_wrap, wr, AD_VK_RC, &pt, &pt_len);
    sodium_memzero(rc_wrap, sizeof rc_wrap);
    free(wr);
    if (e == HB_OK && pt_len != 32)
        e = HB_ERR_CRYPTO;
    if (e == HB_OK)
        memcpy(vk, pt, 32);
    else {
        free(*recovery_token);
        *recovery_token = NULL;
    }
    if (pt) {
        sodium_memzero(pt, pt_len);
        sodium_free(pt);
    }
    cJSON_Delete(j);
    return e;
}

hb_err hb_sync_recover_finish_body(const char *recovery_token,
                                   const char *new_password,
                                   uint64_t kdf_ops, uint64_t kdf_mem,
                                   const char *dev_name,
                                   const char *dev_platform,
                                   const uint8_t vk[32], char **body_out)
{
    uint8_t salt[HB_SYNC_SALT_LEN], mk[32], auth[32], wrap[32];
    char *s_b64 = NULL, *a_b64 = NULL, *wp_b64 = NULL;
    cJSON *j = NULL, *kdf;
    hb_err e;

    *body_out = NULL;
    kdf_ops = kdf_ops_or_def(kdf_ops);
    kdf_mem = kdf_mem_or_def(kdf_mem);
    randombytes_buf(salt, sizeof salt);
    e = hb_sync_pwhash(new_password, salt, kdf_ops, kdf_mem, mk);
    if (e != HB_OK)
        goto out;
    hb_sync_derive_mk(mk, auth, wrap);
    s_b64 = b64_enc(salt, sizeof salt);
    a_b64 = b64_enc(auth, sizeof auth);
    e = seal_to_b64(wrap, vk, 32, AD_VK_PW, &wp_b64);
    if (e != HB_OK)
        goto out;
    if (!s_b64 || !a_b64) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    j = json_obj();
    if (!j) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    cJSON_AddStringToObject(j, "recovery_token", recovery_token);
    cJSON_AddStringToObject(j, "auth_key", a_b64);
    kdf = json_obj();
    cJSON_AddStringToObject(kdf, "salt", s_b64);
    cJSON_AddNumberToObject(kdf, "ops", (double)kdf_ops);
    cJSON_AddNumberToObject(kdf, "mem", (double)kdf_mem);
    cJSON_AddItemToObject(j, "kdf", kdf);
    cJSON_AddStringToObject(j, "wrapped_vk_pw", wp_b64);
    json_add_device(j, dev_name, dev_platform);
    *body_out = json_print_str(j);
    e = *body_out ? HB_OK : HB_ERR_NOMEM;
out:
    cJSON_Delete(j);
    free(s_b64); free(a_b64); free(wp_b64);
    sodium_memzero(mk, sizeof mk);
    sodium_memzero(auth, sizeof auth);
    sodium_memzero(wrap, sizeof wrap);
    sodium_memzero(salt, sizeof salt);
    return e;
}

hb_err hb_sync_change_password_body(const char *old_password,
                                    const uint8_t old_salt[16],
                                    uint64_t old_ops, uint64_t old_mem,
                                    const char *new_password,
                                    uint64_t new_ops, uint64_t new_mem,
                                    const uint8_t vk[32], char **body_out)
{
    uint8_t mk[32], auth[32], wrap[32], oauth[32];
    uint8_t salt[HB_SYNC_SALT_LEN];
    char *s_b64 = NULL, *a_b64 = NULL, *oa_b64 = NULL, *wp_b64 = NULL;
    cJSON *j = NULL, *kdf;
    hb_err e;

    *body_out = NULL;
    e = hb_sync_pwhash(old_password, old_salt, old_ops, old_mem, mk);
    if (e != HB_OK)
        goto out;
    crypto_kdf_derive_from_key(oauth, 32, 1, "hbauth01", mk);
    sodium_memzero(mk, sizeof mk);
    new_ops = kdf_ops_or_def(new_ops);
    new_mem = kdf_mem_or_def(new_mem);
    randombytes_buf(salt, sizeof salt);
    e = hb_sync_pwhash(new_password, salt, new_ops, new_mem, mk);
    if (e != HB_OK)
        goto out;
    hb_sync_derive_mk(mk, auth, wrap);
    s_b64 = b64_enc(salt, sizeof salt);
    a_b64 = b64_enc(auth, sizeof auth);
    oa_b64 = b64_enc(oauth, sizeof oauth);
    e = seal_to_b64(wrap, vk, 32, AD_VK_PW, &wp_b64);
    if (e != HB_OK)
        goto out;
    if (!s_b64 || !a_b64 || !oa_b64) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    j = json_obj();
    if (!j) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    cJSON_AddStringToObject(j, "auth_key_old", oa_b64);
    cJSON_AddStringToObject(j, "auth_key", a_b64);
    kdf = json_obj();
    cJSON_AddStringToObject(kdf, "salt", s_b64);
    cJSON_AddNumberToObject(kdf, "ops", (double)new_ops);
    cJSON_AddNumberToObject(kdf, "mem", (double)new_mem);
    cJSON_AddItemToObject(j, "kdf", kdf);
    cJSON_AddStringToObject(j, "wrapped_vk_pw", wp_b64);
    *body_out = json_print_str(j);
    e = *body_out ? HB_OK : HB_ERR_NOMEM;
out:
    cJSON_Delete(j);
    free(s_b64); free(a_b64); free(oa_b64); free(wp_b64);
    sodium_memzero(mk, sizeof mk);
    sodium_memzero(auth, sizeof auth);
    sodium_memzero(oauth, sizeof oauth);
    sodium_memzero(wrap, sizeof wrap);
    sodium_memzero(salt, sizeof salt);
    return e;
}

/* ======================== secret map：二进制 <-> JSON ======================== */
/* 二进制格式同 vault.c：[u16le nlen][name][u32le vlen][value]*，值为文本。 */

static cJSON *secmap_to_json(const uint8_t *pt, size_t len)
{
    cJSON *m = json_obj();
    size_t pos = 0;
    if (!m)
        return NULL;
    while (pos < len) {
        uint32_t nlen, vlen;
        char *name, *val;
        if (len - pos < 2)
            break;
        nlen = (uint32_t)pt[pos] | ((uint32_t)pt[pos + 1] << 8);
        pos += 2;
        if (len - pos < nlen + 4)
            break;
        name = malloc(nlen + 1);
        if (!name) {
            cJSON_Delete(m);
            return NULL;
        }
        memcpy(name, pt + pos, nlen);
        name[nlen] = 0;
        pos += nlen;
        vlen = (uint32_t)pt[pos] | ((uint32_t)pt[pos + 1] << 8) |
               ((uint32_t)pt[pos + 2] << 16) | ((uint32_t)pt[pos + 3] << 24);
        pos += 4;
        if (len - pos < vlen) {
            free(name);
            break;
        }
        val = malloc(vlen + 1);
        if (!val) {
            free(name);
            cJSON_Delete(m);
            return NULL;
        }
        memcpy(val, pt + pos, vlen);
        val[vlen] = 0;
        pos += vlen;
        cJSON_AddStringToObject(m, name, val);
        free(name);
        free(val);
    }
    return m;
}

/* JSON map → 二进制明文（sodium_malloc'd）。空 map 返回 len=0、*out!=NULL。 */
static uint8_t *secmap_from_json(const cJSON *m, size_t *out_len)
{
    size_t len = 0;
    uint8_t *pt, *w;
    const cJSON *it;

    cJSON_ArrayForEach(it, m) {
        const char *v = it->valuestring ? it->valuestring : "";
        len += 2 + strlen(it->string) + 4 + strlen(v);
    }
    pt = sodium_malloc(len ? len : 1);
    if (!pt)
        return NULL;
    w = pt;
    cJSON_ArrayForEach(it, m) {
        const char *v = it->valuestring ? it->valuestring : "";
        size_t nlen = strlen(it->string), vlen = strlen(v);
        w[0] = (uint8_t)(nlen & 0xff);
        w[1] = (uint8_t)(nlen >> 8);
        w += 2;
        memcpy(w, it->string, nlen);
        w += nlen;
        w[0] = (uint8_t)(vlen & 0xff);
        w[1] = (uint8_t)((vlen >> 8) & 0xff);
        w[2] = (uint8_t)((vlen >> 16) & 0xff);
        w[3] = (uint8_t)((vlen >> 24) & 0xff);
        w += 4;
        memcpy(w, v, vlen);
        w += vlen;
    }
    *out_len = (size_t)(w - pt);
    return pt;
}

/* ============================ blob（§4.2） ============================ */

static uint8_t *obj_ad(const char *id, const char *type, size_t *ad_len)
{
    static const char pfx[] = "hb:obj:v1";
    size_t n = sizeof(pfx) - 1 + 1 + strlen(id) + 1 + strlen(type);
    uint8_t *ad = malloc(n);
    if (!ad)
        return NULL;
    memcpy(ad, pfx, sizeof(pfx) - 1);
    ad[sizeof(pfx) - 1] = 0;
    memcpy(ad + sizeof(pfx), id, strlen(id) + 1);
    memcpy(ad + sizeof(pfx) + strlen(id) + 1, type, strlen(type));
    *ad_len = n;
    return ad;
}

uint8_t *hb_sync_blob_seal(const uint8_t vk[32], const char *id,
                           const char *type, const char *pt_json,
                           size_t *out_len)
{
    uint8_t *ad, *padded, *sealed, *blob;
    size_t ad_len, pt_len = strlen(pt_json), padded_len, seal_len;

    ad = obj_ad(id, type, &ad_len);
    padded = sodium_malloc(pt_len + 256);
    if (!ad || !padded) {
        free(ad);
        sodium_free(padded);
        return NULL;
    }
    memcpy(padded, pt_json, pt_len);
    /* ISO/IEC 7816-4 填充至少追加 1 字节标记：长度恰为 256 倍数时会再补
     * 一整块，必须用 sodium_pad 回报的长度。 */
    if (sodium_pad(&padded_len, padded, pt_len, 256, pt_len + 256) != 0) {
        free(ad);
        sodium_free(padded);
        return NULL;
    }
    sealed = hb_sync_seal(vk, padded, padded_len, ad, ad_len, &seal_len);
    sodium_memzero(padded, pt_len + 256);
    sodium_free(padded);
    free(ad);
    if (!sealed)
        return NULL;
    blob = sodium_malloc(1 + seal_len);
    if (!blob) {
        sodium_free(sealed);
        return NULL;
    }
    blob[0] = 0x01;
    memcpy(blob + 1, sealed, seal_len);
    sodium_free(sealed);
    *out_len = 1 + seal_len;
    return blob;
}

char *hb_sync_blob_open(const uint8_t vk[32], const char *id,
                        const char *type, const uint8_t *blob, size_t n)
{
    uint8_t *ad, *pt;
    size_t ad_len, pt_len, unpad_len;

    if (n < 1 || blob[0] != 0x01)
        return NULL;
    ad = obj_ad(id, type, &ad_len);
    if (!ad)
        return NULL;
    pt = hb_sync_open(vk, blob + 1, n - 1, ad, ad_len, &pt_len);
    free(ad);
    if (!pt)
        return NULL;
    if (sodium_unpad(&unpad_len, pt, pt_len, 256) != 0) {
        sodium_memzero(pt, pt_len);
        sodium_free(pt);
        return NULL;
    }
    pt[unpad_len] = 0;
    return (char *)pt;
}

/* ============================ scope / 设备本地字段 ============================ */

struct hb_sync {
    hb_vault *v;
    uint8_t *vk;    /* sodium_malloc'd, 32B，可为 NULL */
};

static const struct { const char *scope; const char *types[2]; } SCOPES[] = {
    { "hosts",    { "host", "group" } },
    { "keys",     { "key", NULL } },
    { "snippets", { "snippet", NULL } },
    { "forwards", { "forward", NULL } },
    { "known",    { "known", NULL } },
    { "settings", { "settings", NULL } },
};
#define N_SCOPES (sizeof SCOPES / sizeof SCOPES[0])

static const char *scope_of_type(const char *type)
{
    size_t s;
    for (s = 0; s < N_SCOPES; s++) {
        if (!strcmp(type, SCOPES[s].types[0]) ||
            (SCOPES[s].types[1] && !strcmp(type, SCOPES[s].types[1])))
            return SCOPES[s].scope;
    }
    return NULL;
}

static bool scope_valid(const char *scope)
{
    size_t s;
    for (s = 0; s < N_SCOPES; s++)
        if (!strcmp(scope, SCOPES[s].scope))
            return true;
    return false;
}

static bool is_device_local(const char *type, const char *f)
{
    if (!strcmp(type, "host"))
        return !strcmp(f, "identity_file") || !strcmp(f, "last_connected");
    if (!strcmp(type, "forward"))
        return !strcmp(f, "enabled");
    if (!strcmp(type, "settings"))
        return !strcmp(f, "font_size") || !strcmp(f, "line_height") ||
               !strcmp(f, "font_family") || !strcmp(f, "close_to_tray") ||
               !strcmp(f, "start_at_login") || !strcmp(f, "language") ||
               !strcmp(f, "keep_connected") || !strcmp(f, "option_meta");
    return false;
}

static cJSON *strip_local_fields(const cJSON *data, const char *type);

/* ============================ meta 键 ============================ */

static hb_err meta_get_i64(hb_vault *v, const char *key, int64_t *out, int64_t def)
{
    char *s = NULL;
    hb_err e = hb__vault_meta_get(v, key, &s);
    if (e == HB_ERR_NOT_FOUND) {
        *out = def;
        return HB_OK;
    }
    if (e != HB_OK)
        return e;
    *out = s ? strtoll(s, NULL, 10) : def;
    free(s);
    return HB_OK;
}

static hb_err meta_set_i64(hb_vault *v, const char *key, int64_t val)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%lld", (long long)val);
    return hb__vault_meta_set(v, key, buf);
}

/* ============================ hb_sync 生命周期 ============================ */

hb_sync *hb_sync_new(hb_vault *v)
{
    hb_sync *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->v = v;
    return s;
}

void hb_sync_free(hb_sync *s)
{
    if (!s)
        return;
    if (s->vk) {
        sodium_memzero(s->vk, 32);
        sodium_free(s->vk);
    }
    free(s);
}

hb_err hb_sync_set_vk(hb_sync *s, const uint8_t vk[32])
{
    if (!s->vk) {
        s->vk = sodium_malloc(32);
        if (!s->vk)
            return HB_ERR_NOMEM;
    }
    memcpy(s->vk, vk, 32);
    return HB_OK;
}

void hb_sync_clear_vk(hb_sync *s)
{
    if (s && s->vk) {
        sodium_memzero(s->vk, 32);
        sodium_free(s->vk);
        s->vk = NULL;
    }
}

hb_err hb_sync_recovery_rotate_body_vk(const uint8_t vk[32],
                                       const char *password,
                                       const uint8_t salt[16], uint64_t ops,
                                       uint64_t mem, char **body_out,
                                       char **words_out)
{
    uint8_t mk[32], auth[32], wrap[32], E[32], rc_wrap[32], rc_auth[32];
    char *a_b64 = NULL, *ra_b64 = NULL, *wr_b64 = NULL;
    char words[256];
    cJSON *j = NULL;
    hb_err e = HB_ERR_NOMEM;

    *body_out = NULL;
    *words_out = NULL;
    if (!vk || !password)
        return HB_ERR_INVALID;
    randombytes_buf(E, sizeof E);
    e = hb_sync_pwhash(password, salt, ops, mem, mk);
    if (e != HB_OK)
        goto out;
    hb_sync_derive_mk(mk, auth, wrap);
    hb_sync_derive_rc(E, rc_wrap, rc_auth);
    if (hb_bip39_encode(E, words, sizeof words) != 0) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    a_b64 = b64_enc(auth, sizeof auth);
    ra_b64 = b64_enc(rc_auth, sizeof rc_auth);
    e = seal_to_b64(rc_wrap, vk, 32, AD_VK_RC, &wr_b64);
    if (e != HB_OK)
        goto out;
    if (!a_b64 || !ra_b64) {
        e = HB_ERR_NOMEM;
        goto out;
    }
    j = json_obj();
    if (!j)
        goto out;
    cJSON_AddStringToObject(j, "auth_key", a_b64);
    cJSON_AddStringToObject(j, "rc_auth", ra_b64);
    cJSON_AddStringToObject(j, "wrapped_vk_rc", wr_b64);
    *body_out = json_print_str(j);
    *words_out = strdup(words);
    e = (*body_out && *words_out) ? HB_OK : HB_ERR_NOMEM;
out:
    cJSON_Delete(j);
    free(a_b64); free(ra_b64); free(wr_b64);
    sodium_memzero(mk, sizeof mk);
    sodium_memzero(auth, sizeof auth);
    sodium_memzero(wrap, sizeof wrap);
    sodium_memzero(E, sizeof E);
    sodium_memzero(rc_wrap, sizeof rc_wrap);
    sodium_memzero(rc_auth, sizeof rc_auth);
    sodium_memzero(words, sizeof words);
    return e;
}

hb_err hb_sync_recovery_rotate_body(hb_sync *s, const char *password,
                                    const uint8_t salt[16], uint64_t ops,
                                    uint64_t mem, char **body_out,
                                    char **words_out)
{
    if (!s || !s->vk)
        return HB_ERR_INVALID;
    return hb_sync_recovery_rotate_body_vk(s->vk, password, salt, ops, mem,
                                           body_out, words_out);
}

bool hb_sync_scope_get(hb_sync *s, const char *scope)
{
    char key[64], *val = NULL;
    bool on = true;
    if (!scope_valid(scope))
        return false;
    snprintf(key, sizeof key, "sync.scope.%s", scope);
    if (hb__vault_meta_get(s->v, key, &val) == HB_OK && val)
        on = strcmp(val, "0") != 0;
    free(val);
    return on;
}

hb_err hb_sync_scope_set(hb_sync *s, const char *scope, bool on)
{
    char key[64];
    hb_err e;
    bool was;
    if (!scope_valid(scope))
        return HB_ERR_INVALID;
    was = hb_sync_scope_get(s, scope);
    snprintf(key, sizeof key, "sync.scope.%s", scope);
    e = hb__vault_meta_set(s->v, key, on ? "1" : "0");
    if (e != HB_OK)
        return e;
    /* 重新开启：游标重置全量重拉（§4.5）。 */
    if (on && !was)
        return meta_set_i64(s->v, "sync.cursor", 0);
    return HB_OK;
}

hb_err hb_sync_set_account(hb_sync *s, const char *account_id,
                           const char *email, const char *server,
                           const char *device_id)
{
    hb_err e;
    if ((e = hb__vault_meta_set(s->v, "sync.account_id", account_id)) != HB_OK)
        return e;
    if ((e = hb__vault_meta_set(s->v, "sync.email", email)) != HB_OK)
        return e;
    if ((e = hb__vault_meta_set(s->v, "sync.server", server)) != HB_OK)
        return e;
    return hb__vault_meta_set(s->v, "sync.device_id", device_id);
}

hb_err hb_sync_get_account(hb_sync *s, char **account_id, char **email,
                           char **server, char **device_id)
{
    char *aid = NULL;
    hb_err e = hb__vault_meta_get(s->v, "sync.account_id", &aid);
    if (e != HB_OK)
        return e;
    if (account_id)
        *account_id = aid;
    else
        free(aid);
    if (email) {
        if (hb__vault_meta_get(s->v, "sync.email", email) != HB_OK)
            *email = NULL;
    }
    if (server) {
        if (hb__vault_meta_get(s->v, "sync.server", server) != HB_OK)
            *server = NULL;
    }
    if (device_id) {
        if (hb__vault_meta_get(s->v, "sync.device_id", device_id) != HB_OK)
            *device_id = NULL;
    }
    return HB_OK;
}

/* ============================ 版本结构 & 合并 ============================ */

typedef struct {
    cJSON  *data;      /* 含 _fv */
    cJSON  *vv;
    bool    tomb;
    int64_t updated_at;
    cJSON  *secret;    /* map JSON 或 NULL（无 secret） */
    bool    dirty;     /* 仅 L 用 */
    int64_t sync_seq;  /* 仅 L 用 */
} ver_t;

static void ver_free(ver_t *x)
{
    cJSON_Delete(x->data);
    cJSON_Delete(x->vv);
    cJSON_Delete(x->secret);
    memset(x, 0, sizeof *x);
}

/* 从 vault 行构造 L。secret map 解密失败按无 secret 处理并标 *sec_err。 */
static bool ver_from_row(hb_vault *v, const hb__vault_row *r, ver_t *L)
{
    uint8_t *pt = NULL;
    size_t pt_len = 0;

    memset(L, 0, sizeof *L);
    L->data = cJSON_Parse(r->data);
    L->vv = cJSON_Parse(r->vv);
    L->tomb = r->tombstone != 0;
    L->updated_at = r->updated_at;
    L->dirty = r->dirty != 0;
    L->sync_seq = r->sync_seq;
    if (!L->data || !L->vv) {
        ver_free(L);
        return false;
    }
    if (hb__vault_secret_map(v, r->id, &pt, &pt_len) == HB_OK && pt) {
        L->secret = secmap_to_json(pt, pt_len);
        sodium_memzero(pt, pt_len);
        sodium_free(pt);
    }
    return true;
}

/* 从远端明文 pt JSON 构造 R。 */
static bool ver_from_pt(const char *pt_json, ver_t *R)
{
    cJSON *j = cJSON_Parse(pt_json);
    cJSON *it;
    if (!j)
        return false;
    memset(R, 0, sizeof *R);
    R->data = cJSON_DetachItemFromObjectCaseSensitive(j, "data");
    R->vv = cJSON_DetachItemFromObjectCaseSensitive(j, "vv");
    it = cJSON_GetObjectItemCaseSensitive(j, "tomb");
    R->tomb = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(j, "updated_at");
    R->updated_at = cJSON_IsNumber(it) ? (int64_t)it->valuedouble : 0;
    R->secret = cJSON_DetachItemFromObjectCaseSensitive(j, "secret");
    cJSON_Delete(j);
    if (!R->data || !R->vv) {
        ver_free(R);
        return false;
    }
    return true;
}

/* _fv 中 f 的戳：[dev,ctr] 或空。 */
static bool stamp_get(const ver_t *x, const char *f, const char **dev, double *ctr)
{
    cJSON *fv = cJSON_GetObjectItemCaseSensitive(x->data, "_fv");
    cJSON *st = fv ? cJSON_GetObjectItemCaseSensitive(fv, f) : NULL;
    if (!cJSON_IsArray(st) || cJSON_GetArraySize(st) != 2)
        return false;
    *dev = cJSON_GetArrayItem(st, 0)->valuestring;
    *ctr = cJSON_GetArrayItem(st, 1)->valuedouble;
    return *dev != NULL;
}

/* seen(X, s)：s 为空或 X.vv[s.dev] >= s.ctr。 */
static bool seen(const ver_t *x, bool has, const char *dev, double ctr)
{
    cJSON *it;
    if (!has)
        return true;
    it = cJSON_GetObjectItemCaseSensitive(x->vv, dev);
    return cJSON_IsNumber(it) && it->valuedouble >= ctr;
}

static bool stamp_eq(bool h1, const char *d1, double c1,
                     bool h2, const char *d2, double c2)
{
    if (h1 != h2)
        return false;
    if (!h1)
        return true;
    return c1 == c2 && !strcmp(d1, d2);
}

/* vv 逐元素 max。 */
static cJSON *vv_max(const cJSON *a, const cJSON *b)
{
    cJSON *r = cJSON_Duplicate((cJSON *)a, 1);
    const cJSON *it;
    cJSON_ArrayForEach(it, b) {
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(r, it->string);
        if (!cJSON_IsNumber(cur) || it->valuedouble > cur->valuedouble) {
            cJSON_DeleteItemFromObjectCaseSensitive(r, it->string);
            cJSON_AddItemToObject(r, it->string, cJSON_Duplicate((cJSON *)it, 1));
        }
    }
    return r;
}

/* vv1 ≤ vv2：vv1 每个条目都 ≤ vv2 对应条目。 */
static bool vv_leq(const cJSON *a, const cJSON *b)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, a) {
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(b, it->string);
        if (!cJSON_IsNumber(o) || it->valuedouble > o->valuedouble)
            return false;
    }
    return true;
}

/* map 比较：NULL 视为空 map。 */
static bool map_eq(const cJSON *a, const cJSON *b)
{
    const cJSON *it;
    size_t na = a ? (size_t)cJSON_GetArraySize(a) : 0;
    size_t nb = b ? (size_t)cJSON_GetArraySize(b) : 0;
    if (na != nb)
        return false;
    cJSON_ArrayForEach(it, a) {
        const cJSON *o = b ? cJSON_GetObjectItemCaseSensitive(b, it->string) : NULL;
        if (!cJSON_Compare(it, o, 1))
            return false;
    }
    return true;
}

/* 收集 F：L.data ∪ R.data ∪ L._fv ∪ R._fv 的键（去掉 _* 与设备本地字段），
 * 另加 "$secret"、"$tomb"。返回 cJSON 数组（去重）。 */
static cJSON *field_set(const ver_t *L, const ver_t *R, const char *type)
{
    cJSON *f = cJSON_CreateArray();
    const cJSON *it;
    int pass;
    /* pass 0: data keys; pass 1: _fv keys */
    for (pass = 0; pass < 2; pass++) {
        const ver_t *srcs[2] = { L, R };
        int si;
        for (si = 0; si < 2; si++) {
            const cJSON *obj = pass == 0 ? srcs[si]->data
                : cJSON_GetObjectItemCaseSensitive(srcs[si]->data, "_fv");
            if (!obj)
                continue;
            cJSON_ArrayForEach(it, obj) {
                const char *k = it->string;
                if (!k || k[0] == '_' || is_device_local(type, k))
                    continue;
                if (!strcmp(k, "$secret") || !strcmp(k, "$tomb"))
                    continue; /* 最后统一加 */
                {
                    const cJSON *dup;
                    bool have = false;
                    cJSON_ArrayForEach(dup, f)
                        if (!strcmp(dup->valuestring, k)) { have = true; break; }
                    if (!have)
                        cJSON_AddItemToArray(f, cJSON_CreateString(k));
                }
            }
        }
    }
    cJSON_AddItemToArray(f, cJSON_CreateString("$secret"));
    cJSON_AddItemToArray(f, cJSON_CreateString("$tomb"));
    return f;
}

/* 合并结果 */
typedef struct {
    ver_t   m;           /* 合并出的版本（secret 为 map JSON 或 NULL） */
    cJSON  *cf_fields;   /* 冲突字段数组（无冲突 = 空数组） */
    bool    conflict;    /* 是否有规则 5 字段 */
    int     loser_side;  /* 0=L 输, 1=R 输（conflict 时有效） */
} merge_t;

static void merge_free(merge_t *r)
{
    ver_free(&r->m);
    cJSON_Delete(r->cf_fields);
}

/* §5 逐字段合并。loser_side 记录规则 5 的输方（临时胜方的另一边）。 */
static hb_err ver_merge(const ver_t *L, const ver_t *R, const char *type,
                        merge_t *out)
{
    cJSON *F, *nd, *nfv;
    const cJSON *it;

    memset(out, 0, sizeof *out);
    F = field_set(L, R, type);
    nd = json_obj();
    nfv = json_obj();
    out->cf_fields = cJSON_CreateArray();
    out->m.vv = vv_max(L->vv, R->vv);
    out->m.updated_at = L->updated_at > R->updated_at ? L->updated_at : R->updated_at;
    out->m.secret = NULL;
    if (!F || !nd || !nfv || !out->cf_fields || !out->m.vv) {
        cJSON_Delete(F); cJSON_Delete(nd); cJSON_Delete(nfv);
        cJSON_Delete(out->cf_fields); cJSON_Delete(out->m.vv);
        return HB_ERR_NOMEM;
    }

    cJSON_ArrayForEach(it, F) {
        const char *f = it->valuestring;
        const char *dL = NULL, *dR = NULL;
        double cL = 0, cR = 0;
        bool hL = stamp_get(L, f, &dL, &cL);
        bool hR = stamp_get(R, f, &dR, &cR);
        int win; /* 0 = L, 1 = R */
        bool is_sec = !strcmp(f, "$secret");
        bool is_tomb = !strcmp(f, "$tomb");

        if (stamp_eq(hL, dL, cL, hR, dR, cR)) {
            win = 0; /* 取 L（同戳/同空） */

        } else if (seen(L, hR, dR, cR) && !seen(R, hL, dL, cL)) {
            win = 0;

        } else if (seen(R, hL, dL, cL) && !seen(L, hR, dR, cR)) {
            win = 1;

        } else if (seen(L, hR, dR, cR) && seen(R, hL, dL, cL)) {

            /* 互见（不应出现）：取 (ctr,dev) 较大者。 */
            if (cL != cR)
                win = cL > cR ? 0 : 1;
            else
                win = strcmp(dL ? dL : "", dR ? dR : "") >= 0 ? 0 : 1;
        } else {
            /* 并发冲突：对象 updated_at 大者临时胜，平则 device_id 大者。 */
            out->conflict = true;
            if (L->updated_at != R->updated_at)
                win = L->updated_at > R->updated_at ? 0 : 1;
            else
                win = strcmp(dL ? dL : "", dR ? dR : "") >= 0 ? 0 : 1;
            out->loser_side = 1 - win;
            cJSON_AddItemToArray(out->cf_fields, cJSON_CreateString(f));
        }

        /* 胜方戳入 _fv（有戳才写） */
        {
            bool hw = win ? hR : hL;
            if (hw) {
                cJSON *st = cJSON_CreateArray();
                cJSON_AddItemToArray(st, cJSON_CreateString(win ? dR : dL));
                cJSON_AddItemToArray(st, cJSON_CreateNumber(win ? cR : cL));
                cJSON_AddItemToObject(nfv, f, st);
            }
        }

        /* 胜方值 */
        if (is_sec) {
            const cJSON *wmap = win ? R->secret : L->secret;
            cJSON_Delete(out->m.secret);
            out->m.secret = wmap ? cJSON_Duplicate((cJSON *)wmap, 1) : NULL;
        } else if (is_tomb) {
            out->m.tomb = win ? R->tomb : L->tomb;
        } else {
            const ver_t *w = win ? R : L;
            const cJSON *val = cJSON_GetObjectItemCaseSensitive(w->data, f);
            if (val)
                cJSON_AddItemToObject(nd, f, cJSON_Duplicate((cJSON *)val, 1));
            /* 缺失 = 该字段被删除（不写） */
        }
    }
    cJSON_Delete(F);

    /* 设备本地字段保留 L（§4.4）。 */
    {
        const cJSON *d;
        cJSON_ArrayForEach(d, L->data) {
            if (d->string[0] != '_' && is_device_local(type, d->string))
                cJSON_AddItemToObject(nd, d->string,
                                      cJSON_Duplicate((cJSON *)d, 1));
        }
    }
    cJSON_AddItemToObject(nd, "_fv", nfv);
    out->m.data = nd;
    out->m.dirty = L->dirty;
    out->m.sync_seq = L->sync_seq;
    return HB_OK;
}

/* 合并结果与 R 等价（data、secret、tomb、vv 全同）→ 干净（§5）。
 * data 比较前先剥离设备本地字段（§4.4：远端永远不带它们，但合并结果
 * 保留 L 的本地字段，不剥离会永远判 dirty）。 */
static bool merge_eq_remote(const merge_t *r, const ver_t *R, const char *type)
{
    cJSON *d;
    bool eq;
    if (r->m.tomb != R->tomb)
        return false;
    if (!map_eq(r->m.secret, R->secret))
        return false;
    if (!cJSON_Compare(r->m.vv, R->vv, 1))
        return false;
    d = strip_local_fields(r->m.data, type);
    if (!d)
        return false;
    eq = cJSON_Compare(d, R->data, 1) != 0;
    cJSON_Delete(d);
    return eq;
}

/* ============================ 远端版本落库 ============================ */

/* 把远端版本 R 合入本地：无本地 → 直接落干净；干净且 L.vv≤R.vv → 替换；
 * 否则合并。sync_seq = seq；冲突时写 sync_conflicts。 */
static hb_err apply_remote(hb_sync *s, const char *id, const char *type,
                           int64_t seq, const ver_t *R,
                           const hb__vault_row *local_row)
{
    ver_t L;
    bool have_L = false;
    merge_t mr;
    hb_err e = HB_OK;
    int dirty;
    const ver_t *write_v;

    memset(&mr, 0, sizeof mr);
    memset(&L, 0, sizeof L);

    if (local_row) {
        if (!ver_from_row(s->v, local_row, &L))
            return HB_ERR_NOMEM;
        have_L = true;
    }

    if (!have_L) {
        /* 本地无：直接落库为干净。 */
        write_v = R;
        dirty = 0;
        goto write;
    }
    if (!L.dirty && vv_leq(L.vv, R->vv)) {
        /* 干净且被支配：整体替换，设备本地字段并入结果。 */
        e = ver_merge(&L, R, type, &mr); /* 用合并保证设备本地字段保留 */
        if (e != HB_OK)
            goto out;
        write_v = &mr.m;
        dirty = 0;
        /* mr.m 的字段合并结果此时应等价 R（除设备本地字段），
         * 若出现冲突字段仍要记录（理论上不可能：L 被支配无并发）。 */
        goto write;
    }

    e = ver_merge(&L, R, type, &mr);
    if (e != HB_OK)
        goto out;
    write_v = &mr.m;
    dirty = merge_eq_remote(&mr, R, type) ? 0 : 1;

    if (mr.conflict) {
        /* loser = 临时胜方的另一边（0=L 输, 1=R 输）。
         * 输方 tomb 存进 loser data 的 "_tomb"（take_other 用）。 */
        const ver_t *loser = mr.loser_side ? R : &L;
        char *flds = cJSON_PrintUnformatted(mr.cf_fields);
        char *ldata;
        {
            cJSON *ld = cJSON_Duplicate(loser->data, 1);
            if (ld)
                cJSON_AddBoolToObject(ld, "_tomb", loser->tomb);
            ldata = ld ? cJSON_PrintUnformatted(ld) : NULL;
            cJSON_Delete(ld);
        }
        uint8_t *lsec = NULL;
        size_t lsec_len = 0;
        if (loser->secret)
            lsec = secmap_from_json(loser->secret, &lsec_len);
        if (flds && ldata)
            e = hb__vault_conflict_put(s->v, id, type, flds, ldata,
                                       lsec, lsec ? lsec_len : 0);
        else
            e = HB_ERR_NOMEM;
        free(flds);
        free(ldata);
        if (lsec) {
            sodium_memzero(lsec, lsec_len);
            sodium_free(lsec);
        }
        if (e != HB_OK)
            goto out;
    }

write:
    {
        char *data_str = cJSON_PrintUnformatted(write_v->data);
        char *vv_str = cJSON_PrintUnformatted(write_v->vv);
        uint8_t *spt = NULL;
        size_t spt_len = 0;
        if (write_v->secret)
            spt = secmap_from_json(write_v->secret, &spt_len);
        if (!data_str || !vv_str || (write_v->secret && !spt))
            e = HB_ERR_NOMEM;
        else
            e = hb__vault_apply(s->v, id, type, data_str, vv_str,
                                write_v->tomb ? 1 : 0, write_v->updated_at,
                                spt, spt ? spt_len : 0, dirty, seq);
        free(data_str);
        free(vv_str);
        if (spt) {
            sodium_memzero(spt, spt_len);
            sodium_free(spt);
        }
    }
out:
    ver_free(&L);
    merge_free(&mr);
    return e;
}

/* 解密一个远端对象的 pt，构造 R 并合入。blob 为原始二进制。 */
static hb_err apply_remote_blob(hb_sync *s, const char *id, const char *type,
                                int64_t seq, const uint8_t *blob, size_t blob_len,
                                int *decrypt_failed)
{
    char *pt;
    ver_t R;
    hb__vault_row *rows = NULL;
    const hb__vault_row *local = NULL;
    size_t n = 0, i;
    hb_err e;

    *decrypt_failed = 0;
    pt = hb_sync_blob_open(s->vk, id, type, blob, blob_len);
    if (!pt) {
        *decrypt_failed = 1;
        return HB_OK; /* 跳过，游标照常前进（§4.2） */
    }
    memset(&R, 0, sizeof R);
    if (!ver_from_pt(pt, &R)) {
        sodium_memzero(pt, strlen(pt));
        sodium_free(pt);
        *decrypt_failed = 1;
        return HB_OK;
    }
    sodium_memzero(pt, strlen(pt));
    sodium_free(pt);

    e = hb__vault_rows(s->v, &rows, &n);
    if (e != HB_OK)
        goto out;
    for (i = 0; i < n; i++)
        if (!strcmp(rows[i].id, id))
            local = &rows[i];
    /* R 的 type 以服务端记录的明文 type 为准（ad 已绑定验证）。 */
    e = apply_remote(s, id, type, seq, &R, local);
    hb__vault_rows_free(rows, n);
out:
    ver_free(&R);
    return e;
}

/* ============================ pull ============================ */

hb_err hb_sync_pull_path(hb_sync *s, char **out)
{
    int64_t cursor = 0;
    hb_err e = meta_get_i64(s->v, "sync.cursor", &cursor, 0);
    char *p;
    if (e != HB_OK)
        return e;
    p = malloc(80);
    if (!p)
        return HB_ERR_NOMEM;
    snprintf(p, 80, "/v1/objects?since=%lld&limit=500", (long long)cursor);
    *out = p;
    return HB_OK;
}

hb_err hb_sync_apply_pull(hb_sync *s, const char *resp_json,
                          hb_sync_pull_stats *stats)
{
    cJSON *j, *objs, *it;
    hb_err e = HB_OK;

    memset(stats, 0, sizeof *stats);
    j = cJSON_Parse(resp_json);
    if (!j)
        return HB_ERR_INVALID;
    objs = cJSON_GetObjectItemCaseSensitive(j, "objects");
    if (!cJSON_IsArray(objs)) {
        cJSON_Delete(j);
        return HB_ERR_INVALID;
    }
    if (!s->vk) {
        cJSON_Delete(j);
        return HB_ERR_CRYPTO;
    }
    cJSON_ArrayForEach(it, objs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jty = cJSON_GetObjectItemCaseSensitive(it, "type");
        cJSON *jseq = cJSON_GetObjectItemCaseSensitive(it, "seq");
        cJSON *jbl = cJSON_GetObjectItemCaseSensitive(it, "blob");
        const char *scope;
        if (!cJSON_IsString(jid) || !cJSON_IsString(jty) ||
            !cJSON_IsNumber(jseq) || !cJSON_IsString(jbl))
            continue;
        scope = scope_of_type(jty->valuestring);
        if (!scope || !hb_sync_scope_get(s, scope)) {
            stats->skipped_scope++;
            continue;
        }
        {
            size_t blen = 0;
            uint8_t *bin = b64_dec(jbl->valuestring, &blen);
            int df = 0;
            if (!bin) {
                stats->decrypt_failed++;
                continue;
            }
            e = apply_remote_blob(s, jid->valuestring, jty->valuestring,
                                  (int64_t)jseq->valuedouble, bin, blen, &df);
            sodium_memzero(bin, blen);
            sodium_free(bin);
            if (df)
                stats->decrypt_failed++;
            else if (e == HB_OK)
                stats->applied++;
            if (e != HB_OK)
                break;
        }
    }
    if (e == HB_OK) {
        cJSON *next = cJSON_GetObjectItemCaseSensitive(j, "next");
        cJSON *more = cJSON_GetObjectItemCaseSensitive(j, "more");
        int64_t cur;
        if (!cJSON_IsNumber(next)) {
            cJSON_Delete(j);
            return HB_ERR_INVALID;
        }
        cur = (int64_t)next->valuedouble;
        e = meta_set_i64(s->v, "sync.cursor", cur);
        stats->cursor = cur;
        stats->more = cJSON_IsTrue(more);
    }
    cJSON_Delete(j);
    return e;
}

/* ============================ push ============================ */

typedef struct { char *id; char *vv; } push_ch;

struct hb_sync_push {
    char    *body;
    size_t   n;
    push_ch *ch;    /* seal 时快照，条件标干净用 */
};

const char *hb_sync_push_body(const hb_sync_push *p)
{
    return p->body;
}

void hb_sync_push_free(hb_sync_push *p)
{
    size_t i;
    if (!p)
        return;
    for (i = 0; i < p->n; i++) {
        free(p->ch[i].id);
        free(p->ch[i].vv);
    }
    free(p->ch);
    free(p->body);
    free(p);
}

/* 行的 data 剥离设备本地字段后用于 pt。返回新 cJSON。 */
static cJSON *strip_local_fields(const cJSON *data, const char *type)
{
    cJSON *d = cJSON_CreateObject();
    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        if (it->string[0] != '_' && is_device_local(type, it->string))
            continue;
        cJSON_AddItemToObject(d, it->string, cJSON_Duplicate((cJSON *)it, 1));
    }
    return d;
}

hb_err hb_sync_build_push(hb_sync *s, hb_sync_push **out)
{
    hb__vault_row *rows = NULL;
    size_t n = 0, i;
    hb_sync_push *p = NULL;
    cJSON *body = NULL, *changes = NULL;
    hb_err e = HB_ERR_DB;

    *out = NULL;
    if (!s->vk)
        return HB_ERR_CRYPTO;
    e = hb__vault_rows(s->v, &rows, &n);
    if (e != HB_OK)
        return e;
    p = calloc(1, sizeof *p);
    body = json_obj();
    changes = cJSON_CreateArray();
    if (!p || !body || !changes) {
        e = HB_ERR_NOMEM;
        goto fail;
    }
    cJSON_AddItemToObject(body, "changes", changes);
    for (i = 0; i < n && p->n < 200; i++) {
        hb__vault_row *r = &rows[i];
        const char *scope;
        cJSON *data, *pt, *d2;
        char *pt_str;
        uint8_t *blob, *pt_sec = NULL;
        size_t blob_len = 0, pt_sec_len = 0;
        char *b64;
        cJSON *ch;

        if (!r->dirty)
            continue;
        scope = scope_of_type(r->type);
        if (!scope || !hb_sync_scope_get(s, scope))
            continue;

        data = cJSON_Parse(r->data);
        if (!data)
            continue;
        pt = json_obj();
        d2 = strip_local_fields(data, r->type);
        cJSON_Delete(data);
        if (!pt || !d2) {
            cJSON_Delete(pt);
            cJSON_Delete(d2);
            continue;
        }
        cJSON_AddStringToObject(pt, "type", r->type);
        cJSON_AddItemToObject(pt, "data", d2);
        {
            cJSON *vv = cJSON_Parse(r->vv);
            cJSON_AddItemToObject(pt, "vv", vv ? vv : json_obj());
        }
        cJSON_AddBoolToObject(pt, "tomb", r->tombstone);
        cJSON_AddNumberToObject(pt, "updated_at", (double)r->updated_at);
        if (hb__vault_secret_map(s->v, r->id, &pt_sec, &pt_sec_len) == HB_OK
            && pt_sec) {
            cJSON *m = secmap_to_json(pt_sec, pt_sec_len);
            sodium_memzero(pt_sec, pt_sec_len);
            sodium_free(pt_sec);
            pt_sec = NULL;
            if (m && cJSON_GetArraySize(m) > 0)
                cJSON_AddItemToObject(pt, "secret", m);
            else
                cJSON_Delete(m);
        }
        pt_str = cJSON_PrintUnformatted(pt);
        cJSON_Delete(pt);
        if (!pt_str)
            continue;
        blob = hb_sync_blob_seal(s->vk, r->id, r->type, pt_str, &blob_len);
        sodium_memzero(pt_str, strlen(pt_str));
        free(pt_str);
        if (!blob)
            continue;
        b64 = b64_enc(blob, blob_len);
        sodium_free(blob);
        if (!b64)
            continue;
        ch = json_obj();
        cJSON_AddStringToObject(ch, "id", r->id);
        cJSON_AddStringToObject(ch, "type", r->type);
        cJSON_AddNumberToObject(ch, "base_seq", (double)r->sync_seq);
        cJSON_AddStringToObject(ch, "blob", b64);
        free(b64);
        cJSON_AddItemToArray(changes, ch);
        {
            push_ch *nc = realloc(p->ch, (p->n + 1) * sizeof *nc);
            if (!nc) {
                cJSON_Delete(body);
                e = HB_ERR_NOMEM;
                goto fail;
            }
            p->ch = nc;
            p->ch[p->n].id = strdup(r->id);
            p->ch[p->n].vv = strdup(r->vv);
            if (!p->ch[p->n].id || !p->ch[p->n].vv) {
                free(p->ch[p->n].id);
                free(p->ch[p->n].vv);
                e = HB_ERR_NOMEM;
                goto fail;
            }
            p->n++;
        }
    }
    if (p->n == 0) {
        cJSON_Delete(body);
        hb_sync_push_free(p);
        hb__vault_rows_free(rows, n);
        return HB_OK; /* *out 保持 NULL：无变更 */
    }
    p->body = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!p->body) {
        e = HB_ERR_NOMEM;
        goto fail;
    }
    hb__vault_rows_free(rows, n);
    *out = p;
    return HB_OK;
fail:
    cJSON_Delete(body);
    hb_sync_push_free(p);
    hb__vault_rows_free(rows, n);
    return e;
}

hb_err hb_sync_apply_push(hb_sync *s, hb_sync_push *p, const char *resp_json,
                          bool *need_retry)
{
    cJSON *j, *results, *it;
    hb_err e = HB_OK;
    size_t i;

    *need_retry = false;
    j = cJSON_Parse(resp_json);
    if (!j)
        return HB_ERR_INVALID;
    results = cJSON_GetObjectItemCaseSensitive(j, "results");
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(j);
        return HB_ERR_INVALID;
    }
    cJSON_ArrayForEach(it, results) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jok = cJSON_GetObjectItemCaseSensitive(it, "ok");
        const char *id = cJSON_IsString(jid) ? jid->valuestring : NULL;
        const char *snap_vv = NULL;
        if (!id || !cJSON_IsBool(jok))
            continue;
        for (i = 0; i < p->n; i++)
            if (!strcmp(p->ch[i].id, id)) {
                snap_vv = p->ch[i].vv;
                break;
            }
        if (!snap_vv)
            continue;
        if (cJSON_IsTrue(jok)) {
            cJSON *jseq = cJSON_GetObjectItemCaseSensitive(it, "seq");
            bool applied;
            if (!cJSON_IsNumber(jseq))
                continue;
            e = hb__vault_mark_synced(s->v, id, snap_vv,
                                      (int64_t)jseq->valuedouble, &applied);
            if (e != HB_OK)
                break;
        } else {
            cJSON *cf = cJSON_GetObjectItemCaseSensitive(it, "conflict");
            cJSON *jseq = cf ? cJSON_GetObjectItemCaseSensitive(cf, "seq") : NULL;
            cJSON *jbl = cf ? cJSON_GetObjectItemCaseSensitive(cf, "blob") : NULL;
            cJSON *jty = cf ? cJSON_GetObjectItemCaseSensitive(cf, "type") : NULL;
            if (!cJSON_IsNumber(jseq) || !cJSON_IsString(jbl) ||
                !cJSON_IsString(jty)) {
                e = HB_ERR_INVALID;
                break;
            }
            {
                size_t blen = 0;
                uint8_t *bin = b64_dec(jbl->valuestring, &blen);
                int df = 0;
                hb_err e2;
                if (!bin) {
                    e = HB_ERR_CRYPTO;
                    break;
                }
                e2 = apply_remote_blob(s, id, jty->valuestring,
                                       (int64_t)jseq->valuedouble,
                                       bin, blen, &df);
                sodium_memzero(bin, blen);
                sodium_free(bin);
                if (e2 != HB_OK) {
                    e = e2;
                    break;
                }
                /* 合并结果 dirty 待重推（merge_eq_remote 为真时已标干净，
                 * 此时重推只会得到等价状态；仍标需要重试由调用方限次）。 */
                *need_retry = true;
            }
        }
    }
    cJSON_Delete(j);
    return e;
}

/* ============================ conflicts ============================ */

hb_err hb_sync_conflicts(hb_sync *s, hb_sync_conflict **out, size_t *n)
{
    hb__conflict_row *rows = NULL;
    hb_sync_conflict *arr;
    size_t cnt = 0, i;
    hb_err e = hb__vault_conflicts(s->v, &rows, &cnt);
    if (e != HB_OK)
        return e;
    arr = calloc(cnt ? cnt : 1, sizeof *arr);
    if (!arr) {
        hb__vault_conflicts_free(rows, cnt);
        return HB_ERR_NOMEM;
    }
    for (i = 0; i < cnt; i++) {
        arr[i].id = strdup(rows[i].id);
        arr[i].type = strdup(rows[i].type);
        arr[i].fields_json = strdup(rows[i].fields);
        arr[i].loser_summary = strdup(rows[i].loser_data);
        arr[i].created = rows[i].created;
    }
    hb__vault_conflicts_free(rows, cnt);
    *out = arr;
    *n = cnt;
    return HB_OK;
}

void hb_sync_conflicts_free(hb_sync_conflict *c, size_t n)
{
    size_t i;
    if (!c)
        return;
    for (i = 0; i < n; i++) {
        free(c[i].id);
        free(c[i].type);
        free(c[i].fields_json);
        free(c[i].loser_summary);
    }
    free(c);
}

size_t hb_sync_conflict_count(hb_sync *s)
{
    hb__conflict_row *rows = NULL;
    size_t n = 0;
    if (hb__vault_conflicts(s->v, &rows, &n) != HB_OK)
        return 0;
    hb__vault_conflicts_free(rows, n);
    return n;
}

hb_err hb_sync_conflict_keep_local(hb_sync *s, const char *id)
{
    return hb__vault_conflict_del(s->v, id);
}

hb_err hb_sync_conflict_take_other(hb_sync *s, const char *id)
{
    char *type = NULL, *fields = NULL, *ldata = NULL;
    uint8_t *sec_blob = NULL;
    size_t sec_blob_len = 0;
    int64_t created = 0;
    cJSON *fj = NULL, *dj = NULL;
    hb_obj *o = NULL;
    const cJSON *it;
    hb_err e;
    bool have_tomb = false;
    int loser_tomb = 0;

    e = hb__vault_conflict_get(s->v, id, &type, &fields, &ldata,
                               &sec_blob, &sec_blob_len, &created);
    if (e != HB_OK)
        return e;
    fj = cJSON_Parse(fields);
    dj = cJSON_Parse(ldata);
    if (!fj || !dj) {
        e = HB_ERR_INVALID;
        goto out;
    }
    e = hb_vault_get(s->v, id, &o);
    if (e != HB_OK && e != HB_ERR_NOT_FOUND)
        goto out;
    /* 对象可能本地已不存在（少见）：重建一个同 id 对象再 put。 */
    if (!o) {
        o = hb_obj_new_with_id(type, id);
        if (!o) {
            e = HB_ERR_NOMEM;
            goto out;
        }
    }
    cJSON_ArrayForEach(it, fj) {
        const char *f = it->valuestring;
        if (!f)
            continue;
        if (!strcmp(f, "$secret")) {
            /* 输方 secret map 写入（覆盖现有同名、删除多余）。 */
            uint8_t *pt = NULL;
            size_t pt_len = 0;
            cJSON *lmap = NULL, *cur = NULL;
            const cJSON *e1;
            hb_err se = hb__vault_secret_decrypt(s->v, id, type,
                                                 sec_blob, sec_blob_len,
                                                 &pt, &pt_len);
            if (se == HB_OK && pt) {
                lmap = secmap_to_json(pt, pt_len);
                sodium_memzero(pt, pt_len);
                sodium_free(pt);
            }
            if (lmap) {
                cJSON_ArrayForEach(e1, lmap)
                    hb_vault_set_secret(s->v, id, e1->string,
                                        e1->valuestring);
            }
            /* 删除本地有而输方没有的条目 */
            {
                uint8_t *cpt = NULL;
                size_t cpt_len = 0;
                if (hb__vault_secret_map(s->v, id, &cpt, &cpt_len) == HB_OK
                    && cpt) {
                    cur = secmap_to_json(cpt, cpt_len);
                    sodium_memzero(cpt, cpt_len);
                    sodium_free(cpt);
                }
                if (cur) {
                    cJSON_ArrayForEach(e1, cur)
                        if (!lmap || !cJSON_GetObjectItemCaseSensitive(lmap, e1->string))
                            hb_vault_set_secret(s->v, id, e1->string, NULL);
                }
            }
            cJSON_Delete(lmap);
            cJSON_Delete(cur);
        } else if (!strcmp(f, "$tomb")) {
            /* 输方 tomb 值存于 loser data 的 "_tomb"；推迟到 put 之后应用，
             * 避免先 delete 又被 put 复活。 */
            cJSON *t = cJSON_GetObjectItemCaseSensitive(dj, "_tomb");
            loser_tomb = cJSON_IsTrue(t) ? 1 : 0;
            have_tomb = true;
        } else {
            const cJSON *val = cJSON_GetObjectItemCaseSensitive(dj, f);
            if (val) {
                cJSON_DeleteItemFromObjectCaseSensitive(o->data, f);
                cJSON_AddItemToObject(o->data, f, cJSON_Duplicate((cJSON *)val, 1));
            } else {
                cJSON_DeleteItemFromObjectCaseSensitive(o->data, f);
            }
        }
    }
    e = hb_vault_put(s->v, o);
    if (e == HB_OK && have_tomb && loser_tomb)
        e = hb_vault_delete(s->v, id);
    else if (e == HB_OK && have_tomb)
        e = hb_vault_undelete(s->v, id);
    if (e == HB_OK)
        e = hb__vault_conflict_del(s->v, id);
out:
    hb_obj_free(o);
    cJSON_Delete(fj);
    cJSON_Delete(dj);
    free(type); free(fields); free(ldata); free(sec_blob);
    return e;
}

/* ============================ wipe / logout ============================ */

hb_err hb_sync_wipe(hb_sync *s)
{
    size_t sc;
    hb_err e;
    for (sc = 0; sc < N_SCOPES; sc++) {
        if (!hb_sync_scope_get(s, SCOPES[sc].scope))
            continue;
        e = hb__vault_purge_type(s->v, SCOPES[sc].types[0]);
        if (e != HB_OK)
            return e;
        if (SCOPES[sc].types[1]) {
            e = hb__vault_purge_type(s->v, SCOPES[sc].types[1]);
            if (e != HB_OK)
                return e;
        }
    }
    if ((e = hb__vault_conflicts_clear(s->v)) != HB_OK)
        return e;
    return hb__vault_meta_del(s->v, "sync.cursor");
}

hb_err hb_sync_logout_local(hb_sync *s)
{
    hb_err e;
    hb__vault_meta_del(s->v, "sync.account_id");
    hb__vault_meta_del(s->v, "sync.email");
    hb__vault_meta_del(s->v, "sync.server");
    hb__vault_meta_del(s->v, "sync.device_id");
    hb__vault_meta_del(s->v, "sync.cursor");
    if ((e = hb__vault_conflicts_clear(s->v)) != HB_OK)
        return e;
    return hb__vault_mark_all_dirty(s->v);
}
