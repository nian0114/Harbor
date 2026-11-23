/* FR-11 sync core 测试：BIP-39、KDF/wrap、blob、合并、双 vault 端到端。 */

#include "test.h"

#include "harbor/sync.h"
#include "harbor/vault.h"

#include "obj_priv.h"
#include "vault_priv.h"

#include <ctype.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cJSON.h>

static const uint8_t DEK_A[32] = { 0xa0 };
static const uint8_t DEK_B[32] = { 0xb0 };
static const uint8_t VK[32] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
};

static char vp[256];
static int vpn;

static const char *vault_path(void)
{
    snprintf(vp, sizeof vp, "/tmp/hb-sync-test-%d-%d.db", (int)getpid(), vpn++);
    return vp;
}

static hb_vault *open_vault(const uint8_t dek[32], hb_sync **sout)
{
    hb_vault *v = NULL;
    hb_sync *s;
    CHECK_EQ_INT(hb_vault_open(vault_path(), dek, &v), HB_OK);
    s = hb_sync_new(v);
    CHECK(s != NULL);
    CHECK_EQ_INT(hb_sync_set_vk(s, VK), HB_OK);
    *sout = s;
    return v;
}

static void close_vault(hb_vault *v, hb_sync *s)
{
    hb_sync_free(s);
    hb_vault_close(v);
}

static void hex2bin(const char *hex, uint8_t *out)
{
    size_t i, n = strlen(hex) / 2;
    for (i = 0; i < n; i++)
        sscanf(hex + i * 2, "%2hhx", out + i);
}

/* ============================ fake server（§6.2） ============================ */

typedef struct {
    char    *id, *type;
    int64_t  seq;
    uint8_t *blob;
    size_t   blob_len;
    int64_t  updated_at;
} fobj;

typedef struct {
    fobj   *o;
    size_t  n, cap;
    int64_t seq;
} fserver;

static fobj *fs_find(fserver *fs, const char *id)
{
    size_t i;
    for (i = 0; i < fs->n; i++)
        if (!strcmp(fs->o[i].id, id))
            return &fs->o[i];
    return NULL;
}

static void fs_free(fserver *fs)
{
    size_t i;
    for (i = 0; i < fs->n; i++) {
        free(fs->o[i].id);
        free(fs->o[i].type);
        free(fs->o[i].blob);
    }
    free(fs->o);
    memset(fs, 0, sizeof *fs);
}

/* GET /objects?since=N&limit=M → malloc'd JSON 响应。 */
static char *fs_get(fserver *fs, const char *path)
{
    int64_t since = 0;
    int limit = 500;
    size_t i, sent = 0;
    cJSON *j, *arr;
    char *out;

    sscanf(path, "/v1/objects?since=%lld&limit=%d",
           (long long *)&since, &limit);
    j = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    for (i = 0; i < fs->n && sent < (size_t)limit; i++) {
        if (fs->o[i].seq <= since)
            continue;
        {
            size_t blen = sodium_base64_encoded_len(
                fs->o[i].blob_len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            char *b64 = malloc(blen);
            cJSON *o = cJSON_CreateObject();
            sodium_bin2base64(b64, blen, fs->o[i].blob, fs->o[i].blob_len,
                              sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            cJSON_AddStringToObject(o, "id", fs->o[i].id);
            cJSON_AddStringToObject(o, "type", fs->o[i].type);
            cJSON_AddNumberToObject(o, "seq", (double)fs->o[i].seq);
            cJSON_AddStringToObject(o, "blob", b64);
            cJSON_AddNumberToObject(o, "updated_at", (double)fs->o[i].updated_at);
            cJSON_AddItemToArray(arr, o);
            since = fs->o[i].seq;
            sent++;
        }
    }
    cJSON_AddItemToObject(j, "objects", arr);
    cJSON_AddNumberToObject(j, "next", (double)since);
    {
        bool more = false;
        for (; i < fs->n; i++)
            if (fs->o[i].seq > since) { more = true; break; }
        cJSON_AddBoolToObject(j, "more", more);
    }
    out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

/* 手工插入/替换一条已封装的 blob（构造 merge 场景用）。 */
static void fs_insert(fserver *fs, const char *id, const char *type,
                      const uint8_t *blob, size_t blob_len, int64_t seq,
                      int64_t updated_at)
{
    fobj *o = fs_find(fs, id);
    if (!o) {
        if (fs->n == fs->cap) {
            fs->cap = fs->cap ? fs->cap * 2 : 8;
            fs->o = realloc(fs->o, fs->cap * sizeof *fs->o);
        }
        o = &fs->o[fs->n++];
        memset(o, 0, sizeof *o);
        o->id = strdup(id);
        o->type = strdup(type);
    } else {
        free(o->type);
        free(o->blob);
        o->type = strdup(type);
    }
    o->blob = malloc(blob_len);
    memcpy(o->blob, blob, blob_len);
    o->blob_len = blob_len;
    o->seq = seq;
    o->updated_at = updated_at;
    if (seq > fs->seq)
        fs->seq = seq;
}

/* POST /objects：CAS（base_seq 须等于当前 seq；不存在须为 0）。 */
static char *fs_post(fserver *fs, const char *body)
{
    cJSON *j = cJSON_Parse(body), *changes, *it, *rj, *arr;
    char *out;

    rj = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    changes = cJSON_GetObjectItemCaseSensitive(j, "changes");
    cJSON_ArrayForEach(it, changes) {
        const char *id = cJSON_GetObjectItemCaseSensitive(it, "id")->valuestring;
        const char *type = cJSON_GetObjectItemCaseSensitive(it, "type")->valuestring;
        int64_t base = (int64_t)cJSON_GetObjectItemCaseSensitive(it, "base_seq")->valuedouble;
        const char *b64 = cJSON_GetObjectItemCaseSensitive(it, "blob")->valuestring;
        fobj *cur = fs_find(fs, id);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "id", id);
        if ((!cur && base == 0) || (cur && cur->seq == base)) {
            size_t blen = strlen(b64);
            uint8_t *bin = malloc(blen), *bp;
            size_t bn = 0;
            sodium_base642bin(bin, blen, b64, blen, NULL, &bn, NULL,
                              sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            bp = bin;
            if (!cur) {
                if (fs->n == fs->cap) {
                    fs->cap = fs->cap ? fs->cap * 2 : 8;
                    fs->o = realloc(fs->o, fs->cap * sizeof *fs->o);
                }
                cur = &fs->o[fs->n++];
                memset(cur, 0, sizeof *cur);
                cur->id = strdup(id);
                cur->type = strdup(type);
            } else {
                free(cur->type);
                free(cur->blob);
                cur->type = strdup(type);
            }
            cur->blob = bin;
            cur->blob_len = bn;
            cur->seq = ++fs->seq;
            cur->updated_at = hb_now_ms();
            (void)bp;
            cJSON_AddBoolToObject(r, "ok", 1);
            cJSON_AddNumberToObject(r, "seq", (double)cur->seq);
        } else {
            cJSON *cf = cJSON_CreateObject();
            size_t blen = sodium_base64_encoded_len(
                cur->blob_len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            char *b = malloc(blen);
            cJSON_AddBoolToObject(r, "ok", 0);
            sodium_bin2base64(b, blen, cur->blob, cur->blob_len,
                              sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            cJSON_AddNumberToObject(cf, "seq", (double)cur->seq);
            cJSON_AddStringToObject(cf, "blob", b);
            cJSON_AddStringToObject(cf, "type", cur->type);
            cJSON_AddItemToObject(r, "conflict", cf);
            free(b);
        }
        cJSON_AddItemToArray(arr, r);
    }
    cJSON_AddItemToObject(rj, "results", arr);
    out = cJSON_PrintUnformatted(rj);
    cJSON_Delete(rj);
    cJSON_Delete(j);
    return out;
}

static size_t g_decrypt_fail;

static void do_pull(hb_sync *s, fserver *fs)
{
    for (;;) {
        char *path = NULL, *resp;
        hb_sync_pull_stats st;
        CHECK_EQ_INT(hb_sync_pull_path(s, &path), HB_OK);
        resp = fs_get(fs, path);
        free(path);
        CHECK_EQ_INT(hb_sync_apply_pull(s, resp, &st), HB_OK);
        g_decrypt_fail += st.decrypt_failed;
        free(resp);
        if (!st.more)
            break;
    }
}

static void do_push(hb_sync *s, fserver *fs)
{
    int t;
    for (t = 0; t < 3; t++) {
        hb_sync_push *p = NULL;
        char *resp;
        bool retry = false;
        CHECK_EQ_INT(hb_sync_build_push(s, &p), HB_OK);
        if (!p)
            return;
        resp = fs_post(fs, hb_sync_push_body(p));
        CHECK_EQ_INT(hb_sync_apply_push(s, p, resp, &retry), HB_OK);
        free(resp);
        hb_sync_push_free(p);
        if (!retry)
            return;
    }
}

static void sync_round(hb_sync *s, fserver *fs)
{
    do_pull(s, fs);
    do_push(s, fs);
}

/* 两端反复同步直到 quiescent。 */
static void converge(hb_sync *sa, hb_sync *sb, fserver *fs)
{
    int i;
    for (i = 0; i < 6; i++) {
        sync_round(sa, fs);
        sync_round(sb, fs);
    }
}

/* 构造远端 pt 并封成 blob（merge 单测用）。data 须含 _fv。 */
static uint8_t *craft_blob(const char *id, const char *type,
                           const char *data_json, const char *vv_json,
                           bool tomb, int64_t upd, const char *secret_json,
                           size_t *out_len)
{
    cJSON *pt = cJSON_CreateObject();
    char *str;
    uint8_t *blob;

    cJSON_AddStringToObject(pt, "type", type);
    cJSON_AddItemToObject(pt, "data", cJSON_Parse(data_json));
    cJSON_AddItemToObject(pt, "vv", cJSON_Parse(vv_json));
    cJSON_AddBoolToObject(pt, "tomb", tomb);
    cJSON_AddNumberToObject(pt, "updated_at", (double)upd);
    if (secret_json)
        cJSON_AddItemToObject(pt, "secret", cJSON_Parse(secret_json));
    str = cJSON_PrintUnformatted(pt);
    cJSON_Delete(pt);
    blob = hb_sync_blob_seal(VK, id, type, str, out_len);
    free(str);
    return blob;
}

static char *b64url(const uint8_t *bin, size_t n)
{
    size_t blen = sodium_base64_encoded_len(n, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char *s = malloc(blen);
    sodium_bin2base64(s, blen, bin, n, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return s;
}

/* ============================ 1. BIP-39 ============================ */

static void t_bip39(void)
{
    static const struct { const char *hex, *mn; } V[] = {
        { "0000000000000000000000000000000000000000000000000000000000000000",
          "abandon abandon abandon abandon abandon abandon abandon abandon "
          "abandon abandon abandon abandon abandon abandon abandon abandon "
          "abandon abandon abandon abandon abandon abandon abandon art" },
        { "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
          "legal winner thank year wave sausage worth useful legal winner "
          "thank year wave sausage worth useful legal winner thank year "
          "wave sausage worth title" },
        { "8080808080808080808080808080808080808080808080808080808080808080",
          "letter advice cage absurd amount doctor acoustic avoid letter "
          "advice cage absurd amount doctor acoustic avoid letter advice "
          "cage absurd amount doctor acoustic bless" },
        { "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
          "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo "
          "zoo zoo zoo zoo zoo zoo zoo vote" },
        { "68a79eaca2324873eacc50cb9c6eca8cc68ea5d936f98787c60c7ebc74e6ce7c",
          "hamster diagram private dutch cause delay private meat slide "
          "toddler razor book happy fancy gospel tennis maple dilemma loan "
          "word shrug inflict delay length" },
    };
    size_t i;

    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        uint8_t ent[32], back[32];
        char words[512];
        hex2bin(V[i].hex, ent);
        CHECK(hb_bip39_encode(ent, words, sizeof words) == 0);
        CHECK_EQ_STR(words, V[i].mn);
        /* round-trip（大写 + 多余空白容忍） */
        {
            char noisy[600];
            size_t k = 0;
            const char *p;
            noisy[k++] = ' ';
            for (p = words; *p; p++)
                noisy[k++] = (*p == ' ') ? ' ' : (char)toupper((unsigned char)*p);
            noisy[k++] = '\t';
            noisy[k] = 0;
            /* 简单替换连续空格仍可解析（decode 跳过任意空白） */
            CHECK(hb_bip39_decode(noisy, back) == 0);
        }
        CHECK(memcmp(ent, back, 32) == 0);
        CHECK(hb_bip39_decode(words, back) == 0);
        CHECK(memcmp(ent, back, 32) == 0);
    }
    /* 坏校验和：最后一词换 zoo（对 000… 向量而言 checksum 不符） */
    {
        uint8_t e[32];
        CHECK(hb_bip39_decode(
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon zoo", e) != 0);
    }
    /* 未知词 */
    {
        uint8_t e[32];
        CHECK(hb_bip39_decode(
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon notaword",
            e) != 0);
    }
}

/* ============================ 2. KDF / wrap ============================ */

static void t_kdf_wrap(void)
{
    static const uint8_t salt[16] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    };
    uint8_t mk1[32], mk2[32], a1[32], a2[32], w1[32], vk[32], vk2[32];
    uint8_t *sealed;
    size_t n;

    CHECK_EQ_INT(hb_sync_pwhash("pw-正确", salt, 2, 64 * 1024 * 1024, mk1), HB_OK);
    CHECK_EQ_INT(hb_sync_pwhash("pw-正确", salt, 2, 64 * 1024 * 1024, mk2), HB_OK);
    CHECK(memcmp(mk1, mk2, 32) == 0);
    hb_sync_derive_mk(mk1, a1, w1);
    hb_sync_derive_mk(mk2, a2, vk2);
    CHECK(memcmp(a1, a2, 32) == 0); /* auth_key 确定性 */

    randombytes_buf(vk, 32);
    sealed = hb_sync_seal(w1, vk, 32, (const uint8_t *)"hb:vk:pw:v1", 11, &n);
    CHECK(sealed != NULL);
    {
        uint8_t *pt;
        size_t pn;
        pt = hb_sync_open(w1, sealed, n, (const uint8_t *)"hb:vk:pw:v1", 11, &pn);
        CHECK(pt != NULL && pn == 32 && memcmp(pt, vk, 32) == 0);
        if (pt) { sodium_memzero(pt, pn); sodium_free(pt); }
    }
    /* 错口令 → 不同 wrap_key → 解不开 */
    {
        uint8_t mk3[32], a3[32], w3[32], *pt;
        size_t pn;
        CHECK_EQ_INT(hb_sync_pwhash("wrong", salt, 2, 64 * 1024 * 1024, mk3), HB_OK);
        hb_sync_derive_mk(mk3, a3, w3);
        pt = hb_sync_open(w3, sealed, n, (const uint8_t *)"hb:vk:pw:v1", 11, &pn);
        CHECK(pt == NULL);
        sodium_memzero(mk3, 32); sodium_memzero(a3, 32); sodium_memzero(w3, 32);
    }
    /* tampered AD → 失败 */
    {
        uint8_t *pt;
        size_t pn;
        pt = hb_sync_open(w1, sealed, n, (const uint8_t *)"hb:vk:pw:v2", 11, &pn);
        CHECK(pt == NULL);
    }
    sodium_free(sealed);
    /* KDF 低于下限：prelogin_parse 拒绝 */
    {
        uint8_t s2[16];
        uint64_t ops, mem;
        char *b = b64url(salt, 16);
        cJSON *j = cJSON_CreateObject(), *k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "salt", b);
        cJSON_AddNumberToObject(k, "ops", 1);
        cJSON_AddNumberToObject(k, "mem", 64 * 1024 * 1024);
        cJSON_AddItemToObject(j, "kdf", k);
        {
            char *r = cJSON_PrintUnformatted(j);
            CHECK_EQ_INT(hb_sync_prelogin_parse(r, s2, &ops, &mem), HB_ERR_CRYPTO);
            free(r);
        }
        cJSON_Delete(j);
        free(b);
    }
    /* 边界：ops=2 mem=64MiB 接受 */
    {
        uint8_t s2[16];
        uint64_t ops, mem;
        char *b = b64url(salt, 16);
        cJSON *j = cJSON_CreateObject(), *k = cJSON_CreateObject();
        char *r;
        cJSON_AddStringToObject(k, "salt", b);
        cJSON_AddNumberToObject(k, "ops", 2);
        cJSON_AddNumberToObject(k, "mem", 64 * 1024 * 1024);
        cJSON_AddItemToObject(j, "kdf", k);
        r = cJSON_PrintUnformatted(j);
        CHECK_EQ_INT(hb_sync_prelogin_parse(r, s2, &ops, &mem), HB_OK);
        CHECK(memcmp(s2, salt, 16) == 0);
        free(r);
        cJSON_Delete(j);
        free(b);
    }
    sodium_memzero(mk1, 32); sodium_memzero(mk2, 32);
    sodium_memzero(a1, 32); sodium_memzero(a2, 32); sodium_memzero(w1, 32);
}

/* ============================ 3. blob ============================ */

static void t_blob(void)
{
    const char *id = "11111111-2222-3333-4444-555555555555";
    const char *pt = "{\"type\":\"host\",\"data\":{\"name\":\"n\",\"_fv\":{}},"
                     "\"vv\":{},\"tomb\":false,\"updated_at\":1}";
    size_t n = 0;
    uint8_t *blob = hb_sync_blob_seal(VK, id, "host", pt, &n);
    char *back;

    CHECK(blob != NULL);
    /* padding：blob = 0x01 || nonce(24) || padded+tag(16) → n-41 % 256 == 0 */
    CHECK((n - 41) % 256 == 0);
    back = hb_sync_blob_open(VK, id, "host", blob, n);
    CHECK(back != NULL);
    CHECK_EQ_STR(back, pt);
    if (back) { sodium_memzero(back, strlen(back)); sodium_free(back); }
    /* 改 id / type → AEAD 失败 */
    CHECK(hb_sync_blob_open(VK, "other-id-xxxxxxxx", "host", blob, n) == NULL);
    CHECK(hb_sync_blob_open(VK, id, "key", blob, n) == NULL);
    sodium_free(blob);

    /* 填充边界：长度恰为 256 倍数时要多补一整块，否则 unpad 失败 */
    {
        static const size_t lens[] = { 1, 255, 256, 257, 511, 512, 513 };
        size_t i;
        for (i = 0; i < sizeof lens / sizeof lens[0]; i++) {
            char *p2 = malloc(lens[i] + 1);
            size_t bn = 0;
            uint8_t *b2;
            memset(p2, 'x', lens[i]);
            p2[lens[i]] = 0;
            b2 = hb_sync_blob_seal(VK, id, "host", p2, &bn);
            CHECK(b2 != NULL);
            if (b2) {
                CHECK((bn - 41) % 256 == 0);
                CHECK(bn - 41 > lens[i]);
                back = hb_sync_blob_open(VK, id, "host", b2, bn);
                CHECK(back != NULL);
                if (back) {
                    CHECK_EQ_STR(back, p2);
                    sodium_memzero(back, strlen(back));
                    sodium_free(back);
                }
                sodium_free(b2);
            }
            free(p2);
        }
    }
}

/* ============================ 4. merge（经 pull 喂远端版本） ============================ */

static hb_sync *m_sync;
static hb_vault *m_v;
static fserver m_fs;

static void mv_setup(void)
{
    vpn++;
    m_v = open_vault(DEK_A, &m_sync);
    memset(&m_fs, 0, sizeof m_fs);
    g_decrypt_fail = 0;
}

static void mv_teardown(void)
{
    fs_free(&m_fs);
    close_vault(m_v, m_sync);
}

/* 拉一个手工 blob（seq 递增）。 */
static void pull_blob(const char *id, const char *type, uint8_t *blob, size_t n)
{
    fs_insert(&m_fs, id, type, blob, n, ++m_fs.seq, hb_now_ms());
    sodium_free(blob);
    do_pull(m_sync, &m_fs);
}

static char *row_data(hb_vault *v, const char *id)
{
    hb__vault_row *rows = NULL;
    size_t n = 0, i;
    char *d = NULL;
    CHECK_EQ_INT(hb__vault_rows(v, &rows, &n), HB_OK);
    for (i = 0; i < n; i++)
        if (!strcmp(rows[i].id, id)) d = strdup(rows[i].data);
    hb__vault_rows_free(rows, n);
    return d;
}

static void t_merge_cases(void)
{
    hb_obj *o;
    const char *id = "obj-merge-1";
    char *d;
    uint8_t *blob;
    size_t bn;

    mv_setup();

    /* 本地 L：host {name:"A", port:22} */
    o = hb_obj_new_with_id("host", id);
    hb_obj_set_str(o, "name", "A");
    hb_obj_set_int(o, "port", 22);
    CHECK_EQ_INT(hb_vault_put(m_v, o), HB_OK);
    hb_obj_free(o);
    /* put 后标干净，模拟已同步状态（否则 L dirty 时规则不同，见下） */
    {
        hb__vault_row *rows = NULL;
        size_t n = 0, i;
        char *l_data = NULL, *l_vv = NULL;
        int64_t upd = 0;
        CHECK_EQ_INT(hb__vault_rows(m_v, &rows, &n), HB_OK);
        for (i = 0; i < n; i++)
            if (!strcmp(rows[i].id, id)) {
                l_data = strdup(rows[i].data);
                l_vv = strdup(rows[i].vv);
                upd = rows[i].updated_at;
            }
        hb__vault_rows_free(rows, n);

        /* --- case: 不同字段 → 都保留、无冲突 --- */
        {
            char *rdata = malloc(strlen(l_data) + 256);
            /* R：在 L 的 _fv 之外新加 desc 字段（devB 戳） */
            cJSON *rd = cJSON_Parse(l_data), *fv;
            char *rd_s;
            fv = cJSON_GetObjectItemCaseSensitive(rd, "_fv");
            cJSON_AddStringToObject(rd, "desc", "from-B");
            {
                cJSON *st = cJSON_CreateArray();
                cJSON_AddItemToArray(st, cJSON_CreateString("devB"));
                cJSON_AddItemToArray(st, cJSON_CreateNumber(1));
                cJSON_AddItemToObject(fv, "desc", st);
            }
            rd_s = cJSON_PrintUnformatted(rd);
            cJSON_Delete(rd);
            strcpy(rdata, rd_s);
            free(rd_s);
            blob = craft_blob(id, "host", rdata,
                              "{\"devB\":1}", false, upd + 1, NULL, &bn);
            free(rdata);
            pull_blob(id, "host", blob, bn);
            d = row_data(m_v, id);
            CHECK(d && strstr(d, "\"name\":\"A\"") && strstr(d, "\"desc\":\"from-B\""));
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 0);
            free(d);
        }

        /* --- case: 同字段并发 → updated_at 大者胜 + 冲突记录 --- */
        {
            cJSON *rd = cJSON_Parse(l_data), *fv, *st;
            char *rd_s;
            fv = cJSON_GetObjectItemCaseSensitive(rd, "_fv");
            cJSON_DeleteItemFromObjectCaseSensitive(rd, "port");
            cJSON_AddNumberToObject(rd, "port", 2222);
            cJSON_DeleteItemFromObjectCaseSensitive(fv, "port");
            st = cJSON_CreateArray();
            cJSON_AddItemToArray(st, cJSON_CreateString("devB"));
            cJSON_AddItemToArray(st, cJSON_CreateNumber(2));
            cJSON_AddItemToObject(fv, "port", st);
            rd_s = cJSON_PrintUnformatted(rd);
            cJSON_Delete(rd);
            /* R.updated_at 更大 → R 赢 */
            blob = craft_blob(id, "host", rd_s, "{\"devB\":2}",
                              false, upd + 1000, NULL, &bn);
            free(rd_s);
            pull_blob(id, "host", blob, bn);
            d = row_data(m_v, id);
            CHECK(d && strstr(d, "\"port\":2222"));
            free(d);
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 1);
            {
                hb_sync_conflict *cf = NULL;
                size_t nc = 0;
                CHECK_EQ_INT(hb_sync_conflicts(m_sync, &cf, &nc), HB_OK);
                CHECK_EQ_INT(nc, 1);
                if (nc) {
                    CHECK(strstr(cf[0].fields_json, "\"port\"") != NULL);
                    /* loser = L：含本地 port 22 */
                    CHECK(strstr(cf[0].loser_summary, "\"port\":22") != NULL ||
                          strstr(cf[0].loser_summary, "\"port\": 22") != NULL);
                }
                hb_sync_conflicts_free(cf, nc);
            }
            CHECK_EQ_INT(hb_sync_conflict_keep_local(m_sync, id), HB_OK);
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 0);
        }

        /* --- case: R 支配 L（vv ⊋）→ 无冲突直接替换 --- */
        {
            hb_obj *o2;
            char domvv[512] = {0}, *l2, *lvv = NULL;
            o2 = hb_obj_new_with_id("host", "obj-dom");
            hb_obj_set_str(o2, "name", "base");
            CHECK_EQ_INT(hb_vault_put(m_v, o2), HB_OK);
            hb_obj_free(o2);
            l2 = row_data(m_v, "obj-dom");
            {
                hb__vault_row *rr = NULL;
                size_t rn = 0, ri;
                CHECK_EQ_INT(hb__vault_rows(m_v, &rr, &rn), HB_OK);
                for (ri = 0; ri < rn; ri++)
                    if (!strcmp(rr[ri].id, "obj-dom")) lvv = strdup(rr[ri].vv);
                hb__vault_rows_free(rr, rn);
            }
            {
                cJSON *rd = cJSON_Parse(l2), *fv, *dvv;
                char *rd_s;
                /* R.vv = L.vv ∪ {devB:5} → 严格支配 L */
                dvv = cJSON_Parse(lvv ? lvv : "{}");
                cJSON_AddNumberToObject(dvv, "devB", 5);
                {
                    char *t = cJSON_PrintUnformatted(dvv);
                    snprintf(domvv, sizeof domvv, "%s", t);
                    free(t);
                }
                cJSON_Delete(dvv);
                fv = cJSON_GetObjectItemCaseSensitive(rd, "_fv");
                cJSON_DeleteItemFromObjectCaseSensitive(rd, "name");
                cJSON_AddStringToObject(rd, "name", "dominated");
                {
                    cJSON *st = cJSON_CreateArray();
                    cJSON_AddItemToArray(st, cJSON_CreateString("devB"));
                    cJSON_AddItemToArray(st, cJSON_CreateNumber(5));
                    cJSON_DeleteItemFromObjectCaseSensitive(fv, "name");
                    cJSON_AddItemToObject(fv, "name", st);
                }
                rd_s = cJSON_PrintUnformatted(rd);
                cJSON_Delete(rd);
                blob = craft_blob("obj-dom", "host", rd_s, domvv,
                                  false, upd + 2000, NULL, &bn);
                free(rd_s);
            }
            pull_blob("obj-dom", "host", blob, bn);
            d = row_data(m_v, "obj-dom");
            CHECK(d && strstr(d, "\"name\":\"dominated\""));
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 0);
            free(d);
            free(l2);
            free(lvv);
        }

        /* --- case: 删除 vs 另一字段并发编辑（真实历史：R 见过 A 的创建）→
         *    互不冲突：保持删除、编辑值留在已删对象中、无冲突、dirty --- */
        {
            hb_obj *o3;
            const char *id3 = "obj-del";
            char lvv[512], *l3, *rd_s, *put_vv = NULL;
            cJSON *rd, *fv, *dvv;
            uint8_t *b3;
            size_t b3n;

            o3 = hb_obj_new_with_id("host", id3);
            hb_obj_set_str(o3, "name", "victim");
            hb_obj_set_str(o3, "user", "u1");
            CHECK_EQ_INT(hb_vault_put(m_v, o3), HB_OK);
            hb_obj_free(o3);
            /* put 后的 vv（devA:c_put）——R 的版本基于此历史 */
            {
                hb__vault_row *rows = NULL;
                size_t n2 = 0, i2;
                CHECK_EQ_INT(hb__vault_rows(m_v, &rows, &n2), HB_OK);
                for (i2 = 0; i2 < n2; i2++)
                    if (!strcmp(rows[i2].id, id3)) put_vv = strdup(rows[i2].vv);
                hb__vault_rows_free(rows, n2);
            }
            CHECK_EQ_INT(hb_vault_delete(m_v, id3), HB_OK); /* $tomb 戳 devA */
            l3 = row_data(m_v, id3);
            /* R：非 tomb，user 改 devB；vv = put_vv ∪ {devB:1}（见过 A 的创建） */
            rd = cJSON_Parse(l3);
            fv = cJSON_GetObjectItemCaseSensitive(rd, "_fv");
            cJSON_DeleteItemFromObjectCaseSensitive(fv, "$tomb");
            cJSON_DeleteItemFromObjectCaseSensitive(rd, "user");
            cJSON_AddStringToObject(rd, "user", "edited-by-B");
            {
                cJSON *st = cJSON_CreateArray();
                cJSON_AddItemToArray(st, cJSON_CreateString("devB"));
                cJSON_AddItemToArray(st, cJSON_CreateNumber(1));
                cJSON_DeleteItemFromObjectCaseSensitive(fv, "user");
                cJSON_AddItemToObject(fv, "user", st);
            }
            rd_s = cJSON_PrintUnformatted(rd);
            cJSON_Delete(rd);
            dvv = cJSON_Parse(put_vv);
            cJSON_AddNumberToObject(dvv, "devB", 1);
            {
                char *t = cJSON_PrintUnformatted(dvv);
                snprintf(lvv, sizeof lvv, "%s", t);
                free(t);
            }
            cJSON_Delete(dvv);
            b3 = craft_blob(id3, "host", rd_s, lvv, false, 1, NULL, &b3n);
            free(rd_s);
            pull_blob(id3, "host", b3, b3n);
            {
                hb__vault_row *rows = NULL;
                size_t n2 = 0, i2;
                int tomb = -1, dirty = -1;
                CHECK_EQ_INT(hb__vault_rows(m_v, &rows, &n2), HB_OK);
                for (i2 = 0; i2 < n2; i2++)
                    if (!strcmp(rows[i2].id, id3)) {
                        tomb = rows[i2].tombstone;
                        dirty = rows[i2].dirty;
                    }
                hb__vault_rows_free(rows, n2);
                CHECK_EQ_INT(tomb, 1);   /* 仍删除 */
                CHECK_EQ_INT(dirty, 1);  /* 合并结果 ≠ R（含 tomb 状态）→ 待推 */
            }
            /* 编辑值保留在已删除对象中 */
            d = row_data(m_v, id3);
            CHECK(d && strstr(d, "\"user\":\"edited-by-B\"") != NULL);
            free(d);
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 0);
            free(l3);
            free(put_vv);
        }

        /* --- case: 镜像：L 编辑字段 vs R 删除 → 仍删除、L 的编辑保留、无冲突 --- */
        {
            hb_obj *o3b;
            const char *id3b = "obj-del2";
            char lvv[512], *put_vv = NULL, *put_data = NULL, *rd_s;
            cJSON *rd, *fv, *dvv;
            uint8_t *b3;
            size_t b3n;

            o3b = hb_obj_new_with_id("host", id3b);
            hb_obj_set_str(o3b, "name", "victim2");
            hb_obj_set_str(o3b, "user", "u2");
            CHECK_EQ_INT(hb_vault_put(m_v, o3b), HB_OK);
            hb_obj_free(o3b);
            /* 快照 put 后的 data/vv（R 的删除版本基于此） */
            {
                hb__vault_row *rows = NULL;
                size_t n2 = 0, i2;
                CHECK_EQ_INT(hb__vault_rows(m_v, &rows, &n2), HB_OK);
                for (i2 = 0; i2 < n2; i2++)
                    if (!strcmp(rows[i2].id, id3b)) {
                        put_vv = strdup(rows[i2].vv);
                        put_data = strdup(rows[i2].data);
                    }
                hb__vault_rows_free(rows, n2);
            }
            /* L 编辑 user */
            o3b = NULL;
            CHECK_EQ_INT(hb_vault_get(m_v, id3b, &o3b), HB_OK);
            hb_obj_set_str(o3b, "user", "L-edit");
            CHECK_EQ_INT(hb_vault_put(m_v, o3b), HB_OK);
            hb_obj_free(o3b);
            /* R：tomb，$tomb 戳 devB；vv = put_vv ∪ {devB:1} */
            rd = cJSON_Parse(put_data);
            fv = cJSON_GetObjectItemCaseSensitive(rd, "_fv");
            {
                cJSON *st = cJSON_CreateArray();
                cJSON_AddItemToArray(st, cJSON_CreateString("devB"));
                cJSON_AddItemToArray(st, cJSON_CreateNumber(1));
                cJSON_DeleteItemFromObjectCaseSensitive(fv, "$tomb");
                cJSON_AddItemToObject(fv, "$tomb", st);
            }
            rd_s = cJSON_PrintUnformatted(rd);
            cJSON_Delete(rd);
            dvv = cJSON_Parse(put_vv);
            cJSON_AddNumberToObject(dvv, "devB", 1);
            {
                char *t = cJSON_PrintUnformatted(dvv);
                snprintf(lvv, sizeof lvv, "%s", t);
                free(t);
            }
            cJSON_Delete(dvv);
            b3 = craft_blob(id3b, "host", rd_s, lvv, true, 1, NULL, &b3n);
            free(rd_s);
            pull_blob(id3b, "host", b3, b3n);
            {
                hb__vault_row *rows = NULL;
                size_t n2 = 0, i2;
                int tomb = -1;
                CHECK_EQ_INT(hb__vault_rows(m_v, &rows, &n2), HB_OK);
                for (i2 = 0; i2 < n2; i2++)
                    if (!strcmp(rows[i2].id, id3b)) tomb = rows[i2].tombstone;
                hb__vault_rows_free(rows, n2);
                CHECK_EQ_INT(tomb, 1); /* R 的删除赢 */
            }
            /* L 的编辑保留在已删除对象中 */
            d = row_data(m_v, id3b);
            CHECK(d && strstr(d, "\"user\":\"L-edit\"") != NULL);
            free(d);
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 0);
            /* 撤销删除 → 合并的编辑可见 */
            CHECK_EQ_INT(hb_vault_undelete(m_v, id3b), HB_OK);
            {
                hb_obj *uo = NULL;
                CHECK_EQ_INT(hb_vault_get(m_v, id3b, &uo), HB_OK);
                CHECK_EQ_STR(hb_obj_get_str(uo, "user"), "L-edit");
                CHECK(!hb_obj_tombstone(uo));
                hb_obj_free(uo);
            }
            free(put_vv);
            free(put_data);
        }

        /* --- case: $secret 并发 --- */
        {
            hb_obj *o4;
            const char *id4 = "obj-sec";
            char *l4, *rd_s;
            cJSON *rd, *fv;
            uint8_t *b4;
            size_t b4n;

            o4 = hb_obj_new_with_id("host", id4);
            hb_obj_set_str(o4, "name", "s");
            CHECK_EQ_INT(hb_vault_put(m_v, o4), HB_OK);
            hb_obj_free(o4);
            CHECK_EQ_INT(hb_vault_set_secret(m_v, id4, "password", "local-pw"), HB_OK);
            l4 = row_data(m_v, id4);
            rd = cJSON_Parse(l4);
            fv = cJSON_GetObjectItemCaseSensitive(rd, "_fv");
            cJSON_DeleteItemFromObjectCaseSensitive(fv, "$secret");
            {
                cJSON *st = cJSON_CreateArray();
                cJSON_AddItemToArray(st, cJSON_CreateString("devB"));
                cJSON_AddItemToArray(st, cJSON_CreateNumber(9));
                cJSON_AddItemToObject(fv, "$secret", st);
            }
            rd_s = cJSON_PrintUnformatted(rd);
            cJSON_Delete(rd);
            b4 = craft_blob(id4, "host", rd_s, "{\"devB\":9}", false,
                            upd + 5000, "{\"password\":\"remote-pw\"}", &b4n);
            free(rd_s);
            pull_blob(id4, "host", b4, b4n);
            /* R.updated_at 更大 → R 的 secret 赢 */
            {
                char *pw = hb_vault_get_secret(m_v, id4, "password");
                CHECK(pw && strcmp(pw, "remote-pw") == 0);
                if (pw) { sodium_memzero(pw, strlen(pw)); sodium_free(pw); }
            }
            CHECK_EQ_INT(hb_sync_conflict_count(m_sync), 1);
            CHECK_EQ_INT(hb_sync_conflict_keep_local(m_sync, id4), HB_OK);
            free(l4);
        }

        /* --- delete → undo：$tomb 打两次戳，同步为未删除 --- */
        {
            hb_obj *o5;
            const char *id5 = "obj-undo";
            char *l5;
            cJSON *dd, *fv;
            double c2;

            o5 = hb_obj_new_with_id("host", id5);
            hb_obj_set_str(o5, "name", "u");
            CHECK_EQ_INT(hb_vault_put(m_v, o5), HB_OK);
            hb_obj_free(o5);
            CHECK_EQ_INT(hb_vault_delete(m_v, id5), HB_OK);
            CHECK_EQ_INT(hb_vault_undelete(m_v, id5), HB_OK);
            l5 = row_data(m_v, id5);
            dd = cJSON_Parse(l5);
            fv = cJSON_GetObjectItemCaseSensitive(dd, "_fv");
            {
                cJSON *st = cJSON_GetObjectItemCaseSensitive(fv, "$tomb");
                CHECK(st != NULL);
                c2 = cJSON_GetArrayItem(st, 1)->valuedouble;
            }
            /* 计数器：put=1, delete=2, undelete=3 → $tomb 戳 == 3 且戳过两次 */
            CHECK(c2 >= 3);
            cJSON_Delete(dd);
            free(l5);
            {
                hb__vault_row *rows = NULL;
                size_t n5 = 0, i5;
                int tomb = -1;
                CHECK_EQ_INT(hb__vault_rows(m_v, &rows, &n5), HB_OK);
                for (i5 = 0; i5 < n5; i5++)
                    if (!strcmp(rows[i5].id, id5)) tomb = rows[i5].tombstone;
                hb__vault_rows_free(rows, n5);
                CHECK_EQ_INT(tomb, 0); /* 同步为未删除 */
            }
        }
        free(l_data);
        free(l_vv);
    }
    mv_teardown();
}

/* ============================ 5. 端到端（双 vault） ============================ */

static hb_vault *va, *vb;
static hb_sync *sa, *sb;
static fserver fs;

static void e2e_setup(void)
{
    va = open_vault(DEK_A, &sa);
    vb = open_vault(DEK_B, &sb);
    memset(&fs, 0, sizeof fs);
    g_decrypt_fail = 0;
    CHECK_EQ_INT(hb_sync_set_account(sa, "acct1", "a@b.c", "https://s", "srvdevA"), HB_OK);
    CHECK_EQ_INT(hb_sync_set_account(sb, "acct1", "a@b.c", "https://s", "srvdevB"), HB_OK);
}

static void e2e_teardown(void)
{
    fs_free(&fs);
    close_vault(va, sa);
    close_vault(vb, sb);
}

static const char *fstr(hb_vault *v, const char *id, const char *f)
{
    static char buf[512];
    hb_obj *o = NULL;
    buf[0] = 0;
    if (hb_vault_get(v, id, &o) == HB_OK && o) {
        const char *s = hb_obj_get_str(o, f);
        if (s)
            snprintf(buf, sizeof buf, "%s", s);
        hb_obj_free(o);
    }
    return buf;
}

static int64_t fint(hb_vault *v, const char *id, const char *f, int64_t def)
{
    hb_obj *o = NULL;
    int64_t r = def;
    if (hb_vault_get(v, id, &o) == HB_OK && o) {
        r = hb_obj_get_int(o, f, def);
        hb_obj_free(o);
    }
    return r;
}

static bool fexists(hb_vault *v, const char *id)
{
    hb_obj *o = NULL;
    bool r = hb_vault_get(v, id, &o) == HB_OK;
    hb_obj_free(o);
    return r;
}

static void t_e2e(void)
{
    hb_obj *o;
    const char *hid = "host-e2e-1", *kid = "key-e2e-1", *nid = "snip-e2e-1";

    e2e_setup();

    /* --- A 建 host+key(secret)+snippet → push → B pull → 一致 --- */
    o = hb_obj_new_with_id("host", hid);
    hb_obj_set_str(o, "name", "prod-web");
    hb_obj_set_str(o, "address", "10.0.0.7");
    hb_obj_set_int(o, "port", 22);
    hb_obj_set_str(o, "user", "root");
    hb_obj_set_str(o, "identity_file", "/home/me/.ssh/id_local"); /* 设备本地 */
    CHECK_EQ_INT(hb_vault_put(va, o), HB_OK);
    hb_obj_free(o);
    o = hb_obj_new_with_id("key", kid);
    hb_obj_set_str(o, "name", "deploy-key");
    CHECK_EQ_INT(hb_vault_put(va, o), HB_OK);
    hb_obj_free(o);
    CHECK_EQ_INT(hb_vault_set_secret(va, kid, "private_key",
        "-----BEGIN OPENSSH PRIVATE KEY-----\\nfakesecretkeymaterial\n-----END----"), HB_OK);
    CHECK_EQ_INT(hb_vault_set_secret(va, hid, "password", "s3cret-pw"), HB_OK);
    o = hb_obj_new_with_id("snippet", nid);
    hb_obj_set_str(o, "title", "restart-nginx");
    hb_obj_set_str(o, "cmd", "systemctl restart nginx");
    CHECK_EQ_INT(hb_vault_put(va, o), HB_OK);
    hb_obj_free(o);

    sync_round(sa, &fs);
    CHECK(fs.n >= 3);
    sync_round(sb, &fs);

    /* 数据一致 */
    CHECK_EQ_STR(fstr(vb, hid, "name"), "prod-web");
    CHECK_EQ_STR(fstr(vb, hid, "address"), "10.0.0.7");
    CHECK_EQ_STR(fstr(vb, nid, "title"), "restart-nginx");
    CHECK_EQ_STR(fstr(vb, kid, "name"), "deploy-key");
    /* secret 一致 */
    {
        char *pk = hb_vault_get_secret(vb, kid, "private_key");
        char *pw = hb_vault_get_secret(vb, hid, "password");
        CHECK(pk && strstr(pk, "fakesecretkeymaterial") != NULL);
        CHECK(pw && strcmp(pw, "s3cret-pw") == 0);
        if (pk) { sodium_memzero(pk, strlen(pk)); sodium_free(pk); }
        if (pw) { sodium_memzero(pw, strlen(pw)); sodium_free(pw); }
    }
    /* 设备本地字段不同步：B 上没有 identity_file */
    CHECK_EQ_STR(fstr(vb, hid, "identity_file"), "");
    /* A 本地保留 */
    CHECK(strstr(fstr(va, hid, "identity_file"), "id_local") != NULL);

    /* --- 零知识：服务端 blob 无明文 --- */
    {
        size_t i;
        for (i = 0; i < fs.n; i++) {
            const uint8_t *b = fs.o[i].blob;
            size_t bl = fs.o[i].blob_len, k;
            const char *needles[] = {
                "prod-web", "10.0.0.7", "fakesecretkeymaterial", "s3cret-pw",
                "restart-nginx"
            };
            for (k = 0; k < sizeof needles / sizeof needles[0]; k++) {
                const char *nd = needles[k];
                size_t nl = strlen(nd), x;
                bool found = false;
                for (x = 0; x + nl <= bl; x++)
                    if (!memcmp(b + x, nd, nl)) { found = true; break; }
                CHECK(!found);
            }
        }
    }

    /* --- 离线并发不同字段：A 改 port、B 改 user → 各自收敛、无冲突 --- */
    {
        hb_obj *oa = NULL, *ob = NULL;
        CHECK_EQ_INT(hb_vault_get(va, hid, &oa), HB_OK);
        hb_obj_set_int(oa, "port", 2200);
        CHECK_EQ_INT(hb_vault_put(va, oa), HB_OK);
        hb_obj_free(oa);
        CHECK_EQ_INT(hb_vault_get(vb, hid, &ob), HB_OK);
        hb_obj_set_str(ob, "user", "admin");
        CHECK_EQ_INT(hb_vault_put(vb, ob), HB_OK);
        hb_obj_free(ob);
        converge(sa, sb, &fs);
        CHECK_EQ_INT(fint(va, hid, "port", 0), 2200);
        CHECK_EQ_STR(fstr(va, hid, "user"), "admin");
        CHECK_EQ_INT(fint(vb, hid, "port", 0), 2200);
        CHECK_EQ_STR(fstr(vb, hid, "user"), "admin");
        CHECK_EQ_INT(hb_sync_conflict_count(sa), 0);
        CHECK_EQ_INT(hb_sync_conflict_count(sb), 0);
    }

    /* --- 同字段并发 → 恰好一条冲突记录；resolve take-other → 收敛 --- */
    {
        hb_obj *oa = NULL, *ob = NULL;
        /* 让 B 的版本 updated_at 更大（后写）以确定 winner，或依赖 device_id 平手。
         * 这里只验证恰好一条冲突 + take-other 收敛。 */
        CHECK_EQ_INT(hb_vault_get(va, hid, &oa), HB_OK);
        hb_obj_set_str(oa, "name", "name-from-A");
        CHECK_EQ_INT(hb_vault_put(va, oa), HB_OK);
        hb_obj_free(oa);
        usleep(2000);
        CHECK_EQ_INT(hb_vault_get(vb, hid, &ob), HB_OK);
        hb_obj_set_str(ob, "name", "name-from-B");
        CHECK_EQ_INT(hb_vault_put(vb, ob), HB_OK);
        hb_obj_free(ob);
        converge(sa, sb, &fs);
        /* 冲突只出现在执行合并的设备上，且恰好一条 */
        {
            size_t ca = hb_sync_conflict_count(sa);
            size_t cb = hb_sync_conflict_count(sb);
            CHECK(ca + cb == 1);
        }
        {
            hb_sync *own = hb_sync_conflict_count(sa) ? sa : sb;
            CHECK_EQ_INT(hb_sync_conflict_take_other(own, hid), HB_OK);
            converge(sa, sb, &fs);
            CHECK_EQ_INT(hb_sync_conflict_count(sa), 0);
            CHECK_EQ_INT(hb_sync_conflict_count(sb), 0);
            /* take-other 后两端收敛到同一 name */
            CHECK_EQ_STR(fstr(va, hid, "name"), fstr(vb, hid, "name"));
            CHECK(fstr(va, hid, "name")[0] != 0);
        }
    }

    /* --- CAS：B 用过期 base_seq 推 → 冲突合并 → 重推成功 --- */
    {
        hb_obj *oa = NULL, *ob = NULL;
        hb_sync_push *p = NULL;
        char *resp;
        bool retry = false;
        /* A 先推一个变更（B 不知道），B 再推同 id → base_seq 过期 */
        CHECK_EQ_INT(hb_vault_get(va, hid, &oa), HB_OK);
        hb_obj_set_str(oa, "desc", "A-stale-maker");
        CHECK_EQ_INT(hb_vault_put(va, oa), HB_OK);
        hb_obj_free(oa);
        do_push(sa, &fs);
        /* B 本地编辑同对象 → base_seq 过期 */
        CHECK_EQ_INT(hb_vault_get(vb, hid, &ob), HB_OK);
        hb_obj_set_str(ob, "desc", "B-conflicting-push");
        CHECK_EQ_INT(hb_vault_put(vb, ob), HB_OK);
        hb_obj_free(ob);
        /* B 不先拉：本地 sync_seq 已过期 → 服务端 CAS 冲突 */
        CHECK_EQ_INT(hb_sync_build_push(sb, &p), HB_OK);
        CHECK(p != NULL);
        resp = fs_post(&fs, hb_sync_push_body(p));
        CHECK(strstr(resp, "\"ok\":false") != NULL || strstr(resp, "\"ok\":0") != NULL);
        CHECK_EQ_INT(hb_sync_apply_push(sb, p, resp, &retry), HB_OK);
        CHECK(retry);
        free(resp);
        hb_sync_push_free(p);
        /* 重推成功 */
        converge(sa, sb, &fs);
        CHECK(fstr(va, hid, "desc")[0] != 0);
    }

    /* --- scope：B 禁 keys → 不收不发；重开 → 游标重置全量到 --- */
    {
        const char *kid2 = "key-e2e-2";
        hb_obj *k2;
        hb_sync_push *p = NULL;

        CHECK_EQ_INT(hb_sync_scope_set(sb, "keys", false), HB_OK);
        k2 = hb_obj_new_with_id("key", kid2);
        hb_obj_set_str(k2, "name", "scoped-key");
        CHECK_EQ_INT(hb_vault_put(va, k2), HB_OK);
        hb_obj_free(k2);
        do_push(sa, &fs);
        do_pull(sb, &fs);
        /* B 不存 key 对象 */
        CHECK(!fexists(vb, kid2));
        /* B 不推 key 对象（即便本地造一个 dirty key） */
        {
            hb_obj *bk = hb_obj_new_with_id("key", "key-local-b");
            hb_obj_set_str(bk, "name", "b-local-key");
            CHECK_EQ_INT(hb_vault_put(vb, bk), HB_OK);
            hb_obj_free(bk);
            CHECK_EQ_INT(hb_sync_build_push(sb, &p), HB_OK);
            if (p) {
                CHECK(strstr(hb_sync_push_body(p), "\"type\":\"key\"") == NULL);
                hb_sync_push_free(p);
            }
        }
        /* 重开 → 游标重置 → 全量到达 */
        CHECK_EQ_INT(hb_sync_scope_set(sb, "keys", true), HB_OK);
        converge(sa, sb, &fs);
        CHECK_EQ_STR(fstr(vb, kid2, "name"), "scoped-key");
        CHECK_EQ_STR(fstr(vb, kid, "name"), "deploy-key");
    }

    /* --- settings 单例：font_size 设备本地、theme 同步 --- */
    {
        hb_obj *sa_obj = hb_obj_new_with_id("settings", "settings");
        hb_obj *sb_obj;
        hb_obj_set_int(sa_obj, "font_size", 13);
        hb_obj_set_int(sa_obj, "keep_connected", 1);
        hb_obj_set_int(sa_obj, "option_meta", 1);
        hb_obj_set_str(sa_obj, "theme", "dark");
        CHECK_EQ_INT(hb_vault_put(va, sa_obj), HB_OK);
        sb_obj = hb_obj_new_with_id("settings", "settings");
        hb_obj_set_int(sb_obj, "font_size", 15);
        hb_obj_set_int(sb_obj, "keep_connected", 0);
        hb_obj_set_int(sb_obj, "option_meta", 0);
        hb_obj_set_str(sb_obj, "theme", "dark");
        CHECK_EQ_INT(hb_vault_put(vb, sb_obj), HB_OK);
        converge(sa, sb, &fs);
        /* font_size 各保本地值（不收敛） */
        CHECK_EQ_INT(fint(va, "settings", "font_size", 0), 13);
        CHECK_EQ_INT(fint(vb, "settings", "font_size", 0), 15);
        /* keep_connected（macOS 休眠断言）同为设备本地 */
        CHECK_EQ_INT(fint(va, "settings", "keep_connected", -1), 1);
        CHECK_EQ_INT(fint(vb, "settings", "keep_connected", -1), 0);
        /* option_meta（macOS Option-as-Meta，键盘相关）设备本地 */
        CHECK_EQ_INT(fint(va, "settings", "option_meta", -1), 1);
        CHECK_EQ_INT(fint(vb, "settings", "option_meta", -1), 0);
        /* theme 同步 */
        CHECK_EQ_STR(fstr(va, "settings", "theme"), fstr(vb, "settings", "theme"));
        CHECK(fstr(va, "settings", "theme")[0] != 0);
        hb_obj_free(sa_obj);
        hb_obj_free(sb_obj);
    }

    /* --- remote wipe：范围内对象全删 + 冲突 + 游标清零 --- */
    {
        hb__vault_row *rows = NULL;
        size_t n = 999, i, in_scope = 0;
        CHECK_EQ_INT(hb_sync_wipe(sb), HB_OK);
        CHECK_EQ_INT(hb__vault_rows(vb, &rows, &n), HB_OK);
        for (i = 0; i < n; i++) {
            const char *t = rows[i].type;
            if (!strcmp(t, "host") || !strcmp(t, "key") || !strcmp(t, "snippet") ||
                !strcmp(t, "settings") || !strcmp(t, "group") ||
                !strcmp(t, "forward") || !strcmp(t, "known"))
                in_scope++;
        }
        CHECK_EQ_INT(in_scope, 0);
        hb__vault_rows_free(rows, n);
        CHECK_EQ_INT(hb_sync_conflict_count(sb), 0);
    }

    /* --- logout keep-local：对象保留、账号元数据清空、全部 dirty --- */
    {
        char *aid = NULL, *em = NULL, *srv = NULL, *did = NULL;
        hb__vault_row *rows = NULL;
        size_t n = 0, i, dirty_n = 0;
        CHECK_EQ_INT(hb_sync_logout_local(sa), HB_OK);
        CHECK_EQ_INT(hb_sync_get_account(sa, &aid, &em, &srv, &did), HB_ERR_NOT_FOUND);
        CHECK_EQ_INT(hb__vault_rows(va, &rows, &n), HB_OK);
        CHECK(n > 0); /* 本地数据保留 */
        for (i = 0; i < n; i++)
            if (rows[i].dirty && rows[i].sync_seq == 0)
                dirty_n++;
        CHECK_EQ_INT(dirty_n, n);
        hb__vault_rows_free(rows, n);
        free(aid); free(em); free(srv); free(did);
    }

    e2e_teardown();
}

/* ============================ 账号流程 ============================ */

static void t_account_flow(void)
{
    char *body = NULL, *words = NULL;
    uint8_t vk[32], vk2[32], wrap[32];
    uint8_t salt[16];
    uint64_t ops, mem;
    char *aid = NULL, *did = NULL, *tok = NULL;
    bool totp;

    /* register body 字段齐全 */
    CHECK_EQ_INT(hb_sync_register_body("u@x.com", "pw", "dev", "linux",
                                       2, 64 * 1024 * 1024,
                                       &body, &words, vk), HB_OK);
    CHECK(body && words);
    CHECK(body && strstr(body, "\"email\":\"u@x.com\""));
    CHECK(body && strstr(body, "\"auth_key\":"));
    CHECK(body && strstr(body, "\"wrapped_vk_pw\":"));
    CHECK(body && strstr(body, "\"rc_auth\":"));
    CHECK(body && strstr(body, "\"wrapped_vk_rc\":"));
    CHECK(body && strstr(body, "\"device\":{\"name\":\"dev\",\"platform\":\"linux\"}"));
    /* words 可解码 */
    {
        uint8_t e[32];
        CHECK(hb_bip39_decode(words, e) == 0);
        sodium_memzero(e, 32);
    }
    free(body); free(words);

    /* prelogin → login → login_parse 解出同一 VK */
    {
        char *pb = NULL, *lb = NULL, *pre = NULL;
        cJSON *pj, *pk;
        char *wr_b64;
        CHECK_EQ_INT(hb_sync_prelogin_body("u@x.com", &pb), HB_OK);
        CHECK_EQ_STR(pb, "{\"email\":\"u@x.com\"}");
        free(pb);
        randombytes_buf(salt, 16);
        ops = 2; mem = 64 * 1024 * 1024;
        /* 造 prelogin 响应 */
        pj = cJSON_CreateObject();
        pk = cJSON_CreateObject();
        wr_b64 = b64url(salt, 16);
        cJSON_AddStringToObject(pk, "salt", wr_b64);
        cJSON_AddNumberToObject(pk, "ops", (double)ops);
        cJSON_AddNumberToObject(pk, "mem", (double)mem);
        cJSON_AddItemToObject(pj, "kdf", pk);
        pre = cJSON_PrintUnformatted(pj);
        cJSON_Delete(pj);
        free(wr_b64);
        {
            uint8_t s2[16];
            uint64_t o2, m2;
            CHECK_EQ_INT(hb_sync_prelogin_parse(pre, s2, &o2, &m2), HB_OK);
            CHECK(memcmp(s2, salt, 16) == 0);
            CHECK_EQ_INT(o2, ops);
            CHECK_EQ_INT(m2, mem);
        }
        free(pre);
        CHECK_EQ_INT(hb_sync_login_body("u@x.com", "pw", salt, ops, mem,
                                        "123456", "dev", "linux",
                                        &lb, wrap), HB_OK);
        CHECK(strstr(lb, "\"totp\":\"123456\"") != NULL);
        free(lb);
        /* 造 login 响应：wrapped_vk_pw = seal(wrap, vk) */
        {
            uint8_t *sealed;
            size_t sn;
            char *sb64, *resp;
            sealed = hb_sync_seal(wrap, vk, 32,
                                  (const uint8_t *)"hb:vk:pw:v1", 11, &sn);
            CHECK(sealed != NULL);
            sb64 = b64url(sealed, sn);
            sodium_free(sealed);
            resp = malloc(strlen(sb64) + 256);
            sprintf(resp, "{\"account_id\":\"a1\",\"device_id\":\"d1\","
                          "\"token\":\"t1\",\"wrapped_vk_pw\":\"%s\","
                          "\"totp_enabled\":true}", sb64);
            free(sb64);
            memset(vk2, 0, 32);
            CHECK_EQ_INT(hb_sync_login_parse(resp, wrap, vk2,
                                             &aid, &did, &tok, &totp), HB_OK);
            CHECK(memcmp(vk, vk2, 32) == 0);
            CHECK_EQ_STR(aid, "a1");
            CHECK_EQ_STR(did, "d1");
            CHECK_EQ_STR(tok, "t1");
            CHECK(totp);
            free(resp); free(aid); free(did); free(tok);
        }
        sodium_memzero(wrap, 32);
    }

    /* recover/start + parse */
    {
        uint8_t E[32], vk3[32];
        char words[256], *rb = NULL, *rtok = NULL;
        char *rr;
        randombytes_buf(E, 32);
        CHECK(hb_bip39_encode(E, words, sizeof words) == 0);
        CHECK_EQ_INT(hb_sync_recover_start_body("u@x.com", words, &rb), HB_OK);
        CHECK(strstr(rb, "\"rc_auth\":") != NULL);
        free(rb);
        /* recover/start 响应 */
        {
            uint8_t rcw[32], rca[32], *sealed;
            size_t sn;
            char *sb64;
            hb_sync_derive_rc(E, rcw, rca);
            sealed = hb_sync_seal(rcw, vk, 32,
                                  (const uint8_t *)"hb:vk:rc:v1", 11, &sn);
            CHECK(sealed != NULL);
            sb64 = b64url(sealed, sn);
            sodium_free(sealed);
            rr = malloc(strlen(sb64) + 128);
            sprintf(rr, "{\"recovery_token\":\"rt1\",\"wrapped_vk_rc\":\"%s\"}", sb64);
            free(sb64);
            CHECK_EQ_INT(hb_sync_recover_start_parse(rr, words, vk3, &rtok), HB_OK);
            CHECK(memcmp(vk, vk3, 32) == 0);
            CHECK_EQ_STR(rtok, "rt1");
            free(rr); free(rtok);
            sodium_memzero(rcw, 32); sodium_memzero(rca, 32);
        }
        /* recover/finish body 字段 */
        {
            char *fb = NULL;
            CHECK_EQ_INT(hb_sync_recover_finish_body("rt1", "newpw", 2,
                            64 * 1024 * 1024, "dev", "linux", vk, &fb), HB_OK);
            CHECK(strstr(fb, "\"recovery_token\":\"rt1\"") != NULL);
            CHECK(strstr(fb, "\"auth_key\":") != NULL);
            CHECK(strstr(fb, "\"wrapped_vk_pw\":") != NULL);
            free(fb);
        }
        /* 错恢复码 → reject */
        {
            uint8_t e2[32];
            CHECK(hb_bip39_decode("abandon abandon abandon abandon abandon abandon "
                "abandon abandon abandon abandon abandon abandon abandon abandon "
                "abandon abandon abandon abandon abandon abandon abandon abandon "
                "abandon abandon", e2) != 0);
        }
        sodium_memzero(E, 32);
        sodium_memzero(vk3, 32);
    }

    /* change-password body 字段 */
    {
        char *cb = NULL;
        uint8_t os[16] = {0};
        CHECK_EQ_INT(hb_sync_change_password_body("old", os, 2, 64 * 1024 * 1024,
                        "new", 0, 0, vk, &cb), HB_OK);
        CHECK(strstr(cb, "\"auth_key_old\":") != NULL);
        CHECK(strstr(cb, "\"auth_key\":") != NULL);
        CHECK(strstr(cb, "\"kdf\":") != NULL);
        CHECK(strstr(cb, "\"wrapped_vk_pw\":") != NULL);
        free(cb);
    }
    sodium_memzero(vk, 32);
    sodium_memzero(vk2, 32);
}

static void t_recovery_rotate(void)
{
    hb_vault *v = NULL;
    hb_sync *s;
    char *body = NULL, *words = NULL, *nbody = NULL, *nwords = NULL;
    uint8_t vk[32], salt[16];

    CHECK_EQ_INT(hb_vault_open(vault_path(), DEK_A, &v), HB_OK);
    s = hb_sync_new(v);
    CHECK_EQ_INT(hb_sync_register_body("rot@x.y", "pw-rotate", "d", "l",
                                       2, 64 * 1024 * 1024,
                                       &body, &words, vk), HB_OK);
    /* 未 set_vk → INVALID */
    memset(salt, 0x42, 16);
    CHECK_EQ_INT(hb_sync_recovery_rotate_body(s, "pw-rotate", salt, 2,
                                              64 * 1024 * 1024,
                                              &nbody, &nwords),
                 HB_ERR_INVALID);
    CHECK_EQ_INT(hb_sync_set_vk(s, vk), HB_OK);
    CHECK_EQ_INT(hb_sync_recovery_rotate_body(s, "pw-rotate", salt, 2,
                                              64 * 1024 * 1024,
                                              &nbody, &nwords),
                 HB_OK);
    CHECK(strstr(nbody, "\"auth_key\":") != NULL);
    CHECK(strstr(nbody, "\"rc_auth\":") != NULL);
    CHECK(strstr(nbody, "\"wrapped_vk_rc\":") != NULL);
    CHECK(strstr(nbody, "\"email\"") == NULL);
    /* 新词的 rc_auth 与 body 一致；旧词不同 → 旧码立即失效 */
    {
        cJSON *j = cJSON_Parse(nbody);
        cJSON *ra = cJSON_GetObjectItem(j, "rc_auth");
        cJSON *wv = cJSON_GetObjectItem(j, "wrapped_vk_rc");
        uint8_t E[32], rcw[32], rca[32], rca_old[32], vkr[32];
        char *resp, *rtok = NULL;
        CHECK(ra && cJSON_IsString(ra));
        CHECK(wv && cJSON_IsString(wv));
        CHECK_EQ_INT(hb_bip39_decode(nwords, E), 0);
        hb_sync_derive_rc(E, rcw, rca);
        CHECK_EQ_INT(hb_bip39_decode(words, E), 0);
        hb_sync_derive_rc(E, rcw, rca_old);
        {
            /* body 的 rc_auth base64url → bytes */
            const char *b = ra->valuestring;
            size_t n = strlen(b);
            uint8_t dec[64];
            static const char T[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            size_t oi = 0; uint32_t acc = 0; int bits = 0, i;
            for (i = 0; b[i]; i++) {
                const char *p = strchr(T, b[i]);
                CHECK(p != NULL);
                acc = (acc << 6) | (uint32_t)(p - T);
                bits += 6;
                if (bits >= 8) { bits -= 8; dec[oi++] = (uint8_t)(acc >> bits); }
            }
            CHECK_EQ_INT(oi, 32);
            CHECK(memcmp(dec, rca, 32) == 0);      /* 新词 == body */
            CHECK(memcmp(dec, rca_old, 32) != 0);  /* 旧词 != body */
            (void)n;
        }
        /* 新词 + body 的 wrapped_vk_rc → 同一 VK */
        resp = malloc(strlen(wv->valuestring) + 64);
        sprintf(resp, "{\"recovery_token\":\"t\",\"wrapped_vk_rc\":\"%s\"}",
                wv->valuestring);
        CHECK_EQ_INT(hb_sync_recover_start_parse(resp, nwords, vkr, &rtok),
                     HB_OK);
        CHECK(memcmp(vkr, vk, 32) == 0);
        free(resp); free(rtok);
        /* 旧词打不开了 */
        vkr[0] = 0;
        rtok = NULL;
        resp = malloc(strlen(wv->valuestring) + 64);
        sprintf(resp, "{\"recovery_token\":\"t\",\"wrapped_vk_rc\":\"%s\"}",
                wv->valuestring);
        CHECK(hb_sync_recover_start_parse(resp, words, vkr, &rtok) != HB_OK);
        free(resp); free(rtok);
        sodium_memzero(E, 32); sodium_memzero(rcw, 32);
        sodium_memzero(rca, 32); sodium_memzero(rca_old, 32);
        cJSON_Delete(j);
    }
    hb_sync_free(s);
    hb_vault_close(v);
    free(body); free(words); free(nbody); free(nwords);
    sodium_memzero(vk, 32);
}

int main(void)
{
    hb_core_init();
    t_bip39();
    t_kdf_wrap();
    t_blob();
    t_merge_cases();
    t_e2e();
    t_account_flow();
    t_recovery_rotate();
    printf("sync: %d checks, %d failures\n", hb_t_checks, hb_t_failures);
    return hb_t_failures ? 1 : 0;
}
