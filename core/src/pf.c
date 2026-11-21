#ifndef _GNU_SOURCE
#define _GNU_SOURCE          /* accept4 */
#endif

/*
 * 端口转发引擎（FR-06）：HB_SSH_MODE_FORWARD 会话的运行体。
 * 复用 ssh.c 的整条连接管线（ProxyJump / TOFU / 认证 / keepalive / 取消），
 * 本文件负责：local/dynamic 本地监听、remote 服务端监听、SOCKS5 协商、
 * direct-tcpip 串行化打开、双向非阻塞数据泵、半关闭与有界收尾。
 *
 * 一条规则 = 一个会话（pfeng 挂在 hb_ssh.pf_eng 上）。
 */

#include "harbor/pf.h"
#include "ssh_priv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libssh2.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PF_BUF       (64 * 1024)   /* 每连接每方向缓冲 */
#define PF_MAX_CONN  256           /* 每规则并发连接上限 */
#define PF_BACKLOG   64
#define PF_HS_CAP    512           /* SOCKS 握手缓冲上限 */
#define PF_HS_MS     10000         /* SOCKS 握手超时 */
#define PF_WARN_MS   5000          /* WARN 节流 */
#define PF_CLOSE_MS  2000          /* 关闭时 channel 收尾总预算 */
#define PF_RS_MS     8000          /* remote 目标再解析超时 */
#define PF_OPEN_MS   30000         /* channel 打开死线 */
#define PF_TX_MS     8000          /* 出站包挂起死线：超时判链路死 */

/* conn 状态 */
#define CS_S5_GREET   0    /* SOCKS：等 VER/NMETHODS */
#define CS_S5_REQ     1    /* SOCKS：等 CONNECT 请求 */
#define CS_OPEN_Q     2    /* 排在打开队列里 */
#define CS_OPENING    3    /* direct-tcpip EAGAIN 在飞 */
#define CS_CONNECTING 4    /* remote：本地 connect() 进行中 */
#define CS_UP         5    /* 正常泵数据（含"排空后关闭"） */

typedef struct pfconn {
    struct pfconn *next, *qnext;
    int      fd;               /* 客户端/本地目的 socket；-1 无 */
    LIBSSH2_CHANNEL *ch;       /* NULL = 尚未打通 */
    int      st;
    char     dhost[280];       /* direct-tcpip 目标（EAGAIN 需同参重试） */
    int      dport;
    struct addrinfo *ai;       /* remote：本地 connect 进度光标 */
    int64_t  hs_dl;            /* 握手/连接死线 */
    int64_t  open_dl;          /* channel 打开死线 */

    uint8_t  hs[PF_HS_CAP];    /* SOCKS 握手缓冲 */
    size_t   hlen;

    uint8_t  up[PF_BUF];       /* fd → channel */
    size_t   up_len;
    uint8_t  dn[PF_BUF];       /* channel → fd */
    size_t   dn_len;

    unsigned cli_eof  : 1;     /* fd 读到 EOF（不再读） */
    unsigned eof_sent : 1;     /* channel_send_eof 已发 */
    unsigned ch_eof   : 1;     /* channel 已到 EOF（或无 channel 的排空态） */
    unsigned ch_dead  : 1;     /* channel 出错 */
    unsigned cli_shut : 1;     /* shutdown(fd, SHUT_WR) 已做 */
    unsigned dead     : 1;     /* 待回收（sweep 摘除；在飞 open 由 open_step 收） */
} pfconn;

/* 已摘除 conn 遗留的待释放 channel：channel_free 可能自己产生
 * CHANNEL_CLOSE 包（在飞 EAGAIN），且不能在别人的挂起包上误记，
 * 因而排队到 sweep 里受 tx 门控重试。 */
typedef struct pfzombie {
    LIBSSH2_CHANNEL *ch;
    struct pfzombie *next;
} pfzombie;

/* 出站挂起包的属主类别 */
enum {
    TX_NONE = 0,
    TX_W,          /* conn_pump 的 channel_write */
    TX_EOF,        /* conn_pump 的 channel_send_eof */
    TX_READ,       /* conn_pump 的 channel_read（内部 window-adjust） */
    TX_CLOSE,      /* channel_close */
    TX_FREE,       /* channel_free */
    TX_OPEN,       /* direct_tcpip */
    TX_ACCEPT,     /* channel_forward_accept */
    TX_LSTN,       /* channel_forward_listen */
    TX_CANCEL,     /* channel_forward_cancel */
    TX_KA,         /* keepalive（不可同参重试 → 代偿排空） */
    TX_ORPHAN      /* 属主 conn 已摘除（不可重试 → 代偿排空） */
};

typedef struct pfeng {
    hb_session *s;
    hb_ssh     *h;
    hop        *hp;

    hb_pf_kind  kind;
    char       *bind_addr;     /* 规范化（NULL/""/localhost → 127.0.0.1） */
    int         bind_port;
    char       *dest_addr;
    int         dest_port;
    int         bound_port;    /* local/dyn: 实绑端口；remote: 服务器端口 */

    int         lfds[8];       /* local/dynamic 监听器 */
    int         nlfds;
    LIBSSH2_LISTENER *lst;     /* remote 服务端监听句柄 */

    struct addrinfo *dest_ai;  /* remote：dest 本地解析结果 */
    int64_t     dest_retry;    /* 再解析节流时刻 */
    bool        dest_bad;      /* connect 全失败 → 下次接受前再解析 */

    pfconn     *conns;         /* 全部活动连接 */
    pfconn     *qh, *qt;       /* 待打开队列（FIFO） */
    pfconn     *opening;       /* 在飞 open（libssh2 全局只允许一个） */
    pfzombie   *dying;         /* 待 free 的 channel（conn 已摘） */

    /* libssh2 ≥1.11 会话级只有一个出站 packet 槽（session->packet.outbuf）。
     * 有包未发完时，任何参数不匹配的 transport_send 都会替它续发并把成功
     * 错记给调用方 —— 调用方负载根本没入包（丢字节），真正的属主稍后还
     * 会把同一负载再组包发出（dup）。因此所有会产生 SSH 包的调用都必须
     * 经 tx_try 放行：仅"无挂起"或"属主同参重试"可以发起。txop 记录
     * 挂起包属主，txsince 起算挂起时长。 */
    pfconn     *txc;           /* 属主 conn（engine 级 op 为 NULL） */
    LIBSSH2_CHANNEL *txch;     /* 属主 channel（conn 摘除后仍靠它重试） */
    int         txop;          /* TX_* */
    int64_t     txsince;       /* 挂起开始时刻 */

    uint64_t    bytes_up, bytes_down;
    uint32_t    active, total;
    uint64_t    em_up, em_down;
    uint32_t    em_active, em_total;
    int64_t     last_stats;
    int64_t     last_warn;
    bool        cap_warned;
    bool        conn_dead;
    bool        shutting;
} pfeng;

/* ------------------------------ 状态上报 ------------------------------ */

static void emit_st(pfeng *e, hb_pf_evkind kind, const char *msg)
{
    hb_pf_status *st = calloc(1, sizeof *st);
    char *m = msg ? strdup(msg) : NULL;
    if (!st) {
        free(m);
        return;
    }
    st->kind = kind;
    st->bound_port = e->bound_port;
    st->bytes_up = e->bytes_up;
    st->bytes_down = e->bytes_down;
    st->active = e->active;
    st->total = e->total;
    st->message = m;
    hb__session_emit_pf(e->s, st);
}

static void warnf(pfeng *e, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    int64_t now = hb_now_ms();
    if (now - e->last_warn < PF_WARN_MS)
        return;
    e->last_warn = now;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    emit_st(e, HB_PF_EV_WARN, msg);
}

static void maybe_stats(pfeng *e)
{
    int64_t now = hb_now_ms();
    if (e->active != e->em_active || e->total != e->em_total ||
        ((e->bytes_up != e->em_up || e->bytes_down != e->em_down) &&
         now - e->last_stats >= 1000)) {
        emit_st(e, HB_PF_EV_STATS, NULL);
        e->em_up = e->bytes_up;
        e->em_down = e->bytes_down;
        e->em_active = e->active;
        e->em_total = e->total;
        e->last_stats = now;
    }
}

/* ------------------------------ 出站挂起门控 ------------------------------ */

/* session->packet.outbuf 里是否有未发完的包（并顺手清理失效属主）。 */
static int tx_pending(pfeng *e)
{
    if (!(libssh2_session_block_directions(e->hp->sess) &
          LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
        e->txop = TX_NONE;
        e->txc = NULL;
        e->txch = NULL;
        return 0;
    }
    return 1;
}

static bool tx_any_channel(pfeng *e)
{
    pfconn *c;
    for (c = e->conns; c; c = c->next)
        if (c->ch && !c->dead)
            return true;
    return false;
}

/* 本生产者现在可否发起会产生 SSH 包的调用。
 * 放行条件：无挂起 / 属主同参重试。KA/ORPHAN 挂起不可重试：free/close
 * 被误记无实际损失恒放行；其余 op 有活 channel 时等 tx_drain 用空
 * window-adjust 代偿排空，无 channel 时也放行（该 op 被误记成功，
 * 影响有界 —— 如 open 卡在等确认由 open_dl 兜底）。 */
static bool tx_try(pfeng *e, pfconn *c, LIBSSH2_CHANNEL *ch, int op)
{
    if (!tx_pending(e))
        return true;
    if (e->txop == op && e->txc == c && e->txch == ch)
        return true;
    if (e->txop == TX_KA || e->txop == TX_ORPHAN)
        return op == TX_FREE || op == TX_CLOSE || !tx_any_channel(e);
    return false;
}

/* 在每个会产生包的调用之后记录属主。只在"无属主"时登记：
 * 属主重试未排完不改写（txsince 从首次挂起累计）。 */
static void tx_note(pfeng *e, pfconn *c, LIBSSH2_CHANNEL *ch, int op)
{
    if (tx_pending(e) && e->txop == TX_NONE) {
        e->txc = c;
        e->txch = ch;
        e->txop = op;
        e->txsince = hb_now_ms();
    }
}

/* 无属主挂起（libssh2 内部包等漏网生产者）→ 视作 ORPHAN 代偿。 */
static void tx_adopt(pfeng *e)
{
    if (tx_pending(e) && e->txop == TX_NONE) {
        e->txop = TX_ORPHAN;
        e->txsince = hb_now_ms();
    }
}

static void conn_pump(pfeng *e, pfconn *c, int *prog);
static void open_step(pfeng *e, int *prog);
static void accept_step(pfeng *e, int *prog);
static void dying_drain(pfeng *e, int *prog);

/* keepalive / 孤儿挂起不可同参重试：借任一活 channel 发 1 字节
 * window-adjust —— mismatch-resume 把挂起包续发干净，adjust 自身被吞
 * （仅多记 1 字节窗口，远端不知情因而无害）。无 channel 可借则留待
 * tx_try 放行下一个生产者代偿。 */
static void tx_sacrifice(pfeng *e)
{
    pfconn *c;
    for (c = e->conns; c; c = c->next)
        if (c->ch && !c->dead)
            break;
    if (c)
        (void)libssh2_channel_receive_window_adjust2(c->ch, 1, 1, NULL);
}

/* 每轮 sweep 先排空挂起包：只有属主（同参重试）能正确续发。 */
static void tx_drain(pfeng *e, int *prog)
{
    if (!tx_pending(e))
        return;
    tx_adopt(e);
    switch (e->txop) {
    case TX_W:
    case TX_EOF:
        if (e->txc)
            conn_pump(e, e->txc, prog);   /* 门内只放行属主 op */
        break;
    case TX_READ:
        /* 挂起的是 read 内部 window-adjust：0 字节 read 同参续发，
         * 窗口记账留在 libssh2 自身公式里。 */
        if (e->txch) {
            char junk;
            (void)libssh2_channel_read(e->txch, &junk, 0);
        }
        break;
    case TX_CLOSE:
        if (e->txch)
            (void)libssh2_channel_close(e->txch);
        break;
    case TX_FREE:
        dying_drain(e, prog);
        break;
    case TX_OPEN:
        open_step(e, prog);
        break;
    case TX_ACCEPT:
        accept_step(e, prog);
        break;
    case TX_LSTN:
    case TX_CANCEL:
        break;               /* 由各自收尾路径重试 */
    case TX_KA:
    case TX_ORPHAN:
        tx_sacrifice(e);
        break;
    }
}

/* ------------------------------ conn 生命周期 ------------------------------ */

/* channel 推入 dying 队列：由 sweep 在 tx 门控下 channel_free。 */
static void push_dying(pfeng *e, LIBSSH2_CHANNEL *ch)
{
    pfzombie *z = malloc(sizeof *z);
    if (!z) {
        libssh2_channel_free(ch);   /* OOM：尽力直释 */
        return;
    }
    z->ch = ch;
    z->next = e->dying;
    e->dying = z;
}

/* 逐个重试 dying channel 的 free；EAGAIN 留下轮。 */
static void dying_drain(pfeng *e, int *prog)
{
    pfzombie **dp = &e->dying;
    while (*dp) {
        pfzombie *d = *dp;
        int rc;
        if (!tx_try(e, NULL, d->ch, TX_FREE)) {
            dp = &d->next;
            continue;
        }
        rc = libssh2_channel_free(d->ch);
        tx_note(e, NULL, d->ch, TX_FREE);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            dp = &d->next;
            continue;
        }
        *dp = d->next;               /* 成功/真错误都摘除 */
        free(d);
        (*prog)++;
    }
}

static void conn_remove(pfeng *e, pfconn *c)
{
    pfconn **pp = &e->conns, **qp;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;
    if (e->opening == c)
        e->opening = NULL;
    /* 出打开队列（若在排队）；队尾被摘时重算 qt */
    qp = &e->qh;
    while (*qp && *qp != c)
        qp = &(*qp)->qnext;
    if (*qp) {
        *qp = c->qnext;
        if (e->qt == c) {
            pfconn *t;
            for (t = e->qh; t && t->qnext; t = t->qnext)
                ;
            e->qt = t;
        }
    }
    if (c->ch) {
        push_dying(e, c->ch);
        c->ch = NULL;
    }
    if (e->txc == c) {
        /* 在飞包的属主 conn 摘除：channel 仍在 dying 里未释放，
         * 挂起包转 ORPHAN 由代偿排空。 */
        e->txc = NULL;
        e->txch = NULL;
        if (e->txop != TX_NONE)
            e->txop = TX_ORPHAN;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    e->active--;
    free(c);
}

/* 标记待回收；真正摘除在 sweep 尾部（在飞 open 由 open_step 收）。
 * 不立即 free —— 调用方与 sweep 可能还持有指针。 */
static void conn_kill(pfeng *e, pfconn *c)
{
    (void)e;
    c->dead = 1;
}

static pfconn *conn_add(pfeng *e, int fd)
{
    pfconn *c = calloc(1, sizeof *c);
    if (!c) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    c->fd = fd;
    c->next = e->conns;
    e->conns = c;
    e->active++;
    e->total++;
    return c;
}

static void open_enqueue(pfeng *e, pfconn *c)
{
    c->st = CS_OPEN_Q;
    c->qnext = NULL;
    if (e->qt)
        e->qt->qnext = c;
    else
        e->qh = c;
    e->qt = c;
}

/* SOCKS 握手阶段应答：仅 {VER, METHOD} 两字节 */
static void s5_greet_reply(pfconn *c, int method)
{
    if (sizeof c->dn - c->dn_len < 2)
        return;
    c->dn[c->dn_len++] = 0x05;
    c->dn[c->dn_len++] = (uint8_t)method;
}

/* SOCKS CONNECT 应答：定长 10 字节（BND 段填 0） */
static void s5_reply(pfconn *c, int rep)
{
    static const uint8_t tpl[10] =
        { 0x05, 0, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    if (sizeof c->dn - c->dn_len < 10)
        return;
    memcpy(c->dn + c->dn_len, tpl, sizeof tpl);
    c->dn[1] = (uint8_t)rep;
    c->dn_len += 10;
}

/* 应答后走"排空 dn → shutdown → 回收"路径（无 channel 时 ch_eof 触发半关） */
static void s5_drain_close(pfconn *c)
{
    c->cli_eof = 1;
    c->ch_eof = 1;
    c->up_len = 0;
    c->st = CS_UP;
}

static void s5_greet_fail(pfconn *c, int method)
{
    s5_greet_reply(c, method);
    s5_drain_close(c);
}

/* 请求阶段失败：10 字节 CONNECT 应答后关闭 */
static void s5_fail_and_close(pfconn *c, int rep)
{
    s5_reply(c, rep);
    s5_drain_close(c);
}

static int s5_fail_code(int lib_err)
{
    if (lib_err == LIBSSH2_ERROR_CHANNEL_FAILURE ||
        lib_err == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED ||
        lib_err == LIBSSH2_ERROR_REQUEST_DENIED)
        return 0x05;                  /* connection refused */
    return 0x01;                      /* general failure */
}

/* ------------------------------ 监听器绑定（构造期同步） ------------------------------ */

static void set_err(char **err, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!err)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    *err = strdup(buf);
}

/* 规范化 bind 地址：NULL/""/"localhost" → "127.0.0.1"。返回 strdup。 */
static char *norm_bind_addr(const char *addr)
{
    if (!addr || !*addr || !strcmp(addr, "localhost"))
        return strdup("127.0.0.1");
    return strdup(addr);
}

/* 数字地址解析：仅 AI_NUMERICHOST。 */
static struct addrinfo *resolve_numeric(const char *addr, int port)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(addr, ps, &hints, &res) != 0)
        return NULL;
    return res;
}

/* 与真监听器完全相同的 socket 选项（probe 复用）。 */
static int mk_listener_fd(const struct addrinfo *ai)
{
    int fd, one = 1;
    fd = socket(ai->ai_family,
                ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                ai->ai_protocol);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    return fd;
}

static void bind_fail_msg(char **err, const char *addr, int port, int e2)
{
    if (e2 == EADDRINUSE)
        set_err(err, "%s:%d 已被占用（%s）", addr, port, strerror(e2));
    else if (e2 == EACCES && port > 0 && port < 1024)
        set_err(err, "权限不足：绑定 1024 以下端口需要 root");
    else
        set_err(err, "绑定 %s:%d 失败（%s）", addr, port, strerror(e2));
}

/* 绑定全部解析结果；port==0 时首个成功绑定的端口被其余沿用。
 * 失败关闭全部并填 *err。返回 fd 数或 -1。 */
static int bind_listeners(const char *addr, int port, int *fds, int maxfds,
                          int *bound_out, char **err)
{
    struct addrinfo *res, *ai;
    int n = 0, real = port;

    res = resolve_numeric(addr, port);
    if (!res) {
        set_err(err, "无效绑定地址：%s", addr);
        return -1;
    }
    for (ai = res; ai && n < maxfds; ai = ai->ai_next) {
        struct sockaddr_storage ss;
        socklen_t sl = ai->ai_addrlen;
        int fd = mk_listener_fd(ai);
        if (fd < 0)
            continue;
        memcpy(&ss, ai->ai_addr, sl);
        if (ss.ss_family == AF_INET)
            ((struct sockaddr_in *)&ss)->sin_port = htons((uint16_t)real);
        else if (ss.ss_family == AF_INET6)
            ((struct sockaddr_in6 *)&ss)->sin6_port = htons((uint16_t)real);
        if (bind(fd, (struct sockaddr *)&ss, sl) != 0) {
            int e2 = errno;
            close(fd);
            bind_fail_msg(err, addr, real ? real : port, e2);
            goto fail;
        }
        if (real == 0) {
            struct sockaddr_storage g;
            socklen_t gl = sizeof g;
            if (getsockname(fd, (struct sockaddr *)&g, &gl) == 0) {
                if (g.ss_family == AF_INET)
                    real = ntohs(((struct sockaddr_in *)&g)->sin_port);
                else if (g.ss_family == AF_INET6)
                    real = ntohs(((struct sockaddr_in6 *)&g)->sin6_port);
            }
        }
        if (listen(fd, PF_BACKLOG) != 0) {
            int e2 = errno;
            close(fd);
            set_err(err, "监听 %s:%d 失败（%s）", addr, real, strerror(e2));
            goto fail;
        }
        fds[n++] = fd;
    }
    freeaddrinfo(res);
    if (n == 0) {
        set_err(err, "绑定 %s:%d 失败（无可用地址）", addr, port);
        return -1;
    }
    *bound_out = real;
    return n;
fail:
    while (n > 0)
        close(fds[--n]);
    freeaddrinfo(res);
    return -1;
}

hb_err hb_pf_probe_bind(const char *addr, int port, char **err)
{
    char *na;
    struct addrinfo *res, *ai;
    if (err)
        *err = NULL;
    if (port < 0 || port > 65535) {
        set_err(err, "无效端口：%d", port);
        return HB_ERR_INVALID;
    }
    na = norm_bind_addr(addr);
    if (!na)
        return HB_ERR_NOMEM;
    res = resolve_numeric(na, port);
    if (!res) {
        set_err(err, "无效绑定地址：%s", na);
        free(na);
        return HB_ERR_INVALID;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int fd = mk_listener_fd(ai);
        if (fd < 0)
            continue;
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            int e2 = errno;
            close(fd);
            bind_fail_msg(err, na, port, e2);
            freeaddrinfo(res);
            free(na);
            return e2 == EADDRINUSE ? HB_ERR_EXISTS : HB_ERR_IO;
        }
        close(fd);
    }
    freeaddrinfo(res);
    free(na);
    return HB_OK;
}

/* ------------------------------ 构造 ------------------------------ */

static void eng_free(pfeng *e)
{
    int i;
    pfconn *c;
    if (!e)
        return;
    for (i = 0; i < e->nlfds; i++)
        if (e->lfds[i] >= 0)
            close(e->lfds[i]);
    while ((c = e->conns) != NULL) {
        e->conns = c->next;
        if (c->ch)
            libssh2_channel_free(c->ch);
        if (c->fd >= 0)
            close(c->fd);
        free(c);
    }
    while (e->dying) {
        pfzombie *d = e->dying;
        e->dying = d->next;
        libssh2_channel_free(d->ch);
        free(d);
    }
    if (e->lst) {
        libssh2_channel_forward_cancel(e->lst);   /* 内部释放 listener */
        e->lst = NULL;
    }
    if (e->dest_ai)
        freeaddrinfo(e->dest_ai);
    free(e->bind_addr);
    free(e->dest_addr);
    free(e);
}

void hb__pf_eng_free(void *eng)
{
    eng_free(eng);
}

hb_session *hb_session_new_forward(const hb_ssh_opts *opts,
                                   const hb_pf_rule *rule,
                                   int *bound_port, char **err)
{
    pfeng *e;
    hb_ssh_opts oo;
    hb_session *s;
    int bp;

    if (err)
        *err = NULL;
    if (bound_port)
        *bound_port = 0;
    if (!opts || !rule || rule->kind < HB_PF_LOCAL ||
        rule->kind > HB_PF_DYNAMIC ||
        rule->bind_port < 0 || rule->bind_port > 65535 ||
        rule->dest_port < 0 || rule->dest_port > 65535) {
        set_err(err, "无效的转发规则");
        return NULL;
    }
    if (rule->kind != HB_PF_DYNAMIC &&
        (!rule->dest_addr || !*rule->dest_addr || rule->dest_port <= 0)) {
        set_err(err, "转发规则缺少目标地址或端口");
        return NULL;
    }
    e = calloc(1, sizeof *e);
    if (!e) {
        set_err(err, "内存不足");
        return NULL;
    }
    e->kind = rule->kind;
    e->bind_addr = norm_bind_addr(rule->bind_addr);
    e->bind_port = rule->bind_port;
    e->dest_addr = rule->dest_addr ? strdup(rule->dest_addr) : NULL;
    e->dest_port = rule->dest_port;
    if (!e->bind_addr || (rule->dest_addr && !e->dest_addr)) {
        set_err(err, "内存不足");
        goto fail;
    }

    if (e->kind != HB_PF_REMOTE) {
        /* local/dynamic：同步绑定，冲突立即失败，不启动会话线程 */
        e->nlfds = bind_listeners(e->bind_addr, e->bind_port,
                                  e->lfds, 8, &e->bound_port, err);
        if (e->nlfds < 0)
            goto fail;
    }
    bp = e->bound_port;   /* remote 恒 0；线程启动前取值 */

    oo = *opts;
    oo.mode = HB_SSH_MODE_FORWARD;
    oo.detect_os = false;
    oo.command = NULL;
    oo.reuse_vt = NULL;
    s = hb__ssh_new_forward(&oo, e);   /* 无条件接管 e（含失败） */
    if (!s) {
        set_err(err, "无法创建 SSH 会话");
        return NULL;
    }
    if (bound_port)
        *bound_port = bp;
    return s;
fail:
    eng_free(e);
    return NULL;
}

/* ------------------------------ connect 阶段（session 线程） ------------------------------ */

static void stage_pf(pfeng *e, int st, const char *fmt, ...)
{
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    hb__session_emit(e->s, HB_SESS_EV_STAGE, 'T', st, msg, NULL);
}

static void pf_connected(pfeng *e)
{
    if (e->h->keepalive_s > 0) {
        libssh2_keepalive_config(e->hp->sess, 1,
                                 (unsigned int)e->h->keepalive_s);
        e->h->ka_next = hb_now_ms() + (int64_t)e->h->keepalive_s * 1000;
        e->h->ka_last_in = hb_now_ms();
    }
    hb__session_emit(e->s, HB_SESS_EV_CONNECTED, 'T', 0, NULL, NULL);
}

int hb__pf_connect(void *eng, hb_ssh *h, hop *hp, int64_t dl)
{
    pfeng *e = eng;
    int w;

    if (!e)
        return -1;
    e->s = h->s;
    e->h = h;
    e->hp = hp;

    if (e->kind != HB_PF_REMOTE) {
        stage_pf(e, 4, "本地监听 %s:%d 已就绪", e->bind_addr, e->bound_port);
        pf_connected(e);
        return 0;
    }

    /* remote：向服务器申请 tcpip-forward 监听 */
    stage_pf(e, 4, "请求远程转发 %s:%d…", e->bind_addr, e->bind_port);
    for (;;) {
        e->lst = libssh2_channel_forward_listen_ex(
            hp->sess, e->bind_addr, e->bind_port, &e->bound_port, 16);
        if (e->lst)
            break;
        if (libssh2_session_last_errno(hp->sess) != LIBSSH2_ERROR_EAGAIN)
            break;
        w = hb__ssh_wait_dir(h, hp, dl);
        if (w) {
            if (w == W_CANCEL) {
                hb__ssh_emit_error(h, HB_SSH_ERR_CANCELLED, "连接已取消");
                return -HB_SSH_ERR_CANCELLED;
            }
            if (w == W_TMOUT) {
                hb__ssh_emit_error(h, HB_SSH_ERR_TIMEOUT, "请求远程转发超时");
                return -HB_SSH_ERR_TIMEOUT;
            }
            hb__ssh_emit_error(h, HB_SSH_ERR_FORWARD, "远程转发请求失败");
            return -HB_SSH_ERR_FORWARD;
        }
    }
    if (!e->lst) {
        char msg[512];
        hb__ssh_set_lib_err(h, hp->sess);
        snprintf(msg, sizeof msg,
                 "服务器拒绝远程转发 %s:%d：检查 sshd_config "
                 "AllowTcpForwarding（需 yes 或 remote）、"
                 "服务器侧端口是否被占用、1024 以下端口需 root、"
                 "非回环绑定需 GatewayPorts",
                 e->bind_addr, e->bind_port);
        hb__ssh_emit_error(h, HB_SSH_ERR_FORWARD, msg);
        return -HB_SSH_ERR_FORWARD;
    }

    /* dest 本地解析（线程化 + 取消）一次 */
    stage_pf(e, 4, "解析转发目标 %s:%d…", e->dest_addr, e->dest_port);
    w = hb__ssh_resolve(h, e->dest_addr, e->dest_port, 10000, &e->dest_ai);
    if (w == W_CANCEL) {
        libssh2_channel_forward_cancel(e->lst);
        e->lst = NULL;
        hb__ssh_emit_error(h, HB_SSH_ERR_CANCELLED, "连接已取消");
        return -HB_SSH_ERR_CANCELLED;
    }
    if (w || !e->dest_ai) {
        char msg[512];
        snprintf(msg, sizeof msg, "无法解析转发目标 %s:%d",
                 e->dest_addr, e->dest_port);
        libssh2_channel_forward_cancel(e->lst);
        e->lst = NULL;
        /* 解析失败可重试（DNS 抖动），区别于服务器拒转的 FORWARD */
        hb__ssh_emit_error(h, HB_SSH_ERR_RESOLVE, msg);
        return -HB_SSH_ERR_RESOLVE;
    }

    stage_pf(e, 4, "已连接");
    pf_connected(e);
    return 0;
}

/* ------------------------------ SOCKS5 ------------------------------ */

static int s5_step(pfeng *e, pfconn *c, int *prog)
{
    if (c->hlen < sizeof c->hs) {
        ssize_t n = recv(c->fd, c->hs + c->hlen, sizeof c->hs - c->hlen, 0);
        if (n > 0) {
            c->hlen += (size_t)n;
            (*prog)++;
        } else if (n == 0) {
            conn_kill(e, c);
            return 0;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            conn_kill(e, c);
            return 0;
        }
    }
    if (c->st == CS_S5_GREET) {
        uint8_t nm;
        if (c->hlen < 2)
            return 0;
        if (c->hs[0] != 0x05) {
            conn_kill(e, c);       /* 垃圾 / SOCKS4 → 直接断 */
            return 0;
        }
        nm = c->hs[1];
        if (c->hlen < 2u + nm)
            return 0;
        if (!memchr(c->hs + 2, 0x00, nm)) {
            s5_greet_fail(c, 0xFF);   /* 未提供 no-auth */
            (*prog)++;
            return 0;
        }
        s5_greet_reply(c, 0x00);
        /* 客户端可能把 greeting + CONNECT 一次写完：剩余字节保留，
         * 下个 sweep 当 CS_S5_REQ 继续解析，不能丢 */
        memmove(c->hs, c->hs + 2 + nm, c->hlen - (2u + nm));
        c->hlen -= 2u + nm;
        c->st = CS_S5_REQ;
        (*prog)++;
        return 0;
    }
    /* CS_S5_REQ */
    {
        uint8_t cmd, atyp;
        size_t need, alen;
        uint16_t port;
        if (c->hlen < 4)
            return 0;
        if (c->hs[0] != 0x05 || c->hs[2] != 0x00) {
            conn_kill(e, c);
            return 0;
        }
        cmd = c->hs[1];
        atyp = c->hs[3];
        if (atyp == 0x01) {
            alen = 4;
            need = 4 + alen + 2;
        } else if (atyp == 0x04) {
            alen = 16;
            need = 4 + alen + 2;
        } else if (atyp == 0x03) {
            if (c->hlen < 5)
                return 0;
            alen = c->hs[4];
            need = 5 + alen + 2;
        } else {
            s5_fail_and_close(c, 0x08);   /* 不支持的地址类型 */
            (*prog)++;
            return 0;
        }
        if (c->hlen < need)
            return 0;
        if (cmd != 0x01) {
            s5_fail_and_close(c, 0x07);   /* 仅 CONNECT */
            (*prog)++;
            return 0;
        }
        memcpy(&port, c->hs + need - 2, 2);
        c->dport = ntohs(port);
        if (atyp == 0x01) {
            if (!inet_ntop(AF_INET, c->hs + 4, c->dhost, sizeof c->dhost)) {
                s5_fail_and_close(c, 0x01);
                (*prog)++;
                return 0;
            }
        } else if (atyp == 0x04) {
            if (!inet_ntop(AF_INET6, c->hs + 4, c->dhost, sizeof c->dhost)) {
                s5_fail_and_close(c, 0x01);
                (*prog)++;
                return 0;
            }
        } else {
            size_t i;
            bool bad = false;
            /* 域名内 NUL 或控制字符（<0x20）非法 —— RFC 1928 ATYP=0x03
             * 是可打印 FQDN，畸形请求回 0x01 */
            if (!alen || alen >= sizeof c->dhost)
                bad = true;
            else
                for (i = 0; i < alen; i++)
                    if (c->hs[5 + i] < 0x20) {
                        bad = true;
                        break;
                    }
            if (bad) {
                s5_fail_and_close(c, 0x01);
                (*prog)++;
                return 0;
            }
            memcpy(c->dhost, c->hs + 5, alen);
            c->dhost[alen] = 0;
        }
        if (!c->dhost[0] || c->dport <= 0) {
            s5_fail_and_close(c, 0x01);
            (*prog)++;
            return 0;
        }
        /* 请求头之后的字节是先行数据（客户端不等应答就发的上行）：
         * 挪进 up 缓冲，channel 打开后最先写出，既不丢也不乱序 */
        if (c->hlen > need) {
            size_t extra = c->hlen - need;
            if (extra > sizeof c->up)
                extra = sizeof c->up;
            memcpy(c->up, c->hs + need, extra);
            c->up_len = extra;
        }
        c->hlen = 0;
        c->open_dl = hb_now_ms() + PF_OPEN_MS;
        open_enqueue(e, c);
        (*prog)++;
        return 0;
    }
}

/* ------------------------------ channel 打开（串行） ------------------------------ */

/* libssh2 会话级只允许一个 open 在飞：direct_tcpip 严格串行，
 * EAGAIN 期间绝不开第二个（重试必须用相同参数）。 */
static void open_step(pfeng *e, int *prog)
{
    pfconn *c;
    LIBSSH2_CHANNEL *ch;
    int le;

    if (!e->opening && e->qh) {
        c = e->qh;
        e->qh = c->qnext;
        if (!e->qh)
            e->qt = NULL;
        c->qnext = NULL;
        c->st = CS_OPENING;
        e->opening = c;
    }
    c = e->opening;
    if (!c)
        return;
    if (!c->dead && hb_now_ms() > c->open_dl) {
        e->opening = NULL;
        warnf(e, "打开到 %s:%d 的通道超时", c->dhost, c->dport);
        conn_kill(e, c);
        (*prog)++;
        return;
    }
    if (!tx_try(e, c, NULL, TX_OPEN))
        return;                    /* 别人的出站包未发完：下轮再试 */
    ch = libssh2_channel_direct_tcpip_ex(e->hp->sess, c->dhost, c->dport,
                                       "127.0.0.1", e->bound_port);
    tx_note(e, c, NULL, TX_OPEN);
    if (ch) {
        e->opening = NULL;
        (*prog)++;
        if (c->dead) {
            push_dying(e, ch);
            conn_remove(e, c);
            return;
        }
        c->ch = ch;
        if (e->kind == HB_PF_DYNAMIC)
            s5_reply(c, 0x00);       /* 成功应答进 dn，正常排空发出 */
        c->st = CS_UP;
        return;
    }
    le = libssh2_session_last_errno(e->hp->sess);
    if (le == LIBSSH2_ERROR_EAGAIN)
        return;                      /* 仍在飞：下个 sweep 同参重试 */
    e->opening = NULL;
    (*prog)++;
    if (c->dead) {
        conn_remove(e, c);
        return;
    }
    {
        char *em = NULL;
        int el = 0;
        libssh2_session_last_error(e->hp->sess, &em, &el, 0);
        warnf(e, "无法连接目标 %s:%d（经 SSH 服务器）：%s",
              c->dhost, c->dport,
              (em && el) ? em : "channel open failed");
    }
    if (e->kind == HB_PF_DYNAMIC) {
        s5_fail_and_close(c, s5_fail_code(le));
    } else {
        conn_kill(e, c);
    }
}

/* ------------------------------ 接受新连接 ------------------------------ */

/* remote：dest 解析失效后再解析（节流 + 线程化解析）。 */
static bool remote_resolve(pfeng *e)
{
    struct addrinfo *ai = NULL;
    int w;
    if (!e->dest_bad && e->dest_ai)
        return true;
    if (hb_now_ms() < e->dest_retry)
        return false;
    w = hb__ssh_resolve(e->h, e->dest_addr, e->dest_port, PF_RS_MS, &ai);
    if (w || !ai) {
        e->dest_retry = hb_now_ms() + 5000;
        warnf(e, "无法解析转发目标 %s:%d", e->dest_addr, e->dest_port);
        return false;
    }
    if (e->dest_ai)
        freeaddrinfo(e->dest_ai);
    e->dest_ai = ai;
    e->dest_bad = false;
    return true;
}

static void accept_step(pfeng *e, int *prog)
{
    int i, guard = 0;

    if (e->shutting)
        return;

    /* local/dynamic 本地监听 */
    for (i = 0; i < e->nlfds && guard < 64; i++) {
        for (;;) {
            struct sockaddr_storage ss;
            socklen_t sl = sizeof ss;
            int fd = accept4(e->lfds[i], (struct sockaddr *)&ss, &sl,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
            pfconn *c;
            if (fd < 0)
                break;
            (*prog)++;
            if (++guard >= 64)
                break;
            if (e->active >= PF_MAX_CONN) {
                close(fd);
                if (!e->cap_warned) {
                    e->cap_warned = true;
                    warnf(e, "并发连接数已达上限 %d，新连接已丢弃",
                          PF_MAX_CONN);
                }
                continue;
            }
            c = conn_add(e, fd);
            if (!c)
                continue;
            if (e->kind == HB_PF_DYNAMIC) {
                c->st = CS_S5_GREET;
                c->hs_dl = hb_now_ms() + PF_HS_MS;
            } else {
                snprintf(c->dhost, sizeof c->dhost, "%s", e->dest_addr);
                c->dport = e->dest_port;
                c->open_dl = hb_now_ms() + PF_OPEN_MS;
                open_enqueue(e, c);
            }
        }
    }

    /* remote：服务器侧连接经 forwarded-tcpip channel 进来 */
    if (e->kind == HB_PF_REMOTE && e->lst) {
        for (;;) {
            LIBSSH2_CHANNEL *ch =
                libssh2_channel_forward_accept(e->lst);
            pfconn *c;
            if (ch) {
                (*prog)++;
                if (e->active >= PF_MAX_CONN || !remote_resolve(e)) {
                    push_dying(e, ch);
                    if (e->active >= PF_MAX_CONN && !e->cap_warned) {
                        e->cap_warned = true;
                        warnf(e, "并发连接数已达上限 %d，新连接已丢弃",
                              PF_MAX_CONN);
                    }
                    continue;
                }
                c = conn_add(e, -1);
                if (!c) {
                    push_dying(e, ch);
                    continue;
                }
                c->ch = ch;
                c->ai = e->dest_ai;
                c->st = CS_CONNECTING;
                c->hs_dl = hb_now_ms() + 10000;
                continue;
            }
            if (libssh2_session_last_errno(e->hp->sess) ==
                LIBSSH2_ERROR_EAGAIN)
                break;
            /* 真错误 → 监听已死 */
            hb__ssh_set_lib_err(e->h, e->hp->sess);
            e->conn_dead = true;
            e->h->disconnected = true;
            break;
        }
    }
}

/* ------------------------------ 单连接泵 ------------------------------ */

/* remote：本地 connect 推进。0 在连，1 已连上（CS_UP）。 */
static void lconn_step(pfeng *e, pfconn *c, int *prog)
{
    int rc, ce;
    if (c->fd < 0) {
        if (!c->ai) {
            warnf(e, "无法连接目标 %s:%d（本地）：无可用地址",
                  e->dest_addr, e->dest_port);
            conn_kill(e, c);
            return;
        }
        c->fd = socket(c->ai->ai_family,
                       c->ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       c->ai->ai_protocol);
        if (c->fd < 0) {
            c->ai = c->ai->ai_next;
            return;
        }
        c->hs_dl = hb_now_ms() + 10000;
        (*prog)++;
    }
    rc = connect(c->fd, c->ai->ai_addr, c->ai->ai_addrlen);
    if (rc == 0 || errno == EISCONN) {
        c->st = CS_UP;
        (*prog)++;
        return;
    }
    if (errno == EALREADY || errno == EINPROGRESS) {
        if (hb_now_ms() > c->hs_dl) {
            warnf(e, "连接目标 %s:%d（本地）超时", e->dest_addr, e->dest_port);
            conn_kill(e, c);
        }
        return;
    }
    /* 失败 → 下一个地址 */
    ce = errno;
    close(c->fd);
    c->fd = -1;
    c->ai = c->ai->ai_next;
    (*prog)++;
    if (!c->ai) {
        warnf(e, "无法连接目标 %s:%d（本地）：%s",
              e->dest_addr, e->dest_port, strerror(ce));
        conn_kill(e, c);
        e->dest_bad = true;          /* 触发下次接受前再解析 */
    }
}

/* dn → fd 排空 + 下行耗尽后半关闭。握手期应答也走这条路。 */
static void dn_flush(pfeng *e, pfconn *c, int *prog)
{
    if (c->fd >= 0 && c->dn_len) {
        ssize_t n = send(c->fd, c->dn, c->dn_len, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->dn, c->dn + n, c->dn_len - (size_t)n);
            c->dn_len -= (size_t)n;
            (*prog)++;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            conn_kill(e, c);
            return;
        }
    }
    /* 下行耗尽（ch_eof/ch_dead 标志）→ 半关闭客户端写方向。
     * 条件不含 !c->ch：握手期无 channel 的连接还要继续收请求。 */
    if (c->fd >= 0 && !c->cli_shut &&
        (c->ch_eof || c->ch_dead) && c->dn_len == 0) {
        shutdown(c->fd, SHUT_WR);
        c->cli_shut = 1;
        (*prog)++;
    }
}

static void conn_pump(pfeng *e, pfconn *c, int *prog)
{
    /* fd → up（channel 已死/EOF 时上行无意义，停读让客户端收完下行） */
    if (c->fd >= 0 && !c->cli_eof && c->up_len < PF_BUF && c->ch &&
        !c->ch_dead && !c->ch_eof) {
        ssize_t n = recv(c->fd, c->up + c->up_len, PF_BUF - c->up_len, 0);
        if (n > 0) {
            c->up_len += (size_t)n;
            (*prog)++;
        } else if (n == 0) {
            c->cli_eof = 1;
            (*prog)++;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            c->cli_eof = 1;          /* 读错误按 EOF：先冲完已收数据 */
            (*prog)++;
        }
    }
    /* up → channel */
    if (c->ch && c->up_len && !c->ch_dead && tx_try(e, c, c->ch, TX_W)) {
        ssize_t n = libssh2_channel_write(c->ch, (const char *)c->up,
                                          c->up_len);
        tx_note(e, c, c->ch, TX_W);
        if (n > 0) {
            memmove(c->up, c->up + n, c->up_len - (size_t)n);
            c->up_len -= (size_t)n;
            e->bytes_up += (uint64_t)n;
            (*prog)++;
        } else if (n != LIBSSH2_ERROR_EAGAIN) {
            c->ch_dead = 1;
            (*prog)++;
        }
    }
    /* fd EOF → 缓冲发完后 channel_send_eof */
    if (c->ch && c->cli_eof && c->up_len == 0 && !c->eof_sent &&
        !c->ch_dead && tx_try(e, c, c->ch, TX_EOF)) {
        int rc = libssh2_channel_send_eof(c->ch);
        tx_note(e, c, c->ch, TX_EOF);
        if (rc == 0) {
            c->eof_sent = 1;
            (*prog)++;
        } else if (rc != LIBSSH2_ERROR_EAGAIN) {
            c->ch_dead = 1;
            (*prog)++;
        }
    }
    /* channel → dn（dn 满时不读 —— SSH 窗口背压正是靠不读实现；
     * read 内部可能发 window-adjust 包，同样受挂起门控） */
    if (c->ch && !c->ch_eof && !c->ch_dead && c->dn_len < PF_BUF &&
        tx_try(e, c, c->ch, TX_READ)) {
        ssize_t n = libssh2_channel_read(c->ch, (char *)c->dn + c->dn_len,
                                         PF_BUF - c->dn_len);
        tx_note(e, c, c->ch, TX_READ);
        if (n > 0) {
            c->dn_len += (size_t)n;
            e->bytes_down += (uint64_t)n;
            (*prog)++;
        } else if (n == 0) {
            if (libssh2_channel_eof(c->ch)) {
                c->ch_eof = 1;
                (*prog)++;
            }
        } else if (n != LIBSSH2_ERROR_EAGAIN) {
            if (libssh2_channel_eof(c->ch))
                c->ch_eof = 1;
            else
                c->ch_dead = 1;
            (*prog)++;
        }
    }
    dn_flush(e, c, prog);
}

/* 两个方向都终结 → 可回收。channel 一死/EOF，上行即无意义（残存 up 丢弃）；
 * 下行排空 + SHUT_WR 后整连收工。 */
static bool conn_done(pfconn *c)
{
    bool up_done, dn_done;
    if (!c->ch || c->ch_dead || c->ch_eof)
        up_done = true;
    else
        up_done = (c->cli_eof || c->fd < 0) && c->up_len == 0 &&
                  c->eof_sent;
    dn_done = (!c->ch || c->ch_eof || c->ch_dead) &&
              c->dn_len == 0 && (c->cli_shut || c->fd < 0);
    return up_done && dn_done;
}

static void conn_step(pfeng *e, pfconn *c, int *prog)
{
    if (c->dead || c->st == CS_OPEN_Q || c->st == CS_OPENING)
        return;
    if (c->st == CS_CONNECTING) {
        lconn_step(e, c, prog);
        return;
    }
    if (c->st == CS_S5_GREET || c->st == CS_S5_REQ) {
        if (hb_now_ms() > c->hs_dl) {
            conn_kill(e, c);
            (*prog)++;
            return;
        }
        s5_step(e, c, prog);
        if (!c->dead)
            dn_flush(e, c, prog);    /* 握手应答随本轮送出 */
        return;
    }
    conn_pump(e, c, prog);
}

/* ------------------------------ sweep / teardown ------------------------------ */

static int eng_sweep(pfeng *e)
{
    int prog = 0;
    pfconn *c, *next;

    tx_drain(e, &prog);            /* 先排挂在飞包，其他生产者才有门可进 */
    accept_step(e, &prog);
    open_step(e, &prog);
    for (c = e->conns; c; c = next) {
        next = c->next;
        if (!c->dead)
            conn_step(e, c, &prog);
        /* dead 且在飞 open 的连接留给 open_step 收；其余 dead/done 即摘 */
        if (c->dead) {
            if (c->st != CS_OPENING) {
                conn_remove(e, c);
                prog++;
            }
        } else if (conn_done(c)) {
            conn_remove(e, c);
            prog++;
        }
    }
    dying_drain(e, &prog);
    return prog;
}

static void close_listeners(pfeng *e)
{
    int i;
    for (i = 0; i < e->nlfds; i++) {
        if (e->lfds[i] >= 0) {
            close(e->lfds[i]);
            e->lfds[i] = -1;
        }
    }
    /* remote listener：cancel 也是出站包，挂起别人的包时等下轮。 */
    if (e->lst && tx_try(e, NULL, NULL, TX_CANCEL)) {
        int rc = libssh2_channel_forward_cancel(e->lst);
        tx_note(e, NULL, NULL, TX_CANCEL);
        if (rc != LIBSSH2_ERROR_EAGAIN)
            e->lst = NULL;           /* 成功/真失败都收尾 */
    }
}

/* 尽力收尾全部 channel（≤PF_CLOSE_MS）；非优雅时直接 free。
 * 收尾期不做 tx 门控：会话将死，误记无意义。 */
static void conns_teardown(pfeng *e, bool graceful)
{
    pfconn *c;
    if (graceful) {
        int64_t dl = hb_now_ms() + PF_CLOSE_MS;
        for (;;) {
            bool pend = false;
            for (c = e->conns; c; c = c->next)
                if (c->ch && !c->ch_dead && !c->ch_eof) {
                    int rc = libssh2_channel_close(c->ch);
                    if (rc == LIBSSH2_ERROR_EAGAIN)
                        pend = true;
                    else
                        c->ch_dead = 1;
                }
            if (!pend || hb_now_ms() >= dl)
                break;
            usleep(5000);
        }
    }
    e->opening = NULL;
    e->qh = e->qt = NULL;
    while (e->conns)
        conn_remove(e, e->conns);    /* channel 进 dying + close(fd) */
    while (e->dying) {               /* 收尾直释，绕过门控 */
        pfzombie *d = e->dying;
        e->dying = d->next;
        libssh2_channel_free(d->ch);
        free(d);
    }
}

/* ------------------------------ 主循环 ------------------------------ */

int hb__pf_run(void *ctx, hb_session *s)
{
    hb_ssh *h = ctx;
    pfeng *e = h->pf_eng;
    int cmd_fd = hb__session_cmd_fd(s);
    int sweep_capped = 0;
    char junk[64];

    if (!e)
        return -1;
    e->s = s;
    e->h = h;
    e->hp = hb__ssh_target(h);
    e->last_stats = hb_now_ms();
    emit_st(e, HB_PF_EV_LISTENING, NULL);

    for (;;) {
        if (hb__session_cancelled(s))
            e->shutting = true;
        if (e->shutting)
            close_listeners(e);
        if (e->conn_dead)
            break;

        /* SSH 有入站后数据可能已解密进任意 channel → 扫到无进展为止。
         * 但持续流量下每轮都"有进展"会饿死 poll —— cmd 管道、取消、
         * keepalive 判死都挤在 poll 前后。封顶 ~20ms/64 轮后强制走一
         * 圈主循环（poll 用 0 超时立即回来接着扫）。 */
        sweep_capped = 0;
        {
            int64_t cap_dl = hb_now_ms() + 20;
            int rounds = 0;
            while (eng_sweep(e) > 0) {
                if (++rounds >= 64 || hb_now_ms() >= cap_dl) {
                    sweep_capped = 1;
                    break;
                }
            }
        }
        maybe_stats(e);

        /* 出站包挂起超时：发送窗长堵 = 链路实质已死（拔线半开：
         * keepalive 也发不出去，靠这里兜底）。 */
        if (tx_pending(e)) {
            tx_adopt(e);          /* 无属主挂起 → ORPHAN，起算时长 */
            if (hb_now_ms() - e->txsince > PF_TX_MS) {
                e->conn_dead = true;
                h->disconnected = true;
            }
        }

        if (e->shutting || e->conn_dead)
            break;

        /* ---- poll ---- */
        {
            struct pollfd pf[2 + 8 + PF_MAX_CONN];
            int nf = 2;
            int dirs, tmo, n, i;
            pfconn *c;

            {
                /* keepalive 也是出站包：别人的包挂起时只跑判死逻辑不
                 * 发送，否则 mismatch-resume 会替它续发并被错记。 */
                int may = !tx_pending(e) || e->txop == TX_KA ||
                          e->txop == TX_ORPHAN;
                tmo = hb__ssh_ka_tick(h, may);
                tx_adopt(e);      /* keepalive 自挂起 → 转代偿属主 */
            }
            if (h->disconnected) {
                e->conn_dead = true;
                continue;
            }

            pf[0].fd = cmd_fd;
            pf[0].events = POLLIN;
            pf[0].revents = 0;
            pf[1].fd = e->hp->sock;
            /* POLLIN 恒订：keepalive 回复等杂散入站包会刷新 ka_last_in。
             * POLLRDHUP 必订：对端 FIN 或 jump 泵 shutdown(SHUT_WR) 的
             * 半关只报 POLLIN/RDHUP 而不报 POLLHUP；漏订则 EOF 的 POLLIN
             * 一直刷新 ka_last_in，keepalive 判死永不触发（僵尸会话）。 */
            pf[1].events = POLLIN | POLLRDHUP | POLLERR | POLLHUP;
            dirs = libssh2_session_block_directions(e->hp->sess);
            if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND)
                pf[1].events |= POLLOUT;
            pf[1].revents = 0;

            if (!e->shutting) {
                for (i = 0; i < e->nlfds &&
                        nf < (int)(sizeof pf / sizeof pf[0]); i++) {
                    pf[nf].fd = e->lfds[i];
                    pf[nf].events = POLLIN;
                    pf[nf].revents = 0;
                    nf++;
                }
            }
            for (c = e->conns; c &&
                 nf < (int)(sizeof pf / sizeof pf[0]); c = c->next) {
                short ev = 0;
                if (c->dead || c->fd < 0)
                    continue;
                if (c->st == CS_CONNECTING) {
                    ev = POLLOUT;
                } else {
                    if ((c->st == CS_S5_GREET || c->st == CS_S5_REQ) &&
                        c->hlen < PF_HS_CAP)
                        ev |= POLLIN;
                    if (c->st == CS_UP && !c->cli_eof &&
                        c->up_len < PF_BUF && c->ch &&
                        !c->ch_dead && !c->ch_eof)
                        ev |= POLLIN;
                    if (c->dn_len > 0 && !c->cli_shut)
                        ev |= POLLOUT;
                }
                if (!ev)
                    continue;
                pf[nf].fd = c->fd;
                pf[nf].events = ev;
                pf[nf].revents = 0;
                nf++;
            }

            /* 有连接/在飞 open 时封顶 50ms：防漏订方向 + 驱动各死线 */
            if ((e->conns || e->opening || e->qh) &&
                (tmo < 0 || tmo > 50))
                tmo = 50;
            /* sweep 被截断 → 本轮不阻塞，立刻回来接着扫，让取消、
             * cmd 管道、keepalive 先得到服务 */
            if (sweep_capped)
                tmo = 0;
            n = poll(pf, nf, tmo);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                e->conn_dead = true;
                continue;
            }
            /* close_op 会 shutdown 掉 SSH socket → POLLHUP 与 want_close
             * 竞速；先认取消，正常关闭不谎报"连接已断开"。 */
            if (hb__session_cancelled(s)) {
                e->shutting = true;
                continue;
            }
            if (pf[0].revents & POLLIN) {
                ssize_t nr;
                while ((nr = read(cmd_fd, junk, sizeof junk)) > 0)
                    ;
            }
            if (pf[1].revents & POLLIN)
                h->ka_last_in = hb_now_ms();
            if (pf[1].revents &
                (POLLRDHUP | POLLERR | POLLHUP | POLLNVAL)) {
                e->conn_dead = true;
                h->disconnected = true;
            }
        }
        {
            int may = !tx_pending(e) || e->txop == TX_KA ||
                      e->txop == TX_ORPHAN;
            (void)hb__ssh_ka_tick(h, may);
            tx_adopt(e);
        }
        if (h->disconnected)
            e->conn_dead = true;
    }

    /* ---- 收尾 ---- */
    close_listeners(e);
    if (e->conn_dead) {
        if (!h->disconnect_emitted) {
            h->disconnect_emitted = true;
            hb__ssh_set_err(h, "forward transport lost");
            hb__ssh_emit_error(h, HB_SSH_ERR_DISCONNECTED, "连接已断开");
        }
        conns_teardown(e, false);
        return -1;
    }
    conns_teardown(e, true);
    return 0;
}

/* ------------------------------ 公共对象 ------------------------------ */

void hb_pf_status_free(hb_pf_status *st)
{
    if (!st)
        return;
    free(st->message);
    free(st);
}
