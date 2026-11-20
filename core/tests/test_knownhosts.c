/* test_knownhosts.c — known_hosts 导入/导出（FR-09）。
 * 生成 500 行混合文件（plain / [h]:p / 多名 / 哈希 / 注释 / @revoked /
 * 通配），校验精确计数、重导不重复、冲突告警、导出可回导。
 * 哈希条目用 ssh-keygen -H 生成；缺 ssh-keygen → SKIP(77)。 */

#include "test.h"

#include "harbor/harbor_core.h"
#include "harbor/knownhosts.h"

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[256];
static char dbpath[300];

static void mk_tmp(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/hbkh-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(dbpath, sizeof dbpath, "%s/vault.db", tmpdir);
}

static hb_vault *open_v(void)
{
    static const uint8_t DEK[32] = {0};
    hb_vault *v = NULL;
    if (hb_vault_open(dbpath, DEK, &v) != HB_OK) {
        fprintf(stderr, "vault open failed\n");
        exit(2);
    }
    return v;
}

/* 伪 base64 公钥：seed 决定内容，返回 malloc 串 */
static char *fake_key(int seed)
{
    unsigned char bin[64];
    char *b64;
    size_t i;
    for (i = 0; i < sizeof bin; i++)
        bin[i] = (unsigned char)(seed * 31 + (int)i * 7);
    b64 = malloc(sodium_base64_ENCODED_LEN(sizeof bin,
                                         sodium_base64_VARIANT_ORIGINAL));
    sodium_bin2base64(b64, sodium_base64_ENCODED_LEN(sizeof bin,
                          sodium_base64_VARIANT_ORIGINAL),
                      bin, sizeof bin, sodium_base64_VARIANT_ORIGINAL);
    return b64;
}

static size_t count_known(hb_vault *v)
{
    hb_obj **o = NULL;
    size_t n = 0;
    if (hb_vault_list(v, "known", &o, &n) != HB_OK)
        return (size_t)-1;
    hb_obj_list_free(o, n);
    return n;
}

/* 期望计数：
 *   0..339   普通 host%04d                  → created 340
 *   340..399 [h]:port                     → created 60（keytype 变体按 keytype 分）
 *   400..449 multi "a%04d,b%04d"          → created 90
 *   450..459 同 keytype 同 host 同 key 重复（重复 340-359 行的 host）→ skipped 10
 *   460..469 同 host/keytype 不同 key      → skipped 10 + 冲突警告
 *   470..479 字段不全/bad64              → 警告 10
 *   480..489 通配/取反                   → 警告 10
 *   490..494 @revoked/@cert-authority    → 警告 5
 *   495..499 注释/空行                    → skip silently
 */
static char *build_file(const char *extra_hashed)
{
    size_t cap = 512 * 1024, len = 0;
    char *buf = malloc(cap), *k1 = fake_key(1), *k2 = fake_key(2);
    int i;
    for (i = 0; i < 340; i++)
        len += snprintf(buf + len, cap - len,
                        "plain%04d.example.com ssh-ed25519 %s\n", i, k1);
    for (i = 0; i < 60; i++)
        len += snprintf(buf + len, cap - len,
                        "[p%03d.example.com]:%d %s %s\n", i, 2200 + i,
                        i % 2 ? "ssh-rsa" : "ssh-ed25519", k1);
    for (i = 0; i < 45; i++)
        len += snprintf(buf + len, cap - len,
                        "a%04d.internal,b%04d.internal ssh-ed25519 %s"
                        " some comment here\n", i, i, k1);
    for (i = 0; i < 10; i++)                   /* 完全重复 → skipped */
        len += snprintf(buf + len, cap - len,
                        "plain%04d.example.com ssh-ed25519 %s\n", i, k1);
    for (i = 0; i < 10; i++)                   /* 同 host+type 不同 key → 冲突 */
        len += snprintf(buf + len, cap - len,
                        "plain%04d.example.com ssh-ed25519 %s\n", i + 20, k2);
    for (i = 0; i < 5; i++)
        len += snprintf(buf + len, cap - len,
                        "bad%02d.example.com ssh-ed25519 !!!not-base64!!!\n", i);
    for (i = 0; i < 5; i++)
        len += snprintf(buf + len, cap - len,
                        "short%02d.example.com\n", i);   /* 字段不足 */
    for (i = 0; i < 5; i++)
        len += snprintf(buf + len, cap - len,
                        "*.wild%d.example.com ssh-ed25519 %s\n", i, k1);
    for (i = 0; i < 5; i++)
        len += snprintf(buf + len, cap - len,
                        "!neg%d.example.com,ok%02d.example.com"
                        " ssh-ed25519 %s\n", i, i, k1);   /* neg 警告，ok 正常导入 */
    len += snprintf(buf + len, cap - len,
                    "@revoked dead.example.com ssh-rsa %s\n"
                    "@cert-authority ca.example.com ssh-rsa %s\n"
                    "@revoked d2.example.com ssh-rsa %s\n"
                    "@cert-authority ca2.example.com ssh-rsa %s\n"
                    "@revoked d3.example.com ssh-rsa %s\n",
                    k1, k1, k1, k1, k1);
    len += snprintf(buf + len, cap - len,
                    "# comment line\n\n   \n# another\n\t\n");
    if (extra_hashed)
        len += snprintf(buf + len, cap - len, "%s", extra_hashed);
    free(k1); free(k2);
    return buf;
}

static void test_import(void)
{
    hb_vault *v = open_v();
    hb_import_result r;
    char *text, *hashsrc = NULL;
    char path[320];
    FILE *f;
    char line[512];
    char hashed[3][512];
    int nh = 0;

    /* 造哈希条目：写 plain 文件 → ssh-keygen -H */
    snprintf(path, sizeof path, "%s/kh_plain", tmpdir);
    f = fopen(path, "w");
    {
        char *k3 = fake_key(3), *k4 = fake_key(4);
        fprintf(f, "hash1.example.com ssh-ed25519 %s\n"
                   "[hash2.example.com]:2222 ssh-ed25519 %s\n", k3, k4);
        free(k3); free(k4);
    }
    fclose(f);
    {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "ssh-keygen -H -f '%s' >/dev/null 2>&1", path);
        if (system(cmd) != 0) {
            fprintf(stderr, "SKIP: ssh-keygen -H failed\n");
            exit(77);
        }
    }
    f = fopen(path, "r");
    while (f && nh < 3 && fgets(line, sizeof line, f)) {
        if (line[0] == '|')
            strcpy(hashed[nh++], line);
    }
    if (f) fclose(f);
    CHECK(nh >= 1);
    hashsrc = nh ? hashed[0] : NULL;
    if (nh > 1) {                              /* 拼全部哈希行 */
        static char all[1600];
        all[0] = 0;
        for (int i = 0; i < nh; i++)
            strcat(all, hashed[i]);
        hashsrc = all;
    }

    memset(&r, 0, sizeof r);
    text = build_file(hashsrc);
    CHECK_EQ_INT(hb_known_import_text(v, text, &r), HB_OK);

    /* 导入数：340 + 60 + 90 + 5(ok from neg lines) + 哈希行数 */
    CHECK_EQ_INT(r.created, 340 + 60 + 90 + 5 + nh);
    /* skipped：10 完全重复 + 10 冲突 */
    CHECK_EQ_INT(r.skipped, 20);
    /* 警告：10 冲突 + 10 bad/short + 10 通配/取反 + 5 marker */
    CHECK(r.nwarn >= 30);
    CHECK_EQ_INT(r.ncreated, r.created);
    CHECK_EQ_INT(count_known(v), r.created);

    /* 哈希对象字段 */
    {
        hb_obj **o = NULL;
        size_t n = 0, i;
        int found = 0;
        hb_vault_list(v, "known", &o, &n);
        for (i = 0; i < n; i++) {
            const char *h = hb_obj_get_str(o[i], "host");
            if (h && h[0] == '|') {
                const char *ln = hb_obj_get_str(o[i], "line");
                found++;
                CHECK_EQ_INT(hb_obj_get_int(o[i], "port", -1), 0);
                CHECK(ln && ln[0] == '|' && strstr(ln, "ssh-ed25519"));
                CHECK(hb_obj_get_str(o[i], "fingerprint") != NULL);
            }
        }
        CHECK_EQ_INT(found, nh);
        hb_obj_list_free(o, n);
    }

    /* 重导：全部 dedup */
    {
        hb_import_result r2;
        memset(&r2, 0, sizeof r2);
        CHECK_EQ_INT(hb_known_import_text(v, text, &r2), HB_OK);
        CHECK_EQ_INT(r2.created, 0);
        CHECK_EQ_INT(r2.skipped, 340 + 60 + 90 + 5 + nh + 20);
        CHECK_EQ_INT(count_known(v), r.created);
        hb_import_result_free(&r2);
    }
    hb_import_result_free(&r);
    free(text);

    /* 导出 → 新 vault 回导 = 同集合 */
    {
        char *exp = NULL;
        char db2[300];
        hb_vault *v2;
        hb_import_result r3;
        snprintf(db2, sizeof db2, "%s/v2.db", tmpdir);
        {
            static const uint8_t D2[32] = {7};
            hb_err e = hb_vault_open(db2, D2, &v2);
            CHECK_EQ_INT(e, HB_OK);
        }
        CHECK_EQ_INT(hb_known_export_text(v, &exp), HB_OK);
        CHECK(exp && exp[0]);
        memset(&r3, 0, sizeof r3);
        CHECK_EQ_INT(hb_known_import_text(v2, exp, &r3), HB_OK);
        CHECK_EQ_INT(r3.created, count_known(v));
        hb_import_result_free(&r3);
        free(exp);
        hb_vault_close(v2);
    }
    hb_vault_close(v);
}

static void test_import_file_guard(void)
{
    hb_vault *v = open_v();
    hb_import_result r;
    char path[320];
    memset(&r, 0, sizeof r);
    CHECK_EQ_INT(hb_known_import_file(v, "/nonexistent/kh", &r),
                 HB_ERR_IO);
    snprintf(path, sizeof path, "%s", "/tmp");
    CHECK_EQ_INT(hb_known_import_file(v, path, &r), HB_ERR_INVALID);
    hb_import_result_free(&r);
    hb_vault_close(v);
}

int main(void)
{
    if (access("/usr/bin/ssh-keygen", X_OK) != 0 &&
        access("/bin/ssh-keygen", X_OK) != 0) {
        fprintf(stderr, "SKIP: ssh-keygen missing\n");
        return 77;
    }
    mk_tmp();
    hb_core_init();
    test_import();
    test_import_file_guard();
    fprintf(stderr, "test_knownhosts: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
