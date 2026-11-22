#include "harbor/vault.h"

#include "obj_priv.h"
#include "vault_priv.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>
#include <sodium.h>

#define SECRET_NONCE_LEN 24 /* xchacha20poly1305_ietf_NPUBBYTES */
#define SECRET_TAG_LEN   16 /* xchacha20poly1305_ietf_ABYTES */

struct hb_vault {
    sqlite3 *db;
    char     device_id[37];
    uint64_t counter;
    uint8_t *dek; /* sodium_malloc'd, 32 bytes */
    hb_vault_change_cb cb;
    void   *cb_ud;
};

/* ------------------------------- schema ------------------------------- */

static const char *const MIGRATIONS[] = {
    /* v1 */
    "CREATE TABLE objects("
    "  id TEXT PRIMARY KEY,"
    "  type TEXT NOT NULL,"
    "  data TEXT NOT NULL,"
    "  secret BLOB,"
    "  vv TEXT NOT NULL DEFAULT '{}',"
    "  updated_at INTEGER NOT NULL,"
    "  tombstone INTEGER NOT NULL DEFAULT 0,"
    "  dirty INTEGER NOT NULL DEFAULT 1"
    ");"
    "CREATE INDEX idx_objects_type_tomb ON objects(type, tombstone);"
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE logs("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER,"
    "  user TEXT,"
    "  host TEXT,"
    "  host_id TEXT,"
    "  proto TEXT,"
    "  result TEXT,"
    "  reason TEXT,"
    "  duration_s INTEGER,"
    "  saved INTEGER DEFAULT 0"
    ");"
    "CREATE TABLE history("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER,"
    "  host_id TEXT,"
    "  cmd TEXT"
    ");",
    /* v2：同步状态。sync_seq = 本地状态所基于的服务端 seq（0 = 服务端没有）。
     * sync_conflicts：一次合并冲突一行（loser 的 data 为 JSON、secret 用 DEK
     * 加密，编码同 objects.secret）。 */
    "ALTER TABLE objects ADD COLUMN sync_seq INTEGER NOT NULL DEFAULT 0;"
    "CREATE TABLE sync_conflicts("
    "  id TEXT PRIMARY KEY,"
    "  type TEXT NOT NULL,"
    "  fields TEXT NOT NULL,"
    "  data TEXT NOT NULL,"
    "  secret BLOB,"
    "  created INTEGER NOT NULL"
    ");",
};

/* ------------------------------- helpers ------------------------------- */

static char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static hb_err sql_exec(hb_vault *v, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(v->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return HB_ERR_DB;
    }
    return HB_OK;
}

static hb_err txn_begin(hb_vault *v)  { return sql_exec(v, "BEGIN IMMEDIATE"); }
static hb_err txn_commit(hb_vault *v) { return sql_exec(v, "COMMIT"); }
static void    txn_rollback(hb_vault *v) { sql_exec(v, "ROLLBACK"); }

static hb_err mkdir_p(const char *path, mode_t mode)
{
    char *tmp = xstrdup(path);
    char *p;
    if (!tmp)
        return HB_ERR_NOMEM;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                free(tmp);
                return HB_ERR_IO;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        free(tmp);
        return HB_ERR_IO;
    }
    free(tmp);
    return HB_OK;
}

static char *dir_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return xstrdup(".");
    if (slash == path)
        return xstrdup("/");
    return strndup(path, (size_t)(slash - path));
}

static void notify(hb_vault *v, const char *type, const char *id)
{
    if (v->cb)
        v->cb(v, type, id, v->cb_ud);
}

/* meta 表读写 */
static hb_err meta_get(hb_vault *v, const char *key, char **out)
{
    sqlite3_stmt *st;
    hb_err e = HB_ERR_DB;
    *out = NULL;
    if (sqlite3_prepare_v2(v->db, "SELECT value FROM meta WHERE key=?1", -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out = xstrdup((const char *)sqlite3_column_text(st, 0));
        e = *out ? HB_OK : HB_ERR_NOMEM;
    } else {
        e = HB_ERR_NOT_FOUND;
    }
    sqlite3_finalize(st);
    return e;
}

static hb_err meta_set(hb_vault *v, const char *key, const char *value)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO meta(key,value) VALUES(?1,?2) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    return e;
}

/* 设备写计数器 +1 并持久化；返回新值。调用方须在事务内。 */
static hb_err counter_bump(hb_vault *v, uint64_t *out)
{
    char buf[24];
    v->counter++;
    snprintf(buf, sizeof buf, "%llu", (unsigned long long)v->counter);
    if (meta_set(v, "device_counter", buf) != HB_OK) {
        v->counter--;
        return HB_ERR_DB;
    }
    *out = v->counter;
    return HB_OK;
}

/* ------------------------------- secrets ------------------------------- */
/* secret blob = nonce(24) || XChaCha20-Poly1305(明文, AD = id||"\0"||type, dek)
 * 明文为二进制 map：[u16le nlen][name][u32le vlen][value]*                */

static hb_err secret_encrypt(hb_vault *v, const char *id, const char *type,
                             const uint8_t *pt, size_t pt_len,
                             uint8_t **out, size_t *out_len)
{
    uint8_t *blob;
    size_t ad_len, n;
    uint8_t *ad;
    unsigned char nonce[SECRET_NONCE_LEN];

    ad_len = strlen(id) + 1 + strlen(type);
    ad = malloc(ad_len);
    blob = malloc(SECRET_NONCE_LEN + pt_len + SECRET_TAG_LEN);
    if (!ad || !blob) {
        free(ad);
        free(blob);
        return HB_ERR_NOMEM;
    }
    memcpy(ad, id, strlen(id) + 1);
    memcpy(ad + strlen(id) + 1, type, strlen(type));

    randombytes_buf(nonce, sizeof nonce);
    memcpy(blob, nonce, SECRET_NONCE_LEN);
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        blob + SECRET_NONCE_LEN, NULL,
        pt, pt_len,
        ad, ad_len,
        NULL, nonce, v->dek);
    free(ad);
    n = SECRET_NONCE_LEN + pt_len + SECRET_TAG_LEN;
    *out = blob;
    *out_len = n;
    return HB_OK;
}

hb_err hb__vault_secret_decrypt(hb_vault *v, const char *id, const char *type,
                                const uint8_t *blob, size_t blob_len,
                                uint8_t **out, size_t *out_len)
{
    uint8_t *pt, *ad;
    size_t ad_len;
    unsigned long long pt_len = 0;

    if (blob_len < SECRET_NONCE_LEN + SECRET_TAG_LEN)
        return HB_ERR_CRYPTO;
    ad_len = strlen(id) + 1 + strlen(type);
    ad = malloc(ad_len);
    pt = sodium_malloc(blob_len - SECRET_NONCE_LEN - SECRET_TAG_LEN);
    if (!ad || !pt) {
        free(ad);
        sodium_free(pt);
        return HB_ERR_NOMEM;
    }
    memcpy(ad, id, strlen(id) + 1);
    memcpy(ad + strlen(id) + 1, type, strlen(type));

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            pt, &pt_len, NULL,
            blob + SECRET_NONCE_LEN, blob_len - SECRET_NONCE_LEN,
            ad, ad_len,
            blob, v->dek) != 0) {
        sodium_free(pt);
        free(ad);
        return HB_ERR_CRYPTO;
    }
    free(ad);
    *out = pt;
    *out_len = (size_t)pt_len;
    return HB_OK;
}

/* map 迭代：在明文缓冲内就地解析，不做额外堆拷贝。
 * 返回条目数；格式损坏返回 -1。ents 为调用方提供的数组。 */
typedef struct {
    const uint8_t *name;  size_t nlen;
    const uint8_t *value; size_t vlen;
} sec_ent;

static int sec_parse(const uint8_t *buf, size_t len, sec_ent *ents, size_t max)
{
    size_t pos = 0;
    size_t n = 0;
    while (pos < len) {
        uint32_t nlen, vlen;
        if (len - pos < 2)
            return -1;
        nlen = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8);
        pos += 2;
        if (len - pos < nlen + 4)
            return -1;
        if (n < max) {
            ents[n].name = buf + pos;
            ents[n].nlen = nlen;
        }
        pos += nlen;
        vlen = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
               ((uint32_t)buf[pos + 2] << 16) | ((uint32_t)buf[pos + 3] << 24);
        pos += 4;
        if (len - pos < vlen)
            return -1;
        if (n < max) {
            ents[n].value = buf + pos;
            ents[n].vlen = vlen;
        }
        pos += vlen;
        n++;
    }
    return (int)n;
}

static void sec_put_u16(uint8_t **w, uint32_t v)
{
    (*w)[0] = (uint8_t)(v & 0xff);
    (*w)[1] = (uint8_t)((v >> 8) & 0xff);
    *w += 2;
}

static void sec_put_u32(uint8_t **w, uint32_t v)
{
    (*w)[0] = (uint8_t)(v & 0xff);
    (*w)[1] = (uint8_t)((v >> 8) & 0xff);
    (*w)[2] = (uint8_t)((v >> 16) & 0xff);
    (*w)[3] = (uint8_t)((v >> 24) & 0xff);
    *w += 4;
}

/* ------------------------------- open/close ------------------------------- */

static hb_err vault_migrate(hb_vault *v)
{
    int ver = 0;
    size_t nmig = sizeof MIGRATIONS / sizeof MIGRATIONS[0];
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(v->db, "PRAGMA user_version", -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    if (sqlite3_step(st) == SQLITE_ROW)
        ver = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    while ((size_t)ver < nmig) {
        char set_ver[64];
        if (txn_begin(v) != HB_OK)
            return HB_ERR_DB;
        if (sql_exec(v, MIGRATIONS[ver]) != HB_OK) {
            txn_rollback(v);
            return HB_ERR_DB;
        }
        snprintf(set_ver, sizeof set_ver, "PRAGMA user_version=%d", ver + 1);
        if (sql_exec(v, set_ver) != HB_OK) {
            txn_rollback(v);
            return HB_ERR_DB;
        }
        if (txn_commit(v) != HB_OK)
            return HB_ERR_DB;
        ver++;
    }
    return HB_OK;
}

hb_err hb_vault_open(const char *path, const uint8_t dek[32], hb_vault **out)
{
    hb_vault *v;
    char *dir, *dev = NULL, *ctr = NULL;
    hb_err e;

    *out = NULL;
    if (!path || !dek)
        return HB_ERR_INVALID;
    if (hb_core_init() != 0)
        return HB_ERR_CRYPTO;

    dir = dir_name(path);
    if (!dir)
        return HB_ERR_NOMEM;
    e = mkdir_p(dir, 0700);
    free(dir);
    if (e != HB_OK)
        return e;

    v = calloc(1, sizeof *v);
    if (!v)
        return HB_ERR_NOMEM;
    v->dek = sodium_malloc(32);
    if (!v->dek) {
        free(v);
        return HB_ERR_NOMEM;
    }
    memcpy(v->dek, dek, 32);

    if (sqlite3_open_v2(path, &v->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        e = HB_ERR_DB;
        goto fail;
    }
    chmod(path, 0600);

    if (sql_exec(v, "PRAGMA journal_mode=WAL") != HB_OK ||
        sql_exec(v, "PRAGMA foreign_keys=ON") != HB_OK ||
        sql_exec(v, "PRAGMA busy_timeout=5000") != HB_OK) {
        e = HB_ERR_DB;
        goto fail;
    }

    e = vault_migrate(v);
    if (e != HB_OK)
        goto fail;

    e = meta_get(v, "device_id", &dev);
    if (e == HB_ERR_NOT_FOUND) {
        char id[37];
        hb_uuid(id);
        if (meta_set(v, "device_id", id) != HB_OK) {
            e = HB_ERR_DB;
            goto fail;
        }
        dev = xstrdup(id);
        if (!dev) {
            e = HB_ERR_NOMEM;
            goto fail;
        }
    } else if (e != HB_OK) {
        goto fail;
    }
    snprintf(v->device_id, sizeof v->device_id, "%s", dev);
    free(dev);

    v->counter = 0;
    if (meta_get(v, "device_counter", &ctr) == HB_OK) {
        v->counter = strtoull(ctr, NULL, 10);
        free(ctr);
    } else {
        meta_set(v, "device_counter", "0");
    }

    *out = v;
    return HB_OK;

fail:
    if (v->db)
        sqlite3_close(v->db);
    sodium_free(v->dek);
    free(v);
    return e;
}

void hb_vault_close(hb_vault *v)
{
    if (!v)
        return;
    if (v->db)
        sqlite3_close(v->db);
    sodium_free(v->dek);
    free(v);
}

const char *hb_vault_device_id(const hb_vault *v)
{
    return v ? v->device_id : NULL;
}

void hb_vault_set_change_cb(hb_vault *v, hb_vault_change_cb cb, void *ud)
{
    v->cb = cb;
    v->cb_ud = ud;
}

/* ------------------------------- obj <-> row ------------------------------- */

static hb_obj *row_to_obj(sqlite3_stmt *st)
{
    hb_obj *o = hb_obj_new_with_id(
        (const char *)sqlite3_column_text(st, 1),
        (const char *)sqlite3_column_text(st, 0));
    const char *data = (const char *)sqlite3_column_text(st, 2);
    const char *vv = (const char *)sqlite3_column_text(st, 3);
    cJSON *j;

    if (!o)
        return NULL;
    cJSON_Delete(o->data);
    o->data = data ? cJSON_Parse(data) : cJSON_CreateObject();
    j = vv ? cJSON_Parse(vv) : cJSON_CreateObject();
    cJSON_Delete(o->vv);
    o->vv = j ? j : cJSON_CreateObject();
    if (!o->data || !o->vv) {
        hb_obj_free(o);
        return NULL;
    }
    o->updated_at = sqlite3_column_int64(st, 4);
    o->tombstone = sqlite3_column_int(st, 5);
    return o;
}

static hb_err load_obj(hb_vault *v, const char *id, hb_obj **out)
{
    sqlite3_stmt *st;
    hb_err e;
    *out = NULL;
    if (sqlite3_prepare_v2(v->db,
            "SELECT id,type,data,vv,updated_at,tombstone FROM objects WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out = row_to_obj(st);
        e = *out ? HB_OK : HB_ERR_NOMEM;
    } else {
        e = HB_ERR_NOT_FOUND;
    }
    sqlite3_finalize(st);
    return e;
}

hb_err hb_vault_get(hb_vault *v, const char *id, hb_obj **out)
{
    if (!v || !id || !out)
        return HB_ERR_INVALID;
    return load_obj(v, id, out);
}

/* ------------------------------- put ------------------------------- */

/* 比较新旧 data 的公开字段（忽略 _fv），把有变化的字段名记到 fv_patch。
 * fv_patch 是 cJSON object：changed 的 key -> true。调用方负责释放。
 * stored 可为 NULL（新对象）。 */
static cJSON *diff_fields(const cJSON *new_data, const cJSON *stored, bool *any)
{
    cJSON *patch = cJSON_CreateObject();
    const cJSON *it;
    *any = false;
    if (!patch)
        return NULL;
    cJSON_ArrayForEach(it, new_data) {
        const cJSON *old_it;
        if (it->string[0] == '_')
            continue;
        old_it = stored ? cJSON_GetObjectItemCaseSensitive(stored, it->string) : NULL;
        if (!cJSON_Compare(it, old_it, 1)) {
            cJSON_AddBoolToObject(patch, it->string, 1);
            *any = true;
        }
    }
    if (stored) {
        cJSON_ArrayForEach(it, stored) {
            if (it->string[0] == '_')
                continue;
            if (!cJSON_GetObjectItemCaseSensitive(new_data, it->string)) {
                cJSON_AddBoolToObject(patch, it->string, 1);
                *any = true;
            }
        }
    }
    return patch;
}

/* fv[field] = [device_id, counter]（field 可以是普通字段或 "$secret"） */
static void fv_mark(cJSON *fv, const char *field, const char *dev, uint64_t ctr)
{
    cJSON *pair = cJSON_CreateArray();
    if (!pair)
        return;
    cJSON_AddItemToArray(pair, cJSON_CreateString(dev));
    cJSON_AddItemToArray(pair, cJSON_CreateNumber((double)ctr));
    cJSON_DeleteItemFromObjectCaseSensitive(fv, field);
    cJSON_AddItemToObject(fv, field, pair);
}

/* 把存好的行状态回写到调用方 obj（vv/_fv/updated_at/tombstone）。 */
static void reflect(hb_obj *o, const cJSON *fv, const cJSON *vv,
                    int64_t updated_at, int tombstone)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "_fv");
    if (fv)
        cJSON_AddItemToObject(o->data, "_fv", cJSON_Duplicate((cJSON *)fv, 1));
    cJSON_Delete(o->vv);
    o->vv = cJSON_Duplicate((cJSON *)vv, 1);
    o->updated_at = updated_at;
    o->tombstone = tombstone;
}

/* put 主体，调用方已持有事务；*changed 报告是否有真实写入。 */
static hb_err put_locked(hb_vault *v, hb_obj *o, bool *changed)
{
    hb_obj *stored = NULL;
    hb_err e;
    bool any;
    cJSON *patch, *new_fv, *new_vv;
    uint64_t ctr;
    char *data_str, *vv_str;
    sqlite3_stmt *st;

    e = load_obj(v, o->id, &stored);
    if (e != HB_OK && e != HB_ERR_NOT_FOUND)
        return e;

    patch = diff_fields(o->data, stored ? stored->data : NULL, &any);
    if (!patch) {
        hb_obj_free(stored);
        return HB_ERR_NOMEM;
    }

    /* 真正 no-op：字段没变且无需复活 tombstone */
    if (stored && !any && stored->tombstone == 0) {
        cJSON *sfv = cJSON_GetObjectItemCaseSensitive(stored->data, "_fv");
        reflect(o, sfv, stored->vv, stored->updated_at, 0);
        cJSON_Delete(patch);
        hb_obj_free(stored);
        if (changed)
            *changed = false;
        return HB_OK;
    }
    if (changed)
        *changed = true;

    e = counter_bump(v, &ctr);
    if (e != HB_OK) {
        cJSON_Delete(patch);
        hb_obj_free(stored);
        return e;
    }

    /* _fv：以 stored 的为基础，标记本轮变更字段 */
    if (stored) {
        cJSON *sfv = cJSON_GetObjectItemCaseSensitive(stored->data, "_fv");
        new_fv = sfv ? cJSON_Duplicate(sfv, 1) : cJSON_CreateObject();
        new_vv = cJSON_Duplicate(stored->vv, 1);
    } else {
        new_fv = cJSON_CreateObject();
        new_vv = cJSON_CreateObject();
    }
    if (!new_fv || !new_vv) {
        cJSON_Delete(new_fv);
        cJSON_Delete(new_vv);
        cJSON_Delete(patch);
        hb_obj_free(stored);
        return HB_ERR_NOMEM;
    }
    {
        const cJSON *it;
        cJSON_ArrayForEach(it, patch)
            fv_mark(new_fv, it->string, v->device_id, ctr);
    }
    /* 复活 tombstoned 行（un-delete via put）：$tomb 必须打戳（§4.3）。 */
    if (stored && stored->tombstone)
        fv_mark(new_fv, "$tomb", v->device_id, ctr);

    /* vv[device_id] = counter */
    cJSON_DeleteItemFromObjectCaseSensitive(new_vv, v->device_id);
    cJSON_AddItemToObject(new_vv, v->device_id, cJSON_CreateNumber((double)ctr));

    /* 写回 data（含 _fv） */
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "_fv");
    cJSON_AddItemToObject(o->data, "_fv", new_fv);

    data_str = cJSON_PrintUnformatted(o->data);
    vv_str = cJSON_PrintUnformatted(new_vv);
    if (!data_str || !vv_str) {
        free(data_str);
        free(vv_str);
        cJSON_Delete(new_vv);
        cJSON_Delete(patch);
        hb_obj_free(stored);
        return HB_ERR_NOMEM;
    }

    o->updated_at = hb_now_ms();

    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO objects(id,type,data,vv,updated_at,tombstone,dirty)"
            " VALUES(?1,?2,?3,?4,?5,0,1)"
            " ON CONFLICT(id) DO UPDATE SET"
            "   data=excluded.data, vv=excluded.vv,"
            "   updated_at=excluded.updated_at, tombstone=0, dirty=1",
            -1, &st, NULL) != SQLITE_OK) {
        e = HB_ERR_DB;
    } else {
        sqlite3_bind_text(st, 1, o->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, o->type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, data_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, vv_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, o->updated_at);
        e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
        sqlite3_finalize(st);
    }

    free(data_str);
    free(vv_str);
    cJSON_Delete(patch);

    if (e == HB_OK) {
        cJSON_Delete(o->vv);
        o->vv = new_vv; /* adopt */
        o->tombstone = 0;
    } else {
        cJSON_Delete(new_vv);
    }
    hb_obj_free(stored);
    return e;
}

hb_err hb_vault_put(hb_vault *v, hb_obj *o)
{
    hb_err e;
    bool changed = false;
    if (!v || !o || !o->id || !o->type)
        return HB_ERR_INVALID;
    if ((e = txn_begin(v)) != HB_OK)
        return e;
    e = put_locked(v, o, &changed);
    if (e != HB_OK) {
        txn_rollback(v);
        return e;
    }
    if ((e = txn_commit(v)) != HB_OK)
        return e;
    if (changed)
        notify(v, o->type, o->id);
    return HB_OK;
}

/* ------------------------------- list ------------------------------- */

hb_err hb_vault_list(hb_vault *v, const char *type, hb_obj ***out, size_t *n)
{
    sqlite3_stmt *st;
    hb_obj **arr = NULL;
    size_t cnt = 0, cap = 0;

    *out = NULL;
    *n = 0;
    if (sqlite3_prepare_v2(v->db,
            "SELECT id,type,data,vv,updated_at,tombstone FROM objects"
            " WHERE type=?1 AND tombstone=0"
            " ORDER BY CAST(COALESCE(json_extract(data,'$.order'),0) AS INTEGER) ASC,"
            "  COALESCE(json_extract(data,'$.name'),json_extract(data,'$.title'),'')"
            "  COLLATE NOCASE ASC",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        hb_obj *o;
        if (cnt == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            hb_obj **na = realloc(arr, ncap * sizeof *na);
            if (!na) {
                sqlite3_finalize(st);
                hb_obj_list_free(arr, cnt);
                return HB_ERR_NOMEM;
            }
            arr = na;
            cap = ncap;
        }
        o = row_to_obj(st);
        if (!o) {
            sqlite3_finalize(st);
            hb_obj_list_free(arr, cnt);
            return HB_ERR_NOMEM;
        }
        arr[cnt++] = o;
    }
    sqlite3_finalize(st);
    *out = arr;
    *n = cnt;
    return HB_OK;
}

/* ------------------------------- delete/undelete ------------------------------- */

static hb_err set_tombstone(hb_vault *v, const char *id, int tomb)
{
    hb_obj *stored = NULL;
    hb_err e;
    uint64_t ctr;
    cJSON *vv, *fv;
    char *vv_str, *data_str;
    sqlite3_stmt *st;

    e = load_obj(v, id, &stored);
    if (e != HB_OK)
        return e;
    if ((e = counter_bump(v, &ctr)) != HB_OK) {
        hb_obj_free(stored);
        return e;
    }
    vv = cJSON_Duplicate(stored->vv, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(vv, v->device_id);
    cJSON_AddItemToObject(vv, v->device_id, cJSON_CreateNumber((double)ctr));

    /* 删除/撤销删除给 _fv["$tomb"] 打戳（§4.3）。 */
    fv = cJSON_GetObjectItemCaseSensitive(stored->data, "_fv");
    if (!fv) {
        fv = cJSON_CreateObject();
        cJSON_AddItemToObject(stored->data, "_fv", fv);
    }
    fv_mark(fv, "$tomb", v->device_id, ctr);

    vv_str = cJSON_PrintUnformatted(vv);
    data_str = cJSON_PrintUnformatted(stored->data);
    cJSON_Delete(vv);
    hb_obj_free(stored);
    if (!vv_str || !data_str) {
        free(vv_str);
        free(data_str);
        return HB_ERR_NOMEM;
    }

    if (sqlite3_prepare_v2(v->db,
            "UPDATE objects SET tombstone=?2, vv=?3, data=?4, updated_at=?5, dirty=1"
            " WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK) {
        free(vv_str);
        free(data_str);
        return HB_ERR_DB;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, tomb);
    sqlite3_bind_text(st, 3, vv_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, data_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, hb_now_ms());
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    free(vv_str);
    free(data_str);
    return e;
}

static hb_err delete_or_undelete(hb_vault *v, const char *id, int tomb)
{
    hb_err e;
    char type[64];
    hb_obj *o;

    if (!v || !id)
        return HB_ERR_INVALID;
    if ((e = txn_begin(v)) != HB_OK)
        return e;
    e = set_tombstone(v, id, tomb);
    if (e != HB_OK) {
        txn_rollback(v);
        return e;
    }
    /* 取 type 用于通知 */
    o = NULL;
    type[0] = 0;
    if (load_obj(v, id, &o) == HB_OK && o) {
        snprintf(type, sizeof type, "%s", o->type);
        hb_obj_free(o);
    }
    if ((e = txn_commit(v)) != HB_OK)
        return e;
    if (type[0])
        notify(v, type, id);
    return HB_OK;
}

hb_err hb_vault_delete(hb_vault *v, const char *id)
{
    return delete_or_undelete(v, id, 1);
}

hb_err hb_vault_undelete(hb_vault *v, const char *id)
{
    return delete_or_undelete(v, id, 0);
}

hb_err hb_vault_purge_tombstones(hb_vault *v, int64_t older_than_ms)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db,
            "DELETE FROM objects WHERE tombstone=1 AND updated_at<?1",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_int64(st, 1, older_than_ms);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    return e;
}

/* ------------------------------- secrets API ------------------------------- */

static hb_err load_secret(hb_vault *v, const char *id,
                          char **type_out, uint8_t **blob, size_t *blob_len,
                          cJSON **data_out, cJSON **vv_out)
{
    sqlite3_stmt *st;
    hb_err e;

    *type_out = NULL;
    *blob = NULL;
    *blob_len = 0;
    *data_out = NULL;
    *vv_out = NULL;
    if (sqlite3_prepare_v2(v->db,
            "SELECT type,secret,data,vv FROM objects WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return HB_ERR_NOT_FOUND;
    }
    *type_out = xstrdup((const char *)sqlite3_column_text(st, 0));
    if (sqlite3_column_type(st, 1) == SQLITE_BLOB) {
        *blob_len = (size_t)sqlite3_column_bytes(st, 1);
        *blob = malloc(*blob_len);
        if (*blob)
            memcpy(*blob, sqlite3_column_blob(st, 1), *blob_len);
        else
            *blob_len = 0;
    }
    *data_out = cJSON_Parse((const char *)sqlite3_column_text(st, 2));
    *vv_out = cJSON_Parse((const char *)sqlite3_column_text(st, 3));
    e = (*type_out && *data_out && *vv_out) ? HB_OK : HB_ERR_NOMEM;
    if (e != HB_OK) {
        free(*type_out);
        free(*blob);
        cJSON_Delete(*data_out);
        cJSON_Delete(*vv_out);
        *type_out = NULL;
        *blob = NULL;
        *data_out = *vv_out = NULL;
    }
    sqlite3_finalize(st);
    return e;
}

hb_err hb_vault_set_secret(hb_vault *v, const char *id, const char *field,
                           const char *value)
{
    hb_err e;
    char *type = NULL;
    uint8_t *blob = NULL, *pt = NULL, *new_blob = NULL;
    size_t blob_len = 0, pt_len = 0, new_len = 0;
    cJSON *data = NULL, *vv = NULL;
    sqlite3_stmt *st;
    uint64_t ctr;

    if (!v || !id || !field)
        return HB_ERR_INVALID;
    if ((e = txn_begin(v)) != HB_OK)
        return e;

    e = load_secret(v, id, &type, &blob, &blob_len, &data, &vv);
    if (e != HB_OK)
        goto out;

    /* 解密现有 map（没有则空） */
    if (blob) {
        e = hb__vault_secret_decrypt(v, id, type, blob, blob_len, &pt, &pt_len);
        if (e != HB_OK)
            goto out;
    }

    /* 重建明文：去掉旧 field，追加新值（value==NULL 即删除） */
    {
        sec_ent *ents;
        int nent, i;
        uint8_t *newpt, *w;
        size_t field_len = strlen(field), val_len = value ? strlen(value) : 0;
        size_t cap = pt_len + 2 + field_len + 4 + val_len + 16;

        ents = malloc(sizeof *ents * ((pt_len / 4) + 8));
        newpt = sodium_malloc(cap);
        if (!ents || !newpt) {
            free(ents);
            sodium_free(newpt);
            e = HB_ERR_NOMEM;
            goto out;
        }
        nent = pt ? sec_parse(pt, pt_len, ents, (pt_len / 4) + 8) : 0;
        if (nent < 0) {
            free(ents);
            sodium_free(newpt);
            e = HB_ERR_CRYPTO;
            goto out;
        }
        w = newpt;
        for (i = 0; i < nent; i++) {
            if (ents[i].nlen == field_len &&
                memcmp(ents[i].name, field, field_len) == 0)
                continue; /* 覆盖/删除 */
            sec_put_u16(&w, (uint32_t)ents[i].nlen);
            memcpy(w, ents[i].name, ents[i].nlen);
            w += ents[i].nlen;
            sec_put_u32(&w, (uint32_t)ents[i].vlen);
            memcpy(w, ents[i].value, ents[i].vlen);
            w += ents[i].vlen;
        }
        if (value) {
            sec_put_u16(&w, (uint32_t)field_len);
            memcpy(w, field, field_len);
            w += field_len;
            sec_put_u32(&w, (uint32_t)val_len);
            memcpy(w, value, val_len);
            w += val_len;
        }
        free(ents);
        new_len = (size_t)(w - newpt);
        e = secret_encrypt(v, id, type, newpt, new_len, &new_blob, &new_len);
        sodium_memzero(newpt, cap);
        sodium_free(newpt);
        if (e != HB_OK)
            goto out;
    }

    /* bump 计数器、_fv["$secret"]、vv */
    if ((e = counter_bump(v, &ctr)) != HB_OK)
        goto out;
    {
        cJSON *fv = cJSON_GetObjectItemCaseSensitive(data, "_fv");
        if (!fv) {
            fv = cJSON_CreateObject();
            cJSON_AddItemToObject(data, "_fv", fv);
        }
        fv_mark(fv, "$secret", v->device_id, ctr);
        cJSON_DeleteItemFromObjectCaseSensitive(vv, v->device_id);
        cJSON_AddItemToObject(vv, v->device_id, cJSON_CreateNumber((double)ctr));
    }

    {
        char *data_str = cJSON_PrintUnformatted(data);
        char *vv_str = cJSON_PrintUnformatted(vv);
        if (!data_str || !vv_str) {
            free(data_str);
            free(vv_str);
            e = HB_ERR_NOMEM;
            goto out;
        }
        if (sqlite3_prepare_v2(v->db,
                "UPDATE objects SET secret=?2, data=?3, vv=?4, updated_at=?5, dirty=1"
                " WHERE id=?1",
                -1, &st, NULL) != SQLITE_OK) {
            free(data_str);
            free(vv_str);
            e = HB_ERR_DB;
            goto out;
        }
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 2, new_blob, (int)new_len, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, data_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, vv_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, hb_now_ms());
        e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
        sqlite3_finalize(st);
        free(data_str);
        free(vv_str);
    }

out:
    if (pt) {
        sodium_memzero(pt, pt_len);
        sodium_free(pt);
    }
    free(blob);
    free(new_blob);
    cJSON_Delete(data);
    cJSON_Delete(vv);
    if (e != HB_OK) {
        free(type);
        txn_rollback(v);
        return e;
    }
    if ((e = txn_commit(v)) != HB_OK) {
        free(type);
        return e;
    }
    if (type)
        notify(v, type, id);
    free(type);
    return HB_OK;
}

char *hb_vault_get_secret(hb_vault *v, const char *id, const char *field)
{
    char *type = NULL, *ret = NULL;
    uint8_t *blob = NULL, *pt = NULL;
    size_t blob_len = 0, pt_len = 0;
    cJSON *data = NULL, *vv = NULL;
    sec_ent *ents = NULL;
    int nent, i;
    size_t field_len;

    if (!v || !id || !field)
        return NULL;
    if (load_secret(v, id, &type, &blob, &blob_len, &data, &vv) != HB_OK || !blob)
        goto out;
    if (hb__vault_secret_decrypt(v, id, type, blob, blob_len, &pt, &pt_len) != HB_OK)
        goto out;
    ents = malloc(sizeof *ents * ((pt_len / 4) + 8));
    if (!ents)
        goto out;
    nent = sec_parse(pt, pt_len, ents, (pt_len / 4) + 8);
    if (nent < 0)
        goto out;
    field_len = strlen(field);
    for (i = 0; i < nent; i++) {
        if (ents[i].nlen == field_len &&
            memcmp(ents[i].name, field, field_len) == 0) {
            ret = sodium_malloc(ents[i].vlen + 1);
            if (ret) {
                memcpy(ret, ents[i].value, ents[i].vlen);
                ret[ents[i].vlen] = 0;
            }
            break;
        }
    }
out:
    free(ents);
    if (pt) {
        sodium_memzero(pt, pt_len);
        sodium_free(pt);
    }
    free(blob);
    free(type);
    cJSON_Delete(data);
    cJSON_Delete(vv);
    return ret;
}

/* ------------------------------- 领域辅助 ------------------------------- */

size_t hb_vault_key_ref_count(hb_vault *v, const char *key_id)
{
    sqlite3_stmt *st;
    size_t n = 0;
    if (sqlite3_prepare_v2(v->db,
            "SELECT COUNT(*) FROM objects"
            " WHERE type='host' AND tombstone=0"
            " AND json_extract(data,'$.key_id')=?1",
            -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, key_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = (size_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

void hb_key_refs_free(hb_key_ref *r, size_t n)
{
    size_t i;
    if (!r)
        return;
    for (i = 0; i < n; i++) {
        free(r[i].host_id);
        free(r[i].prev_auth);
    }
    free(r);
}

hb_err hb_vault_delete_key_cascade(hb_vault *v, const char *key_id,
                                   hb_key_ref **refs_out, size_t *n_out)
{
    hb_err e;
    sqlite3_stmt *st;
    hb_obj **hosts = NULL;
    hb_key_ref *refs = NULL;
    size_t nh = 0, nrefs = 0, i;
    char **notify_ids = NULL;
    size_t nnotify = 0;

    if (refs_out)
        *refs_out = NULL;
    if (n_out)
        *n_out = 0;
    if (!v || !key_id)
        return HB_ERR_INVALID;
    if ((e = txn_begin(v)) != HB_OK)
        return e;

    e = set_tombstone(v, key_id, 1);
    if (e != HB_OK)
        goto out;

    /* 收集引用该 key 的主机，逐个清掉 key_id、auth 置 auto；
     * 同时记录 (host_id, 原 auth) 供撤销恢复 */
    if (sqlite3_prepare_v2(v->db,
            "SELECT id FROM objects WHERE type='host' AND tombstone=0"
            " AND json_extract(data,'$.key_id')=?1",
            -1, &st, NULL) != SQLITE_OK) {
        e = HB_ERR_DB;
        goto out;
    }
    sqlite3_bind_text(st, 1, key_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        hb_obj **na = realloc(hosts, (nh + 1) * sizeof *na);
        hb_key_ref *nr;
        const char *hid = (const char *)sqlite3_column_text(st, 0);
        if (!na) {
            sqlite3_finalize(st);
            e = HB_ERR_NOMEM;
            goto out;
        }
        hosts = na;
        e = load_obj(v, hid, &hosts[nh]);
        if (e != HB_OK) {
            sqlite3_finalize(st);
            goto out;
        }
        nr = realloc(refs, (nrefs + 1) * sizeof *nr);
        if (!nr) {
            sqlite3_finalize(st);
            e = HB_ERR_NOMEM;
            goto out;
        }
        refs = nr;
        refs[nrefs].host_id = xstrdup(hosts[nh]->id);
        refs[nrefs].prev_auth = xstrdup(hb_obj_get_str(hosts[nh], "auth"));
        if (!refs[nrefs].host_id ||
            (hb_obj_get_str(hosts[nh], "auth") && !refs[nrefs].prev_auth)) {
            sqlite3_finalize(st);
            nrefs++;            /* 让 out 清理这块 */
            e = HB_ERR_NOMEM;
            goto out;
        }
        nh++;
        nrefs++;
    }
    sqlite3_finalize(st);

    for (i = 0; i < nh; i++) {
        hb_obj_set_str(hosts[i], "auth", "auto");
        hb_obj_set_str(hosts[i], "key_id", NULL);
        e = put_locked(v, hosts[i], NULL);
        if (e != HB_OK)
            goto out;
    }

    /* commit 后再通知：记录涉及的 id */
    notify_ids = malloc((nh + 1) * sizeof *notify_ids);
    if (notify_ids) {
        notify_ids[nnotify++] = xstrdup(key_id);
        for (i = 0; i < nh; i++)
            notify_ids[nnotify++] = xstrdup(hosts[i]->id);
    }

    e = txn_commit(v);
    if (e != HB_OK)
        goto out;
    for (i = 0; i < nnotify; i++) {
        notify(v, i == 0 ? "key" : "host", notify_ids[i]);
        free(notify_ids[i]);
    }
    if (refs_out)
        *refs_out = refs;
    else
        hb_key_refs_free(refs, nrefs);
    if (n_out)
        *n_out = nrefs;
    refs = NULL;

out:
    free(notify_ids);
    hb_obj_list_free(hosts, nh);
    hb_key_refs_free(refs, nrefs);
    if (e != HB_OK) {
        txn_rollback(v);
        return e;
    }
    return HB_OK;
}

hb_err hb_vault_undo_key_cascade(hb_vault *v, const char *key_id,
                                 const hb_key_ref *refs, size_t n)
{
    hb_err e;
    hb_obj **restored = NULL;
    size_t nr = 0, i;

    if (!v || !key_id || (n && !refs))
        return HB_ERR_INVALID;
    if ((e = txn_begin(v)) != HB_OK)
        return e;

    e = set_tombstone(v, key_id, 0);
    if (e != HB_OK)
        goto out;

    for (i = 0; i < n; i++) {
        hb_obj *h = NULL;
        e = load_obj(v, refs[i].host_id, &h);
        if (e == HB_ERR_NOT_FOUND) {       /* 期间被真删 */
            e = HB_OK;
            continue;
        }
        if (e != HB_OK)
            goto out;
        /* 已 tombstone 或已被设了别 key 的不动 */
        if (!h->tombstone && !hb_obj_get_str(h, "key_id")) {
            hb_obj_set_str(h, "key_id", key_id);
            hb_obj_set_str(h, "auth", refs[i].prev_auth);
            e = put_locked(v, h, NULL);
            if (e == HB_OK) {
                hb_obj **na = realloc(restored, (nr + 1) * sizeof *na);
                if (!na) {
                    e = HB_ERR_NOMEM;
                    hb_obj_free(h);
                    goto out;
                }
                restored = na;
                restored[nr++] = h;
            } else {
                hb_obj_free(h);
                goto out;
            }
        } else {
            hb_obj_free(h);
        }
    }

    e = txn_commit(v);
    if (e != HB_OK)
        goto out;
    notify(v, "key", key_id);
    for (i = 0; i < nr; i++)
        notify(v, "host", restored[i]->id);

out:
    hb_obj_list_free(restored, nr);
    if (e != HB_OK) {
        txn_rollback(v);
        return e;
    }
    return HB_OK;
}

/* ------------------------------- export/import ------------------------------- */

hb_err hb_vault_export_json(hb_vault *v, const char *type, char **out)
{
    sqlite3_stmt *st;
    cJSON *arr;
    char *str;
    hb_err e = HB_OK;

    *out = NULL;
    arr = cJSON_CreateArray();
    if (!arr)
        return HB_ERR_NOMEM;
    if (sqlite3_prepare_v2(v->db,
            "SELECT id,data FROM objects WHERE type=?1 AND tombstone=0",
            -1, &st, NULL) != SQLITE_OK) {
        cJSON_Delete(arr);
        return HB_ERR_DB;
    }
    sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        cJSON *d = cJSON_Parse((const char *)sqlite3_column_text(st, 1));
        if (!d)
            continue;
        cJSON_DeleteItemFromObjectCaseSensitive(d, "_fv");
        cJSON_AddItemToObject(d, "id",
            cJSON_CreateString((const char *)sqlite3_column_text(st, 0)));
        cJSON_AddItemToArray(arr, d);
    }
    sqlite3_finalize(st);
    str = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!str)
        return HB_ERR_NOMEM;
    *out = str;
    return e;
}

hb_err hb_vault_import_json(hb_vault *v, const char *type, const char *json,
                            size_t *imported)
{
    cJSON *arr, *it;
    size_t n = 0;

    if (imported)
        *imported = 0;
    if (!v || !type || !json)
        return HB_ERR_INVALID;
    arr = cJSON_Parse(json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return HB_ERR_INVALID;
    }
    cJSON_ArrayForEach(it, arr) {
        const cJSON *f;
        hb_obj *o;
        if (!cJSON_IsObject(it))
            continue;
        o = hb_obj_new(type);
        if (!o)
            continue;
        cJSON_ArrayForEach(f, it) {
            if (!f->string || f->string[0] == '_')
                continue;
            if (strcmp(f->string, "id") == 0)
                continue;
            cJSON_AddItemToObject(o->data, f->string, cJSON_Duplicate((cJSON *)f, 1));
        }
        if (hb_vault_put(v, o) == HB_OK)
            n++;
        hb_obj_free(o);
    }
    cJSON_Delete(arr);
    if (imported)
        *imported = n;
    return HB_OK;
}

/* ------------------------------- settings ------------------------------- */

hb_err hb_vault_settings(hb_vault *v, hb_obj **out)
{
    hb_err e;
    hb_obj *o;

    *out = NULL;
    e = load_obj(v, "settings", out);
    if (e == HB_OK)
        return HB_OK;
    if (e != HB_ERR_NOT_FOUND)
        return e;

    o = hb_obj_new_with_id("settings", "settings");
    if (!o)
        return HB_ERR_NOMEM;
    hb_obj_set_str(o, "theme", "harbor");
    hb_obj_set_str(o, "ui_theme", "light");
    hb_obj_set_int(o, "font_size", 13);
    hb_obj_set_double(o, "line_height", 1.5);
    hb_obj_set_bool(o, "autocomplete", false);
    hb_obj_set_str(o, "font_family", "Monospace");
    hb_obj_set_int(o, "scrollback", 10000);
    hb_obj_set_bool(o, "close_to_tray", true);
    hb_obj_set_str(o, "notify_body", "full");
    hb_obj_set_bool(o, "watch_default_ssh", true);
    hb_obj_set_bool(o, "watch_default_local", false);
    hb_obj_set_int(o, "watch_min_duration_s", 5);
    hb_obj_set_bool(o, "watch_idle", false);
    hb_obj_set_int(o, "watch_idle_n", 10);
    hb_obj_set_int(o, "watch_idle_m", 5);
    hb_obj_set_bool(o, "watch_focus_dnd", true);
    hb_obj_set_str(o, "watch_dnd", "");
    hb_obj_set_bool(o, "watch_sound", false);
    hb_obj_set_bool(o, "osc52", false);
    hb_obj_set_bool(o, "cursor_blink", true);
    *out = o;
    return HB_OK;
}

/* ------------------------------- logs ------------------------------- */

hb_err hb_vault_log_add(hb_vault *v, const hb_log *l, int64_t *out_id)
{
    sqlite3_stmt *st;
    hb_err e;
    if (!v || !l)
        return HB_ERR_INVALID;
    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO logs(ts,user,host,host_id,proto,result,reason,duration_s,saved)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_int64(st, 1, l->ts ? l->ts : hb_now_ms());
    sqlite3_bind_text(st, 2, l->user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, l->host, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, l->host_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, l->proto, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, l->result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, l->reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, l->duration_s);
    sqlite3_bind_int(st, 9, l->saved ? 1 : 0);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    if (e == HB_OK && out_id)
        *out_id = sqlite3_last_insert_rowid(v->db);
    if (e == HB_OK)
        notify(v, "log", NULL);
    return e;
}

hb_err hb_vault_log_finish(hb_vault *v, int64_t id, const char *result,
                           const char *reason, int64_t duration_s)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db,
            "UPDATE logs SET result=?2, reason=?3, duration_s=?4 WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, duration_s);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    if (e == HB_OK && sqlite3_changes(v->db))
        notify(v, "log", NULL);
    return e;
}

hb_err hb_vault_log_set_saved(hb_vault *v, int64_t id, bool saved)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db,
            "UPDATE logs SET saved=?2 WHERE id=?1", -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int(st, 2, saved ? 1 : 0);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    if (e == HB_OK && sqlite3_changes(v->db))
        notify(v, "log", NULL);
    return e;
}

hb_err hb_vault_log_foreach(hb_vault *v, const char *filter, bool desc,
                            hb_log_cb cb, void *ud)
{
    sqlite3_stmt *st;
    hb_err e = HB_OK;
    const char *sql =
        "SELECT id,ts,user,host,host_id,proto,result,reason,duration_s,saved"
        " FROM logs"
        " WHERE (?1 IS NULL OR instr(COALESCE(user,''),?1)>0"
        "   OR instr(COALESCE(host,''),?1)>0"
        "   OR instr(COALESCE(proto,''),?1)>0"
        "   OR instr(COALESCE(result,''),?1)>0)"
        " ORDER BY ts %s, id %s";
    char buf[512];
    snprintf(buf, sizeof buf, sql,
             desc ? "DESC" : "ASC", desc ? "DESC" : "ASC");
    if (sqlite3_prepare_v2(v->db, buf, -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    if (filter)
        sqlite3_bind_text(st, 1, filter, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 1);
    while (sqlite3_step(st) == SQLITE_ROW) {
        hb_log l = {
            .id = sqlite3_column_int64(st, 0),
            .ts = sqlite3_column_int64(st, 1),
            .user = (const char *)sqlite3_column_text(st, 2),
            .host = (const char *)sqlite3_column_text(st, 3),
            .host_id = (const char *)sqlite3_column_text(st, 4),
            .proto = (const char *)sqlite3_column_text(st, 5),
            .result = (const char *)sqlite3_column_text(st, 6),
            .reason = (const char *)sqlite3_column_text(st, 7),
            .duration_s = sqlite3_column_int64(st, 8),
            .saved = sqlite3_column_int(st, 9) != 0,
        };
        if (!cb(&l, ud)) {
            sqlite3_finalize(st);
            return HB_OK;
        }
    }
    sqlite3_finalize(st);
    return e;
}

/* ------------------------------- 字符串缓冲 ------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} sbuf;

static hb_err sb_init(sbuf *b, size_t cap)
{
    b->p = malloc(cap);
    if (!b->p)
        return HB_ERR_NOMEM;
    b->len = 0;
    b->cap = cap;
    b->p[0] = 0;
    return HB_OK;
}

static hb_err sb_reserve(sbuf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t ncap = b->cap * 2;
        char *np;
        while (ncap < b->len + extra + 1)
            ncap *= 2;
        np = realloc(b->p, ncap);
        if (!np)
            return HB_ERR_NOMEM;
        b->p = np;
        b->cap = ncap;
    }
    return HB_OK;
}

static hb_err sb_add(sbuf *b, const char *s, size_t n)
{
    if (sb_reserve(b, n) != HB_OK)
        return HB_ERR_NOMEM;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return HB_OK;
}

static hb_err sb_addstr(sbuf *b, const char *s)
{
    return sb_add(b, s, strlen(s));
}

static hb_err sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return HB_ERR_INVALID;
    if (sb_reserve(b, (size_t)n) != HB_OK)
        return HB_ERR_NOMEM;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return HB_OK;
}

/* RFC4180：字段加引号，内部引号双写；始终对文本字段加引号。 */
static hb_err sb_add_quoted(sbuf *b, const char *s)
{
    const char *p;
    if (sb_addstr(b, "\"") != HB_OK)
        return HB_ERR_NOMEM;
    if (s) {
        for (p = s; *p; p++) {
            if (*p == '"' && sb_addstr(b, "\"\"") != HB_OK)
                return HB_ERR_NOMEM;
            else if (*p != '"' && sb_add(b, p, 1) != HB_OK)
                return HB_ERR_NOMEM;
        }
    }
    return sb_addstr(b, "\"");
}

hb_err hb_vault_log_export_csv(hb_vault *v, char **out)
{
    sbuf b;
    sqlite3_stmt *st;
    hb_err e;

    *out = NULL;
    if ((e = sb_init(&b, 4096)) != HB_OK)
        return e;
    sb_addstr(&b, "\xef\xbb\xbf" "Date,User,Host,Protocol,Result,Reason,Duration,Saved\r\n");

    if (sqlite3_prepare_v2(v->db,
            "SELECT ts,user,host,proto,result,reason,duration_s,saved"
            " FROM logs ORDER BY ts ASC, id ASC",
            -1, &st, NULL) != SQLITE_OK) {
        free(b.p);
        return HB_ERR_DB;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        time_t secs = (time_t)(sqlite3_column_int64(st, 0) / 1000);
        struct tm tmv;
        char dbuf[32];
        const char *s;
        localtime_r(&secs, &tmv);
        strftime(dbuf, sizeof dbuf, "%Y-%m-%d %H:%M:%S", &tmv);
        sb_add_quoted(&b, dbuf);
        for (int col = 1; col <= 5; col++) {
            sb_addstr(&b, ",");
            s = (const char *)sqlite3_column_text(st, col);
            sb_add_quoted(&b, s);
        }
        sb_printf(&b, ",%lld,%s\r\n",
                  (long long)sqlite3_column_int64(st, 6),
                  sqlite3_column_int(st, 7) ? "yes" : "no");
    }
    sqlite3_finalize(st);
    *out = b.p;
    return HB_OK;
}

hb_err hb_vault_log_export_json(hb_vault *v, char **out)
{
    sqlite3_stmt *st;
    cJSON *arr;

    *out = NULL;
    arr = cJSON_CreateArray();
    if (!arr)
        return HB_ERR_NOMEM;
    if (sqlite3_prepare_v2(v->db,
            "SELECT id,ts,user,host,host_id,proto,result,reason,duration_s,saved"
            " FROM logs ORDER BY ts ASC, id ASC",
            -1, &st, NULL) != SQLITE_OK) {
        cJSON_Delete(arr);
        return HB_ERR_DB;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        cJSON *o = cJSON_CreateObject();
        const char *cols[] = {"user", "host", "host_id", "proto", "result", "reason"};
        int i;
        if (!o)
            continue;
        cJSON_AddNumberToObject(o, "id", (double)sqlite3_column_int64(st, 0));
        cJSON_AddNumberToObject(o, "ts", (double)sqlite3_column_int64(st, 1));
        for (i = 0; i < 6; i++) {
            const char *s = (const char *)sqlite3_column_text(st, 2 + i);
            if (s)
                cJSON_AddStringToObject(o, cols[i], s);
        }
        cJSON_AddNumberToObject(o, "duration_s", (double)sqlite3_column_int64(st, 8));
        cJSON_AddBoolToObject(o, "saved", sqlite3_column_int(st, 9));
        cJSON_AddItemToArray(arr, o);
    }
    sqlite3_finalize(st);
    *out = cJSON_Print(arr);
    cJSON_Delete(arr);
    return *out ? HB_OK : HB_ERR_NOMEM;
}

hb_err hb_vault_log_purge(hb_vault *v, int retention_days)
{
    sqlite3_stmt *st;
    hb_err e;
    int64_t cutoff;
    if (retention_days <= 0)
        return HB_OK;
    cutoff = hb_now_ms() - (int64_t)retention_days * 86400 * 1000;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM logs WHERE ts<?1", -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_int64(st, 1, cutoff);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    if (e == HB_OK && sqlite3_changes(v->db))
        notify(v, "log", NULL);
    return e;
}

hb_err hb_vault_log_clear(hb_vault *v)
{
    char *err = NULL;
    if (!v)
        return HB_ERR_INVALID;
    if (sqlite3_exec(v->db, "DELETE FROM logs", NULL, NULL, &err)
        != SQLITE_OK) {
        sqlite3_free(err);
        return HB_ERR_DB;
    }
    notify(v, "log", NULL);
    return HB_OK;
}

/* ------------------------------- history ------------------------------- */

hb_err hb_vault_history_add(hb_vault *v, const char *host_id, const char *cmd)
{
    sqlite3_stmt *st;
    hb_err e;
    const char *start, *end;
    char *trimmed;
    size_t tlen;

    if (!v || !cmd)
        return HB_ERR_INVALID;

    /* trim */
    start = cmd;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
        start++;
    end = cmd + strlen(cmd);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        end--;
    tlen = (size_t)(end - start);
    if (tlen == 0)
        return HB_OK;
    trimmed = strndup(start, tlen);
    if (!trimmed)
        return HB_ERR_NOMEM;

    /* 同一 host 的最近一条相同则跳过（IS 兼容 NULL host_id = 本地） */
    if (sqlite3_prepare_v2(v->db,
            "SELECT cmd FROM history WHERE host_id IS ?1 ORDER BY id DESC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        free(trimmed);
        return HB_ERR_DB;
    }
    if (host_id)
        sqlite3_bind_text(st, 1, host_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *last = (const char *)sqlite3_column_text(st, 0);
        if (last && strcmp(last, trimmed) == 0) {
            sqlite3_finalize(st);
            free(trimmed);
            return HB_OK;
        }
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO history(ts,host_id,cmd) VALUES(?1,?2,?3)",
            -1, &st, NULL) != SQLITE_OK) {
        free(trimmed);
        return HB_ERR_DB;
    }
    sqlite3_bind_int64(st, 1, hb_now_ms());
    sqlite3_bind_text(st, 2, host_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, trimmed, -1, SQLITE_TRANSIENT);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    free(trimmed);
    if (e == HB_OK)
        notify(v, "history", NULL);
    return e;
}

hb_err hb_vault_history_search(hb_vault *v, const char *prefix_or_substr,
                               size_t limit, char ***out, size_t *n)
{
    sqlite3_stmt *st;
    char **arr = NULL;
    size_t cnt = 0;

    *out = NULL;
    *n = 0;
    if (!v || !prefix_or_substr || limit == 0)
        return HB_ERR_INVALID;
    if (sqlite3_prepare_v2(v->db,
            "SELECT cmd, MAX(id) AS mid FROM history"
            " WHERE instr(cmd,?1)>0"
            " GROUP BY cmd ORDER BY mid DESC LIMIT ?2",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, prefix_or_substr, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)limit);
    while (sqlite3_step(st) == SQLITE_ROW && cnt < limit) {
        char *s = xstrdup((const char *)sqlite3_column_text(st, 0));
        char **na;
        if (!s) {
            sqlite3_finalize(st);
            hb_strfreev(arr, cnt);
            return HB_ERR_NOMEM;
        }
        na = realloc(arr, (cnt + 1) * sizeof *na);
        if (!na) {
            free(s);
            sqlite3_finalize(st);
            hb_strfreev(arr, cnt);
            return HB_ERR_NOMEM;
        }
        arr = na;
        arr[cnt++] = s;
    }
    sqlite3_finalize(st);
    *out = arr;
    *n = cnt;
    return HB_OK;
}

hb_err hb_vault_history_foreach(hb_vault *v, const char *substr,
                                const char *host_id,
                                hb_history_cb cb, void *ud)
{
    sqlite3_stmt *st;
    char *sql = NULL;
    hb_err e = HB_OK;
    int go = 1;

    if (!v || !cb)
        return HB_ERR_INVALID;
    {
        const char *w_sub = (substr && *substr)
            ? " AND instr(cmd,?2)>0" : "";
        const char *w_host = host_id ? (*host_id ? " AND host_id=?3"
                                                : " AND host_id IS NULL")
                                     : "";
        sql = sqlite3_mprintf(
            "SELECT id,ts,host_id,cmd FROM history WHERE 1%w%w"
            " ORDER BY id DESC", w_sub, w_host);
    }
    if (!sql)
        return HB_ERR_NOMEM;
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_free(sql);
        return HB_ERR_DB;
    }
    sqlite3_free(sql);
    if (substr && *substr)
        sqlite3_bind_text(st, 2, substr, -1, SQLITE_TRANSIENT);
    if (host_id && *host_id)
        sqlite3_bind_text(st, 3, host_id, -1, SQLITE_TRANSIENT);
    while (go && sqlite3_step(st) == SQLITE_ROW) {
        hb_history_entry ent;
        ent.id = sqlite3_column_int64(st, 0);
        ent.ts = sqlite3_column_int64(st, 1);
        ent.host_id = (const char *)sqlite3_column_text(st, 2);
        ent.cmd = (const char *)sqlite3_column_text(st, 3);
        if (!cb(&ent, ud))
            go = 0;
    }
    sqlite3_finalize(st);
    return e;
}

hb_err hb_vault_history_delete(hb_vault *v, int64_t id)
{
    sqlite3_stmt *st;
    hb_err e;
    if (!v)
        return HB_ERR_INVALID;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM history WHERE id=?1",
                           -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_int64(st, 1, id);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    if (e == HB_OK && sqlite3_changes(v->db))
        notify(v, "history", NULL);
    return e;
}

hb_err hb_vault_history_clear(hb_vault *v)
{
    char *err = NULL;
    if (!v)
        return HB_ERR_INVALID;
    if (sqlite3_exec(v->db, "DELETE FROM history", NULL, NULL, &err)
        != SQLITE_OK) {
        sqlite3_free(err);
        return HB_ERR_DB;
    }
    notify(v, "history", NULL);
    return HB_OK;
}

/* ------------------------------- sync internals ------------------------------- */

hb_err hb__vault_meta_get(hb_vault *v, const char *key, char **out)
{
    return meta_get(v, key, out);
}

hb_err hb__vault_meta_set(hb_vault *v, const char *key, const char *value)
{
    return meta_set(v, key, value);
}

hb_err hb__vault_meta_del(hb_vault *v, const char *key)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM meta WHERE key=?1",
                           -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    return e;
}

void hb__vault_rows_free(hb__vault_row *r, size_t n)
{
    size_t i;
    if (!r)
        return;
    for (i = 0; i < n; i++) {
        free(r[i].id);
        free(r[i].type);
        free(r[i].data);
        free(r[i].vv);
    }
    free(r);
}

hb_err hb__vault_rows(hb_vault *v, hb__vault_row **out, size_t *n)
{
    sqlite3_stmt *st;
    hb__vault_row *arr = NULL;
    size_t cnt = 0, cap = 0;

    *out = NULL;
    *n = 0;
    if (sqlite3_prepare_v2(v->db,
            "SELECT id,type,data,vv,tombstone,dirty,sync_seq,updated_at"
            " FROM objects ORDER BY id",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    while (sqlite3_step(st) == SQLITE_ROW) {
        hb__vault_row *r;
        if (cnt == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            hb__vault_row *na = realloc(arr, ncap * sizeof *na);
            if (!na) {
                sqlite3_finalize(st);
                hb__vault_rows_free(arr, cnt);
                return HB_ERR_NOMEM;
            }
            arr = na;
            cap = ncap;
        }
        r = &arr[cnt];
        memset(r, 0, sizeof *r);
        r->id = xstrdup((const char *)sqlite3_column_text(st, 0));
        r->type = xstrdup((const char *)sqlite3_column_text(st, 1));
        r->data = xstrdup((const char *)sqlite3_column_text(st, 2));
        r->vv = xstrdup((const char *)sqlite3_column_text(st, 3));
        r->tombstone = sqlite3_column_int(st, 4);
        r->dirty = sqlite3_column_int(st, 5);
        r->sync_seq = sqlite3_column_int64(st, 6);
        r->updated_at = sqlite3_column_int64(st, 7);
        if (!r->id || !r->type || !r->data || !r->vv) {
            sqlite3_finalize(st);
            hb__vault_rows_free(arr, cnt + 1);
            return HB_ERR_NOMEM;
        }
        cnt++;
    }
    sqlite3_finalize(st);
    *out = arr;
    *n = cnt;
    return HB_OK;
}

/* 解密对象的整个 secret map（二进制 map 明文，sodium_malloc'd）。
 * 无 secret 返回 NOT_FOUND。 */
hb_err hb__vault_secret_map(hb_vault *v, const char *id,
                            uint8_t **pt, size_t *pt_len)
{
    char *type = NULL;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    cJSON *data = NULL, *vv = NULL;
    hb_err e;

    *pt = NULL;
    *pt_len = 0;
    e = load_secret(v, id, &type, &blob, &blob_len, &data, &vv);
    cJSON_Delete(data);
    cJSON_Delete(vv);
    if (e != HB_OK) {
        free(type);
        free(blob);
        return e;
    }
    if (!blob) {
        free(type);
        return HB_ERR_NOT_FOUND;
    }
    e = hb__vault_secret_decrypt(v, id, type, blob, blob_len, pt, pt_len);
    free(type);
    free(blob);
    return e;
}

/* 写整条对象状态：不产生计数器/tomb 戳，dirty/sync_seq 显式给定。
 * data_json 须含 _fv。sec_pt==NULL 清除 secret；否则为新 map 明文（DEK 加密存入）。
 * 提交后触发变更回调。 */
hb_err hb__vault_apply(hb_vault *v, const char *id, const char *type,
                       const char *data_json, const char *vv_json,
                       int tomb, int64_t updated_at,
                       const uint8_t *sec_pt, size_t sec_len,
                       int dirty, int64_t sync_seq)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    sqlite3_stmt *st;
    hb_err e;

    if (!v || !id || !type || !data_json || !vv_json)
        return HB_ERR_INVALID;
    if (sec_pt) {
        e = secret_encrypt(v, id, type, sec_pt, sec_len, &blob, &blob_len);
        if (e != HB_OK)
            return e;
    }
    if ((e = txn_begin(v)) != HB_OK) {
        free(blob);
        return e;
    }
    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO objects(id,type,data,secret,vv,updated_at,tombstone,"
            " dirty,sync_seq) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
            " ON CONFLICT(id) DO UPDATE SET type=excluded.type,"
            " data=excluded.data, secret=excluded.secret, vv=excluded.vv,"
            " updated_at=excluded.updated_at, tombstone=excluded.tombstone,"
            " dirty=excluded.dirty, sync_seq=excluded.sync_seq",
            -1, &st, NULL) != SQLITE_OK) {
        e = HB_ERR_DB;
    } else {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, data_json, -1, SQLITE_TRANSIENT);
        if (blob)
            sqlite3_bind_blob(st, 4, blob, (int)blob_len, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 4);
        sqlite3_bind_text(st, 5, vv_json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, updated_at);
        sqlite3_bind_int(st, 7, tomb);
        sqlite3_bind_int(st, 8, dirty);
        sqlite3_bind_int64(st, 9, sync_seq);
        e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
        sqlite3_finalize(st);
    }
    free(blob);
    if (e != HB_OK) {
        txn_rollback(v);
        return e;
    }
    if ((e = txn_commit(v)) != HB_OK)
        return e;
    notify(v, type, id);
    return HB_OK;
}

/* 仅当行内 vv 仍等于 seal 时捕获的 vv（期间无本地改动）才标干净。 */
hb_err hb__vault_mark_synced(hb_vault *v, const char *id, const char *vv_json,
                             int64_t sync_seq, bool *applied)
{
    sqlite3_stmt *st;
    hb_err e;

    *applied = false;
    if (sqlite3_prepare_v2(v->db,
            "UPDATE objects SET dirty=0, sync_seq=?2 WHERE id=?1 AND vv=?3",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, sync_seq);
    sqlite3_bind_text(st, 3, vv_json, -1, SQLITE_TRANSIENT);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    if (e == HB_OK && sqlite3_changes(v->db) > 0)
        *applied = true;
    /* vv 变了（本地又改过）：仍要把 sync_seq 推进，否则 base_seq 永远过期。 */
    if (e == HB_OK && !*applied) {
        sqlite3_stmt *st2;
        if (sqlite3_prepare_v2(v->db,
                "UPDATE objects SET sync_seq=?2 WHERE id=?1",
                -1, &st2, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st2, 1, id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st2, 2, sync_seq);
            if (sqlite3_step(st2) != SQLITE_DONE)
                e = HB_ERR_DB;
            sqlite3_finalize(st2);
        } else {
            e = HB_ERR_DB;
        }
    }
    sqlite3_finalize(st);
    return e;
}

hb_err hb__vault_purge_type(hb_vault *v, const char *type)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM objects WHERE type=?1",
                           -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    if (e == HB_OK)
        notify(v, type, NULL);
    return e;
}

hb_err hb__vault_mark_all_dirty(hb_vault *v)
{
    return sql_exec(v, "UPDATE objects SET dirty=1, sync_seq=0");
}

/* ------------------------------ sync_conflicts ------------------------------ */

hb_err hb__vault_conflict_put(hb_vault *v, const char *id, const char *type,
                              const char *fields_json, const char *loser_data,
                              const uint8_t *loser_sec, size_t loser_sec_len)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    sqlite3_stmt *st;
    hb_err e;

    if (loser_sec) {
        e = secret_encrypt(v, id, type, loser_sec, loser_sec_len, &blob, &blob_len);
        if (e != HB_OK)
            return e;
    }
    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO sync_conflicts(id,type,fields,data,secret,created)"
            " VALUES(?1,?2,?3,?4,?5,?6)"
            " ON CONFLICT(id) DO UPDATE SET type=excluded.type,"
            " fields=excluded.fields, data=excluded.data,"
            " secret=excluded.secret, created=excluded.created",
            -1, &st, NULL) != SQLITE_OK) {
        free(blob);
        return HB_ERR_DB;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, fields_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, loser_data, -1, SQLITE_TRANSIENT);
    if (blob)
        sqlite3_bind_blob(st, 5, blob, (int)blob_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 5);
    sqlite3_bind_int64(st, 6, hb_now_ms());
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    free(blob);
    return e;
}

hb_err hb__vault_conflict_get(hb_vault *v, const char *id,
                              char **type, char **fields, char **loser_data,
                              uint8_t **sec, size_t *sec_len, int64_t *created)
{
    sqlite3_stmt *st;
    hb_err e = HB_ERR_NOT_FOUND;

    *type = *fields = *loser_data = NULL;
    *sec = NULL;
    *sec_len = 0;
    *created = 0;
    if (sqlite3_prepare_v2(v->db,
            "SELECT type,fields,data,secret,created FROM sync_conflicts"
            " WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *type = xstrdup((const char *)sqlite3_column_text(st, 0));
        *fields = xstrdup((const char *)sqlite3_column_text(st, 1));
        *loser_data = xstrdup((const char *)sqlite3_column_text(st, 2));
        if (sqlite3_column_type(st, 3) == SQLITE_BLOB) {
            *sec_len = (size_t)sqlite3_column_bytes(st, 3);
            *sec = malloc(*sec_len);
            if (*sec)
                memcpy(*sec, sqlite3_column_blob(st, 3), *sec_len);
            else
                *sec_len = 0;
        }
        *created = sqlite3_column_int64(st, 4);
        e = (*type && *fields && *loser_data) ? HB_OK : HB_ERR_NOMEM;
        if (e != HB_OK) {
            free(*type);
            free(*fields);
            free(*loser_data);
            free(*sec);
            *type = *fields = *loser_data = NULL;
            *sec = NULL;
        }
    }
    sqlite3_finalize(st);
    return e;
}

hb_err hb__vault_conflict_del(hb_vault *v, const char *id)
{
    sqlite3_stmt *st;
    hb_err e;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM sync_conflicts WHERE id=?1",
                           -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    e = sqlite3_step(st) == SQLITE_DONE ? HB_OK : HB_ERR_DB;
    sqlite3_finalize(st);
    return e;
}

void hb__vault_conflicts_free(hb__conflict_row *c, size_t n)
{
    size_t i;
    if (!c)
        return;
    for (i = 0; i < n; i++) {
        free(c[i].id);
        free(c[i].type);
        free(c[i].fields);
        free(c[i].loser_data);
    }
    free(c);
}

hb_err hb__vault_conflicts(hb_vault *v, hb__conflict_row **out, size_t *n)
{
    sqlite3_stmt *st;
    hb__conflict_row *arr = NULL;
    size_t cnt = 0, cap = 0;

    *out = NULL;
    *n = 0;
    if (sqlite3_prepare_v2(v->db,
            "SELECT id,type,fields,data,created FROM sync_conflicts ORDER BY created",
            -1, &st, NULL) != SQLITE_OK)
        return HB_ERR_DB;
    while (sqlite3_step(st) == SQLITE_ROW) {
        hb__conflict_row *r;
        if (cnt == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            hb__conflict_row *na = realloc(arr, ncap * sizeof *na);
            if (!na) {
                sqlite3_finalize(st);
                hb__vault_conflicts_free(arr, cnt);
                return HB_ERR_NOMEM;
            }
            arr = na;
            cap = ncap;
        }
        r = &arr[cnt];
        memset(r, 0, sizeof *r);
        r->id = xstrdup((const char *)sqlite3_column_text(st, 0));
        r->type = xstrdup((const char *)sqlite3_column_text(st, 1));
        r->fields = xstrdup((const char *)sqlite3_column_text(st, 2));
        r->loser_data = xstrdup((const char *)sqlite3_column_text(st, 3));
        r->created = sqlite3_column_int64(st, 4);
        if (!r->id || !r->type || !r->fields || !r->loser_data) {
            sqlite3_finalize(st);
            hb__vault_conflicts_free(arr, cnt + 1);
            return HB_ERR_NOMEM;
        }
        cnt++;
    }
    sqlite3_finalize(st);
    *out = arr;
    *n = cnt;
    return HB_OK;
}

hb_err hb__vault_conflicts_clear(hb_vault *v)
{
    return sql_exec(v, "DELETE FROM sync_conflicts");
}
