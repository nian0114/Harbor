/* SFTP 集成测试（FR-05 + FR-19 Phase 1）：用户态 sshd fixture
 *（internal-sftp），覆盖 realpath/list/stat、mkdir/rename/chmod/
 * remove、200MB 传输与原子性、冲突策略、目录递归、并发上限、
 * ProxyJump 与错误映射。
 * 远端 == 本机 FS（fixture 目录），可直接 stat/造文件。 */

#include "test.h"
#include "sshd_fixture.h"

#include "harbor/sftp.h"
#include "harbor/ssh.h"

#include "sftp_priv.h"      /* 目录穿越防护判定函数（直接单测） */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static hb_sshd fx;
static char RD[PATH_MAX];           /* 远端工作目录（fixture 内） */
static char LD[PATH_MAX];           /* 本地工作目录 */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------ 驱动 ------------------------------ */

typedef struct {
    int     connected, exited, exit_code, err_code;
    int     saw_sftp_stage;         /* 见到 "启动 SFTP 子系统…" */
    /* DONE 结果（owned，测试结束时统一 free） */
    hb_sftp_result *done[300];
    int             ndone;
    /* 进度簿：并发检测用 */
    struct { uint32_t req; int seen; } pr[64];
    int npr;
    int active_now, active_max;
} sdrv;

static void sdrv_init(sdrv *d)
{
    memset(d, 0, sizeof *d);
    d->exit_code = -999;
}

static void sdrv_take(sdrv *d, hb_sess_event *ev)
{
    switch (ev->type) {
    case HB_SESS_EV_STAGE:
        if (ev->text && strstr(ev->text, "SFTP"))
            d->saw_sftp_stage = 1;
        break;
    case HB_SESS_EV_CONNECTED:
        d->connected = 1;
        break;
    case HB_SESS_EV_ERROR:
        d->err_code = ev->code;
        break;
    case HB_SESS_EV_EXIT:
        d->exited = 1;
        d->exit_code = ev->code;
        break;
    case HB_SESS_EV_SFTP: {
        hb_sftp_result *r = ev->sftp;
        int i;
        if (r->kind == HB_SFTP_R_PROGRESS) {
            for (i = 0; i < d->npr; i++)
                if (d->pr[i].req == r->req)
                    break;
            if (i == d->npr && d->npr < 64) {
                d->pr[i].req = r->req;
                d->pr[i].seen = 1;
                d->npr++;
                d->active_now++;
                if (d->active_now > d->active_max)
                    d->active_max = d->active_now;
            }
        } else {
            for (i = 0; i < d->npr; i++)
                if (d->pr[i].req == r->req && d->pr[i].seen) {
                    d->pr[i].seen = 0;
                    d->active_now--;
                }
            if (d->ndone < 300) {
                d->done[d->ndone++] = r;
                ev->sftp = NULL;        /* 归测试所有 */
            }
        }
        break;
    }
    default:
        break;
    }
    hb_sess_event_clear(ev);
}

/* 排干所有挂起事件 */
static void pump(hb_session *s, sdrv *d)
{
    hb_sess_event ev;
    while (hb_session_next_event(s, &ev))
        sdrv_take(d, &ev);
}

static hb_sftp_result *find_done(sdrv *d, uint32_t req)
{
    int i;
    for (i = 0; i < d->ndone; i++)
        if (d->done[i]->req == req && d->done[i]->kind == HB_SFTP_R_DONE)
            return d->done[i];
    return NULL;
}

/* 等 req 的 DONE；NULL = 超时 */
static hb_sftp_result *wait_done(hb_session *s, sdrv *d, uint32_t req,
                                 int timeout_ms)
{
    int64_t dl = now_ms() + timeout_ms;
    hb_sftp_result *r;
    while (now_ms() < dl) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        if ((r = find_done(d, req)))
            return r;
        if (poll(&pf, 1, 20) > 0 && (pf.revents & POLLIN))
            pump(s, d);
    }
    return find_done(d, req);
}

static void sdrv_free(sdrv *d)
{
    int i;
    for (i = 0; i < d->ndone; i++)
        hb_sftp_result_free(d->done[i]);
}

/* ------------------------------ 会话 ------------------------------ */

static hb_ssh_opts base_opts(void)
{
    hb_ssh_opts o;
    memset(&o, 0, sizeof o);
    o.target.host = "127.0.0.1";
    o.target.port = fx.port;
    o.target.user = fx.user;
    o.target.auth = HB_SSH_AUTH_KEY;
    o.target.privkey = fx.key;
    o.connect_timeout_s = 5;
    o.keepalive_s = 1;
    return o;
}

static hb_session *sftp_conn(sdrv *d, const hb_ssh_opts *o)
{
    hb_session *s;
    int64_t dl;
    const char *kl[] = { fx.khline };
    hb_ssh_opts oo = *o;
    oo.known_lines = kl;
    oo.nknown = 1;
    s = hb_session_new_sftp(&oo);
    CHECK(s != NULL);
    if (!s)
        return NULL;
    dl = now_ms() + 15000;
    while (!d->connected && now_ms() < dl && !d->exited) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, 50) > 0 && (pf.revents & POLLIN))
            pump(s, d);
    }
    if (!d->connected) {
        hb_session_free(s);
        return NULL;
    }
    return s;
}

/* ------------------------------ 文件工具 ------------------------------ */

static int wfile(const char *path, uint64_t size, int mode, int seed)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
    char buf[65536];
    uint64_t left = size;
    unsigned x = (unsigned)seed;
    if (fd < 0)
        return -1;
    while (left) {
        size_t n = left > sizeof buf ? sizeof buf : (size_t)left, i;
        for (i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            buf[i] = (char)(x >> 16);
        }
        if (write(fd, buf, n) != (ssize_t)n) {
            close(fd);
            return -1;
        }
        left -= n;
    }
    close(fd);
    chmod(path, (mode_t)mode);
    return 0;
}

static int sha256(const char *path, char out[65])
{
    char cmd[PATH_MAX + 64];
    FILE *f;
    snprintf(cmd, sizeof cmd, "sha256sum '%s' | cut -d' ' -f1", path);
    f = popen(cmd, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%64s", out) != 1) {
        pclose(f);
        return -1;
    }
    pclose(f);
    return 0;
}

static int fexists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int fmode(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 ? (int)(st.st_mode & 07777) : -1;
}

static int64_t fmtime(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 ? (int64_t)st.st_mtime : -1;
}

/* 目录内 .harbor-part-* 数量 */
static int count_parts(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (!d)
        return -1;
    while ((de = readdir(d)))
        if (strstr(de->d_name, "harbor-part-"))
            n++;
    closedir(d);
    return n;
}

static void joinp(char *out, size_t cap, const char *d, const char *n)
{
    snprintf(out, cap, "%s/%s", d, n);
}

/* 杀掉 sshd master 下整棵会话子树（sshd 连接 = [priv] 监控进程 + 其下的
 * [net] 数据进程；socket 在 net 手里，只杀 priv 连接不死） */
static void kill_session_child(void)
{
    char cmd[256], pid_s[32];
    FILE *f;
    snprintf(cmd, sizeof cmd,
             "for p in $(pgrep -P %d); do pgrep -P $p; echo $p; done",
             (int)fx.pid);
    f = popen(cmd, "r");
    if (!f)
        return;
    while (fscanf(f, "%31s", pid_s) == 1)
        kill((pid_t)atoi(pid_s), SIGKILL);
    pclose(f);
}

/* ------------------------------ 测试 ------------------------------ */

/* T1+T2+T3+T5+T6+T7+T9：一个会话顺串 */
static void test_main_ops(void)
{
    sdrv d;
    hb_session *s;
    hb_ssh_opts o = base_opts();
    hb_sftp_result *r;
    uint32_t q;
    char p[PATH_MAX], p2[PATH_MAX], h1[65], h2[65];
    struct passwd *pw = getpwuid(getuid());
    int i;

    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(d.saw_sftp_stage);            /* "启动 SFTP 子系统…" stage */

    /* ---- T1: realpath / list ---- */
    q = hb_sftp_realpath(s, ".");
    CHECK(q > 0);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    if (r && pw)
        CHECK_EQ_STR(r->path, pw->pw_dir);

    /* 造一个目录：文件/目录/符号链接 */
    joinp(p, sizeof p, RD, "t1");
    mkdir(p, 0755);
    joinp(p2, sizeof p2, p, "fileA");
    wfile(p2, 1234, 0640, 1);
    joinp(p2, sizeof p2, p, "subd");
    mkdir(p2, 0700);
    joinp(p2, sizeof p2, p, "ln1");
    symlink("fileA", p2);

    q = hb_sftp_list(s, p);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    if (r) {
        int fa = 0, sd = 0, ln = 0;
        for (i = 0; i < (int)r->n; i++) {
            hb_sftp_entry *e = &r->entries[i];
            if (!strcmp(e->name, "fileA")) {
                fa = 1;
                CHECK_EQ_INT((long long)e->size, 1234);
                CHECK_EQ_INT(e->mode & 0777, 0640);
                CHECK(!e->is_dir && !e->is_link);
            } else if (!strcmp(e->name, "subd")) {
                sd = 1;
                CHECK(e->is_dir);
            } else if (!strcmp(e->name, "ln1")) {
                ln = 1;
                CHECK(e->is_link);
                CHECK_EQ_STR(e->link_target, "fileA");
            } else if (!strcmp(e->name, ".") ||
                       !strcmp(e->name, "..")) {
                CHECK(0);               /* 不应列出 */
            }
        }
        CHECK(fa && sd && ln);
    }

    /* ---- T2: mkdir/rename/chmod/remove ---- */
    joinp(p, sizeof p, RD, "m1");
    q = hb_sftp_mkdir(s, p, 0750);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    CHECK_EQ_INT(fmode(p), 0750);

    joinp(p, sizeof p, RD, "m1/a");
    wfile(p, 10, 0644, 2);
    joinp(p2, sizeof p2, RD, "m1/b");
    q = hb_sftp_rename(s, p, p2, false);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    CHECK(fexists(p2) && !fexists(p));

    /* rename 到已存在目标（overwrite=false）→ EXISTS */
    joinp(p, sizeof p, RD, "m1/c");
    wfile(p, 5, 0644, 3);
    q = hb_sftp_rename(s, p2, p, false);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_ERR_EXISTS);
    /* overwrite=true → 成功 */
    q = hb_sftp_rename(s, p2, p, true);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    CHECK(fexists(p) && !fexists(p2));

    /* chmod 0644→0755 → stat 验 */
    q = hb_sftp_chmod(s, p, 0755);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    q = hb_sftp_stat(s, p);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK && r->n == 1);
    if (r && r->n == 1)
        CHECK_EQ_INT(r->entries[0].mode & 0777, 0755);

    /* 非空目录非递归删 → NOTEMPTY；递归 → 删光 */
    joinp(p, sizeof p, RD, "m2/sub");
    mkdir(p, 0755);  /* m2 不存在 → 先建 */
    /* mkdir -p */
    {
        char m2[PATH_MAX];
        joinp(m2, sizeof m2, RD, "m2");
        mkdir(m2, 0755);
        mkdir(p, 0755);
        joinp(p2, sizeof p2, p, "inner");
        wfile(p2, 3, 0644, 4);
    }
    joinp(p, sizeof p, RD, "m2");
    q = hb_sftp_remove(s, p, false);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_ERR_NOTEMPTY);
    q = hb_sftp_remove(s, p, true);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_OK);
    CHECK(!fexists(p));

    /* ---- T3: 200MB 上传 + 下载（sha256 + mtime） ---- */
    joinp(p, sizeof p, LD, "big");
    {
        struct stat st;
        if (lstat(p, &st) != 0 || st.st_size != 200u * 1024 * 1024)
            wfile(p, 200u * 1024 * 1024, 0600, 7);
    }
    joinp(p2, sizeof p2, RD, "big");
    {
        int64_t t0 = now_ms(), t1;
        q = hb_sftp_upload(s, p, p2, HB_SFTP_OVERWRITE);
        CHECK(q > 0);
        r = wait_done(s, &d, q, 60000);
        t1 = now_ms();
        CHECK(r && r->status == HB_SFTP_OK);
        if (r)
            CHECK_EQ_STR(r->path, p2);
        printf("SFTP upload 200MB: %.1f MB/s\n",
               200.0 / ((t1 - t0) / 1000.0));
    }
    sha256(p, h1);
    sha256(p2, h2);                      /* 远端 == 本地 FS */
    CHECK_EQ_STR(h1, h2);

    joinp(p2, sizeof p2, LD, "big2");
    {
        int64_t t0 = now_ms(), t1;
        joinp(p, sizeof p, RD, "big");
        q = hb_sftp_download(s, p, p2, HB_SFTP_OVERWRITE);
        r = wait_done(s, &d, q, 60000);
        t1 = now_ms();
        CHECK(r && r->status == HB_SFTP_OK);
        printf("SFTP download 200MB: %.1f MB/s\n",
               200.0 / ((t1 - t0) / 1000.0));
    }
    sha256(p2, h2);
    CHECK_EQ_STR(h1, h2);
    joinp(p, sizeof p, LD, "big");
    CHECK_EQ_INT(fmtime(p2), fmtime(p));   /* mtime 保真 */
    CHECK_EQ_INT(fmode(p2) & 0777, 0600);

    /* ---- T5: 冲突策略 ---- */
    joinp(p, sizeof p, LD, "cf");
    wfile(p, 64, 0644, 11);
    joinp(p2, sizeof p2, RD, "cf/a.txt");
    {
        char dp[PATH_MAX];
        joinp(dp, sizeof dp, RD, "cf");
        mkdir(dp, 0755);
        wfile(p2, 8, 0644, 99);          /* "OLD" */
    }
    q = hb_sftp_upload(s, p, p2, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK);
    {
        struct stat st;
        lstat(p2, &st);
        CHECK_EQ_INT((long long)st.st_size, 64);
    }
    q = hb_sftp_upload(s, p, p2, HB_SFTP_SKIP);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK && r->skipped);
    {
        char oldc[8] = {0};
        int fd = open(p2, O_RDONLY);
        if (fd >= 0) {
            ssize_t nn = read(fd, oldc, 8);
            (void)nn;
            close(fd);
        }
        /* SKIP 后内容应保持上次 64B 内容（旧文件未被新传覆盖） */
        struct stat st;
        lstat(p2, &st);
        CHECK_EQ_INT((long long)st.st_size, 64);
    }
    /* RENAME → "a (1).txt"、"a (2).txt" */
    q = hb_sftp_upload(s, p, p2, HB_SFTP_RENAME);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK);
    {
        char want[PATH_MAX];
        joinp(want, sizeof want, RD, "cf/a (1).txt");
        if (r) {
            CHECK_EQ_STR(r->path, want);
            CHECK(fexists(want));
        }
    }
    q = hb_sftp_upload(s, p, p2, HB_SFTP_RENAME);
    r = wait_done(s, &d, q, 15000);
    {
        char want[PATH_MAX];
        joinp(want, sizeof want, RD, "cf/a (2).txt");
        if (r) {
            CHECK_EQ_STR(r->path, want);
            CHECK(fexists(want));
        }
    }
    /* 点文件不算扩展名：.bashrc → ".bashrc (1)" */
    joinp(p2, sizeof p2, RD, "cf/.bashrc");
    wfile(p2, 8, 0644, 99);
    joinp(p, sizeof p, LD, "cf");
    q = hb_sftp_upload(s, p, p2, HB_SFTP_RENAME);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK);
    {
        char want[PATH_MAX];
        joinp(want, sizeof want, RD, "cf/.bashrc (1)");
        if (r) {
            CHECK_EQ_STR(r->path, want);
            CHECK(fexists(want));
        }
    }
    /* 多段扩展按最后一段切：archive.tar.gz → "archive.tar (1).gz" */
    joinp(p2, sizeof p2, RD, "cf/archive.tar.gz");
    wfile(p2, 8, 0644, 99);
    q = hb_sftp_upload(s, p, p2, HB_SFTP_RENAME);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK);
    {
        char want[PATH_MAX];
        joinp(want, sizeof want, RD, "cf/archive.tar (1).gz");
        if (r) {
            CHECK_EQ_STR(r->path, want);
            CHECK(fexists(want));
        }
    }

    /* ---- T6: 目录递归上传 + 下载 ---- */
    {
        char tr[PATH_MAX], lp[PATH_MAX];
        int f;
        joinp(tr, sizeof tr, LD, "tree");
        mkdir(tr, 0755);
        for (f = 0; f < 50; f++) {
            { char nb[40]; snprintf(nb, sizeof nb, "f%02d.txt", f);
            joinp(lp, sizeof lp, tr, nb); }
            wfile(lp, 100 + f, 0644, f);
        }
        joinp(lp, sizeof lp, tr, "d1");
        mkdir(lp, 0755);
        joinp(lp, sizeof lp, tr, "d1/x");
        wfile(lp, 77, 0600, 5);
        joinp(lp, sizeof lp, tr, "d1/d2");
        mkdir(lp, 0755);
        joinp(lp, sizeof lp, tr, "d1/d2/y");
        wfile(lp, 88, 0600, 6);
        joinp(lp, sizeof lp, tr, "emptyd");
        mkdir(lp, 0755);
    }
    joinp(p, sizeof p, LD, "tree");
    joinp(p2, sizeof p2, RD, "rtree");
    q = hb_sftp_upload(s, p, p2, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 30000);
    CHECK(r && r->status == HB_SFTP_OK);
    if (r) {
        CHECK_EQ_INT(r->files_total, 52);
        CHECK_EQ_INT(r->files_done, 52);
        CHECK(r->total > 0);
    }
    {
        char cmd[PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof cmd, "diff -r %s %s/rtree >/dev/null", p, RD);
        CHECK_EQ_INT(system(cmd), 0);    /* 远端树一致 */
    }
    /* 下载回另一个目录 */
    joinp(p2, sizeof p2, RD, "rtree");
    joinp(p, sizeof p, LD, "tree2");
    q = hb_sftp_download(s, p2, p, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 30000);
    CHECK(r && r->status == HB_SFTP_OK);
    {
        char cmd[PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof cmd, "diff -r %s %s >/dev/null", p2, p);
        CHECK_EQ_INT(system(cmd), 0);
    }

    /* ---- T7: 并发上限 3 + LIST 延迟 ---- */
    joinp(p, sizeof p, LD, "med");
    wfile(p, 20u * 1024 * 1024, 0600, 21);
    {
        uint32_t reqs[6];
        int j;
        int64_t lt0, base, bound;
        /* 无竞争基线：同一个会话、同样的目录。重负载机器上
         * 进程本身被调度饿死时，连无竞争 LIST 都会变慢；
         * 预算随基线放宽，否则环境噪声会顶爆固定阈值。 */
        lt0 = now_ms();
        q = hb_sftp_list(s, RD);
        r = wait_done(s, &d, q, 10000);
        CHECK(r && r->status == HB_SFTP_OK);
        base = now_ms() - lt0;
        bound = 500 > 4 * base ? 500 : 4 * base;
        hb_sftp_set_concurrency(s, 3);
        for (j = 0; j < 6; j++) {
            { char nb[40]; snprintf(nb, sizeof nb, "conc%d", j);
            joinp(p2, sizeof p2, RD, nb); }
            reqs[j] = hb_sftp_upload(s, p, p2, HB_SFTP_OVERWRITE);
            CHECK(reqs[j] > 0);
        }
        usleep(150000);                 /* 让传输跑起来 */
        lt0 = now_ms();
        q = hb_sftp_list(s, RD);
        r = wait_done(s, &d, q, 10000);
        CHECK(r && r->status == HB_SFTP_OK);
        CHECK(now_ms() - lt0 < bound);  /* 元数据插空快回 */
        for (j = 0; j < 6; j++) {
            r = wait_done(s, &d, reqs[j], 60000);
            CHECK(r && r->status == HB_SFTP_OK);
        }
        CHECK(d.active_max <= 3);       /* 并发不超上限 */
        CHECK(d.active_max >= 2);       /* 确实有并行 */
        sha256(p, h1);
        for (j = 0; j < 6; j++) {
            { char nb[40]; snprintf(nb, sizeof nb, "conc%d", j);
            joinp(p2, sizeof p2, RD, nb); }
            sha256(p2, h2);
            CHECK_EQ_STR(h1, h2);       /* 内容正确 */
        }
    }

    /* ---- T9: 错误映射 ---- */
    q = hb_sftp_list(s, "/nonexistent-dir-xyz");
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_ERR_NOENT);
    if (r)
        CHECK_EQ_STR(r->message, "没有那个文件或目录");
    {
        char nw[PATH_MAX];
        joinp(nw, sizeof nw, RD, "nowrite");
        mkdir(nw, 0555);
        joinp(p2, sizeof p2, nw, "x");
        joinp(p, sizeof p, LD, "cf");
        q = hb_sftp_upload(s, p, p2, HB_SFTP_OVERWRITE);
        r = wait_done(s, &d, q, 15000);
        CHECK(r && r->status == HB_SFTP_ERR_PERM);
        chmod(nw, 0755);
    }
    q = hb_sftp_upload(s, "/nonexistent-local-xyz", p2,
                       HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 10000);
    CHECK(r && r->status == HB_SFTP_ERR_LOCAL);
    if (r && r->message)
        CHECK(strlen(r->message) > 0);

    hb_session_free(s);
    sdrv_free(&d);
}

/* T4: 原子性（取消 + 杀 sshd 子进程），上传/下载各一遍 */
static void test_atomicity(void)
{
    sdrv d;
    hb_session *s;
    hb_ssh_opts o = base_opts();
    hb_sftp_result *r;
    uint32_t q;
    char big[PATH_MAX], rp[PATH_MAX], lp[PATH_MAX];
    int64_t dl;
    uint64_t target;

    joinp(big, sizeof big, LD, "big");

    /* (a) 上传 ~30% 取消 → 无最终文件、无远端 part */
    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;
    joinp(rp, sizeof rp, RD, "atomic1");
    q = hb_sftp_upload(s, big, rp, HB_SFTP_OVERWRITE);
    CHECK(q > 0);
    /* 等到传输进入读写段（进度事件出现 + 字节数到位） */
    dl = now_ms() + 30000;
    target = 60u * 1024 * 1024;
    for (;;) {
        hb_sftp_result *pr = NULL;
        int i;
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, 20) > 0 && (pf.revents & POLLIN))
            pump(s, &d);
        /* 从最近的 DONE/PROGRESS 估 done —— PROGRESS 被 take 释放，
           改为看远端 part 文件大小作近似 */
        {
            DIR *dd = opendir(RD);
            struct dirent *de;
            uint64_t psz = 0;
            if (dd) {
                while ((de = readdir(dd)))
                    if (strstr(de->d_name, "atomic1") ||
                        strstr(de->d_name, "harbor-part")) {
                        char fp[PATH_MAX];
                        struct stat st;
                        joinp(fp, sizeof fp, RD, de->d_name);
                        if (!lstat(fp, &st))
                            psz = (uint64_t)st.st_size;
                    }
                closedir(dd);
            }
            if (psz >= target || now_ms() > dl || fexists(rp))
                break;
        }
        (void)pr;
        (void)i;
    }
    hb_sftp_cancel(s, q);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_ERR_CANCELLED);
    usleep(200000);                     /* 清理序列收尾 */
    CHECK(!fexists(rp));                /* 最终文件不出现 */
    CHECK_EQ_INT(count_parts(RD), 0);   /* 无远端 part 残留 */
    hb_session_free(s);
    sdrv_free(&d);

    /* (b) 上传中途杀 sshd 子进程 → CONN，最终文件不在（part 可留） */
    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;
    joinp(rp, sizeof rp, RD, "atomic2");
    q = hb_sftp_upload(s, big, rp, HB_SFTP_OVERWRITE);
    CHECK(q > 0);
    usleep(400000);                     /* 让它写进一段 */
    kill_session_child();
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_ERR_CONN);
    if (r)
        CHECK_EQ_STR(r->message, "连接已断开");
    CHECK(!fexists(rp));
    /* 残件可留：清掉免得影响后续 */
    {
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof cmd, "rm -f %s/.atomic2.harbor-part-*", RD);
        if (system(cmd) != 0) {}
    }
    /* 会话应已 EXIT */
    dl = now_ms() + 5000;
    while (!d.exited && now_ms() < dl)
        usleep(20000);
    pump(s, &d);
    CHECK(d.exited);
    hb_session_free(s);
    sdrv_free(&d);

    /* (c) 下载 ~30% 取消 → 无最终本地文件、无本地 part */
    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;
    joinp(rp, sizeof rp, RD, "big");
    joinp(lp, sizeof lp, LD, "atomic3");
    q = hb_sftp_download(s, rp, lp, HB_SFTP_OVERWRITE);
    CHECK(q > 0);
    dl = now_ms() + 30000;
    target = 60u * 1024 * 1024;
    for (;;) {
        DIR *dd = opendir(LD);
        struct dirent *de;
        uint64_t psz = 0;
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, 10) > 0 && (pf.revents & POLLIN))
            pump(s, &d);
        if (dd) {
            while ((de = readdir(dd)))
                if (strstr(de->d_name, "harbor-part")) {
                    char fp[PATH_MAX];
                    struct stat st;
                    joinp(fp, sizeof fp, LD, de->d_name);
                    if (!lstat(fp, &st))
                        psz = (uint64_t)st.st_size;
                }
            closedir(dd);
        }
        if (psz >= target || now_ms() > dl || fexists(lp))
            break;
    }
    hb_sftp_cancel(s, q);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_ERR_CANCELLED);
    usleep(200000);
    CHECK(!fexists(lp));
    CHECK_EQ_INT(count_parts(LD), 0);   /* 本地 part 必删 */
    hb_session_free(s);
    sdrv_free(&d);

    /* (d) 下载中途杀子进程 → CONN，本地 part 也清掉 */
    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;
    joinp(lp, sizeof lp, LD, "atomic4");
    q = hb_sftp_download(s, rp, lp, HB_SFTP_OVERWRITE);
    CHECK(q > 0);
    usleep(400000);
    kill_session_child();
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_ERR_CONN);
    CHECK(!fexists(lp));
    CHECK_EQ_INT(count_parts(LD), 0);
    hb_session_free(s);
    sdrv_free(&d);
}

/* T8: 两级 ProxyJump SFTP */
static void test_proxyjump(void)
{
    sdrv d;
    hb_session *s;
    hb_ssh_opts o = base_opts();
    hb_ssh_hop jumps[2];
    hb_sftp_result *r;
    uint32_t q;
    char lp[PATH_MAX], rp[PATH_MAX];

    memset(jumps, 0, sizeof jumps);
    jumps[0] = o.target;
    jumps[0].label = "hop1";
    jumps[1] = o.target;
    jumps[1].label = "hop2";
    o.jumps = jumps;
    o.njumps = 2;

    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;
    q = hb_sftp_realpath(s, ".");
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK);

    joinp(lp, sizeof lp, LD, "cf");
    joinp(rp, sizeof rp, RD, "jumped");
    q = hb_sftp_upload(s, lp, rp, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 20000);
    CHECK(r && r->status == HB_SFTP_OK);
    CHECK(fexists(rp));
    q = hb_sftp_list(s, RD);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK && r->n > 0);
    hb_session_free(s);
    sdrv_free(&d);
}

/* T10: 目录穿越防护 —— 判定函数单测（假服务器无法伪造，直接测逻辑） */
static void test_safenames(void)
{
    char *n;
    /* 目录项名：空、.、..、'/'、内嵌 NUL、<0x20 控制符全拒 */
    CHECK(!hb__sftp_name_ok("", 0));
    CHECK(!hb__sftp_name_ok(".", 1));
    CHECK(!hb__sftp_name_ok("..", 2));
    CHECK(!hb__sftp_name_ok("a/b", 3));
    CHECK(!hb__sftp_name_ok("a\0b", 3));      /* 内嵌 NUL 截短成真路径 */
    CHECK(!hb__sftp_name_ok("a\x1f", 2));
    CHECK(!hb__sftp_name_ok("\t", 1));
    CHECK(hb__sftp_name_ok("a.txt", 5));
    CHECK(hb__sftp_name_ok("..x", 3));        /* 点开头但不是 .. 本身 */
    CHECK(hb__sftp_name_ok(".hidden", 7));
    CHECK(hb__sftp_name_ok("a (1).txt", 9));
    /* 词法归一化 */
    n = hb__sftp_normpath("/a/b/../c");  CHECK_EQ_STR(n, "/a/c");   free(n);
    n = hb__sftp_normpath("/a//b/./c");  CHECK_EQ_STR(n, "/a/b/c"); free(n);
    n = hb__sftp_normpath("/a/b/c/..");  CHECK_EQ_STR(n, "/a/b");   free(n);
    n = hb__sftp_normpath("/../../etc"); CHECK_EQ_STR(n, "/etc");   free(n);
    n = hb__sftp_normpath("/");          CHECK_EQ_STR(n, "/");      free(n);
    /* 归一后前缀 containment */
    CHECK(hb__sftp_path_under("/a/b", "/a/b"));
    CHECK(hb__sftp_path_under("/a/b", "/a/b/c/d"));
    CHECK(hb__sftp_path_under("/a/b", "/a/b/./c"));
    CHECK(hb__sftp_path_under("/a/b", "/a/b/../b/c"));   /* 归一后仍在内 */
    CHECK(!hb__sftp_path_under("/a/b", "/a/bc"));        /* 前缀但不分隔 */
    CHECK(!hb__sftp_path_under("/a/b", "/a/b/../c"));
    CHECK(!hb__sftp_path_under("/a/b", "/a/b/../../etc"));
    CHECK(!hb__sftp_path_under("/a/b", "/etc/passwd"));
    CHECK(hb__sftp_path_under("/", "/x"));
    CHECK(hb__sftp_path_under("/a/", "/a/b"));
}

/* T11: 路径穿越防护实测 —— 递归传输不跟随符号链接（双向） */
static void test_links(void)
{
    sdrv d;
    hb_session *s;
    hb_ssh_opts o = base_opts();
    hb_sftp_result *r;
    uint32_t q;
    char p[PATH_MAX], p2[PATH_MAX];

    /* 远端树：真文件/子目录 + 逃逸链接 + 树内链接（sshd 在本机，
     * RD 就是本地路径，可直接造链接）。sentineld 是逃逸目标：若
     * 链接被跟随会把 secret.txt 拖进下载目录。 */
    joinp(p, sizeof p, RD, "linktree");
    mkdir(p, 0755);
    joinp(p2, sizeof p2, p, "real.txt");
    wfile(p2, 32, 0644, 7);
    joinp(p2, sizeof p2, p, "sub");
    mkdir(p2, 0755);
    joinp(p2, sizeof p2, p, "sub/inner.txt");
    wfile(p2, 24, 0644, 8);
    joinp(p2, sizeof p2, RD, "sentineld");
    mkdir(p2, 0755);
    joinp(p2, sizeof p2, RD, "sentineld/secret.txt");
    wfile(p2, 16, 0600, 9);
    {
        char lk[PATH_MAX];
        joinp(lk, sizeof lk, p, "esc-parent");  symlink("../sentineld", lk);
        joinp(lk, sizeof lk, p, "esc-etc");     symlink("/etc", lk);
        joinp(lk, sizeof lk, p, "esc-file");    symlink("/etc/hostname", lk);
        joinp(lk, sizeof lk, p, "inner-link");  symlink("real.txt", lk);
    }

    sdrv_init(&d);
    s = sftp_conn(&d, &o);
    CHECK(s != NULL);
    if (!s)
        return;

    /* 递归下载：链接全跳过并计入结果消息，无任何文件出树 */
    joinp(p, sizeof p, RD, "linktree");
    joinp(p2, sizeof p2, LD, "linkdl");
    q = hb_sftp_download(s, p, p2, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 20000);
    CHECK(r && r->status == HB_SFTP_OK);
    if (r) {
        CHECK_EQ_INT(r->files_total, 2);   /* 链接不算文件 */
        CHECK_EQ_INT(r->files_done, 2);
        CHECK(r->message != NULL);          /* 结果提及跳过 */
        if (r->message)
            CHECK(strstr(r->message, "符号链接") != NULL);
    }
    {
        char t[PATH_MAX];
        joinp(t, sizeof t, p2, "real.txt");           CHECK(fexists(t));
        joinp(t, sizeof t, p2, "sub/inner.txt");      CHECK(fexists(t));
        joinp(t, sizeof t, p2, "esc-parent");         CHECK(!fexists(t));
        joinp(t, sizeof t, p2, "esc-parent/secret.txt"); CHECK(!fexists(t));
        joinp(t, sizeof t, p2, "esc-etc");            CHECK(!fexists(t));
        joinp(t, sizeof t, p2, "esc-file");           CHECK(!fexists(t));
        joinp(t, sizeof t, p2, "inner-link");         CHECK(!fexists(t));
    }
    /* 下载目录内恰好 2 项（real.txt + sub），无任何意外落盘 */
    {
        DIR *dd = opendir(p2);
        struct dirent *de;
        int cnt = 0;
        if (dd) {
            while ((de = readdir(dd)))
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                    cnt++;
            closedir(dd);
        }
        CHECK_EQ_INT(cnt, 2);
    }

    /* 显式单文件下载链接 → 跟随目标，拿到 /etc/hostname 内容 */
    joinp(p, sizeof p, RD, "linktree/esc-file");
    joinp(p2, sizeof p2, LD, "hostnamedl");
    q = hb_sftp_download(s, p, p2, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 15000);
    CHECK(r && r->status == HB_SFTP_OK);
    CHECK(fexists(p2));

    /* 本地上传：本地树里的符号链接同样不跟随 */
    joinp(p, sizeof p, LD, "ltree");
    mkdir(p, 0755);
    joinp(p2, sizeof p2, p, "a.txt");
    wfile(p2, 10, 0644, 3);
    {
        char lk[PATH_MAX];
        joinp(lk, sizeof lk, p, "esc");
        symlink("/etc", lk);
    }
    joinp(p2, sizeof p2, RD, "ltree-up");
    q = hb_sftp_upload(s, p, p2, HB_SFTP_OVERWRITE);
    r = wait_done(s, &d, q, 20000);
    CHECK(r && r->status == HB_SFTP_OK);
    if (r) {
        CHECK_EQ_INT(r->files_total, 1);
        CHECK(r->message != NULL);
    }
    {
        char t[PATH_MAX];
        joinp(t, sizeof t, p2, "a.txt");  CHECK(fexists(t));
        joinp(t, sizeof t, p2, "esc");    CHECK(!fexists(t));
    }

    hb_session_free(s);
    sdrv_free(&d);
}

int main(void)
{
    char cmd[PATH_MAX + 64];
    int rc = hb_sshd_start(&fx);
    if (rc == 77)
        exit(77);
    CHECK_EQ_INT(rc, 0);
    if (rc != 0)
        return 1;

    joinp(RD, sizeof RD, fx.dir, "remote");
    joinp(LD, sizeof LD, fx.dir, "local");
    mkdir(RD, 0755);
    mkdir(LD, 0755);

    test_safenames();
    test_main_ops();
    test_atomicity();
    test_links();
    test_proxyjump();

    hb_sshd_stop(&fx);
    snprintf(cmd, sizeof cmd, "true");
    (void)cmd;
    fprintf(stderr, "test_sftp: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
