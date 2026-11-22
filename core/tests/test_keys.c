/* 私钥生成/检查/落盘加密测试。
 * 生成结果用系统 ssh-keygen/openssl 交叉验证；缺工具 → exit(77) SKIP。 */

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harbor/harbor_core.h"

static char tmpdir[4096];
static char vdir[4096 + 8];

static void mk_tmp(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/hbkeys-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(vdir, sizeof vdir, "%s/v", tmpdir);
}
static void rm_tmp(void)
{
    char cmd[4400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed\n");
}

/* popen 收集 stdout（去掉尾换行）；失败返回 NULL。 */
static char *run_out(const char *fmt, ...)
{
    char cmd[8192];
    va_list ap;
    FILE *f;
    char *buf;
    size_t cap = 65536, n = 0;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    f = popen(cmd, "r");
    if (!f)
        return NULL;
    buf = malloc(cap);
    while (buf && n + 2048 < cap &&
           fgets(buf + n, (int)(cap - n), f)) {
        n = strlen(buf);
    }
    pclose(f);
    if (!buf)
        return NULL;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    return buf;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(n + 1);
    if (b && fread(b, 1, n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b)
        b[n] = 0;
    return b;
}

static void put_file(const char *name, const char *text, mode_t mode)
{
    char path[4400];
    int fd;
    snprintf(path, sizeof path, "%s/%s", tmpdir, name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        exit(2);
    if (write(fd, text, strlen(text)) != (ssize_t)strlen(text))
        exit(2);
    close(fd);
}

/* 取第 field（1 起）个空白分隔字段 */
static void field_dup(const char *line, int field, char *out, size_t cap)
{
    const char *p = line;
    int i;
    size_t n;
    for (i = 1; i < field; i++) {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' &&
           n + 1 < cap)
        n++;
    memcpy(out, p, n);
    out[n] = 0;
}

/* 目录里所有文件做二进制子串搜索；找到返回 1。 */
static int dir_grep(const char *dir, const char *needle, size_t nlen)
{
    char cmd[4600];
    /* grep -r 按字节匹配，needle 是 ASCII/base64 切片 */
    char *esc = malloc(nlen * 2 + 1);
    size_t i, j = 0;
    for (i = 0; i < nlen; i++) {          /* ' 转义为 '\'' */
        if (needle[i] == '\'') {
            memcpy(esc + j, "'\\''", 4);
            j += 4;
        } else
            esc[j++] = needle[i];
    }
    esc[j] = 0;
    snprintf(cmd, sizeof cmd, "grep -rqF '%s' '%s'", esc, dir);
    free(esc);
    return system(cmd) == 0;
}

/* ------------------------------ 1. 生成 ------------------------------ */

static void gen_and_verify(hb_key_type t, int rsa_bits, int want_bits,
                           const char *comment)
{
    hb_key_info k = {0};
    hb_err e = hb_key_generate(t, rsa_bits, comment, &k);
    char path[4400], pub[4096], line[4096], f1[64], f2[256];
    char *out;

    CHECK_EQ_INT(e, HB_OK);
    if (e != HB_OK)
        return;
    CHECK(k.private_text != NULL);
    CHECK(k.public_line != NULL);
    CHECK(k.fingerprint != NULL);
    CHECK_EQ_INT(k.bits, want_bits);
    CHECK_EQ_INT(k.format, HB_KEYFMT_OPENSSH);
    CHECK(!k.encrypted);
    CHECK(strstr(k.private_text, "BEGIN OPENSSH PRIVATE KEY") != NULL);
    if (comment)
        CHECK(k.comment && !strcmp(k.comment, comment));

    /* 写 0600 文件，让 ssh-keygen 交叉验证 */
    snprintf(path, sizeof path, "%s/genkey", tmpdir);
    put_file("genkey", k.private_text, 0600);

    out = run_out("/usr/bin/ssh-keygen -y -f '%s'", path);
    CHECK(out != NULL);
    if (out) {
        /* public_line 前两字段(type b64) == ssh-keygen -y */
        field_dup(k.public_line, 1, f1, sizeof f1);
        field_dup(k.public_line, 2, f2, sizeof f2);
        snprintf(pub, sizeof pub, "%s %s", f1, f2);
        field_dup(out, 1, f1, sizeof f1);
        field_dup(out, 2, f2, sizeof f2);
        snprintf(line, sizeof line, "%s %s", f1, f2);
        CHECK_EQ_STR(line, pub);
    }
    free(out);

    out = run_out("/usr/bin/ssh-keygen -lf '%s' -E sha256", path);
    CHECK(out != NULL);
    if (out) {
        field_dup(out, 1, f1, sizeof f1);   /* bits */
        field_dup(out, 2, f2, sizeof f2);   /* fingerprint */
        CHECK_EQ_INT(atoi(f1), want_bits);
        CHECK_EQ_STR(f2, k.fingerprint);
    }
    free(out);
    hb_key_info_clear(&k);
}

static void test_generate(void)
{
    hb_key_info k = {0};

    gen_and_verify(HB_KEY_ED25519, 0, 256, "gen@harbor");
    gen_and_verify(HB_KEY_ECDSA_P256, 0, 256, NULL);
    gen_and_verify(HB_KEY_ECDSA_P384, 0, 384, "p384");
    gen_and_verify(HB_KEY_ECDSA_P521, 0, 521, NULL);
    gen_and_verify(HB_KEY_RSA, 3072, 3072, "rsa3072");
    gen_and_verify(HB_KEY_RSA, 4096, 4096, NULL);
    /* 2048 不收 */
    CHECK_EQ_INT(hb_key_generate(HB_KEY_RSA, 2048, NULL, &k),
                 HB_ERR_INVALID);
}

/* ------------------------------ 2. inspect 各格式 ------------------------------ */

/* public_line 的前两字段与给定 .pub 文件一致 */
static void check_pub_eq(const hb_key_info *k, const char *pubfile)
{
    char *pub = slurp(pubfile);
    char a1[64], a2[512], b1[64], b2[512];
    CHECK(pub != NULL);
    if (!pub)
        return;
    field_dup(k->public_line, 1, a1, sizeof a1);
    field_dup(k->public_line, 2, a2, sizeof a2);
    field_dup(pub, 1, b1, sizeof b1);
    field_dup(pub, 2, b2, sizeof b2);
    CHECK_EQ_STR(a1, b1);
    CHECK_EQ_STR(a2, b2);
    free(pub);
}

static void kgen(const char *fmt, ...)
{
    char cmd[4400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd) != 0)
        exit(2);
}

static hb_err inspect_ret(const char *name, const char *pw, hb_key_info *k)
{
    char path[4400];
    hb_err e;
    snprintf(path, sizeof path, "%s/%s", tmpdir, name);
    memset(k, 0, sizeof *k);
    e = hb_key_inspect_file(path, pw, k);
    if (e != HB_OK)
        memset(k, 0, sizeof *k);
    return e;
}

static void test_inspect_openssh(void)
{
    hb_key_info k;
    char *text;
    hb_err e;

    /* 明文 ed25519 */
    kgen("/usr/bin/ssh-keygen -t ed25519 -N '' -C 'c1@test' "
         "-f %s/k1 -q", tmpdir);
    {
        char pp[4400];
        snprintf(pp, sizeof pp, "%s/k1", tmpdir);
        text = slurp(pp);
    }
    CHECK(text != NULL);
    if (!text) return;
    e = hb_key_inspect((unsigned char *)text, strlen(text), NULL, &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK_EQ_INT(k.format, HB_KEYFMT_OPENSSH);
        CHECK(!k.encrypted);
        CHECK_EQ_STR(k.kind, "ed25519");
        CHECK_EQ_INT(k.bits, 256);
        CHECK(k.comment && !strcmp(k.comment, "c1@test"));
        {
            char pp[4400];
            snprintf(pp, sizeof pp, "%s/k1.pub", tmpdir);
            check_pub_eq(&k, pp);
        }
        CHECK(k.private_text && !strcmp(k.private_text, text));
        hb_key_info_clear(&k);
    }
    free(text);

    /* 加密 ed25519：公钥在明文头部，无口令也能给全 public 元数据 */
    kgen("/usr/bin/ssh-keygen -t ed25519 -N 'pw1' -f %s/k2 -q", tmpdir);
    e = inspect_ret("k2", NULL, &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK_EQ_INT(k.format, HB_KEYFMT_OPENSSH);
        CHECK(k.encrypted);
        CHECK_EQ_STR(k.kind, "ed25519");
        CHECK_EQ_INT(k.bits, 256);
        CHECK(k.public_line != NULL);
        CHECK(k.fingerprint != NULL);
        CHECK(k.comment == NULL);          /* comment 在密文段里 */
        {
            char pp[4400];
            snprintf(pp, sizeof pp, "%s/k2.pub", tmpdir);
            check_pub_eq(&k, pp);
        }
        hb_key_info_clear(&k);
    }
    /* 口令不校验：随便给也不该报错（header 解析即可） */
    e = inspect_ret("k2", "wrong", &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK(k.encrypted);
        hb_key_info_clear(&k);
    }
}

static void test_inspect_pem(void)
{
    hb_key_info k;
    hb_err e;

    /* 传统 PEM 明文 RSA */
    kgen("/usr/bin/ssh-keygen -t rsa -b 2048 -m PEM -N '' -f %s/p1 -q",
         tmpdir);
    e = inspect_ret("p1", NULL, &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK_EQ_INT(k.format, HB_KEYFMT_PEM);
        CHECK(!k.encrypted);
        CHECK_EQ_STR(k.kind, "rsa");
        CHECK_EQ_INT(k.bits, 2048);
        {
            char pp[4400];
            snprintf(pp, sizeof pp, "%s/p1.pub", tmpdir);
            check_pub_eq(&k, pp);
        }
        hb_key_info_clear(&k);
    }

    /* 加密 PEM：NULL 口令 → encrypted 元数据；错口令 → PASSPHRASE；对 → 全量 */
    kgen("/usr/bin/ssh-keygen -t rsa -b 2048 -m PEM -N 'pwpw12' -f %s/p2 -q",
         tmpdir);
    e = inspect_ret("p2", NULL, &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK(k.encrypted);
        CHECK_EQ_STR(k.kind, "rsa");       /* 头部可猜类型 */
        CHECK(k.public_line == NULL);
        CHECK(k.fingerprint == NULL);
        CHECK(k.private_text != NULL);
        hb_key_info_clear(&k);
    }
    e = inspect_ret("p2", "bad", &k);
    CHECK_EQ_INT(e, HB_ERR_PASSPHRASE);
    e = inspect_ret("p2", "pwpw12", &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK(k.encrypted);                /* 源文本仍然加密 */
        CHECK_EQ_STR(k.kind, "rsa");
        CHECK_EQ_INT(k.bits, 2048);
        {
            char pp[4400];
            snprintf(pp, sizeof pp, "%s/p2.pub", tmpdir);
            check_pub_eq(&k, pp);
        }
        hb_key_info_clear(&k);
    }

    /* PKCS8（ssh-keygen -m PKCS8） */
    kgen("/usr/bin/ssh-keygen -t ecdsa -b 256 -m PKCS8 -N '' -f %s/p3 -q",
         tmpdir);
    e = inspect_ret("p3", NULL, &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK_EQ_INT(k.format, HB_KEYFMT_PKCS8);
        CHECK(!k.encrypted);
        CHECK_EQ_STR(k.kind, "ecdsa");
        CHECK_EQ_INT(k.bits, 256);
        {
            char pp[4400];
            snprintf(pp, sizeof pp, "%s/p3.pub", tmpdir);
            check_pub_eq(&k, pp);
        }
        hb_key_info_clear(&k);
    }
}

static void test_inspect_p12(void)
{
    hb_key_info k;
    hb_err e;
    char *out;

    /* EC p12：openssl req + pkcs12 -export */
    kgen("/usr/bin/openssl req -x509 -newkey ec "
         "-pkeyopt ec_paramgen_curve:P-256 -keyout %s/ec.key "
         "-out %s/ec.crt -days 1 -nodes -subj /CN=t 2>/dev/null",
         tmpdir, tmpdir);
    /* AES-CBC PBE：LibreSSL 默认的 3DES PBE 需要 OpenSSL 3 的 legacy
     * provider 才能解 → 两处都显式指定（OpenSSL 3 与 LibreSSL 都接受）。 */
    kgen("/usr/bin/openssl pkcs12 -export -inkey %s/ec.key -in %s/ec.crt "
         "-out %s/ec.p12 -passout pass:pw12 "
         "-certpbe AES-256-CBC -keypbe AES-256-CBC 2>/dev/null",
         tmpdir, tmpdir, tmpdir);

    e = inspect_ret("ec.p12", "bad", &k);
    CHECK_EQ_INT(e, HB_ERR_PASSPHRASE);
    e = inspect_ret("ec.p12", "pw12", &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK_EQ_INT(k.format, HB_KEYFMT_PKCS12);
        CHECK(!k.encrypted);               /* 已转成 openssh-key-v1 */
        CHECK_EQ_STR(k.kind, "ecdsa");
        CHECK_EQ_INT(k.bits, 256);
        CHECK(strstr(k.private_text, "BEGIN OPENSSH PRIVATE KEY") != NULL);
        /* 转出的 private_text 能被 ssh-keygen 认，且公钥对得上原 key */
        put_file("ec_conv", k.private_text, 0600);
        out = run_out("/usr/bin/ssh-keygen -y -f '%s/ec_conv'", tmpdir);
        {
            char *orig = run_out("/usr/bin/ssh-keygen -y -f '%s/ec.key'",
                                 tmpdir);
            CHECK(out != NULL && orig != NULL);
            if (out && orig) {
                char a1[64], a2[512], b1[64], b2[512];
                field_dup(out, 1, a1, sizeof a1);
                field_dup(out, 2, a2, sizeof a2);
                field_dup(orig, 1, b1, sizeof b1);
                field_dup(orig, 2, b2, sizeof b2);
                CHECK_EQ_STR(a1, b1);
                CHECK_EQ_STR(a2, b2);
            }
            free(out);
            free(orig);
        }
        hb_key_info_clear(&k);
    }

    /* RSA p12 */
    kgen("/usr/bin/openssl req -x509 -newkey rsa:2048 -keyout %s/rsa.key "
         "-out %s/rsa.crt -days 1 -nodes -subj /CN=t 2>/dev/null",
         tmpdir, tmpdir);
    kgen("/usr/bin/openssl pkcs12 -export -inkey %s/rsa.key -in %s/rsa.crt "
         "-out %s/rsa.p12 -passout pass:pw12 "
         "-certpbe AES-256-CBC -keypbe AES-256-CBC 2>/dev/null",
         tmpdir, tmpdir, tmpdir);
    e = inspect_ret("rsa.p12", "pw12", &k);
    CHECK_EQ_INT(e, HB_OK);
    if (e == HB_OK) {
        CHECK_EQ_INT(k.format, HB_KEYFMT_PKCS12);
        CHECK_EQ_STR(k.kind, "rsa");
        CHECK_EQ_INT(k.bits, 2048);
        hb_key_info_clear(&k);
    }
}

static void test_inspect_invalid(void)
{
    hb_key_info k;
    hb_err e;

    /* 垃圾 */
    put_file("garbage", "not a key at all, just text\n", 0600);
    e = inspect_ret("garbage", NULL, &k);
    CHECK_EQ_INT(e, HB_ERR_INVALID);

    /* DSA PEM：不支持的类型。macOS 的 OpenSSH ≥10 删了 ssh-keygen -t dsa，
     * 用 openssl 生成（LibreSSL/OpenSSL 3 都有 dsaparam/gendsa，产出直接
     * 是 "BEGIN DSA PRIVATE KEY" 传统 PEM）。 */
    kgen("/usr/bin/openssl dsaparam -out %s/dsap.pem 1024 2>/dev/null",
         tmpdir);
    kgen("/usr/bin/openssl gendsa -out %s/d1 %s/dsap.pem 2>/dev/null",
         tmpdir, tmpdir);
    e = inspect_ret("d1", NULL, &k);
    CHECK_EQ_INT(e, HB_ERR_INVALID);
}

/* ------------------------------ 3. 落盘无明文（FR-02 验收 3） ------------------------------ */

static void test_forensic(void)
{
    const uint8_t dek[32] = {7};
    hb_vault *v;
    hb_obj *o;
    hb_key_info k = {0};
    char dbpath[4400], slice[48];
    const char *b64, *p;
    char *sec;
    size_t i, j;

    CHECK_EQ_INT(hb_key_generate(HB_KEY_ED25519, 0, "forensic", &k), HB_OK);
    snprintf(dbpath, sizeof dbpath, "%s/vault.db", vdir);

    CHECK_EQ_INT(hb_vault_open(dbpath, dek, &v), HB_OK);
    o = hb_obj_new("key");
    hb_obj_set_str(o, "name", "fk");
    CHECK_EQ_INT(hb_vault_put(v, o), HB_OK);
    CHECK_EQ_INT(hb_vault_set_secret(v, hb_obj_id(o), "private",
                                   k.private_text), HB_OK);
    /* 先验证能读回 */
    sec = hb_vault_get_secret(v, hb_obj_id(o), "private");
    CHECK(sec && !strcmp(sec, k.private_text));
    hb_secret_free(sec);
    hb_obj_free(o);
    hb_vault_close(v);

    /* 扫描 vault 目录所有文件：BEGIN 行与一段 40 字符 base64 都不得出现 */
    CHECK(dir_grep(vdir, "PRIVATE KEY", 11) == 0);
    /* 取 base64 体中部一段（跳过 BEGIN 行与换行） */
    p = strstr(k.private_text, "-----\n");
    CHECK(p != NULL);
    if (p) {
        b64 = p + 6;
        /* 跳过前 60 个 base64 字符（避开可能重复的头部），再取 40 */
        for (i = 0, j = 0; b64[i] && j < 60; i++)
            if (b64[i] != '\n')
                j++;
        memcpy(slice, b64 + i, 40);
        slice[40] = 0;
        CHECK(dir_grep(vdir, slice, 40) == 0);
    }
    hb_key_info_clear(&k);
}

int main(void)
{
    if (access("/usr/bin/ssh-keygen", X_OK) != 0 ||
        access("/usr/bin/openssl", X_OK) != 0) {
        fprintf(stderr, "SKIP: ssh-keygen/openssl missing\n");
        exit(77);
    }
    /* LibreSSL 按 umask 写私钥（OpenSSL 恒 0600）→ 收紧保证
     * ssh-keygen -y 不因权限过宽拒读生成的 key */
    umask(0077);
    mk_tmp();
    test_generate();
    test_inspect_openssh();
    test_inspect_pem();
    test_inspect_p12();
    test_inspect_invalid();
    test_forensic();
    rm_tmp();
    fprintf(stderr, "test_keys: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
