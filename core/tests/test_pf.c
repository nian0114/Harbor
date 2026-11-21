/* 端口转发引擎集成测试（FR-06）：用户态 sshd fixture，
 * 覆盖 local(+ProxyJump)/remote/dynamic(SOCKS5) 三种规则、
 * 并发回声、半关闭、吞吐、绑定冲突、关闭/断线收尾。
 * 本地对端用线程化 echo/sink/half-close 服务器（本机回环）。 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test.h"
#include "sshd_fixture.h"

#include "harbor/pf.h"
#include "harbor/ssh.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static hb_sshd fx;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------ 驱动 ------------------------------ */

typedef struct {
    int     connected, exited, exit_code, err_code;
    char    err_text[512];
    /* FORWARD 事件簿 */
    int     listening;
    int     listen_port;
    uint64_t st_up, st_down;
    uint32_t st_active, st_total;
    int     nstats;
    int     nwarn;
    char    warn[512];
} drv;

static void drv_init(drv *d)
{
    memset(d, 0, sizeof *d);
    d->exit_code = -999;
}

static void drv_take(drv *d, hb_sess_event *ev)
{
    switch (ev->type) {
    case HB_SESS_EV_CONNECTED:
        d->connected = 1;
        break;
    case HB_SESS_EV_ERROR:
        d->err_code = ev->code;
        if (ev->text)
            snprintf(d->err_text, sizeof d->err_text, "%s", ev->text);
        break;
    case HB_SESS_EV_EXIT:
        d->exited = 1;
        d->exit_code = ev->code;
        break;
    case HB_SESS_EV_FORWARD:
        if (ev->pf) {
            hb_pf_status *st = ev->pf;
            if (st->kind == HB_PF_EV_LISTENING) {
                d->listening = 1;
                d->listen_port = st->bound_port;
            } else if (st->kind == HB_PF_EV_STATS) {
                d->st_up = st->bytes_up;
                d->st_down = st->bytes_down;
                d->st_active = st->active;
                d->st_total = st->total;
                d->nstats++;
            } else if (st->kind == HB_PF_EV_WARN) {
                d->nwarn++;
                if (st->message)
                    snprintf(d->warn, sizeof d->warn, "%s", st->message);
            }
        }
        break;
    default:
        break;
    }
    hb_sess_event_clear(ev);
}

static void pump(hb_session *s, drv *d)
{
    hb_sess_event ev;
    while (hb_session_next_event(s, &ev))
        drv_take(d, &ev);
}

/* 等谓词为真或超时；返回最终取值（由调用方重读 d） */
static int wait_for(hb_session *s, drv *d, int (*pred)(drv *), int timeout_ms)
{
    int64_t dl = now_ms() + timeout_ms;
    while (now_ms() < dl) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        if (pred(d))
            return 1;
        if (poll(&pf, 1, 20) > 0 && (pf.revents & POLLIN))
            pump(s, d);
    }
    return pred(d);
}

static int p_connected(drv *d) { return d->connected; }
static int p_listening(drv *d) { return d->listening; }
static int p_err(drv *d) { return d->err_code != 0 || d->exited; }
static int p_exited(drv *d) { return d->exited; }

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

/* 建转发会话并等 CONNECTED+LISTENING；NULL = 失败 */
static hb_session *fwd_conn(drv *d, const hb_ssh_opts *o,
                            const hb_pf_rule *r, int *bport)
{
    hb_session *s;
    char *err = NULL;
    const char *kl[] = { fx.khline };
    hb_ssh_opts oo = *o;
    oo.known_lines = kl;
    oo.nknown = 1;
    s = hb_session_new_forward(&oo, r, bport, &err);
    if (!s) {
        fprintf(stderr, "new_forward failed: %s\n", err ? err : "?");
        free(err);
        return NULL;
    }
    if (!wait_for(s, d, p_connected, 15000)) {
        hb_session_free(s);
        return NULL;
    }
    if (r->kind == HB_PF_REMOTE && !wait_for(s, d, p_listening, 5000)) {
        hb_session_free(s);
        return NULL;
    }
    return s;
}

/* ------------------------------ TCP 工具 ------------------------------ */

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int port;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (void *)&a, sizeof a) || getsockname(fd, (void *)&a, &al)) {
        close(fd);
        return 0;
    }
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int tcp_conn(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0)
        return -1;
    if (connect(fd, (void *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 读满 len 或 EOF；返回读到的字节数 */
static size_t read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    return got;
}

/* ------------------------------ echo / sink 服务器 ------------------------------ */

typedef struct {
    int      lfd, port;
    volatile int stop;
    pthread_t th;
    int      mode;          /* 0=echo 1=sink */
    volatile uint64_t nbytes;   /* sink 收到的字节数 */
} srv;

typedef struct { srv *s; int fd; } warg;

static void *echo_worker(void *arg)
{
    int fd = ((warg *)arg)->fd;
    free(arg);
    for (;;) {
        char buf[65536];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        if (write_all(fd, buf, (size_t)n) != 0)
            break;
    }
    close(fd);
    return NULL;
}

static void *sink_worker(void *arg)
{
    warg *w = arg;
    int fd = w->fd;
    srv *s = w->s;
    free(arg);
    for (;;) {
        char buf[65536];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        s->nbytes += (uint64_t)n;
    }
    close(fd);
    return NULL;
}

static void *accept_loop(void *arg)
{
    srv *s = arg;
    while (!s->stop) {
        warg *w = malloc(sizeof *w);
        pthread_t t;
        if (!w)
            break;
        w->s = s;
        w->fd = accept(s->lfd, NULL, NULL);
        if (w->fd < 0) {
            free(w);
            if (s->stop)
                break;
            usleep(2000);
            continue;
        }
        if (pthread_create(&t, NULL,
                           s->mode == 1 ? sink_worker : echo_worker,
                           w) != 0) {
            close(w->fd);
            free(w);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

static int srv_start(srv *s, int mode)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(s, 0, sizeof *s);
    s->mode = mode;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->lfd, (void *)&a, sizeof a) ||
        getsockname(s->lfd, (void *)&a, &al) ||
        listen(s->lfd, 32)) {
        close(s->lfd);
        s->lfd = -1;
        return -1;
    }
    s->port = ntohs(a.sin_port);
    if (pthread_create(&s->th, NULL, accept_loop, s) != 0) {
        close(s->lfd);
        s->lfd = -1;
        return -1;
    }
    return 0;
}

static void srv_stop(srv *s)
{
    s->stop = 1;
    if (s->lfd >= 0) {
        shutdown(s->lfd, SHUT_RDWR);
        close(s->lfd);
        s->lfd = -1;
    }
    pthread_join(s->th, NULL);
}

/* half-close 服务器：读至 EOF → 回 reply → 关（单连接、一次性） */
typedef struct {
    int      lfd, port;
    pthread_t th;
    char     reply[128];
} hcsrv;

static void *hc_worker(void *arg)
{
    hcsrv *s = arg;
    char buf[4096];
    int fd = accept(s->lfd, NULL, NULL);
    if (fd < 0)
        return NULL;
    while (read(fd, buf, sizeof buf) > 0)
        ;
    write_all(fd, s->reply, strlen(s->reply));
    close(fd);
    return NULL;
}

static int hc_start(hcsrv *s, const char *reply)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(s, 0, sizeof *s);
    snprintf(s->reply, sizeof s->reply, "%s", reply);
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->lfd, (void *)&a, sizeof a) ||
        getsockname(s->lfd, (void *)&a, &al) ||
        listen(s->lfd, 4)) {
        close(s->lfd);
        return -1;
    }
    s->port = ntohs(a.sin_port);
    if (pthread_create(&s->th, NULL, hc_worker, s) != 0) {
        close(s->lfd);
        return -1;
    }
    return 0;
}

static void hc_stop(hcsrv *s)
{
    pthread_join(s->th, NULL);
    if (s->lfd >= 0)
        close(s->lfd);
}

/* ------------------------------ 用例 ------------------------------ */

/* echo 客户端线程：发 len 字节随机数，逐字节回读校验 */
typedef struct {
    int            port;
    const uint8_t *data;
    size_t         len;
    int            ok;
} ejob;

static void *echo_client(void *arg)
{
    ejob *j = arg;
    uint8_t *got;
    int fd = tcp_conn(j->port);
    if (fd < 0)
        return NULL;
    if (write_all(fd, j->data, j->len) != 0)
        goto out;
    got = malloc(j->len);
    if (!got)
        goto out;
    if (read_all(fd, got, j->len) == j->len &&
        memcmp(got, j->data, j->len) == 0)
        j->ok = 1;
    free(got);
out:
    close(fd);
    return NULL;
}

/* 1. local 经 ProxyJump：8 并发各 1MiB 随机回声 */
static void t_local_jump(void)
{
    srv es;
    hb_pf_rule r;
    hb_ssh_opts o;
    hb_ssh_hop j;
    drv d;
    hb_session *s;
    int bp = 0, i;
    uint8_t *data;
    ejob jobs[8];
    pthread_t ths[8];
    int64_t dl;

    CHECK(srv_start(&es, 0) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = es.port;
    o = base_opts();
    j = o.target;              /* fixture 同时当跳板与目标 */
    o.jumps = &j;
    o.njumps = 1;

    data = malloc(1u << 20);
    CHECK(data != NULL);
    srand(12345);
    if (data)
        for (i = 0; i < (1 << 20); i++)
            data[i] = (uint8_t)rand();

    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    CHECK(bp > 0);
    if (!s || bp <= 0 || !data)
        goto out;

    for (i = 0; i < 8; i++) {
        jobs[i].port = bp;
        jobs[i].data = data;
        jobs[i].len = 1u << 20;
        jobs[i].ok = 0;
        CHECK(pthread_create(&ths[i], NULL, echo_client, &jobs[i]) == 0);
    }
    for (i = 0; i < 8; i++)
        pthread_join(ths[i], NULL);
    for (i = 0; i < 8; i++)
        CHECK(jobs[i].ok);

    /* 终态 STATS：8 连接、双向各 ≥8MiB */
    dl = now_ms() + 8000;
    while (now_ms() < dl &&
           !(d.st_total >= 8 && d.st_up >= (8u << 20) &&
             d.st_down >= (8u << 20))) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, 50) > 0)
            pump(s, &d);
    }
    CHECK(d.st_total >= 8);
    CHECK(d.st_up >= (8u << 20));
    CHECK(d.st_down >= (8u << 20));
    CHECK(d.listening);
    CHECK(d.listen_port == bp);
out:
    if (s)
        hb_session_free(s);
    free(data);
    srv_stop(&es);
}

/* 2. 吞吐：64MiB 单向 → sink，打印 MB/s */
static void t_throughput(void)
{
    srv sk;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    int bp = 0, fd;
    uint64_t total = 64ull << 20, sent = 0;
    uint8_t buf[1 << 20];
    int64_t t0, dl;
    double mbs;

    CHECK(srv_start(&sk, 1) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = sk.port;
    o = base_opts();
    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    if (!s || bp <= 0)
        goto out;

    memset(buf, 0xA5, sizeof buf);
    t0 = now_ms();
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    if (fd < 0)
        goto out;
    while (sent < total) {
        size_t n = (size_t)((total - sent) > sizeof buf ? sizeof buf
                                                       : (total - sent));
        if (write_all(fd, buf, n) != 0)
            break;
        sent += n;
    }
    shutdown(fd, SHUT_WR);
    close(fd);
    {
        double dt = (double)(now_ms() - t0) / 1000.0;
        mbs = dt > 0 ? (double)sent / dt / (1 << 20) : 0;
    }
    /* 等 sink 收完（它的 EOF 来自 sshd 侧 channel 收尾） */
    dl = now_ms() + 15000;
    {
        volatile uint64_t *nb = &sk.nbytes;
        while (now_ms() < dl && *nb < total)
            usleep(50000);
        CHECK(sent == total);
        CHECK(*nb == total);
    }
    printf("  throughput: %.1f MB/s (%.0f MiB in %.2fs)\n",
           mbs, (double)sent / (1 << 20),
           (double)(now_ms() - t0) / 1000.0);
out:
    if (s)
        hb_session_free(s);
    srv_stop(&sk);
}

/* 3. 半关闭：客户端发完 SHUT_WR → 服务端读到 EOF 回复 → 客户端收全 + EOF */
static void t_halfclose(void)
{
    hcsrv hs;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    int bp = 0, fd;
    char buf[4096];
    size_t got;

    CHECK(hc_start(&hs, "pong-payload-0123456789") == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = hs.port;
    o = base_opts();
    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    if (!s || bp <= 0)
        goto out;

    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    if (fd < 0)
        goto out;
    CHECK(write_all(fd, "ping", 4) == 0);
    CHECK(shutdown(fd, SHUT_WR) == 0);
    got = read_all(fd, buf, sizeof buf);   /* 读到 EOF 为止 */
    CHECK(got == strlen("pong-payload-0123456789"));
    CHECK(memcmp(buf, "pong-payload-0123456789", got) == 0);
    close(fd);
out:
    if (s)
        hb_session_free(s);
    hc_stop(&hs);
}

/* SOCKS 握手工具：完成问候 + CONNECT；返回 reply 码（<0 出错），
 * fd 保持打开给调用方继续用。req[] 已是完整请求体（cmd/atyp/addr/port）。 */
static int s5_handshake(int fd, const uint8_t *req, size_t rlen)
{
    uint8_t g[3] = { 0x05, 0x01, 0x00 };
    uint8_t buf[32];
    if (write_all(fd, g, sizeof g) != 0)
        return -1;
    if (read_all(fd, buf, 2) != 2 || buf[0] != 0x05 || buf[1] != 0x00)
        return -2;
    if (write_all(fd, req, rlen) != 0)
        return -3;
    memset(buf, 0, sizeof buf);
    if (read_all(fd, buf, 10) != 10 || buf[0] != 0x05)
        return -4;
    return buf[1];
}

static void s5_req_v4(uint8_t *req, uint8_t cmd, uint32_t ip_be, uint16_t port)
{
    req[0] = 0x05;
    req[1] = cmd;
    req[2] = 0x00;
    req[3] = 0x01;
    memcpy(req + 4, &ip_be, 4);
    req[8] = (uint8_t)(port >> 8);
    req[9] = (uint8_t)port;
}

/* 4. dynamic SOCKS5 */
static void t_dynamic(void)
{
    srv es;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    int bp = 0, fd, rep;
    uint8_t req[280];
    uint8_t buf[64];

    CHECK(srv_start(&es, 0) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_DYNAMIC;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    o = base_opts();
    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    if (!s || bp <= 0)
        goto out;

    /* ATYP IPv4 → echo */
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    s5_req_v4(req, 0x01, htonl(INADDR_LOOPBACK), (uint16_t)es.port);
    rep = s5_handshake(fd, req, 10);
    CHECK_EQ_INT(rep, 0x00);
    CHECK(write_all(fd, "ABCD", 4) == 0);
    CHECK(read_all(fd, buf, 4) == 4 && memcmp(buf, "ABCD", 4) == 0);
    close(fd);

    /* ATYP domain "localhost" → echo */
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    {
        size_t n = 0;
        req[n++] = 0x05; req[n++] = 0x01; req[n++] = 0x00; req[n++] = 0x03;
        req[n++] = 9;
        memcpy(req + n, "localhost", 9); n += 9;
        req[n++] = (uint8_t)(es.port >> 8);
        req[n++] = (uint8_t)es.port;
        rep = s5_handshake(fd, req, n);
    }
    CHECK_EQ_INT(rep, 0x00);
    CHECK(write_all(fd, "WXYZ", 4) == 0);
    CHECK(read_all(fd, buf, 4) == 4 && memcmp(buf, "WXYZ", 4) == 0);
    close(fd);

    /* 流水线：一次 write 装下 greeting + CONNECT + 先行负载 ——
     * 问候后剩余字节不能丢；请求后的负载必须在 channel 打开后
     * 最先发出（echo 回来，不丢不重排） */
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    {
        uint8_t pipe_buf[64];
        size_t n = 0;
        const char pay[] = "PIPE1234";
        /* greeting */
        pipe_buf[n++] = 0x05; pipe_buf[n++] = 0x01; pipe_buf[n++] = 0x00;
        /* CONNECT domain */
        pipe_buf[n++] = 0x05; pipe_buf[n++] = 0x01; pipe_buf[n++] = 0x00;
        pipe_buf[n++] = 0x03; pipe_buf[n++] = 9;
        memcpy(pipe_buf + n, "localhost", 9); n += 9;
        pipe_buf[n++] = (uint8_t)(es.port >> 8);
        pipe_buf[n++] = (uint8_t)es.port;
        /* 先行负载（客户端不等应答就发的上行数据） */
        memcpy(pipe_buf + n, pay, 8); n += 8;
        CHECK(write_all(fd, pipe_buf, n) == 0);
        /* 2 字节问候应答 + 10 字节 CONNECT 应答 + 8 字节回显 */
        memset(buf, 0, sizeof buf);
        CHECK(read_all(fd, buf, 2) == 2);
        CHECK(buf[0] == 0x05 && buf[1] == 0x00);
        memset(buf, 0, sizeof buf);
        CHECK(read_all(fd, buf, 10) == 10);
        CHECK(buf[0] == 0x05 && buf[1] == 0x00);
        memset(buf, 0, sizeof buf);
        CHECK(read_all(fd, buf, 8) == 8);
        CHECK(memcmp(buf, pay, 8) == 0);
    }
    close(fd);

    /* 域名含 NUL / 控制字节 → 0x01 */
    {
        static const uint8_t badnames[][8] = {
            { 'h', 'o', 0x00, 's', 't' },        /* NUL 内嵌 */
            { 'h', 0x01, 's', 't' },             /* <0x20 */
            { 'h', 0x1F, 's', 't' },
        };
        size_t bi;
        for (bi = 0; bi < sizeof badnames / sizeof badnames[0]; bi++) {
            size_t n = 0;
            fd = tcp_conn(bp);
            CHECK(fd >= 0);
            req[n++] = 0x05; req[n++] = 0x01; req[n++] = 0x00;
            req[n++] = 0x03;
            req[n++] = 5;
            memcpy(req + n, badnames[bi], 5); n += 5;
            req[n++] = (uint8_t)(es.port >> 8);
            req[n++] = (uint8_t)es.port;
            rep = s5_handshake(fd, req, n);
            CHECK_EQ_INT(rep, 0x01);
            close(fd);
        }
    }

    /* 关闭端口 → 失败码（refused=0x05 或非零） */
    {
        int dead = free_port();
        fd = tcp_conn(bp);
        CHECK(fd >= 0);
        s5_req_v4(req, 0x01, htonl(INADDR_LOOPBACK), (uint16_t)dead);
        rep = s5_handshake(fd, req, 10);
        CHECK(rep != 0);
        close(fd);
    }

    /* BIND 命令 → 0x07 */
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    s5_req_v4(req, 0x02, htonl(INADDR_LOOPBACK), (uint16_t)es.port);
    rep = s5_handshake(fd, req, 10);
    CHECK_EQ_INT(rep, 0x07);
    close(fd);

    /* 不提供 no-auth → 0xFF */
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    {
        uint8_t g[3] = { 0x05, 0x01, 0x02 };   /* 只提供 gssapi */
        CHECK(write_all(fd, g, sizeof g) == 0);
        CHECK(read_all(fd, buf, 2) == 2);
        CHECK(buf[0] == 0x05 && buf[1] == 0xFF);
    }
    close(fd);

    /* 垃圾输入 → 连接被关 */
    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    CHECK(write_all(fd, "GET / HTTP/1.0\r\n\r\n", 18) == 0);
    {
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        ssize_t n = -2;
        if (poll(&p, 1, 3000) > 0)
            n = read(fd, buf, sizeof buf);
        CHECK(n <= 0);              /* EOF 或读错，不给应答 */
    }
    close(fd);
out:
    if (s)
        hb_session_free(s);
    srv_stop(&es);
}

/* 5. remote：服务器侧挑端口 → 连接回环数据 */
static void t_remote(void)
{
    srv es;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    int fd;
    char buf[64];

    CHECK(srv_start(&es, 0) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_REMOTE;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = es.port;
    o = base_opts();
    s = fwd_conn(&d, &o, &r, NULL);
    CHECK(s != NULL);
    CHECK(d.listen_port > 0);
    if (!s || d.listen_port <= 0)
        goto out;

    fd = tcp_conn(d.listen_port);
    CHECK(fd >= 0);
    if (fd < 0)
        goto out;
    CHECK(write_all(fd, "remote-echo!", 12) == 0);
    CHECK(read_all(fd, buf, 12) == 12);
    CHECK(memcmp(buf, "remote-echo!", 12) == 0);
    close(fd);
out:
    if (s)
        hb_session_free(s);
    srv_stop(&es);
}

/* 6. remote 被拒：AllowTcpForwarding local → HB_SSH_ERR_FORWARD */
static void t_remote_denied(void)
{
    hb_sshd fx2;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s = NULL;
    char *err = NULL;
    const char *kl[1];

    memset(&fx2, 0, sizeof fx2);
    snprintf(fx2.extra_cfg, sizeof fx2.extra_cfg,
             "AllowTcpForwarding local\n");
    if (hb_sshd_start(&fx2) != 0) {
        fprintf(stderr, "fx2 start failed\n");
        CHECK(0);
        return;
    }
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_REMOTE;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = 80;
    o = base_opts();
    o.target.port = fx2.port;
    o.target.user = fx2.user;
    o.target.privkey = fx2.key;
    kl[0] = fx2.khline;
    o.known_lines = kl;
    o.nknown = 1;

    s = hb_session_new_forward(&o, &r, NULL, &err);
    free(err);
    if (s) {
        wait_for(s, &d, p_err, 15000);
        CHECK_EQ_INT(d.err_code, HB_SSH_ERR_FORWARD);
        CHECK(strstr(d.err_text, "AllowTcpForwarding") != NULL);
        hb_session_free(s);
    } else {
        CHECK(0);   /* remote 绑定不在本地做 —— 会话应能创建 */
    }
    hb_sshd_stop(&fx2);
}

/* 7. 绑定冲突与地址校验 */
static void t_conflict(void)
{
    int port = free_port();
    int fd;
    char *err = NULL;
    hb_pf_rule r;
    hb_ssh_opts o;
    hb_session *s;
    struct sockaddr_in a;
    drv d;

    CHECK(port > 0);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (void *)&a, sizeof a) == 0);

    CHECK(hb_pf_probe_bind("127.0.0.1", port, &err) != HB_OK);
    CHECK(err && strstr(err, "已被占用"));
    free(err);
    err = NULL;

    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = port;
    r.dest_addr = "127.0.0.1";
    r.dest_port = 1;
    o = base_opts();
    s = hb_session_new_forward(&o, &r, NULL, &err);
    CHECK(s == NULL);
    CHECK(err && strstr(err, "已被占用"));
    free(err);
    err = NULL;

    /* 无效地址 */
    CHECK(hb_pf_probe_bind("bogus.host.name", port, &err) == HB_ERR_INVALID);
    CHECK(err != NULL);
    free(err);
    err = NULL;
    CHECK(hb_pf_probe_bind("999.999.999.999", port, NULL) == HB_ERR_INVALID);
    /* 空闲端口/localhost/v6 可绑 */
    CHECK(hb_pf_probe_bind("localhost", 0, NULL) == HB_OK);
    CHECK(hb_pf_probe_bind("::1", 0, NULL) == HB_OK);
    close(fd);
}

/* 8. close/free → 端口 5s 内可再绑、客户端见 EOF */
static void t_close(void)
{
    srv es;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    int bp = 0, fd;
    int64_t dl;

    CHECK(srv_start(&es, 0) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = es.port;
    o = base_opts();
    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    if (!s || bp <= 0)
        goto out;

    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    CHECK(write_all(fd, "Q", 1) == 0);
    {
        char b;
        CHECK(read_all(fd, &b, 1) == 1);   /* 通路确认 */
    }

    hb_session_close(s);
    hb_session_free(s);
    s = NULL;

    /* 客户端见 EOF（5s 预算） */
    {
        int eof_seen = 0;
        dl = now_ms() + 5000;
        while (now_ms() < dl && !eof_seen) {
            struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
            char b;
            if (poll(&p, 1, 100) > 0 && read(fd, &b, 1) <= 0)
                eof_seen = 1;
        }
        CHECK(eof_seen);
    }
    close(fd);

    /* 端口已释放 */
    CHECK(hb_pf_probe_bind("127.0.0.1", bp, NULL) == HB_OK);
out:
    if (s)
        hb_session_free(s);
    srv_stop(&es);
}

/* 9. 断线：杀掉 sshd 及其子进程 → HB_SSH_ERR_DISCONNECTED + 端口释放 */
static void kill_tree_sig(pid_t root, int sig)
{
    pid_t lvl[256], nxt[256], dead[512];
    int nl = 0, n_dead = 0, i, j;
    DIR *dd;
    struct dirent *de;

    lvl[nl++] = root;
    /* 先按层收集整棵树，再统一 SIGKILL —— 边杀边扫会让孙辈在下一轮
     * /proc 扫描前被 reparent 到 init，从而漏杀（sshd 的 privsep 网络
     * 子进程正是这种孙辈）。 */
    for (i = 0; i < 6 && nl; i++) {   /* BFS 6 层足够 */
        int nn = 0;
        dd = opendir("/proc");
        if (!dd)
            break;
        while ((de = readdir(dd))) {
            char pth[64], buf[512];
            pid_t p = (pid_t)atoi(de->d_name), ppid = 0;
            char *rp;
            int fd2;
            ssize_t nr;
            if (p <= 0)
                continue;
            snprintf(pth, sizeof pth, "/proc/%d/stat", (int)p);
            fd2 = open(pth, O_RDONLY);
            if (fd2 < 0)
                continue;
            nr = read(fd2, buf, sizeof buf - 1);
            close(fd2);
            if (nr <= 0)
                continue;
            buf[nr] = 0;
            rp = strrchr(buf, ')');   /* "pid (comm) S ppid" */
            if (!rp)
                continue;
            if (sscanf(rp + 1, " %*c %d", (int *)&ppid) != 1)
                continue;
            for (j = 0; j < nl; j++)
                if (ppid == lvl[j])
                    break;
            if (j < nl && nn < 256) {
                int k;
                for (k = 0; k < nn; k++)
                    if (nxt[k] == p)
                        break;
                if (k == nn)
                    nxt[nn++] = p;
            }
        }
        closedir(dd);
        for (j = 0; j < nn && n_dead < (int)(sizeof dead / sizeof dead[0]);
             j++)
            dead[n_dead++] = nxt[j];
        memcpy(lvl, nxt, sizeof(pid_t) * (size_t)nn);
        nl = nn;
    }
    for (j = 0; j < n_dead; j++)
        kill(dead[j], sig);
    kill(root, sig);
    waitpid(root, NULL, 0);
}

static void kill_tree(pid_t root)
{
    kill_tree_sig(root, SIGKILL);
}

/* 9b. 断线（优雅 FIN）：对端正常关闭只报 POLLIN|POLLRDHUP 而无
 * POLLERR/POLLHUP。keepalive 关闭时没有任何出站包 → 收不到
 * RST/POLLERR，唯一侦测途径就是 POLLRDHUP；漏订时 EOF 的
 * POLLIN 让 poll 持续空转、DISCONNECTED 永不发出（僵尸会话）
 * → 本用例在修复前必定超时失败。独立 fixture：fx 留给
 * t_disconnect 的 SIGKILL。 */
static void t_disconnect_fin(void)
{
    hb_sshd fx2;
    srv es;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s = NULL;
    char *err = NULL;
    const char *kl[1];
    int bp = 0;
    int64_t dl;

    memset(&fx2, 0, sizeof fx2);
    if (hb_sshd_start(&fx2) != 0) {
        fprintf(stderr, "fx2 start failed\n");
        CHECK(0);
        return;
    }
    CHECK(srv_start(&es, 0) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = es.port;
    o = base_opts();
    o.target.port = fx2.port;
    o.target.user = fx2.user;
    o.target.privkey = fx2.key;
    o.keepalive_s = 0;   /* 关键：无 keepalive 写 → 无 RST 兜底 */
    kl[0] = fx2.khline;
    o.known_lines = kl;
    o.nknown = 1;
    s = hb_session_new_forward(&o, &r, &bp, &err);
    free(err);
    CHECK(s != NULL && bp > 0);
    if (!s)
        goto out;
    wait_for(s, &d, p_listening, 15000);
    CHECK(d.listening);

    /* 会话上没有任何客户端连接 —— 纯 idle；SIGTERM 使 sshd 正常
     * 退出，内核按序 close socket → 对端收到 FIN。 */
    kill_tree_sig(fx2.pid, SIGTERM);
    fx2.pid = -1;                    /* hb_sshd_stop 不再 kill */

    dl = now_ms() + 10000;
    while (now_ms() < dl && d.err_code != HB_SSH_ERR_DISCONNECTED) {
        struct pollfd p = { .fd = hb_session_event_fd(s),
                            .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, 50) > 0)
            pump(s, &d);
    }
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_DISCONNECTED);

    wait_for(s, &d, p_exited, 3000);
    hb_session_free(s);
    s = NULL;
out:
    if (s)
        hb_session_free(s);
    srv_stop(&es);
    hb_sshd_stop(&fx2);
}

static void t_disconnect(void)
{
    srv es;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    int bp = 0, fd;
    int64_t dl;

    CHECK(srv_start(&es, 0) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = es.port;
    o = base_opts();
    o.keepalive_s = 1;
    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    if (!s || bp <= 0)
        goto out;

    fd = tcp_conn(bp);
    CHECK(fd >= 0);
    CHECK(write_all(fd, "Z", 1) == 0);
    {
        char b;
        CHECK(read_all(fd, &b, 1) == 1);
    }

    kill_tree(fx.pid);
    fx.pid = -1;                    /* hb_sshd_stop 不再 kill */

    /* DISCONNECTED：socket 直接挂或 keepalive 3×1s 判死 */
    dl = now_ms() + 15000;
    while (now_ms() < dl && d.err_code != HB_SSH_ERR_DISCONNECTED) {
        struct pollfd p = { .fd = hb_session_event_fd(s),
                            .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, 50) > 0)
            pump(s, &d);
    }
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_DISCONNECTED);

    /* 监听器已释放 */
    dl = now_ms() + 5000;
    while (now_ms() < dl &&
           hb_pf_probe_bind("127.0.0.1", bp, NULL) != HB_OK)
        usleep(50000);
    CHECK(hb_pf_probe_bind("127.0.0.1", bp, NULL) == HB_OK);

    wait_for(s, &d, p_exited, 3000);
    close(fd);
out:
    if (s)
        hb_session_free(s);
    srv_stop(&es);
}

/* 10. 持续流中关闭：eng_sweep 封顶 → close/free 不能被饿死 */
typedef struct {
    int              port;
    volatile int     stop;
    volatile uint64_t sent;
} flood_arg;

static void *flood_client(void *arg)
{
    flood_arg *f = arg;
    uint8_t buf[1 << 16];
    int fd;
    memset(buf, 0x5A, sizeof buf);
    fd = tcp_conn(f->port);
    if (fd < 0)
        return NULL;
    while (!f->stop) {
        /* MSG_NOSIGNAL：session 关闭会立刻断 fd，write 会 EPIPE */
        ssize_t n = send(fd, buf, sizeof buf, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        f->sent += (uint64_t)n;
    }
    close(fd);
    return NULL;
}

static void t_close_midstream(void)
{
    srv sk;
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    flood_arg fl;
    pthread_t th;
    int bp = 0, th_on = 0;
    int64_t dl, t_free;

    CHECK(srv_start(&sk, 1) == 0);
    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_LOCAL;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "127.0.0.1";
    r.dest_port = sk.port;
    o = base_opts();
    s = fwd_conn(&d, &o, &r, &bp);
    CHECK(s != NULL);
    if (!s || bp <= 0)
        goto out;

    /* 持续流：sink 无限收，eng_sweep 每轮都有进展 —— 无界的话
     * poll/取消/cmd 管道被饿死，free() 的 join 会挂住。 */
    memset(&fl, 0, sizeof fl);
    fl.port = bp;
    CHECK(pthread_create(&th, NULL, flood_client, &fl) == 0);
    th_on = 1;

    dl = now_ms() + 5000;
    while (now_ms() < dl && sk.nbytes < (1u << 20))
        usleep(5000);
    CHECK(sk.nbytes >= (1u << 20));

    hb_session_close(s);
    t_free = now_ms();
    hb_session_free(s);
    t_free = now_ms() - t_free;
    s = NULL;
    CHECK(t_free < 3000);

    dl = now_ms() + 5000;
    while (now_ms() < dl &&
           hb_pf_probe_bind("127.0.0.1", bp, NULL) != HB_OK)
        usleep(50000);
    CHECK(hb_pf_probe_bind("127.0.0.1", bp, NULL) == HB_OK);

    fl.stop = 1;
    pthread_join(th, NULL);
    th_on = 0;
out:
    if (th_on) {
        fl.stop = 1;
        pthread_join(th, NULL);
    }
    if (s)
        hb_session_free(s);
    srv_stop(&sk);
}

/* 11. remote：目标解析失败 → HB_SSH_ERR_RESOLVE（可重试，区别于
 *     服务器拒转的 FORWARD） */
static void t_remote_resolve(void)
{
    hb_pf_rule r;
    hb_ssh_opts o;
    drv d;
    hb_session *s;
    char *err = NULL;

    drv_init(&d);
    memset(&r, 0, sizeof r);
    r.kind = HB_PF_REMOTE;
    r.bind_addr = "127.0.0.1";
    r.bind_port = 0;
    r.dest_addr = "no.such.host.invalid";   /* getaddrinfo 必失败 */
    r.dest_port = 80;
    o = base_opts();
    {
        const char *kl[] = { fx.khline };
        o.known_lines = kl;
        o.nknown = 1;
        s = hb_session_new_forward(&o, &r, NULL, &err);
    }
    free(err);
    CHECK(s != NULL);
    if (!s)
        return;
    wait_for(s, &d, p_err, 20000);
    CHECK_EQ_INT(d.err_code, HB_SSH_ERR_RESOLVE);
    hb_session_free(s);
}

/* ------------------------------ main ------------------------------ */

int main(void)
{
    int r = hb_sshd_start(&fx);
    if (r == 77) {
        fprintf(stderr, "SKIP: no sshd/ssh-keygen\n");
        return 77;
    }
    if (r != 0) {
        fprintf(stderr, "FAIL: fixture start %d\n", r);
        return 1;
    }

    t_local_jump();
    printf("  t_local_jump done\n");
    t_throughput();
    printf("  t_throughput done\n");
    t_halfclose();
    printf("  t_halfclose done\n");
    t_dynamic();
    printf("  t_dynamic done\n");
    t_remote();
    printf("  t_remote done\n");
    t_remote_denied();
    printf("  t_remote_denied done\n");
    t_conflict();
    printf("  t_conflict done\n");
    t_close();
    printf("  t_close done\n");
    t_close_midstream();
    printf("  t_close_midstream done\n");
    t_remote_resolve();
    printf("  t_remote_resolve done\n");
    t_disconnect_fin();      /* 优雅 FIN 断线（独立 fixture） */
    printf("  t_disconnect_fin done\n");
    t_disconnect();          /* 杀掉 fixture sshd —— 必须最后跑 */
    printf("  t_disconnect done\n");

    hb_sshd_stop(&fx);
    printf("pf: %d checks, %d failures\n", hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
