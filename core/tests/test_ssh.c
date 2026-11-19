/* SSH 集成测试：起一个用户态 sshd（127.0.0.1:随机端口），覆盖
 * TOFU/known_hosts/MITM、内存/文件/加密密钥、密码提示、ProxyJump、
 * 交互 shell、取消、keepalive，以及 hb_ssh_parse_target 单元测试。
 * 缺 /usr/sbin/sshd 或 ssh-keygen → exit(77)（meson 记为 SKIP）。 */

#include "test.h"

#include "harbor/ssh.h"
#include "harbor/vt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#define FIX_USER_MAX 64

/* ------------------------------ 工具 ------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* popen 读 stdout 到静态缓冲（截断到 cap-1） */
static char *run_out(const char *fmt, ...)
{
    static char buf[16384];
    char cmd[4600];
    va_list ap;
    FILE *f;
    size_t n = 0;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    f = popen(cmd, "r");
    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    pclose(f);
    return buf;
}

static char *slurp(const char *path)
{
    static char buf[16384];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int port;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (void *)&a, sizeof a) || getsockname(fd, (void *)&a, &al)) {
        close(fd);
        return 0;
    }
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

/* ------------------------------ sshd fixture ------------------------------ */

static char fix_dir[4096];
static int fix_port;
static pid_t fix_sshd = -1;
static char fix_user[FIX_USER_MAX];
static char fix_khline[1024];        /* "[127.0.0.1]:port ssh-ed25519 b64" */
static char fix_khline_bad[1024];
static char fix_fp[128];             /* "SHA256:..." */
static char fix_art[4096];           /* ssh-keygen -lv 的 randomart 框 */
static char fix_key[16384];
static char fix_key_enc[16384];
static char fix_key_bad[16384];

static void field_dup(const char *line, int field, char *out, size_t cap)
{
    /* 取第 field（1 起）个空白分隔字段 */
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

static void fixture_setup(void)
{
    char path[4600];
    char *out;

    if (access("/usr/sbin/sshd", X_OK) != 0 ||
        access("/usr/bin/ssh-keygen", X_OK) != 0) {
        fprintf(stderr, "SKIP: sshd/ssh-keygen missing\n");
        exit(77);
    }
    if (!getpwuid(getuid()) || !getpwuid(getuid())->pw_name) {
        fprintf(stderr, "SKIP: no username\n");
        exit(77);
    }
    snprintf(fix_user, sizeof fix_user, "%s", getpwuid(getuid())->pw_name);

    snprintf(fix_dir, sizeof fix_dir, "/tmp/hb-ssh-test-XXXXXX");
    if (!mkdtemp(fix_dir)) {
        perror("mkdtemp");
        exit(1);
    }
    fix_port = free_port();
    if (!fix_port)
        exit(1);

#define KGEN(...)                                                         \
    do {                                                                  \
        snprintf(path, sizeof path, __VA_ARGS__);                         \
        if (system(path) != 0)                                            \
            exit(1);                                                      \
    } while (0)

    KGEN("/usr/bin/ssh-keygen -t ed25519 -N '' -f %s/hostkey -q", fix_dir);
    KGEN("/usr/bin/ssh-keygen -t ed25519 -N '' -f %s/client -q", fix_dir);
    KGEN("/usr/bin/ssh-keygen -t ed25519 -N 'pw123' -f %s/client_enc -q",
         fix_dir);
    KGEN("/usr/bin/ssh-keygen -t ed25519 -N '' -f %s/badkey -q", fix_dir);
    KGEN("/usr/bin/ssh-keygen -t ed25519 -N '' -f %s/hostkey2 -q", fix_dir);
#undef KGEN

    /* authorized_keys = client.pub + client_enc.pub（badkey 不在列） */
    {
        FILE *f;
        snprintf(path, sizeof path, "%s/authorized_keys", fix_dir);
        f = fopen(path, "w");
        snprintf(path, sizeof path, "%s/client.pub", fix_dir);
        fputs(slurp(path), f);
        snprintf(path, sizeof path, "%s/client_enc.pub", fix_dir);
        fputs(slurp(path), f);
        fclose(f);
        snprintf(path, sizeof path, "%s/authorized_keys", fix_dir);
    }

    /* sshd_config */
    {
        FILE *f;
        snprintf(path, sizeof path, "%s/sshd_config", fix_dir);
        f = fopen(path, "w");
        fprintf(f,
                "Port %d\n"
                "ListenAddress 127.0.0.1\n"
                "HostKey %s/hostkey\n"
                "PidFile %s/sshd.pid\n"
                "AuthorizedKeysFile %s/authorized_keys\n"
                "UsePAM no\n"
                "StrictModes no\n"
                "PasswordAuthentication no\n"
                "KbdInteractiveAuthentication no\n"
                "AllowTcpForwarding yes\n"
                "PermitTTY yes\n"
                "Subsystem sftp internal-sftp\n"
                "LogLevel ERROR\n",
                fix_port, fix_dir, fix_dir, fix_dir);
        fclose(f);
    }

    /* 预期指纹 / randomart / known_hosts 行 */
    snprintf(path, sizeof path, "%s/hostkey.pub", fix_dir);
    out = run_out("/usr/bin/ssh-keygen -lf %s -E sha256", path);
    if (!out)
        exit(1);
    field_dup(out, 2, fix_fp, sizeof fix_fp);          /* "SHA256:..." */

    out = run_out("/usr/bin/ssh-keygen -lv -f %s", path);
    if (!out)
        exit(1);
    {
        /* 第 2 行起是 randomart 框（11 行） */
        const char *nl = strchr(out, '\n');
        if (nl)
            snprintf(fix_art, sizeof fix_art, "%s", nl + 1);
    }
    {
        char b64[600];
        field_dup(slurp(path), 2, b64, sizeof b64);   /* hostkey.pub 第 2 字段 */
        snprintf(fix_khline, sizeof fix_khline,
                 "[127.0.0.1]:%d ssh-ed25519 %s", fix_port, b64);
    }
    {
        char b64[600];
        snprintf(path, sizeof path, "%s/hostkey2.pub", fix_dir);
        field_dup(slurp(path), 2, b64, sizeof b64);
        snprintf(fix_khline_bad, sizeof fix_khline_bad,
                 "[127.0.0.1]:%d ssh-ed25519 %s", fix_port, b64);
    }

    snprintf(path, sizeof path, "%s/client", fix_dir);
    snprintf(fix_key, sizeof fix_key, "%s", slurp(path));
    snprintf(path, sizeof path, "%s/client_enc", fix_dir);
    snprintf(fix_key_enc, sizeof fix_key_enc, "%s", slurp(path));
    snprintf(path, sizeof path, "%s/badkey", fix_dir);
    snprintf(fix_key_bad, sizeof fix_key_bad, "%s", slurp(path));

    /* 启动 sshd */
    fix_sshd = fork();
    if (fix_sshd == 0) {
        int errfd;
        char cfg[4200], logp[4200];
        snprintf(cfg, sizeof cfg, "%s/sshd_config", fix_dir);
        snprintf(logp, sizeof logp, "%s/sshd.err", fix_dir);
        errfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (errfd >= 0)
            dup2(errfd, STDERR_FILENO);
        execl("/usr/sbin/sshd", "/usr/sbin/sshd", "-D", "-e", "-f", cfg,
              NULL);
        _exit(127);
    }
    /* 等端口可连 */
    {
        int i, ok = 0;
        for (i = 0; i < 100 && !ok; i++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)fix_port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(fd, (void *)&a, sizeof a) == 0)
                ok = 1;
            close(fd);
            if (!ok)
                usleep(50000);
        }
        if (!ok) {
            fprintf(stderr, "sshd did not start\n");
            exit(1);
        }
    }
}

static void fixture_teardown(void)
{
    char cmd[4400];
    if (fix_sshd > 0) {
        kill(fix_sshd, SIGTERM);
        waitpid(fix_sshd, NULL, 0);
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", fix_dir);
    system(cmd);
}

/* ------------------------------ 事件驱动 ------------------------------ */

typedef struct {
    int         hk_reply;              /* -1 不应答，0 拒，1 收 */
    const char *prompt_replies[8];     /* 脚本报文（NULL 元素 = 取消回复） */
    int         n_prompts_set;
    /* collected */
    int  stages_j, stages_t;
    int  connected;
    int  hk_n, hk_code_last;
    char hk_text[300], hk_text2[300];
    char hk_extra[8192];
    int  prompt_n, prompt_codes[8];
    int  err_code;
    char err_text[300], err_text2[400];
    int  exit_code;
    bool exited;
} drv;

static void drv_init(drv *d)
{
    memset(d, 0, sizeof *d);
    d->hk_reply = -1;
    d->exit_code = -999;
}

static void drv_take(drv *d, hb_session *s, hb_sess_event *ev)
{
    switch (ev->type) {
    case HB_SESS_EV_STAGE:
        if (ev->mark == 'J')
            d->stages_j++;
        else if (ev->mark == 'T')
            d->stages_t++;
        break;
    case HB_SESS_EV_CONNECTED:
        d->connected++;
        break;
    case HB_SESS_EV_HOSTKEY:
        d->hk_n++;
        d->hk_code_last = ev->code;
        snprintf(d->hk_text, sizeof d->hk_text, "%s", ev->text ? ev->text : "");
        snprintf(d->hk_text2, sizeof d->hk_text2, "%s",
                 ev->text2 ? ev->text2 : "");
        snprintf(d->hk_extra, sizeof d->hk_extra, "%s",
                 ev->extra ? ev->extra : "");
        if (d->hk_reply >= 0)
            hb_session_hostkey_reply(s, d->hk_reply == 1);
        break;
    case HB_SESS_EV_PROMPT:
        if (d->prompt_n < 8) {
            const char *r = NULL;
            d->prompt_codes[d->prompt_n] = ev->code;
            if (d->prompt_n < d->n_prompts_set)
                r = d->prompt_replies[d->prompt_n];
            d->prompt_n++;
            hb_session_prompt_reply(s, r);
        }
        break;
    case HB_SESS_EV_ERROR:
        d->err_code = ev->code;
        snprintf(d->err_text, sizeof d->err_text, "%s",
                 ev->text ? ev->text : "");
        snprintf(d->err_text2, sizeof d->err_text2, "%s",
                 ev->text2 ? ev->text2 : "");
        break;
    case HB_SESS_EV_EXIT:
        d->exited = true;
        d->exit_code = ev->code;
        break;
    default:
        break;
    }
    hb_sess_event_clear(ev);
}

/* 排空事件直到 EXIT 或超时；返回是否见到 EXIT */
static bool drv_run(hb_session *s, drv *d, int timeout_ms)
{
    int64_t dl = now_ms() + timeout_ms;
    while (!d->exited && now_ms() < dl) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        hb_sess_event ev;
        int r = poll(&pf, 1, 50);
        if (r > 0 && (pf.revents & POLLIN))
            while (hb_session_next_event(s, &ev))
                drv_take(d, s, &ev);
    }
    /* EXIT 后再排空一次 */
    {
        hb_sess_event ev;
        while (hb_session_next_event(s, &ev))
            drv_take(d, s, &ev);
    }
    return d->exited;
}

/* 排空事件直到屏幕上出现 needle 或超时（事件仍记录进 drv） */
static bool wait_screen(hb_session *s, drv *d, const char *needle,
                        int timeout_ms)
{
    int64_t dl = now_ms() + timeout_ms;
    while (now_ms() < dl) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        hb_sess_event ev;
        hb_vt_match *m = NULL;
        size_t n;
        int r = poll(&pf, 1, 50);
        if (r > 0 && (pf.revents & POLLIN))
            while (hb_session_next_event(s, &ev))
                drv_take(d, s, &ev);
        hb_vt_lock(hb_session_vt(s));
        n = hb_vt_search(hb_session_vt(s), needle, false, &m);
        free(m);
        hb_vt_unlock(hb_session_vt(s));
        if (n)
            return true;
        if (d->exited)
            return false;
    }
    return false;
}

static hb_ssh_opts base_opts(void)
{
    hb_ssh_opts o;
    memset(&o, 0, sizeof o);
    o.target.host = "127.0.0.1";
    o.target.port = fix_port;
    o.target.user = fix_user;
    o.target.auth = HB_SSH_AUTH_KEY;
    o.target.privkey = fix_key;
    o.connect_timeout_s = 5;
    o.term = "xterm-256color";
    return o;
}

/* ------------------------------ 测试 ------------------------------ */

/* 1. TOFU：未知主机 → HOSTKEY code 0 + 指纹/randomart → 接受 → exec → exit 5 */
static void test_tofu(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    char want2[300], want_extra[9000];

    o.command = "echo hi; exit 5";
    drv_init(&d);
    d.hk_reply = 1;
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));

    CHECK_EQ_INT(d.hk_n, 1);
    CHECK_EQ_INT(d.hk_code_last, 0);
    {
        char hp[64];
        snprintf(hp, sizeof hp, "127.0.0.1:%d", fix_port);
        CHECK_EQ_STR(d.hk_text, hp);
    }
    snprintf(want2, sizeof want2, "ssh-ed25519 %s", fix_fp);
    CHECK_EQ_STR(d.hk_text2, want2);
    /* extra = known_hosts 行 + randomart */
    snprintf(want_extra, sizeof want_extra, "%s\n%s", fix_khline, fix_art);
    CHECK_EQ_STR(d.hk_extra, want_extra);
    CHECK_EQ_INT(d.connected, 1);
    CHECK_EQ_INT(d.exit_code, 5);
    CHECK(d.stages_t >= 4);
    {
        /* 屏幕内容已随 EXIT 排入 vt；直接搜 */
        hb_vt_match *m = NULL;
        size_t n;
        hb_vt_lock(hb_session_vt(s));
        n = hb_vt_search(hb_session_vt(s), "hi", false, &m);
        free(m);
        hb_vt_unlock(hb_session_vt(s));
        CHECK(n > 0);
    }
    hb_session_free(s);
}

/* 2. 已知主机匹配 → 无 HOSTKEY 事件，直连 */
static void test_known_match(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    o.known_lines = kl;
    o.nknown = 1;
    o.command = "echo ok; exit 0";
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));
    CHECK_EQ_INT(d.hk_n, 0);
    CHECK_EQ_INT(d.connected, 1);
    CHECK_EQ_INT(d.exit_code, 0);
    hb_session_free(s);
}

/* 3. 密钥变更 → HOSTKEY code 1；拒绝 → HOSTKEY_CHANGED */
static void test_mismatch_reject(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline_bad };

    o.known_lines = kl;
    o.nknown = 1;
    o.command = "echo x; exit 0";
    drv_init(&d);
    d.hk_reply = 0;          /* 拒绝 */
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));
    CHECK_EQ_INT(d.hk_n, 1);
    CHECK_EQ_INT(d.hk_code_last, 1);
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_HOSTKEY_CHANGED);
    CHECK_EQ_INT(d.connected, 0);
    CHECK(strstr(d.err_text, "中间人") != NULL || d.err_text[0]);
    hb_session_free(s);
}

/* 4. 未知主机拒绝 → HOSTKEY_REJECTED，无 CONNECTED */
static void test_unknown_reject(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;

    o.command = "echo x; exit 0";
    drv_init(&d);
    d.hk_reply = 0;
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));
    CHECK_EQ_INT(d.hk_n, 1);
    CHECK_EQ_INT(d.hk_code_last, 0);
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_HOSTKEY_REJECTED);
    CHECK_EQ_INT(d.connected, 0);
    CHECK(d.exited);
    hb_session_free(s);
}

/* 5a. 加密内存密钥：无口令 → PROMPT code 1 → 回复 pw123 → 连上 */
static void test_key_prompt_ok(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    o.known_lines = kl;
    o.nknown = 1;
    o.target.privkey = fix_key_enc;
    o.target.passphrase = NULL;
    o.command = "echo enc; exit 0";
    drv_init(&d);
    d.prompt_replies[0] = "pw123";
    d.n_prompts_set = 1;
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));
    CHECK(d.prompt_n >= 1);
    if (d.prompt_n >= 1)
        CHECK_EQ_INT(d.prompt_codes[0], 1);
    CHECK_EQ_INT(d.connected, 1);
    CHECK_EQ_INT(d.exit_code, 0);
    hb_session_free(s);
}

/* 5b. 加密内存密钥：错误口令 → 再提示 → NULL 回复 → CANCELLED */
static void test_key_prompt_cancel(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    o.known_lines = kl;
    o.nknown = 1;
    o.target.privkey = fix_key_enc;
    o.command = "echo enc; exit 0";
    drv_init(&d);
    d.prompt_replies[0] = "wrong";
    d.prompt_replies[1] = NULL;   /* 第二次取消 */
    d.n_prompts_set = 2;
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));
    CHECK(d.prompt_n >= 2);
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_CANCELLED);
    CHECK_EQ_INT(d.connected, 0);
    hb_session_free(s);
}

/* 6. 未授权密钥 → AUTH 错误，text 为中文人类可读 */
static void test_auth_failed(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    o.known_lines = kl;
    o.nknown = 1;
    o.target.privkey = fix_key_bad;
    o.command = "echo x; exit 0";
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 10000));
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_AUTH);
    CHECK(strstr(d.err_text, "认证失败") != NULL);
    CHECK(strstr(d.err_text, "libssh2") == NULL);
    CHECK_EQ_INT(d.connected, 0);
    hb_session_free(s);
}

/* 7. 关闭端口 → REFUSED；不存在域名 → RESOLVE */
static void test_refused_resolve(void)
{
    hb_ssh_opts o;
    hb_session *s;
    drv d;
    int dead = free_port();

    o = base_opts();
    o.target.port = dead;          /* 随机空闲端口 → 拒绝 */
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (s) {
        CHECK(drv_run(s, &d, 10000));
        CHECK_EQ_INT(d.err_code, HB_SSH_ERR_REFUSED);
        CHECK(d.exited);
        hb_session_free(s);
    }

    o = base_opts();
    o.target.host = "nonexistent.invalid";
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (s) {
        CHECK(drv_run(s, &d, 15000));
        CHECK_EQ_INT(d.err_code, HB_SSH_ERR_RESOLVE);
        CHECK(d.exited);
        hb_session_free(s);
    }
}

/* 8. 两级 ProxyJump（同一 sshd）→ 连接并跑命令；'J' stage 事件 */
static void test_proxyjump(void)
{
    hb_ssh_opts o = base_opts();
    hb_ssh_hop jumps[2];
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    memset(jumps, 0, sizeof jumps);
    jumps[0] = o.target;
    jumps[0].label = "hop1";
    jumps[1] = o.target;
    jumps[1].label = "hop2";
    o.jumps = jumps;
    o.njumps = 2;
    o.known_lines = kl;
    o.nknown = 1;
    o.command = "echo jumped; exit 0";
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 20000));
    CHECK_EQ_INT(d.connected, 1);
    CHECK_EQ_INT(d.err_code, 0);
    CHECK_EQ_INT(d.exit_code, 0);
    CHECK(d.stages_j >= 6);          /* 每个跳 ≥3 个 stage（tcp/handshake/auth） */
    {
        hb_vt_match *m = NULL;
        size_t n;
        hb_vt_lock(hb_session_vt(s));
        n = hb_vt_search(hb_session_vt(s), "jumped", false, &m);
        free(m);
        hb_vt_unlock(hb_session_vt(s));
        CHECK(n > 0);
    }
    hb_session_free(s);
}

/* 8b. ProxyJump 大流量回归：300KB base64 必须完整送达（pump 缓冲尾包） */
static void test_jump_bulk(void)
{
    hb_ssh_opts o = base_opts();
    hb_ssh_hop jumps[2];
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    memset(jumps, 0, sizeof jumps);
    jumps[0] = o.target;
    jumps[1] = o.target;
    o.jumps = jumps;
    o.njumps = 2;
    o.known_lines = kl;
    o.nknown = 1;
    /* /dev/zero → base64 全 'A'：300000B → 400000 字符 ≈ 5263 行(76/行) */
    o.command = "head -c 300000 /dev/zero | base64 -w 76; echo END";
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 8000);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 30000));
    CHECK_EQ_INT(d.connected, 1);
    CHECK_EQ_INT(d.exit_code, 0);

    /* END 出现在屏幕上，且 76-'A' 满行数量达标 → 流未被截断 */
    {
        hb_vt *vt = hb_session_vt(s);
        char *txt;
        int full = 0;

        hb_vt_lock(vt);
        txt = hb_vt_get_text(vt, -hb_vt_scrollback_count(vt), 0,
                             23, 79, false);
        hb_vt_unlock(vt);
        CHECK(txt != NULL);
        if (txt) {
            char *save = NULL;
            const char *ln;

            /* strstr 必须在 strtok_r 之前：后者会把 '\n' 改成 '\0' */
            CHECK(strstr(txt, "END") != NULL);
            ln = strtok_r(txt, "\n", &save);
            while (ln) {
                size_t l = strlen(ln), i;
                bool all_a = l >= 70;
                for (i = 0; all_a && i < l; i++)
                    if (ln[i] != 'A')
                        all_a = false;
                if (all_a)
                    full++;
                ln = strtok_r(NULL, "\n", &save);
            }
            CHECK(full >= 5000);
            free(txt);
        }
    }
    hb_session_free(s);
}

/* 9. 交互 shell：TERM / resize / exit */
static void test_interactive(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    o.known_lines = kl;
    o.nknown = 1;
    o.command = NULL;
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    /* 等 CONNECTED 再输入 */
    {
        int64_t dl = now_ms() + 8000;
        while (!d.connected && now_ms() < dl && !d.exited) {
            struct pollfd pf = { .fd = hb_session_event_fd(s),
                                 .events = POLLIN, .revents = 0 };
            hb_sess_event ev;
            if (poll(&pf, 1, 50) > 0 && (pf.revents & POLLIN))
                while (hb_session_next_event(s, &ev))
                    drv_take(&d, s, &ev);
        }
    }
    CHECK_EQ_INT(d.connected, 1);

    hb_session_write(s, "echo $TERM\n", 11);
    CHECK(wait_screen(s, &d, "xterm-256color", 5000));

    hb_session_resize(s, 40, 120);
    usleep(300000);
    hb_session_write(s, "stty size\n", 10);
    CHECK(wait_screen(s, &d, "40 120", 5000));

    hb_session_write(s, "exit\n", 5);
    CHECK(drv_run(s, &d, 10000));
    CHECK_EQ_INT(d.exit_code, 0);
    hb_session_free(s);
}

/* 10a. 不可达目标 + 200ms 内 close → 迅速结束（CANCELLED 或 UNREACHABLE） */
static void test_cancel_connect(void)
{
    hb_ssh_opts o;
    hb_session *s;
    drv d;
    int64_t t0;

    memset(&o, 0, sizeof o);
    o.target.host = "10.255.255.1";
    o.target.port = 22;
    o.target.user = fix_user;
    o.target.auth = HB_SSH_AUTH_KEY;
    o.target.privkey = fix_key;
    o.connect_timeout_s = 30;
    drv_init(&d);
    t0 = now_ms();
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    usleep(150000);
    hb_session_close(s);
    CHECK(drv_run(s, &d, 5000));
    CHECK(now_ms() - t0 < 3000);                 /* free 也要快 */
    {
        int64_t tf = now_ms();
        hb_session_free(s);
        CHECK(now_ms() - tf < 1000);
    }
}

/* 10b. HOSTKEY 悬起时 close → CANCELLED */
static void test_cancel_during_hostkey(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    int64_t t0;

    o.command = "echo x; exit 0";
    drv_init(&d);          /* hk_reply = -1：不回应 */
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    /* 等 HOSTKEY 事件到达再 close */
    {
        int64_t dl = now_ms() + 8000;
        while (!d.hk_n && now_ms() < dl) {
            struct pollfd pf = { .fd = hb_session_event_fd(s),
                                 .events = POLLIN, .revents = 0 };
            hb_sess_event ev;
            if (poll(&pf, 1, 50) > 0 && (pf.revents & POLLIN))
                while (hb_session_next_event(s, &ev))
                    drv_take(&d, s, &ev);
        }
    }
    CHECK_EQ_INT(d.hk_n, 1);
    t0 = now_ms();
    hb_session_close(s);
    CHECK(drv_run(s, &d, 5000));
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_CANCELLED);
    {
        int64_t tf = now_ms();
        hb_session_free(s);
        CHECK(now_ms() - tf < 1000);
    }
    CHECK(now_ms() - t0 < 2000);
}

/* 11. keepalive=1s：~2.5s 空闲后仍正常 */
static void test_keepalive(void)
{
    hb_ssh_opts o = base_opts();
    hb_session *s;
    drv d;
    const char *kl[] = { fix_khline };

    o.known_lines = kl;
    o.nknown = 1;
    o.keepalive_s = 1;
    o.command = "sleep 3; echo alive; exit 0";
    drv_init(&d);
    s = hb_session_new_ssh(&o, 24, 80, 200);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(drv_run(s, &d, 15000));
    CHECK_EQ_INT(d.connected, 1);
    CHECK_EQ_INT(d.exit_code, 0);
    CHECK_EQ_INT(d.err_code, 0);
    hb_session_free(s);
}

/* ------------------------------ parse_target 单元 ------------------------------ */

static void test_parse_target(void)
{
    char *u = NULL, *h = NULL;
    int p = 0;

    CHECK_EQ_INT(hb_ssh_parse_target("root@10.0.0.5:2222", &u, &h, &p), HB_OK);
    CHECK_EQ_STR(u, "root");
    CHECK_EQ_STR(h, "10.0.0.5");
    CHECK_EQ_INT(p, 2222);
    free(u); free(h); u = h = NULL; p = 0;

    CHECK_EQ_INT(hb_ssh_parse_target("example.com", &u, &h, &p), HB_OK);
    CHECK(u == NULL);
    CHECK_EQ_STR(h, "example.com");
    CHECK_EQ_INT(p, 0);
    free(h); h = NULL;

    CHECK_EQ_INT(hb_ssh_parse_target("user@[::1]:2200", &u, &h, &p), HB_OK);
    CHECK_EQ_STR(u, "user");
    CHECK_EQ_STR(h, "::1");
    CHECK_EQ_INT(p, 2200);
    free(u); free(h); u = h = NULL; p = 0;

    CHECK_EQ_INT(hb_ssh_parse_target("[fe80::1]", &u, &h, &p), HB_OK);
    CHECK_EQ_STR(h, "fe80::1");
    CHECK_EQ_INT(p, 0);
    free(h); h = NULL;

    CHECK_EQ_INT(hb_ssh_parse_target("host:22", &u, &h, &p), HB_OK);
    CHECK_EQ_STR(h, "host");
    CHECK_EQ_INT(p, 22);
    free(h); h = NULL;

    /* 裸 IPv6 无括号 → 当 host，无端口 */
    CHECK_EQ_INT(hb_ssh_parse_target("fe80::dead:beef", &u, &h, &p), HB_OK);
    CHECK_EQ_STR(h, "fe80::dead:beef");
    CHECK_EQ_INT(p, 0);
    free(h); h = NULL;

    CHECK(hb_ssh_parse_target("", &u, &h, &p) != HB_OK);
    CHECK(hb_ssh_parse_target("@host", &u, &h, &p) != HB_OK);
    CHECK(hb_ssh_parse_target("user@", &u, &h, &p) != HB_OK);
    CHECK(hb_ssh_parse_target("host:abc", &u, &h, &p) != HB_OK);
    CHECK(hb_ssh_parse_target("host:99999", &u, &h, &p) != HB_OK);
    CHECK(hb_ssh_parse_target("[v6", &u, &h, &p) != HB_OK);

    /* err_name */
    CHECK_EQ_STR(hb_ssh_err_name(HB_SSH_ERR_AUTH), "auth_failed");
    CHECK_EQ_STR(hb_ssh_err_name(HB_SSH_ERR_HOSTKEY_CHANGED),
                 "hostkey_changed");
    CHECK_EQ_STR(hb_ssh_err_name(HB_SSH_ERR_CANCELLED), "cancelled");
    CHECK_EQ_STR(hb_ssh_err_name(HB_SSH_ERR_TIMEOUT), "timeout");
}

/* ------------------------------ main ------------------------------ */

int main(void)
{
    fixture_setup();
    test_parse_target();        /* 不依赖 sshd 的先跑 */
    test_tofu();
    test_known_match();
    test_mismatch_reject();
    test_unknown_reject();
    test_key_prompt_ok();
    test_key_prompt_cancel();
    test_auth_failed();
    test_refused_resolve();
    test_proxyjump();
    test_jump_bulk();
    test_interactive();
    test_cancel_connect();
    test_cancel_during_hostkey();
    test_keepalive();
    fixture_teardown();
    fprintf(stderr, "test_ssh: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
