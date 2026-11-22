/* test_backup.c — 加密备份导入/导出单元测试（FR-15，HBBK v1）。 */

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harbor/harbor_core.h"
#include "harbor/backup.h"

static char tmpdir[256];
static char dbpath[300];
static const uint8_t DEK[32] = {0};

static void mk_tmp(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/hbbk-XXXXXX");
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); exit(2); }
    snprintf(dbpath, sizeof dbpath, "%s/vault.db", tmpdir);
}

static void rm_tmp(void)
{
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd)) fprintf(stderr, "cleanup failed\n");
}

static hb_vault *open_v(void)
{
    hb_vault *v = NULL;
    if (hb_vault_open(dbpath, DEK, &v) != HB_OK) {
        fprintf(stderr, "vault_open failed\n");
        exit(2);
    }
    return v;
}

static hb_obj *mk_host(hb_vault *v, const char *name)
{
    hb_obj *o = hb_obj_new("host");
    hb_obj_set_str(o, "name", name);
    hb_obj_set_str(o, "hostname", "10.0.0.1");
    hb_obj_set_int(o, "port", 22);
    hb_obj_set_str(o, "identity_file", "/home/x/.ssh/id_dev"); /* 设备本地字段 */
    hb_vault_put(v, o);
    return o;
}

static int bytes_grep(const uint8_t *buf, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i;
    for (i = 0; i + nl <= n; i++)
        if (!memcmp(buf + i, needle, nl)) return 1;
    return 0;
}

int main(void)
{
    hb_vault *v, *v2;
    hb_obj *h, *g;
    uint8_t *file = NULL;
    size_t flen = 0;
    hb_backup_stat st;
    hb_err e;

    mk_tmp();
    v = open_v();
    h = mk_host(v, "Prod-Web");
    hb_vault_set_secret(v, hb_obj_id(h), "password", "s3cret-pw");
    hb_obj_free(h);

    /* ---- 导出 ---- */
    CHECK_EQ_INT(hb_backup_export(v, "bkpw", &file, &flen), HB_OK);
    CHECK(file && flen > 60);
    CHECK(!memcmp(file, "HBBK", 4));

    /* 明文不得出现在文件字节里 */
    CHECK(!bytes_grep(file, flen, "Prod-Web"));
    CHECK(!bytes_grep(file, flen, "s3cret-pw"));
    CHECK(!bytes_grep(file, flen, "hostname"));

    /* ---- 口令错误：preview 与 import 都失败且无写入 ---- */
    {
        hb_obj **l = NULL;
        size_t n = 9;
        CHECK(hb_backup_preview(file, flen, "wrong", &st) != HB_OK);
        mk_tmp();  /* 新库 */
        v2 = open_v();
        CHECK(hb_backup_import(v2, file, flen, "wrong", &st) != HB_OK);
        CHECK_EQ_INT(hb_vault_list(v2, "host", &l, &n), HB_OK);
        CHECK_EQ_INT((int)n, 0);
        hb_obj_list_free(l, n);
        hb_vault_close(v2);
        rm_tmp();
        mk_tmp();
    }

    /* ---- 头部篡改：magic/版本/kdf/ops/mem/salt 改动 → 解密失败 ---- */
    {
        static const int offs[] = {0, 4, 5, 20, 35, 60};
        size_t i;
        for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
            uint8_t *bad = malloc(flen);
            memcpy(bad, file, flen);
            bad[offs[i]] ^= 0x5A;
            CHECK(hb_backup_preview(bad, flen, "bkpw", &st) != HB_OK);
            free(bad);
        }
    }

    /* ---- KDF 参数越界（ops=1, mem=32MiB, mem=2GiB） ---- */
    {
        uint8_t *bad = malloc(flen);
        memcpy(bad, file, flen);
        bad[6] = 0; bad[7] = 0; bad[8] = 0; bad[9] = 1;   /* ops = 1 */
        CHECK(hb_backup_preview(bad, flen, "bkpw", &st) != HB_OK);
        memcpy(bad, file, flen);
        bad[10] = 0; bad[11] = 0; bad[12] = 0x80; bad[13] = 0; /* mem 32MiB */
        CHECK(hb_backup_preview(bad, flen, "bkpw", &st) != HB_OK);
        memcpy(bad, file, flen);
        bad[10] = 0; bad[11] = 0x20; bad[12] = 0; bad[13] = 0; /* mem 2GiB */
        CHECK(hb_backup_preview(bad, flen, "bkpw", &st) != HB_OK);
        free(bad);
    }

    /* ---- preview 计数 ---- */
    CHECK_EQ_INT(hb_backup_preview(file, flen, "bkpw", &st), HB_OK);
    CHECK(st.total >= 2);            /* host + settings 单例 */

    /* ---- 导入到新库：round-trip ---- */
    {
        const char *hid;
        h = NULL;
        /* 源库里记住 host id */
        {
            hb_obj **l = NULL;
            size_t n = 0;
            hb_vault_list(v, "host", &l, &n);
            CHECK_EQ_INT((int)n, 1);
            hid = hb_obj_id(l[0]);
            {
                char idbuf[64];
                snprintf(idbuf, sizeof idbuf, "%s", hid);
                hb_obj_list_free(l, n);
                /* 新库导入 */
                {
                    hb_backup_stat st2;
                    char *sec;
                    hb_obj *imp = NULL, *so = NULL;
                    CHECK_EQ_INT(
                        hb_backup_import(v, file, flen, "bkpw", &st2),
                        HB_OK);   /* 同库覆盖 */
                    CHECK(st2.overwritten >= 1);
                    /* 全新 vault */
                    hb_vault_close(v);
                    rm_tmp();
                    mk_tmp();
                    v = open_v();
                    memset(&st2, 0, sizeof st2);
                    CHECK_EQ_INT(
                        hb_backup_import(v, file, flen, "bkpw", &st2),
                        HB_OK);
                    CHECK(st2.created >= 2);
                    CHECK_EQ_INT(st2.overwritten, 0);
                    CHECK_EQ_INT(hb_vault_get(v, idbuf, &imp), HB_OK);
                    CHECK_EQ_STR(hb_obj_get_str(imp, "name"), "Prod-Web");
                    CHECK_EQ_STR(hb_obj_get_str(imp, "identity_file"),
                                 "/home/x/.ssh/id_dev");   /* 设备本地字段也回来了 */
                    CHECK_EQ_INT(hb_obj_get_int(imp, "port", 0), 22);
                    sec = hb_vault_get_secret(v, idbuf, "password");
                    CHECK_EQ_STR(sec, "s3cret-pw");
                    hb_secret_free(sec);
                    hb_obj_free(imp);
                    /* settings 字段覆盖合并 */
                    CHECK_EQ_INT(hb_vault_settings(v, &so), HB_OK);
                    CHECK_EQ_STR(hb_obj_get_str(so, "theme"), "harbor");
                    hb_obj_free(so);
                }
                (void)hid;
            }
        }
    }

    /* ---- tombstoned id 恢复 ---- */
    {
        hb_obj **l = NULL;
        size_t n = 0;
        char idbuf[64];
        hb_vault_list(v, "host", &l, &n);
        snprintf(idbuf, sizeof idbuf, "%s", hb_obj_id(l[0]));
        hb_obj_list_free(l, n);
        CHECK_EQ_INT(hb_vault_delete(v, idbuf), HB_OK);
        {
            hb_backup_stat st2;
            hb_obj *imp = NULL;
            CHECK_EQ_INT(hb_backup_import(v, file, flen, "bkpw", &st2),
                         HB_OK);
            CHECK_EQ_INT(hb_vault_get(v, idbuf, &imp), HB_OK);
            CHECK(!hb_obj_tombstone(imp));
            hb_obj_free(imp);
        }
    }

    hb_vault_close(v);
    free(file);
    rm_tmp();
    (void)g; (void)e; (void)h;
    return hb_t_failures ? 1 : 0;
}
