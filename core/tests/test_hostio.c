/* test_hostio.c — ssh_config 解析 + 主机导入/导出测试。 */

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>

#include "harbor/harbor_core.h"

static char tmpdir[256];
static char dbpath[300];

static const uint8_t DEK[32] = {0};

static void mk_tmp(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/hbtest-io-XXXXXX");
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); exit(2); }
    snprintf(dbpath, sizeof dbpath, "%s/vault.db", tmpdir);
}

static void rm_tmp(void)
{
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
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

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(text, f);
    fclose(f);
}

static hb_obj *find_host(hb_vault *v, const char *name)
{
    hb_obj **hs = NULL;
    size_t n = 0, i;
    hb_obj *hit = NULL;
    hb_vault_list(v, "host", &hs, &n);
    for (i = 0; i < n; i++)
        if (!strcmp(hb_obj_get_str(hs[i], "name") ?: "", name)) { hit = hs[i]; break; }
    if (hit) {
        /* 把它挪到最后释放之外 —— 直接复制 */
        hb_obj *copy = hb_obj_copy(hit);
        hb_obj_list_free(hs, n);
        return copy;
    }
    hb_obj_list_free(hs, n);
    return NULL;
}

static size_t n_hosts(hb_vault *v)
{
    hb_obj **hs = NULL;
    size_t n = 0;
    hb_vault_list(v, "host", &hs, &n);
    hb_obj_list_free(hs, n);
    return n;
}

static const char *luser(void)
{
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_name ? pw->pw_name : "user";
}

/* ------------------------------ 1. 基础解析 ------------------------------ */

static void test_parse_basic(void)
{
    hb_sshcfg c;
    hb_err e = hb_sshcfg_parse_string(
        "# 注释行\r\n"
        "\r\n"
        "Host web\r\n"
        "  HostName=web.example.com\r\n"
        "  User = \"deploy\"\r\n"
        "  PORT 2222\r\n"
        "\r\n"
        "Host db app*\r\n"
        "  IdentityFile ~/keys/id_prod\n",
        "/nonexistent", &c);
    CHECK_EQ_INT(e, HB_OK);
    /* web, db 两个具体别名（app* 含通配符不产生别名） */
    CHECK_EQ_INT(c.n, 2);
    CHECK_EQ_STR(c.hosts[0].alias, "web");
    CHECK_EQ_STR(c.hosts[0].hostname, "web.example.com");
    CHECK_EQ_STR(c.hosts[0].user, "deploy");
    CHECK_EQ_INT(c.hosts[0].port, 2222);
    CHECK_EQ_STR(c.hosts[1].alias, "db");
    CHECK_EQ_STR(c.hosts[1].hostname, "db");   /* 缺省 = alias */
    CHECK(c.hosts[1].identity_file &&
          strstr(c.hosts[1].identity_file, "/keys/id_prod"));
    hb_sshcfg_free(&c);
}

/* ---------------- 2. first-wins / 通配 / 取反 / Host * ---------------- */

static void test_parse_matching(void)
{
    hb_sshcfg c;
    hb_err e = hb_sshcfg_parse_string(
        "Host web\n"
        "  User first\n"
        "Host *\n"
        "  User fallback\n"
        "  Port 2200\n"
        "Host * !bastion\n"
        "  ServerAliveInterval 60\n"
        "Host *\n"
        "  User later\n",            /* 不生效：first-wins */
        NULL, &c);
    size_t i;
    const hb_sshcfg_host *web = NULL, *bastion = NULL;
    CHECK_EQ_INT(e, HB_OK);
    /* 别名：web, bastion（Host * !bastion 的 !bastion 不算别名；
     * "Host *" 也没有具体别名）—— 等等 bastion 只是取反模式，不产生别名。 */
    for (i = 0; i < c.n; i++) {
        if (!strcmp(c.hosts[i].alias, "web")) web = &c.hosts[i];
        if (!strcmp(c.hosts[i].alias, "bastion")) bastion = &c.hosts[i];
    }
    CHECK(web != NULL);
    CHECK_EQ_INT(c.n, 1);            /* 只有 web 一个具体别名 */
    CHECK_EQ_STR(web->user, "first");/* first-wins，不是 fallback/later */
    CHECK_EQ_INT(web->port, 2200);   /* Host * 兜底 */
    CHECK_EQ_INT(web->keepalive, 60);
    CHECK(bastion == NULL);
    hb_sshcfg_free(&c);

    /* 取反命中：bastion 自己声明时不适用 * !bastion 块 */
    e = hb_sshcfg_parse_string(
        "Host bastion\n"
        "Host * !bastion\n"
        "  User negated\n"
        "Host *\n"
        "  ServerAliveInterval 30\n",
        NULL, &c);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(c.n, 1);
    CHECK(c.hosts[0].user == NULL);  /* !bastion 块不适用，无其它 User */
    CHECK_EQ_INT(c.hosts[0].keepalive, 30);
    hb_sshcfg_free(&c);
}

/* ------------------------------ 3. Include ------------------------------ */

static void test_include(void)
{
    char dir[300], cfg[300], sub[300], file[300];
    hb_sshcfg c;
    hb_err e;
    size_t i;
    const hb_sshcfg_host *inc1 = NULL, *scoped = NULL, *self = NULL;

    mk_tmp();
    snprintf(dir, sizeof dir, "%s/cfg", tmpdir);
    mkdir(dir, 0700);
    snprintf(sub, sizeof sub, "%s/cfg/config.d", tmpdir);
    mkdir(sub, 0700);
    snprintf(file, sizeof file, "%s/cfg/config.d/10.conf", tmpdir);
    write_file(file,
        "Host inc1\n"
        "  HostName inc.example.com\n");
    /* 目录也命中 glob → 应警告并跳过，不影响正常文件 */
    snprintf(file, sizeof file, "%s/cfg/config.d/20.dir", tmpdir);
    mkdir(file, 0700);
    snprintf(file, sizeof file, "%s/cfg/scoped.conf", tmpdir);
    write_file(file,
        "  User scoped-user\n"     /* 无 Host → 归入外层块 */
        "Host inner\n"
        "  User inner-user\n");
    snprintf(file, sizeof file, "%s/cfg/self.conf", tmpdir);
    write_file(file, "Include self.conf\nHost self\n");
    snprintf(cfg, sizeof cfg, "%s/cfg/config", tmpdir);
    write_file(cfg,
        "Include config.d/*\n"
        "Host scoped\n"
        "  Include scoped.conf\n"
        "Host self\n"
        "  Include self.conf\n");

    e = hb_sshcfg_parse_file(cfg, &c);
    CHECK_EQ_INT(e, HB_OK);
    for (i = 0; i < c.n; i++) {
        if (!strcmp(c.hosts[i].alias, "inc1")) inc1 = &c.hosts[i];
        if (!strcmp(c.hosts[i].alias, "scoped")) scoped = &c.hosts[i];
        if (!strcmp(c.hosts[i].alias, "self")) self = &c.hosts[i];
    }
    /* 别名：inc1, scoped, inner(被包含文件自己的 Host), self */
    CHECK(inc1 != NULL && inc1->hostname &&
          !strcmp(inc1->hostname, "inc.example.com"));
    CHECK(scoped != NULL);
    CHECK_EQ_STR(scoped->user, "scoped-user");  /* Include 无 Host → 归当前块 */
    CHECK(self != NULL);
    CHECK(c.nwarn >= 1);                        /* 深度超限警告 */
    {
        int depth_warn = 0, dir_warn = 0;
        for (i = 0; i < c.nwarn; i++) {
            if (strstr(c.warnings[i], "嵌套")) depth_warn = 1;
            if (strstr(c.warnings[i], "不是普通文件")) dir_warn = 1;
        }
        CHECK(depth_warn);
        CHECK(dir_warn);
    }
    hb_sshcfg_free(&c);
    rm_tmp();
}

/* -------------------- 4. Match/ProxyCommand/令牌展开 -------------------- */

static void test_warnings_and_expand(void)
{
    char home[512];
    hb_sshcfg c;
    hb_err e;
    size_t i;
    int w_match = 0, w_pcmd = 0, w_badport = 0;
    const hb_sshcfg_host *h = NULL;

    snprintf(home, sizeof home, "%s", getenv("HOME") ? getenv("HOME") : "");
    e = hb_sshcfg_parse_string(
        "Host t1\n"
        "  Match user bob\n"
        "    User ignored\n"
        "Host t2\n"
        "  ProxyCommand nc %h %p\n"
        "  Port notaport\n"
        "Host t3\n"
        "  IdentityFile %d/.ssh/%u@%h-%r-id%%x\n"
        "  User alice\n",
        NULL, &c);
    CHECK_EQ_INT(e, HB_OK);
    for (i = 0; i < c.nwarn; i++) {
        if (strstr(c.warnings[i], "Match")) w_match = 1;
        if (strstr(c.warnings[i], "ProxyCommand")) w_pcmd = 1;
        if (strstr(c.warnings[i], "Port")) w_badport = 1;
    }
    CHECK(w_match && w_pcmd && w_badport);
    for (i = 0; i < c.n; i++)
        if (!strcmp(c.hosts[i].alias, "t3")) h = &c.hosts[i];
    CHECK(h != NULL);
    CHECK_EQ_INT(c.n, 3);            /* t1 t2 t3（Match 块不产生别名） */
    if (h && h->identity_file) {
        char exp[700];
        /* %% → %：期待 "...-alice-id%x" */
        snprintf(exp, sizeof exp, "%s/.ssh/%s@t3-alice-id%%x",
                 home, luser());
        CHECK_EQ_STR(h->identity_file, exp);
    } else
        CHECK(0);
    /* t1 的 Match 块 User ignored 不生效 */
    CHECK(c.hosts[0].user == NULL);
    CHECK_EQ_INT(c.hosts[1].port, 0); /* 非法端口 → 0 */
    hb_sshcfg_free(&c);
}

/* -------------------- 5. 50 条导入验收 -------------------- */

static void test_import_50(void)
{
    char *buf = malloc(64 * 1024), *w = buf;
    hb_sshcfg c;
    hb_import_result r;
    hb_vault *v;
    hb_err e;
    int i;
    size_t expect_jumps = 0;

    w += sprintf(w, "User cfguser\n");
    for (i = 0; i < 50; i++) {
        w += sprintf(w, "Host h%02d\n", i);
        w += sprintf(w, "  HostName 10.0.%d.%d\n", i / 256, i % 256);
        if (i % 3 == 0) w += sprintf(w, "  Port %d\n", 2200 + i);
        if (i % 2 == 0) w += sprintf(w, "  IdentityFile ~/.ssh/id_h%02d\n", i);
        if (i % 5 == 0 && i + 1 < 50) {
            /* 引用下一个别名 → (a) */
            w += sprintf(w, "  ProxyJump h%02d\n", i + 1);
        }
        if (i == 7)  w += sprintf(w, "  ProxyJump h08\n");        /* 2 级外：别名 */
        if (i == 13) w += sprintf(w, "  ProxyJump admin@10.9.9.9:2200\n"); /* 字面量 */
        if (i == 21) w += sprintf(w, "  ProxyJump ops@192.168.7.7\n");     /* 字面量 */
    }
    *w = '\0';

    mk_tmp();
    e = hb_sshcfg_parse_string(buf, NULL, &c);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(c.n, 50);

    v = open_v();
    e = hb_hosts_import_sshcfg(v, &c, &r);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(r.created, 50);
    /* 跳板字面量：10.9.9.9:2200 和 192.168.7.7 各 1 个新建（h08 是别名） */
    expect_jumps = 2;
    CHECK_EQ_INT(r.jumps_created, expect_jumps);
    CHECK_EQ_INT(r.skipped, 0);
    /* created_ids = 别名 50 + 跳板字面量 2（UI 撤销要全删） */
    CHECK_EQ_INT(r.ncreated, 50 + expect_jumps);
    CHECK_EQ_INT(n_hosts(v), 50 + expect_jumps);

    /* 抽查字段 */
    {
        hb_obj *h00 = find_host(v, "h00");
        hb_obj *h07 = find_host(v, "h07");
        hb_obj *lit = find_host(v, "admin@10.9.9.9:2200");
        CHECK(h00 != NULL);
        CHECK_EQ_STR(hb_obj_get_str(h00, "address"), "10.0.0.0");
        CHECK_EQ_INT(hb_obj_get_int(h00, "port", -1), 2200);
        CHECK_EQ_STR(hb_obj_get_str(h00, "proto"), "ssh");
        CHECK_EQ_STR(hb_obj_get_str(h00, "auth"), "auto");
        CHECK_EQ_STR(hb_obj_get_str(h00, "os"), "auto");
        CHECK_EQ_STR(hb_obj_get_str(h00, "user"), "cfguser"); /* Host * 全局 */
        CHECK(hb_obj_get_str(h00, "identity_file") &&
              strstr(hb_obj_get_str(h00, "identity_file"), "/.ssh/id_h00"));
        /* h07 的 jump：外→内 = h08 → admin@10.9.9.9:2200 */
        CHECK(h07 != NULL);
        {
            const char **jv = NULL;
            size_t nj = hb_obj_get_strv(h07, "jump", &jv);
            CHECK_EQ_INT(nj, 1);
            if (nj == 1) {
                hb_obj *j0 = NULL;
                CHECK_EQ_INT(hb_vault_get(v, jv[0], &j0), HB_OK);
                CHECK_EQ_STR(hb_obj_get_str(j0, "name"), "h08");
                hb_obj_free(j0);
            }
            free(jv);
        }
        CHECK(lit != NULL);
        CHECK_EQ_STR(hb_obj_get_str(lit, "address"), "10.9.9.9");
        CHECK_EQ_INT(hb_obj_get_int(lit, "port", -1), 2200);
        CHECK_EQ_STR(hb_obj_get_str(lit, "user"), "admin");
        hb_obj_free(h00); hb_obj_free(h07); hb_obj_free(lit);
    }
    hb_vault_close(v);
    rm_tmp();
    hb_sshcfg_free(&c);
    hb_import_result_free(&r);
    free(buf);
}

/* -------------------- 6. 重复导入幂等 -------------------- */

static void test_reimport_skip(void)
{
    hb_sshcfg c;
    hb_import_result r;
    hb_vault *v;
    hb_err e;

    e = hb_sshcfg_parse_string(
        "Host a\n  HostName a.example\n"
        "Host b\n  HostName b.example\n  ProxyJump a\n", NULL, &c);
    CHECK_EQ_INT(e, HB_OK);
    mk_tmp();
    v = open_v();
    e = hb_hosts_import_sshcfg(v, &c, &r);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(r.created, 2);
    hb_import_result_free(&r);
    memset(&r, 0, sizeof r);
    e = hb_hosts_import_sshcfg(v, &c, &r);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(r.created, 0);
    CHECK_EQ_INT(r.skipped, 2);
    CHECK_EQ_INT(n_hosts(v), 2);
    hb_import_result_free(&r);
    hb_vault_close(v);
    rm_tmp();
    hb_sshcfg_free(&c);
}

/* -------------------- 7. JSON bundle 往返 -------------------- */

static void test_json_roundtrip(void)
{
    hb_vault *a, *b;
    hb_import_result r;
    hb_err e;
    char *bundle = NULL;
    char db2[300];

    mk_tmp();
    a = open_v();
    /* A：分组 + 3 台主机（两级 jump + via）+ 一个密码 secret */
    {
        hb_obj *g = hb_obj_new("group");
        hb_obj *j1 = hb_obj_new("host"), *j2 = hb_obj_new("host"),
               *t  = hb_obj_new("host");
        hb_obj_set_str(g, "name", "prod");
        hb_obj_set_int(g, "order", 1);
        hb_vault_put(a, g);
        hb_obj_set_str(j1, "name", "jp1"); hb_obj_set_str(j1, "address", "jp1.lan");
        hb_obj_set_int(j1, "port", 22); hb_obj_set_str(j1, "proto", "ssh");
        hb_obj_set_str(j1, "user", "ops");
        hb_obj_set_str(j2, "name", "jp2"); hb_obj_set_str(j2, "address", "jp2.lan");
        hb_obj_set_int(j2, "port", 22); hb_obj_set_str(j2, "proto", "ssh");
        {
            const char *jj[] = { hb_obj_id(j1) };
            hb_vault_put(a, j1);
            hb_obj_set_strv(j2, "jump", jj, 1);
        }
        hb_vault_put(a, j2);
        hb_obj_set_str(t, "name", "tgt"); hb_obj_set_str(t, "address", "tgt.lan");
        hb_obj_set_int(t, "port", 22); hb_obj_set_str(t, "proto", "ssh");
        hb_obj_set_str(t, "user", "root");
        hb_obj_set_str(t, "group", "prod");
        {
            const char *jj[] = { hb_obj_id(j1), hb_obj_id(j2) };
            hb_obj_set_strv(t, "jump", jj, 2);
        }
        hb_vault_put(a, t);
        hb_vault_set_secret(a, hb_obj_id(t), "password", "s3cret-pw");
        hb_obj_free(g); hb_obj_free(j1); hb_obj_free(j2); hb_obj_free(t);
    }
    e = hb_hosts_export_json(a, &bundle);
    CHECK_EQ_INT(e, HB_OK);
    CHECK(bundle != NULL);
    CHECK(strstr(bundle, "harbor-hosts"));
    CHECK(strstr(bundle, "tgt.lan"));
    CHECK(!strstr(bundle, "s3cret-pw"));       /* 无 secret 泄漏 */
    CHECK(!strstr(bundle, "_fv"));

    /* 导入到空 vault B */
    snprintf(db2, sizeof db2, "%s/b.db", tmpdir);
    e = hb_vault_open(db2, DEK, &b);
    CHECK_EQ_INT(e, HB_OK);
    memset(&r, 0, sizeof r);
    e = hb_hosts_import_json(b, bundle, &r);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(r.created, 3);
    CHECK_EQ_INT(r.skipped, 0);
    CHECK_EQ_INT(n_hosts(b), 3);
    {
        hb_obj *t = find_host(b, "tgt");
        const char **jv = NULL;
        size_t nj;
        /* 分组对象已建 */
        {
            hb_obj **gs = NULL; size_t ng = 0;
            hb_vault_list(b, "group", &gs, &ng);
            CHECK_EQ_INT(ng, 1);
            CHECK_EQ_STR(hb_obj_get_str(gs[0], "name"), "prod");
            hb_obj_list_free(gs, ng);
        }
        /* jump 链解析到 B 中同名主机（顺序不变） */
        nj = hb_obj_get_strv(t, "jump", &jv);
        CHECK_EQ_INT(nj, 2);
        if (nj == 2) {
            hb_obj *j0 = NULL, *j1 = NULL;
            CHECK_EQ_INT(hb_vault_get(b, jv[0], &j0), HB_OK);
            CHECK_EQ_INT(hb_vault_get(b, jv[1], &j1), HB_OK);
            CHECK_EQ_STR(hb_obj_get_str(j0, "name"), "jp1");
            CHECK_EQ_STR(hb_obj_get_str(j1, "name"), "jp2");
            hb_obj_free(j0); hb_obj_free(j1);
        }
        free(jv);
        /* secret 没有跟过来 */
        CHECK(hb_vault_get_secret(b, hb_obj_id(t), "password") == NULL);
        hb_obj_free(t);
    }
    /* 再导入 B → 全部 skipped */
    hb_import_result_free(&r);
    memset(&r, 0, sizeof r);
    e = hb_hosts_import_json(b, bundle, &r);
    CHECK_EQ_INT(e, HB_OK);
    CHECK_EQ_INT(r.created, 0);
    CHECK_EQ_INT(r.skipped, 3);

    /* 裸数组也接受（hb_vault_export_json 输出形状） */
    {
        char *bare = NULL;
        hb_vault *cw;
        char db3[300];
        hb_import_result r2;
        CHECK_EQ_INT(hb_vault_export_json(a, "host", &bare), HB_OK);
        snprintf(db3, sizeof db3, "%s/c.db", tmpdir);
        CHECK_EQ_INT(hb_vault_open(db3, DEK, &cw), HB_OK);
        memset(&r2, 0, sizeof r2);
        e = hb_hosts_import_json(cw, bare, &r2);
        CHECK_EQ_INT(e, HB_OK);
        CHECK_EQ_INT(r2.created, 3);
        hb_import_result_free(&r2);
        free(bare);
        hb_vault_close(cw);
    }
    /* 垃圾 → 错误 */
    e = hb_hosts_import_json(b, "not json", &r);
    CHECK_EQ_INT(e, HB_ERR_INVALID);
    e = hb_hosts_import_json(b, "{\"foo\":1}", &r);
    CHECK_EQ_INT(e, HB_ERR_INVALID);

    hb_import_result_free(&r);
    free(bundle);
    hb_vault_close(a);
    hb_vault_close(b);
    rm_tmp();
}

int main(void)
{
    test_parse_basic();
    test_parse_matching();
    test_include();
    test_warnings_and_expand();
    test_import_50();
    test_reimport_skip();
    test_json_roundtrip();
    fprintf(stderr, "test_hostio: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
