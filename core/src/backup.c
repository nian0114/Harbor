/* FR-15 加密备份 — docs/api/backup-format.md (HBBK v1)。
 * header(30B) 作 AEAD AD；Argon2id 参数有上下限；明文 JSON 与
 * secret 只在锁定内存中组装/解析，用完即擦；导入先全量校验。 */

#include "harbor/backup.h"
#include "harbor/sync.h"          /* hb_sync_pwhash */
#include "obj_priv.h"             /* hb_obj 内部（data cJSON） */
#include "vault_priv.h"           /* hb__vault_secret_map */

#include <sodium.h>
#include <string.h>
#include <stdio.h>

#define BK_MAGIC "HBBK"
#define BK_VER   0x01
#define BK_KDF   0x01
#define BK_HDR   30          /* magic+ver+kdf+ops+mem+salt，作 AD */
#define BK_NONCE 24
#define BK_PAD   4096
#define BK_MIN_OPS   2ULL
#define BK_MIN_MEM   (64 * 1024 * 1024ULL)
#define BK_MAX_MEM   (1024 * 1024 * 1024ULL)

static const char *const BK_TYPES[] = {
    "host", "group", "key", "snippet", "forward", "known", "settings",
};

static void be32(uint8_t *w, uint32_t v)
{
    w[0] = (uint8_t)(v >> 24); w[1] = (uint8_t)(v >> 16);
    w[2] = (uint8_t)(v >> 8);  w[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------- 业务字段导出：拷贝 data，去掉 _fv ---------- */

static cJSON *obj_data_public(const hb_obj *o)
{
    cJSON *d = cJSON_Duplicate(o->data, 1);
    if (d)
        cJSON_DeleteItemFromObjectCaseSensitive(d, "_fv");
    return d;
}

/* secret map 明文 (u16 name + u32 value，LE) → cJSON object */
static cJSON *secret_to_json(const uint8_t *pt, size_t n)
{
    cJSON *j = cJSON_CreateObject();
    size_t off = 0;
    if (!j) return NULL;
    while (off + 2 <= n) {
        uint16_t nl = le16(pt + off);
        uint32_t vl;
        char *name;
        off += 2;
        if (off + nl + 4 > n || !nl) { cJSON_Delete(j); return NULL; }
        name = malloc((size_t)nl + 1);
        if (!name) { cJSON_Delete(j); return NULL; }
        memcpy(name, pt + off, nl);
        name[nl] = 0;
        off += nl;
        vl = le32(pt + off);
        off += 4;
        if (off + vl > n) { free(name); cJSON_Delete(j); return NULL; }
        {
            char *val = malloc((size_t)vl + 1);
            if (!val) { free(name); cJSON_Delete(j); return NULL; }
            memcpy(val, pt + off, vl);
            val[vl] = 0;
            cJSON_AddStringToObject(j, name, val);
            free(val);
        }
        free(name);
        off += vl;
    }
    return j;
}

hb_err hb_backup_export(hb_vault *v, const char *password,
                        uint8_t **out, size_t *out_len)
{
    cJSON *root = NULL, *objs = NULL;
    char *json = NULL;
    uint8_t *padded = NULL, *file = NULL;
    size_t plen_sz = 0;
    size_t json_len, file_len;
    uint8_t key[32];
    hb_err e = HB_ERR_NOMEM;
    size_t ti;

    if (!v || !password || !out || !out_len)
        return HB_ERR_INVALID;
    *out = NULL;
    *out_len = 0;

    root = cJSON_CreateObject();
    objs = cJSON_CreateArray();
    if (!root || !objs)
        goto out;
    cJSON_AddStringToObject(root, "format", "harbor-backup");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "created", (double)hb_now_ms());
    cJSON_AddStringToObject(root, "device", hb_vault_device_id(v));
    cJSON_AddItemToObject(root, "objects", objs);

    for (ti = 0; ti < (sizeof(BK_TYPES)/sizeof(BK_TYPES[0])); ti++) {
        hb_obj **list = NULL;
        size_t n = 0, i;
        if (hb_vault_list(v, BK_TYPES[ti], &list, &n) != HB_OK)
            continue;
        if (!n && !strcmp(BK_TYPES[ti], "settings")) {
            /* 未持久化的 settings 单例：导出默认值 */
            hb_obj *so = NULL;
            if (hb_vault_settings(v, &so) == HB_OK && so) {
                list = malloc(sizeof *list);
                if (list) { list[0] = so; n = 1; }
                else hb_obj_free(so);
            }
        }
        for (i = 0; i < n; i++) {
            cJSON *ent = cJSON_CreateObject();
            if (!ent) { hb_obj_list_free(list, n); goto out; }
            cJSON_AddStringToObject(ent, "id", hb_obj_id(list[i]));
            cJSON_AddStringToObject(ent, "type", BK_TYPES[ti]);
            {
                cJSON *d = obj_data_public(list[i]);
                if (d) cJSON_AddItemToObject(ent, "data", d);
            }
            {
                uint8_t *spt = NULL;
                size_t sn = 0;
                if (hb__vault_secret_map(v, hb_obj_id(list[i]),
                                         &spt, &sn) == HB_OK && spt) {
                    cJSON *sj = secret_to_json(spt, sn);
                    sodium_memzero(spt, sn);
                    sodium_free(spt);
                    if (sj && cJSON_GetArraySize(sj) > 0)
                        cJSON_AddItemToObject(ent, "secret", sj);
                    else
                        cJSON_Delete(sj);
                }
            }
            cJSON_AddItemToArray(objs, ent);
        }
        hb_obj_list_free(list, n);
    }

    json = cJSON_PrintUnformatted(root);
    if (!json)
        goto out;
    json_len = strlen(json);

    /* header + Argon2id + XChaCha20-Poly1305，AD = 前 30 字节 */
    file_len = BK_HDR + BK_NONCE + json_len + BK_PAD +
               crypto_aead_xchacha20poly1305_ietf_ABYTES;
    file = malloc(file_len);
    padded = sodium_malloc(json_len + BK_PAD);
    if (!file || !padded)
        goto out;
    memcpy(file, BK_MAGIC, 4);
    file[4] = BK_VER;
    file[5] = BK_KDF;
    be32(file + 6, crypto_pwhash_OPSLIMIT_MODERATE);
    be32(file + 10, crypto_pwhash_MEMLIMIT_MODERATE / 1024);
    randombytes_buf(file + 14, 16);
    randombytes_buf(file + 30, BK_NONCE);

    if (hb_sync_pwhash(password, file + 14,
                     crypto_pwhash_OPSLIMIT_MODERATE,
                     crypto_pwhash_MEMLIMIT_MODERATE, key) != HB_OK) {
        e = HB_ERR_CRYPTO;
        goto out;
    }
    memcpy(padded, json, json_len);
    if (sodium_pad(&plen_sz, padded, json_len, BK_PAD,
                   json_len + BK_PAD) != 0) {
        e = HB_ERR_CRYPTO;
        goto out;
    }
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        file + BK_HDR + BK_NONCE, NULL, padded, plen_sz,
        file, BK_HDR, NULL, file + BK_HDR, key);
    *out = file;
    *out_len = BK_HDR + BK_NONCE + plen_sz +
               crypto_aead_xchacha20poly1305_ietf_ABYTES;
    file = NULL;
    e = HB_OK;

out:
    sodium_memzero(key, sizeof key);
    if (padded) { sodium_memzero(padded, plen_sz); sodium_free(padded); }
    free(file);
    free(json);
    cJSON_Delete(root);
    return e;
}

/* ---------- 解密 + 校验：成功返回锁定内存中的明文 JSON ---------- */

static hb_err open_backup(const uint8_t *data, size_t len,
                          const char *password, uint8_t **pt, size_t *pt_n)
{
    uint8_t key[32];
    uint8_t *dec = NULL;
    unsigned long long dn = 0;
    size_t un = 0;
    uint32_t ops, mem_kib;
    uint64_t mem;
    hb_err e = HB_ERR_INVALID;

    *pt = NULL;
    *pt_n = 0;
    if (!data || !password ||
        len < BK_HDR + BK_NONCE + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
        memcmp(data, BK_MAGIC, 4) || data[4] != BK_VER || data[5] != BK_KDF)
        return HB_ERR_INVALID;

    ops = rd32(data + 6);
    mem_kib = rd32(data + 10);
    mem = (uint64_t)mem_kib * 1024;
    if (ops < BK_MIN_OPS || mem < BK_MIN_MEM || mem > BK_MAX_MEM)
        return HB_ERR_INVALID;
    if (hb_sync_pwhash(password, data + 14, ops, mem, key) != HB_OK)
        return HB_ERR_CRYPTO;

    dec = sodium_malloc(len - BK_HDR - BK_NONCE);
    if (!dec) {
        sodium_memzero(key, sizeof key);
        return HB_ERR_NOMEM;
    }
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            dec, &dn, NULL, data + BK_HDR + BK_NONCE,
            len - BK_HDR - BK_NONCE, data, BK_HDR,
            data + BK_HDR, key) == 0 &&
        sodium_unpad(&un, dec, (size_t)dn, BK_PAD) == 0 && un > 0) {
        *pt = dec;
        *pt_n = un;
        dec = NULL;
        e = HB_OK;
    }
    if (dec) {
        sodium_memzero(dec, len - BK_HDR - BK_NONCE);
        sodium_free(dec);
    }
    sodium_memzero(key, sizeof key);
    return e;
}

/* ---------- 明文 JSON → 校验后的对象列表 ---------- */

typedef struct {
    char   *id;
    char   *type;
    cJSON  *data;     /* 业务字段 */
    cJSON  *secret;   /* 可为 NULL */
} bk_ent;

static void bk_ent_free(bk_ent *e)
{
    free(e->id);
    free(e->type);
    cJSON_Delete(e->data);
    cJSON_Delete(e->secret);
}

static bool type_ok(const char *t)
{
    size_t i;
    for (i = 0; i < (sizeof(BK_TYPES)/sizeof(BK_TYPES[0])); i++)
        if (!strcmp(t, BK_TYPES[i])) return true;
    return false;
}

static hb_err parse_backup(const uint8_t *pt, size_t n,
                           bk_ent **ents, size_t *cnt)
{
    cJSON *root, *arr, *it;
    bk_ent *list = NULL;
    size_t used = 0;
    hb_err e = HB_ERR_INVALID;

    *ents = NULL;
    *cnt = 0;
    {
        char *tmp = malloc(n + 1);
        if (!tmp) return HB_ERR_NOMEM;
        memcpy(tmp, pt, n);
        tmp[n] = 0;
        root = cJSON_Parse(tmp);
        sodium_memzero(tmp, n + 1);
        free(tmp);
    }
    if (!root)
        return HB_ERR_INVALID;
    arr = cJSON_GetObjectItem(root, "objects");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(arr))
        goto out;
    cJSON_ArrayForEach(it, arr) {
        cJSON *id = cJSON_GetObjectItem(it, "id");
        cJSON *ty = cJSON_GetObjectItem(it, "type");
        cJSON *da = cJSON_GetObjectItem(it, "data");
        cJSON *se = cJSON_GetObjectItem(it, "secret");
        bk_ent *ne;
        if (!cJSON_IsString(id) || !cJSON_IsString(ty) ||
            !type_ok(ty->valuestring) ||
            (da && !cJSON_IsObject(da)) || (se && !cJSON_IsObject(se)))
            goto out;
        ne = realloc(list, (used + 1) * sizeof *ne);
        if (!ne) { e = HB_ERR_NOMEM; goto out; }
        list = ne;
        list[used].id = strdup(id->valuestring);
        list[used].type = strdup(ty->valuestring);
        list[used].data = da ? cJSON_Duplicate(da, 1) : cJSON_CreateObject();
        list[used].secret = se ? cJSON_Duplicate(se, 1) : NULL;
        if (!list[used].id || !list[used].type || !list[used].data ||
            (se && !list[used].secret)) {
            bk_ent_free(&list[used]);
            e = HB_ERR_NOMEM;
            goto out;
        }
        used++;
    }
    *ents = list;
    *cnt = used;
    e = HB_OK;
out:
    if (e != HB_OK)
        while (used)
            bk_ent_free(&list[--used]);
    free(e == HB_OK ? NULL : list);
    cJSON_Delete(root);
    return e;
}

hb_err hb_backup_preview(const uint8_t *data, size_t len,
                         const char *password, hb_backup_stat *stat)
{
    uint8_t *pt = NULL;
    size_t n = 0;
    bk_ent *ents = NULL;
    size_t cnt = 0, i;
    hb_err e;

    if (!stat) return HB_ERR_INVALID;
    memset(stat, 0, sizeof *stat);
    e = open_backup(data, len, password, &pt, &n);
    if (e != HB_OK) return e;
    e = parse_backup(pt, n, &ents, &cnt);
    sodium_memzero(pt, n);
    sodium_free(pt);
    if (e != HB_OK) return e;
    stat->total = cnt;
    for (i = 0; i < cnt; i++)
        bk_ent_free(&ents[i]);
    free(ents);
    return HB_OK;
}

/* cJSON 字段 → hb_obj setter（数字保整型语义） */
static void apply_field(hb_obj *o, const char *name, const cJSON *j)
{
    if (cJSON_IsString(j))
        hb_obj_set_str(o, name, j->valuestring);
    else if (cJSON_IsBool(j))
        hb_obj_set_bool(o, name, cJSON_IsTrue(j));
    else if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        int64_t i = (int64_t)d;
        if ((double)i == d)
            hb_obj_set_int(o, name, i);
        else
            hb_obj_set_double(o, name, d);
    } else if (cJSON_IsArray(j)) {
        const cJSON *it;
        const char **sv;
        size_t n = cJSON_GetArraySize(j), k = 0;
        bool all_str = true;
        sv = malloc((n ? n : 1) * sizeof *sv);
        if (!sv) return;
        cJSON_ArrayForEach(it, j) {
            if (!cJSON_IsString(it)) { all_str = false; break; }
            sv[k++] = it->valuestring;
        }
        if (all_str)
            hb_obj_set_strv(o, name, sv, k);
        free(sv);
    }
    /* 其他类型（object/null）跳过 */
}

hb_err hb_backup_import(hb_vault *v, const uint8_t *data, size_t len,
                        const char *password, hb_backup_stat *stat)
{
    uint8_t *pt = NULL;
    size_t n = 0, cnt = 0, i;
    bk_ent *ents = NULL;
    hb_err e;

    if (!v || !stat) return HB_ERR_INVALID;
    memset(stat, 0, sizeof *stat);
    e = open_backup(data, len, password, &pt, &n);
    if (e != HB_OK) return e;
    e = parse_backup(pt, n, &ents, &cnt);   /* 先全量校验 */
    sodium_memzero(pt, n);
    sodium_free(pt);
    if (e != HB_OK) return e;

    stat->total = cnt;
    for (i = 0; i < cnt; i++) {
        bk_ent *be = &ents[i];
        hb_obj *old = NULL, *o;
        bool exists = false;

        if (hb_vault_get(v, be->id, &old) == HB_OK && old) {
            exists = true;
            if (hb_obj_tombstone(old))
                hb_vault_undelete(v, be->id);
            hb_obj_free(old);
        }
        o = hb_obj_new_with_id(be->type, be->id);
        if (!o) { e = HB_ERR_NOMEM; break; }
        {
            const cJSON *f;
            cJSON_ArrayForEach(f, be->data)
                apply_field(o, f->string, f);
        }
        if ((e = hb_vault_put(v, o)) != HB_OK) {
            hb_obj_free(o);
            break;
        }
        hb_obj_free(o);
        if (be->secret) {
            const cJSON *f;
            cJSON_ArrayForEach(f, be->secret) {
                if (cJSON_IsString(f) && f->string)
                    hb_vault_set_secret(v, be->id, f->string,
                                        f->valuestring);
            }
        }
        if (exists) stat->overwritten++;
        else stat->created++;
    }
    for (i = 0; i < cnt; i++)
        bk_ent_free(&ents[i]);
    free(ents);
    return e;
}
