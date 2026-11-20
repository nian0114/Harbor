/* test_vault.c — vault 对象库单元测试。临时目录在 /tmp，测试后清理。 */

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harbor/harbor_core.h"
#include "vault_priv.h"
#include "cJSON.h"
#include <sqlite3.h>

static char tmpdir[256];
static char dbpath[300];

static const uint8_t DEK_A[32] = {0};
static const uint8_t DEK_B[32] = {1};

static void mk_tmp(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/hbtest-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(dbpath, sizeof dbpath, "%s/vault.db", tmpdir);
}

static void rm_tmp(void)
{
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed\n");
}

static hb_vault *open_v(const uint8_t *dek)
{
    hb_vault *v = NULL;
    hb_err e = hb_vault_open(dbpath, dek, &v);
    if (e != HB_OK) {
        fprintf(stderr, "vault_open: %s\n", hb_strerror(e));
        exit(2);
    }
    return v;
}

/* 在目录所有文件里做二进制子串搜索；找到返回 1。 */
static int dir_grep(const char *needle)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "grep -rqF '%s' '%s'", needle, tmpdir);
    return system(cmd) == 0;
}

static hb_obj *mk_host(const char *name, int order)
{
    hb_obj *o = hb_obj_new("host");
    hb_obj_set_str(o, "name", name);
    hb_obj_set_int(o, "order", order);
    return o;
}

/* 从 data JSON 里取 _fv[field][1]（counter），不存在返回 -1。 */
static int64_t fv_counter(const hb_obj *o, const char *field)
{
    char *js = hb_obj_to_json(o);
    cJSON *d = cJSON_Parse(js);
    cJSON *fv, *ent;
    int64_t r = -1;
    free(js);
    fv = cJSON_GetObjectItemCaseSensitive(d, "_fv");
    if (fv) {
        ent = cJSON_GetObjectItemCaseSensitive(fv, field);
        if (cJSON_IsArray(ent)) {
            cJSON *c = cJSON_GetArrayItem(ent, 1);
            if (cJSON_IsNumber(c))
                r = (int64_t)c->valuedouble;
        }
    }
    cJSON_Delete(d);
    return r;
}

static void test_open_device_id(void)
{
    hb_vault *v = open_v(DEK_A);
    char dev1[64];
    snprintf(dev1, sizeof dev1, "%s", hb_vault_device_id(v));
    CHECK(strlen(dev1) == 36);
    CHECK(access(dbpath, F_OK) == 0);
    hb_vault_close(v);

    v = open_v(DEK_A);
    CHECK_EQ_STR(hb_vault_device_id(v), dev1);
    hb_vault_close(v);
}

static void test_put_get_list(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *a = mk_host("bravo", 2);
    hb_obj *b = mk_host("Alpha", 2);
    hb_obj *c = mk_host("charlie", 1);
    hb_obj *g = NULL;
    hb_obj **list = NULL;
    size_t n = 0;

    CHECK_EQ_INT(hb_vault_put(v, a), HB_OK);
    CHECK_EQ_INT(hb_vault_put(v, b), HB_OK);
    CHECK_EQ_INT(hb_vault_put(v, c), HB_OK);

    CHECK_EQ_INT(hb_vault_get(v, hb_obj_id(b), &g), HB_OK);
    CHECK_EQ_STR(hb_obj_get_str(g, "name"), "Alpha");
    CHECK_EQ_INT(hb_obj_get_int(g, "order", -1), 2);
    CHECK(hb_obj_updated_at(g) > 0);
    hb_obj_free(g);

    /* order asc, then name nocase: charlie(1), Alpha(2), bravo(2) */
    CHECK_EQ_INT(hb_vault_list(v, "host", &list, &n), HB_OK);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_STR(hb_obj_get_str(list[0], "name"), "charlie");
    CHECK_EQ_STR(hb_obj_get_str(list[1], "name"), "Alpha");
    CHECK_EQ_STR(hb_obj_get_str(list[2], "name"), "bravo");
    hb_obj_list_free(list, n);

    CHECK_EQ_INT(hb_vault_get(v, "nonexistent", &g), HB_ERR_NOT_FOUND);

    hb_obj_free(a);
    hb_obj_free(b);
    hb_obj_free(c);
    hb_vault_close(v);
}

static void test_fv_and_noop(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *o = mk_host("web", 1);
    int64_t c_name1, c_port1, c_name2, c_name3;

    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    c_name1 = fv_counter(o, "name");
    c_port1 = fv_counter(o, "port");
    CHECK(c_name1 > 0);
    CHECK_EQ_INT(c_port1, -1); /* 未设置的字段无 _fv 项 */

    /* 重复 put 同一数据 = no-op，不 bump counter */
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(fv_counter(o, "name"), c_name1);

    /* 改 name → 只有 name 的 _fv 更新 */
    hb_obj_set_str(o, "name", "web2");
    hb_obj_set_int(o, "port", 22);
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    c_name2 = fv_counter(o, "name");
    c_port1 = fv_counter(o, "port");
    CHECK(c_name2 > c_name1);
    CHECK(c_port1 > 0);

    /* 再 noop */
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(fv_counter(o, "name"), c_name2);

    /* 删除字段 → _fv 也标记 */
    hb_obj_set_str(o, "name", NULL);
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    c_name3 = fv_counter(o, "name");
    CHECK(c_name3 > c_name2);

    /* 保留键被拒绝 */
    hb_obj_set_str(o, "_evil", "x");
    CHECK(!hb_obj_has(o, "_evil"));

    hb_obj_free(o);
    hb_vault_close(v);
}

static void test_delete_undelete(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *o = mk_host("temp", 1);
    hb_obj **list = NULL;
    hb_obj *g = NULL;
    size_t n = 0;
    char id[64], *sec;

    snprintf(id, sizeof id, "%s", hb_obj_id(o));
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(hb_vault_set_secret(v, id, "password", "s3cret"), HB_OK);

    CHECK_EQ_INT(hb_vault_delete(v, id), HB_OK);
    CHECK_EQ_INT(hb_vault_list(v, "host", &list, &n), HB_OK);
    CHECK_EQ_INT(n, 0);
    hb_obj_list_free(list, n);

    CHECK_EQ_INT(hb_vault_get(v, id, &g), HB_OK);
    CHECK(hb_obj_tombstone(g));
    hb_obj_free(g);

    CHECK_EQ_INT(hb_vault_undelete(v, id), HB_OK);
    CHECK_EQ_INT(hb_vault_get(v, id, &g), HB_OK);
    CHECK(!hb_obj_tombstone(g));
    CHECK_EQ_STR(hb_obj_get_str(g, "name"), "temp");
    hb_obj_free(g);

    /* secret 保留 */
    sec = hb_vault_get_secret(v, id, "password");
    CHECK_EQ_STR(sec, "s3cret");
    hb_secret_free(sec);

    /* 不存在的 id */
    CHECK_EQ_INT(hb_vault_delete(v, "nope"), HB_ERR_NOT_FOUND);

    hb_obj_free(o);
    hb_vault_close(v);
}

static void test_secrets(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *o = mk_host("s", 1);
    char id[64], *sec;
    sqlite3 *db;
    sqlite3_stmt *st;
    const void *blob = NULL;
    int blob_len = 0;
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    hb_vault *v2;

    snprintf(id, sizeof id, "%s", hb_obj_id(o));
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);

    CHECK_EQ_INT(hb_vault_set_secret(v, id, "password", "P@ss,w\"ord"), HB_OK);
    CHECK_EQ_INT(hb_vault_set_secret(v, id, "otp", "123456"), HB_OK);

    sec = hb_vault_get_secret(v, id, "password");
    CHECK_EQ_STR(sec, "P@ss,w\"ord");
    hb_secret_free(sec);
    sec = hb_vault_get_secret(v, id, "otp");
    CHECK_EQ_STR(sec, "123456");
    hb_secret_free(sec);

    /* 不存在字段 -> NULL */
    CHECK(hb_vault_get_secret(v, id, "nothing") == NULL);
    CHECK(hb_vault_get_secret(v, "no-such-obj", "password") == NULL);

    /* 清除字段 */
    CHECK_EQ_INT(hb_vault_set_secret(v, id, "otp", NULL), HB_OK);
    CHECK(hb_vault_get_secret(v, id, "otp") == NULL);
    sec = hb_vault_get_secret(v, id, "password");
    CHECK_EQ_STR(sec, "P@ss,w\"ord");
    hb_secret_free(sec);

    /* put 改 data 后 secret 仍保留 */
    hb_obj_set_int(o, "port", 2222);
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    sec = hb_vault_get_secret(v, id, "password");
    CHECK_EQ_STR(sec, "P@ss,w\"ord");
    hb_secret_free(sec);

    /* 明文不落盘 */
    CHECK(!dir_grep("P@ss,w\"ord"));
    CHECK(!dir_grep("123456"));

    /* 错误 DEK：get_secret -> NULL，直接解密 -> HB_ERR_CRYPTO */
    hb_vault_close(v);
    v2 = open_v(DEK_B);
    CHECK(hb_vault_get_secret(v2, id, "password") == NULL);

    /* 直接从 db 读 blob，调用内部解密 */
    CHECK_EQ_INT(sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    CHECK_EQ_INT(sqlite3_prepare_v2(db,
        "SELECT secret FROM objects WHERE id=?1", -1, &st, NULL), SQLITE_OK);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    CHECK_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    blob_len = sqlite3_column_bytes(st, 0);
    blob = sqlite3_column_blob(st, 0);
    CHECK(blob_len > 40);
    CHECK_EQ_INT(hb__vault_secret_decrypt(v2, id, "host", blob, blob_len,
                                          &pt, &pt_len), HB_ERR_CRYPTO);
    sqlite3_finalize(st);
    sqlite3_close(db);

    hb_vault_close(v2);
    hb_obj_free(o);
}

static void test_key_cascade(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *k = hb_obj_new("key");
    hb_obj *h1 = mk_host("h1", 1);
    hb_obj *h2 = mk_host("h2", 2);
    hb_obj *h3 = mk_host("h3", 3);
    hb_obj *g = NULL;
    hb_key_ref *refs = NULL;
    size_t nrefs = 0;
    char kid[64], h3id[64];

    hb_obj_set_str(k, "name", "mykey");
    snprintf(kid, sizeof kid, "%s", hb_obj_id(k));
    hb_obj_set_str(h1, "auth", "key");
    hb_obj_set_str(h1, "key_id", kid);
    hb_obj_set_str(h2, "auth", "auto");        /* 引用 key 但 auth 已是 auto */
    hb_obj_set_str(h2, "key_id", kid);
    hb_obj_set_str(h3, "auth", "key");
    hb_obj_set_str(h3, "key_id", kid);
    snprintf(h3id, sizeof h3id, "%s", hb_obj_id(h3));

    CHECK_EQ_INT(hb_vault_put(v, k), HB_OK);
    CHECK_EQ_INT(hb_vault_put(v, h1), HB_OK);
    CHECK_EQ_INT(hb_vault_put(v, h2), HB_OK);
    CHECK_EQ_INT(hb_vault_put(v, h3), HB_OK);

    CHECK_EQ_INT(hb_vault_key_ref_count(v, kid), 3);
    CHECK_EQ_INT(hb_vault_delete_key_cascade(v, kid, &refs, &nrefs), HB_OK);
    CHECK_EQ_INT(nrefs, 3);
    CHECK_EQ_INT(hb_vault_key_ref_count(v, kid), 0);

    CHECK_EQ_INT(hb_vault_get(v, kid, &g), HB_OK);
    CHECK(hb_obj_tombstone(g));
    hb_obj_free(g);

    CHECK_EQ_INT(hb_vault_get(v, hb_obj_id(h1), &g), HB_OK);
    CHECK_EQ_STR(hb_obj_get_str(g, "auth"), "auto");   /* 回落 auto，不是 none */
    CHECK(!hb_obj_has(g, "key_id"));
    hb_obj_free(g);

    /* 级联后撤销前删掉 h3 → undo 必须跳过它 */
    CHECK_EQ_INT(hb_vault_delete(v, h3id), HB_OK);
    CHECK_EQ_INT(hb_vault_undo_key_cascade(v, kid, refs, nrefs), HB_OK);
    hb_key_refs_free(refs, nrefs);

    CHECK_EQ_INT(hb_vault_get(v, kid, &g), HB_OK);
    CHECK(!hb_obj_tombstone(g));
    hb_obj_free(g);

    CHECK_EQ_INT(hb_vault_get(v, hb_obj_id(h1), &g), HB_OK);
    CHECK_EQ_STR(hb_obj_get_str(g, "auth"), "key");    /* prev_auth 恢复 */
    CHECK_EQ_STR(hb_obj_get_str(g, "key_id"), kid);
    hb_obj_free(g);

    CHECK_EQ_INT(hb_vault_get(v, hb_obj_id(h2), &g), HB_OK);
    CHECK_EQ_STR(hb_obj_get_str(g, "auth"), "auto");
    CHECK_EQ_STR(hb_obj_get_str(g, "key_id"), kid);
    hb_obj_free(g);

    CHECK_EQ_INT(hb_vault_get(v, h3id, &g), HB_OK);
    CHECK(hb_obj_tombstone(g));                       /* 不复活已删主机 */
    CHECK(!hb_obj_has(g, "key_id"));
    hb_obj_free(g);

    hb_obj_free(k);
    hb_obj_free(h1);
    hb_obj_free(h2);
    hb_obj_free(h3);
    hb_vault_close(v);
}

static void test_export_import(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *o = mk_host("exp", 1);
    char *js = NULL, *js2 = NULL;
    size_t imported = 0;
    cJSON *arr, *it;
    int count = 0;

    hb_obj_set_str(o, "address", "10.0.0.1");
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(hb_vault_set_secret(v, hb_obj_id(o), "password", "hideme"), HB_OK);

    CHECK_EQ_INT(hb_vault_export_json(v, "host", &js), HB_OK);
    arr = cJSON_Parse(js);
    CHECK(cJSON_IsArray(arr));
    CHECK_EQ_INT(cJSON_GetArraySize(arr), 1);
    it = cJSON_GetArrayItem(arr, 0);
    CHECK(cJSON_GetObjectItemCaseSensitive(it, "id") != NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(it, "_fv") == NULL);
    CHECK(strstr(js, "hideme") == NULL);
    CHECK_EQ_STR(cJSON_GetObjectItemCaseSensitive(it, "name")->valuestring, "exp");

    /* 导入：新 id */
    cJSON_Delete(arr);
    CHECK_EQ_INT(hb_vault_import_json(v, "host", js, &imported), HB_OK);
    CHECK_EQ_INT(imported, 1);
    CHECK_EQ_INT(hb_vault_export_json(v, "host", &js2), HB_OK);
    {
        cJSON *a2 = cJSON_Parse(js2);
        count = cJSON_GetArraySize(a2);
        cJSON_Delete(a2);
    }
    CHECK_EQ_INT(count, 2);

    /* 导入畸形 json */
    CHECK_EQ_INT(hb_vault_import_json(v, "host", "not json", NULL), HB_ERR_INVALID);
    CHECK_EQ_INT(hb_vault_import_json(v, "host", "{\"a\":1}", NULL), HB_ERR_INVALID);
    CHECK_EQ_INT(hb_vault_import_json(v, "host", "[1,\"x\",{\"name\":\"ok\"}]", &imported), HB_OK);
    CHECK_EQ_INT(imported, 1);

    free(js);
    free(js2);
    hb_obj_free(o);
    hb_vault_close(v);
}

static int log_count;
static bool count_cb(const hb_log *l, void *ud)
{
    (void)ud;
    (void)l;
    log_count++;
    return true;
}

static bool grab_cb(const hb_log *l, void *ud)
{
    hb_log *dst = ud;
    if (log_count++ == 0) {
        memcpy(dst, l, sizeof *dst);
    }
    return true;
}

static void test_logs(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_log l = {0};
    int64_t id1 = 0, id2 = 0;
    hb_log g = {0};
    char *csv = NULL, *js = NULL;

    l.user = "alice"; l.host = "web1"; l.proto = "ssh";
    l.result = "ok"; l.duration_s = -1; l.host_id = "h-1";
    CHECK_EQ_INT(hb_vault_log_add(v, &l, &id1), HB_OK);
    CHECK(id1 > 0);

    l.user = "bob"; l.host = "db,prod"; l.proto = "rdp";
    l.result = "auth_failed"; l.reason = "bad \"password\"";
    CHECK_EQ_INT(hb_vault_log_add(v, &l, &id2), HB_OK);

    CHECK_EQ_INT(hb_vault_log_finish(v, id1, "timeout", "no route", 42), HB_OK);
    CHECK_EQ_INT(hb_vault_log_set_saved(v, id1, true), HB_OK);

    log_count = 0;
    CHECK_EQ_INT(hb_vault_log_foreach(v, NULL, false, count_cb, NULL), HB_OK);
    CHECK_EQ_INT(log_count, 2);

    /* filter 子串 */
    log_count = 0;
    CHECK_EQ_INT(hb_vault_log_foreach(v, "alice", false, count_cb, NULL), HB_OK);
    CHECK_EQ_INT(log_count, 1);
    log_count = 0;
    CHECK_EQ_INT(hb_vault_log_foreach(v, "auth_failed", false, count_cb, NULL), HB_OK);
    CHECK_EQ_INT(log_count, 1);

    /* desc：后插的(id2) 在前 */
    log_count = 0;
    CHECK_EQ_INT(hb_vault_log_foreach(v, NULL, true, grab_cb, &g), HB_OK);
    CHECK_EQ_INT(g.id, id2);

    /* CSV */
    CHECK_EQ_INT(hb_vault_log_export_csv(v, &csv), HB_OK);
    CHECK((unsigned char)csv[0] == 0xef && (unsigned char)csv[1] == 0xbb &&
          (unsigned char)csv[2] == 0xbf);
    CHECK(strstr(csv, "Date,User,Host,Protocol,Result,Reason,Duration,Saved") != NULL);
    CHECK(strstr(csv, "\"db,prod\"") != NULL);        /* 逗号字段被引号包裹 */
    CHECK(strstr(csv, "\"bad \"\"password\"\"\"") != NULL); /* 引号双写 */
    CHECK(strstr(csv, "\"timeout\"") != NULL);

    /* JSON */
    CHECK_EQ_INT(hb_vault_log_export_json(v, &js), HB_OK);
    {
        cJSON *a = cJSON_Parse(js);
        CHECK(cJSON_IsArray(a));
        CHECK_EQ_INT(cJSON_GetArraySize(a), 2);
        cJSON_Delete(a);
    }

    /* purge：retention<=0 不清理；大 days 也不清理新日志 */
    CHECK_EQ_INT(hb_vault_log_purge(v, 0), HB_OK);
    CHECK_EQ_INT(hb_vault_log_purge(v, 90), HB_OK);
    log_count = 0;
    hb_vault_log_foreach(v, NULL, false, count_cb, NULL);
    CHECK_EQ_INT(log_count, 2);

    free(csv);
    free(js);
    hb_vault_close(v);
}

static void test_history(void)
{
    hb_vault *v = open_v(DEK_A);
    char **out = NULL;
    size_t n = 0;

    CHECK_EQ_INT(hb_vault_history_add(v, "h1", "  ls -la  "), HB_OK);
    CHECK_EQ_INT(hb_vault_history_add(v, "h1", "ls -la"), HB_OK);   /* 重复跳过 */
    CHECK_EQ_INT(hb_vault_history_add(v, "h1", "   "), HB_OK);      /* 空白跳过 */
    CHECK_EQ_INT(hb_vault_history_add(v, "h1", "uptime"), HB_OK);
    CHECK_EQ_INT(hb_vault_history_add(v, "h2", "ls -la"), HB_OK);   /* 别的 host 不判重 */
    CHECK_EQ_INT(hb_vault_history_add(v, "h1", "top"), HB_OK);

    CHECK_EQ_INT(hb_vault_history_search(v, "ls", 10, &out, &n), HB_OK);
    CHECK_EQ_INT(n, 1);                       /* distinct */
    CHECK_EQ_STR(out[0], "ls -la");
    hb_strfreev(out, n);

    CHECK_EQ_INT(hb_vault_history_search(v, "", 10, &out, &n), HB_OK);
    CHECK_EQ_INT(n, 3);                        /* 全部 3 条 distinct 命令 */
    CHECK_EQ_STR(out[0], "top");               /* 最近优先 */
    hb_strfreev(out, n);

    CHECK_EQ_INT(hb_vault_history_search(v, "zzz", 10, &out, &n), HB_OK);
    CHECK_EQ_INT(n, 0);
    hb_strfreev(out, n);

    hb_vault_close(v);
}

static void test_settings(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *s = NULL;

    CHECK_EQ_INT(hb_vault_settings(v, &s), HB_OK);
    CHECK_EQ_STR(hb_obj_get_str(s, "theme"), "harbor");
    CHECK_EQ_INT(hb_obj_get_int(s, "font_size", 0), 13);
    CHECK(hb_obj_get_double(s, "line_height", 0) > 1.4 &&
          hb_obj_get_double(s, "line_height", 0) < 1.6);
    CHECK_EQ_INT(hb_obj_get_bool(s, "autocomplete", true), false);
    CHECK_EQ_STR(hb_obj_get_str(s, "font_family"), "Monospace");
    CHECK_EQ_INT(hb_obj_get_int(s, "scrollback", 0), 10000);
    CHECK_EQ_INT(hb_obj_get_bool(s, "close_to_tray", false), true);
    CHECK_EQ_STR(hb_obj_get_str(s, "notify_body"), "full");
    CHECK_EQ_INT(hb_obj_get_bool(s, "watch_default_ssh", false), true);
    hb_obj_free(s);

    hb_vault_close(v);
}

static void test_migration_reopen(void)
{
    hb_vault *v = open_v(DEK_A);
    hb_obj *o = mk_host("persist", 1);
    hb_obj *g = NULL;
    char id[64];

    snprintf(id, sizeof id, "%s", hb_obj_id(o));
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    hb_obj_free(o);
    hb_vault_close(v);

    /* 重开：数据还在，迁移幂等 */
    v = open_v(DEK_A);
    CHECK_EQ_INT(hb_vault_get(v, id, &g), HB_OK);
    CHECK_EQ_STR(hb_obj_get_str(g, "name"), "persist");
    hb_obj_free(g);
    hb_vault_close(v);
}

struct cb_ctx { int calls; char last_type[64]; };
static void on_change(hb_vault *v, const char *type, const char *id, void *ud)
{
    struct cb_ctx *c = ud;
    (void)v;
    c->calls++;
    snprintf(c->last_type, sizeof c->last_type, "%s", type);
    (void)id;
}

static void test_change_cb(void)
{
    hb_vault *v = open_v(DEK_A);
    struct cb_ctx c = {0, {0}};
    hb_obj *o = mk_host("cb", 1);

    hb_vault_set_change_cb(v, on_change, &c);
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(c.calls, 1);
    CHECK_EQ_STR(c.last_type, "host");

    /* no-op put 不触发 */
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(c.calls, 1);

    CHECK_EQ_INT(hb_vault_delete(v, hb_obj_id(o)), HB_OK);
    CHECK_EQ_INT(c.calls, 2);

    hb_obj_free(o);
    hb_vault_close(v);
}

static void test_strv_and_copy(void)
{
    hb_obj *o = mk_host("arr", 1);
    const char *jump[] = {"j1", "j2", "j3"};
    const char **got = NULL;
    hb_obj *c;
    size_t n;

    hb_obj_set_strv(o, "jump", jump, 3);
    n = hb_obj_get_strv(o, "jump", &got);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_STR(got[1], "j2");
    free((void *)got);

    c = hb_obj_copy(o);
    CHECK_EQ_STR(hb_obj_id(c), hb_obj_id(o));
    CHECK_EQ_STR(hb_obj_get_str(c, "name"), "arr");
    hb_obj_free(c);

    c = hb_obj_clone_new(o);
    CHECK(strcmp(hb_obj_id(c), hb_obj_id(o)) != 0);
    CHECK_EQ_STR(hb_obj_get_str(c, "name"), "arr");
    hb_obj_free(c);

    hb_obj_free(o);
}

int main(void)
{
    mk_tmp();
    test_open_device_id();      rm_tmp();
    mk_tmp();
    test_put_get_list();        rm_tmp();
    mk_tmp();
    test_fv_and_noop();         rm_tmp();
    mk_tmp();
    test_delete_undelete();     rm_tmp();
    mk_tmp();
    test_secrets();             rm_tmp();
    mk_tmp();
    test_key_cascade();         rm_tmp();
    mk_tmp();
    test_export_import();       rm_tmp();
    mk_tmp();
    test_logs();                rm_tmp();
    mk_tmp();
    test_history();             rm_tmp();
    mk_tmp();
    test_settings();            rm_tmp();
    mk_tmp();
    test_migration_reopen();    rm_tmp();
    mk_tmp();
    test_change_cb();           rm_tmp();
    mk_tmp();
    test_strv_and_copy();       rm_tmp();

    fprintf(stderr, "%d checks, %d failures\n", hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
