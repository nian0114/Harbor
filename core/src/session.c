#include "harbor/session.h"

#include "session_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READ_CHUNK (64 * 1024)

typedef struct ev_node {
    hb_sess_event ev;
    struct ev_node *next;
} ev_node;

struct hb_session {
    hb_session_transport tr;
    hb_vt   *vt;
    pthread_t th;
    bool     th_started;

    int cmd_r, cmd_w;   /* UI→线程（写队列/resize/close 唤醒） */
    int ev_r,  ev_w;    /* 线程→UI（事件可读） */

    pthread_mutex_t mu;
    ev_node *ev_head, *ev_tail;
    bool out_pending;               /* 合并的 OUTPUT 事件 */

    /* 待写字节队列（UI 追加、线程冲刷） */
    uint8_t *wq;
    size_t   wq_len, wq_cap;

    /* coalesced resize：只保留最新 */
    int  pend_rows, pend_cols;

    bool want_close;
    bool exited;
    int  exit_code;

    /* 应答管道（HOSTKEY / PROMPT）：连接线程阻塞等待 UI 回复 */
    pthread_cond_t reply_cv;
    int  reply_kind;        /* 0=无等待, HB__REPLY_* */
    bool reply_done;
    int  reply_accept;      /* HOSTKEY：1 接受 0 拒绝 */
    char *reply_secret;     /* PROMPT：sodium_malloc 的回复，NULL=取消 */

    hb_session_tap_fn tap;
    void *tap_ud;

    /* 完成监控引擎（FR-13）：仅 session 线程在 mu 下触碰；
     * set_watch 换引擎也在 mu 内完成。watch_next = poll 建议的下个检查时刻。 */
    hb_watch *watch;
    int64_t   watch_next;
};

/* ------------------------------ 事件队列 ------------------------------ */

void hb_sess_event_clear(hb_sess_event *ev)
{
    free(ev->text);
    free(ev->text2);
    free(ev->extra);
    if (ev->watch) {
        hb_watch_hit_clear(ev->watch);
        free(ev->watch);
    }
    ev->text = ev->text2 = ev->extra = NULL;
    ev->watch = NULL;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p;
    if (!s)
        return NULL;
    p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

void hb__session_emit_ex(hb_session *s, hb_sess_ev_type type, char mark,
                         int code, const char *text, const char *text2,
                         const char *extra)
{
    ev_node *n;
    char w = 'e';
    if (!s)
        return;
    n = calloc(1, sizeof *n);
    if (!n)
        return;
    n->ev.type = type;
    n->ev.mark = mark;
    n->ev.code = code;
    n->ev.text = text ? xstrndup(text, strlen(text)) : NULL;
    n->ev.text2 = text2 ? xstrndup(text2, strlen(text2)) : NULL;
    n->ev.extra = extra ? xstrndup(extra, strlen(extra)) : NULL;
    n->ev.ts_ms = hb_now_ms();

    pthread_mutex_lock(&s->mu);
    if (type == HB_SESS_EV_OUTPUT) {
        if (s->out_pending) {          /* 已有一条未消费的 OUTPUT */
            pthread_mutex_unlock(&s->mu);
            free(n->ev.text);
            free(n->ev.text2);
            free(n->ev.extra);
            free(n);
            return;
        }
        s->out_pending = true;
    }
    if (s->ev_tail)
        s->ev_tail->next = n;
    else
        s->ev_head = n;
    s->ev_tail = n;
    pthread_mutex_unlock(&s->mu);
    if (write(s->ev_w, &w, 1) < 0) { /* 忽略 EAGAIN */ }
}

void hb__session_emit(hb_session *s, hb_sess_ev_type type, char mark, int code,
                      const char *text, const char *text2)
{
    hb__session_emit_ex(s, type, mark, code, text, text2, NULL);
}

/* WATCH 事件：hit 需要深拷贝（emit_ex 的 text 槽位不够） */
static void emit_watch_hit(hb_session *s, const hb_watch_hit *hit)
{
    ev_node *n;
    char w = 'e';
    hb_watch_hit *cp;
    if (!s || !hit)
        return;
    n = calloc(1, sizeof *n);
    cp = calloc(1, sizeof *cp);
    if (!n || !cp) { free(n); free(cp); return; }
    *cp = *hit;
    cp->rule    = hit->rule    ? xstrndup(hit->rule, strlen(hit->rule))       : NULL;
    cp->line    = hit->line    ? xstrndup(hit->line, strlen(hit->line))       : NULL;
    cp->command = hit->command ? xstrndup(hit->command, strlen(hit->command)) : NULL;
    cp->title   = hit->title   ? xstrndup(hit->title, strlen(hit->title))     : NULL;
    n->ev.type = HB_SESS_EV_WATCH;
    n->ev.watch = cp;
    n->ev.ts_ms = hb_now_ms();

    pthread_mutex_lock(&s->mu);
    if (s->ev_tail)
        s->ev_tail->next = n;
    else
        s->ev_head = n;
    s->ev_tail = n;
    pthread_mutex_unlock(&s->mu);
    if (write(s->ev_w, &w, 1) < 0) {}
}

/* 引擎全部调用都收拢在 mu 内，替换/销毁才与线程安全。 */
static void watch_feed(hb_session *s, const char *bytes, size_t len)
{
    pthread_mutex_lock(&s->mu);
    if (s->watch)
        hb_watch_feed(s->watch, bytes, len, hb_now_ms());
    pthread_mutex_unlock(&s->mu);
}

static void watch_input(hb_session *s)
{
    pthread_mutex_lock(&s->mu);
    if (s->watch)
        hb_watch_input(s->watch, hb_now_ms());
    pthread_mutex_unlock(&s->mu);
}

/* 排空命中队列并更新建议轮询时刻；session 线程每轮循环后调用。 */
static void watch_drain(hb_session *s)
{
    for (;;) {
        hb_watch_hit hit;
        int64_t nx = -1;
        bool got = false;
        memset(&hit, 0, sizeof hit);
        pthread_mutex_lock(&s->mu);
        if (s->watch) {
            got = hb_watch_poll(s->watch, hb_now_ms(), &hit, &nx);
            if (!got)
                s->watch_next = nx;
        } else {
            s->watch_next = -1;
        }
        pthread_mutex_unlock(&s->mu);
        if (!got)
            break;
        emit_watch_hit(s, &hit);
        hb_watch_hit_clear(&hit);
    }
}

/* 会话终结：EOF hit 必须先于 EXIT 进队列。 */
static void watch_finish(hb_session *s, int code)
{
    pthread_mutex_lock(&s->mu);
    if (s->watch)
        hb_watch_eof(s->watch, code, hb_now_ms());
    pthread_mutex_unlock(&s->mu);
    watch_drain(s);
}

/* ------------------------------ 应答管道 ------------------------------ */

void hb__session_reply_reset(hb_session *s, int kind)
{
    pthread_mutex_lock(&s->mu);
    s->reply_kind = kind;
    s->reply_done = false;
    s->reply_accept = 0;
    if (s->reply_secret) {
        hb_secret_free(s->reply_secret);
        s->reply_secret = NULL;
    }
    pthread_mutex_unlock(&s->mu);
}

int hb__session_reply_wait(hb_session *s, int *accept_out, char **sec_out)
{
    int kind, r = 0;
    pthread_mutex_lock(&s->mu);
    kind = s->reply_kind;
    while (!s->reply_done && !s->want_close)
        pthread_cond_wait(&s->reply_cv, &s->mu);
    if (s->reply_done && !s->want_close) {
        r = 1;
        if (kind == HB__REPLY_HOSTKEY && accept_out)
            *accept_out = s->reply_accept;
        if (kind == HB__REPLY_PROMPT && sec_out) {
            *sec_out = s->reply_secret;
            s->reply_secret = NULL;      /* 移交所有权 */
        }
    }
    if (s->reply_secret) {
        hb_secret_free(s->reply_secret); /* 取消路径：清掉未取走的回复 */
        s->reply_secret = NULL;
    }
    s->reply_kind = 0;
    s->reply_done = false;
    pthread_mutex_unlock(&s->mu);
    return r;
}

void hb_session_hostkey_reply(hb_session *s, bool accept)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    if (s->reply_kind == HB__REPLY_HOSTKEY && !s->reply_done) {
        s->reply_accept = accept ? 1 : 0;
        s->reply_done = true;
        pthread_cond_broadcast(&s->reply_cv);
    }
    pthread_mutex_unlock(&s->mu);
}

void hb_session_prompt_reply(hb_session *s, const char *secret)
{
    char *cp = NULL;
    if (!s)
        return;
    if (secret)
        cp = hb_secret_strdup(secret);
    pthread_mutex_lock(&s->mu);
    if (s->reply_kind == HB__REPLY_PROMPT && !s->reply_done) {
        if (s->reply_secret)
            hb_secret_free(s->reply_secret);
        s->reply_secret = cp;   /* NULL = 用户取消 */
        s->reply_done = true;
        pthread_cond_broadcast(&s->reply_cv);
        cp = NULL;
    }
    pthread_mutex_unlock(&s->mu);
    if (cp)
        hb_secret_free(cp);
}

bool hb__session_cancelled(hb_session *s)
{
    bool c;
    pthread_mutex_lock(&s->mu);
    c = s->want_close;
    pthread_mutex_unlock(&s->mu);
    return c;
}

int hb_session_event_fd(hb_session *s)
{
    return s ? s->ev_r : -1;
}

bool hb_session_next_event(hb_session *s, hb_sess_event *out)
{
    ev_node *n;
    char buf[64];
    bool empty;

    pthread_mutex_lock(&s->mu);
    n = s->ev_head;
    if (n) {
        s->ev_head = n->next;
        if (!s->ev_head)
            s->ev_tail = NULL;
        if (n->ev.type == HB_SESS_EV_OUTPUT)
            s->out_pending = false;
    }
    empty = (s->ev_head == NULL);
    if (empty) {
        /* 队列空：持锁排干唤醒字节。生产者是入队(锁内) → 写字节(锁外)，
         * 我们排干期间它拿不到锁，所以它写进管道的任何字节必然对应
         * 我们解锁之后入队的事件 —— 不会丢唤醒。 */
        while (read(s->ev_r, buf, sizeof buf) > 0) {}
    }
    pthread_mutex_unlock(&s->mu);

    if (!n)
        return false;
    *out = n->ev;
    free(n);
    return true;
}

/* ------------------------------ vt → session 桥 ------------------------------ */

static void sess_vt_out(const char *bytes, size_t len, void *ud)
{
    hb_session *s = ud;
    char w = 'w';
    pthread_mutex_lock(&s->mu);
    if (s->wq_len + len > s->wq_cap) {
        size_t ncap = (s->wq_len + len) * 2 + 256;
        uint8_t *nb = realloc(s->wq, ncap);
        if (nb) {
            s->wq = nb;
            s->wq_cap = ncap;
        }
    }
    if (s->wq_len + len <= s->wq_cap) {
        memcpy(s->wq + s->wq_len, bytes, len);
        s->wq_len += len;
    }
    pthread_mutex_unlock(&s->mu);
    if (write(s->cmd_w, &w, 1) < 0) {}
}

static void sess_vt_event(const hb_vt_event *ev, void *ud)
{
    hb_session *s = ud;
    switch (ev->type) {
    case HB_VT_EV_BELL:
        pthread_mutex_lock(&s->mu);
        if (s->watch) hb_watch_bell(s->watch, hb_now_ms());
        pthread_mutex_unlock(&s->mu);
        hb__session_emit(s, HB_SESS_EV_BELL, 0, 0, NULL, NULL);
        break;
    case HB_VT_EV_TITLE:
        hb__session_emit(s, HB_SESS_EV_TITLE, 0, 0, ev->text, NULL);
        break;
    case HB_VT_EV_OSC133:
        pthread_mutex_lock(&s->mu);
        if (s->watch) hb_watch_osc133(s->watch, ev->mark, ev->code, hb_now_ms());
        pthread_mutex_unlock(&s->mu);
        hb__session_emit(s, HB_SESS_EV_OSC133, ev->mark, ev->code, NULL, NULL);
        break;
    case HB_VT_EV_NOTIFY:
        pthread_mutex_lock(&s->mu);
        if (s->watch)
            hb_watch_notify(s->watch, ev->text, ev->text2, hb_now_ms());
        pthread_mutex_unlock(&s->mu);
        hb__session_emit(s, HB_SESS_EV_NOTIFY, 0, 0, ev->text, ev->text2);
        break;
    case HB_VT_EV_CLIPBOARD:
        hb__session_emit(s, HB_SESS_EV_CLIPBOARD, 0, 0, ev->text, NULL);
        break;
    case HB_VT_EV_ALTSCREEN:
        hb__session_emit(s, HB_SESS_EV_ALTSCREEN, 0, ev->code, NULL, NULL);
        break;
    case HB_VT_EV_CWD:
        hb__session_emit(s, HB_SESS_EV_CWD, 0, 0, ev->text, NULL);
        break;
    }
}

/* ------------------------------ 线程 ------------------------------ */

static void sess_flush_write(hb_session *s)
{
    uint8_t *chunk = NULL;
    size_t len = 0;
    ssize_t n;

    /* 取出整个队列（写可能短写 → 未写部分塞回队首） */
    pthread_mutex_lock(&s->mu);
    if (s->wq_len) {
        len = s->wq_len;
        chunk = malloc(len);
        if (chunk) {
            memcpy(chunk, s->wq, len);
            s->wq_len = 0;
        } else {
            len = 0;
        }
    }
    pthread_mutex_unlock(&s->mu);

    if (!chunk || len == 0) {
        free(chunk);
        return;
    }
    n = s->tr.write(s->tr.ctx, (const char *)chunk, len);
    if (n < 0)
        n = 0;
    if ((size_t)n < len) {           /* 没写完，塞回队首 */
        pthread_mutex_lock(&s->mu);
        if (len - (size_t)n + s->wq_len > s->wq_cap) {
            size_t ncap = (len - (size_t)n + s->wq_len) * 2;
            uint8_t *nb = realloc(s->wq, ncap);
            if (nb) {
                s->wq = nb;
                s->wq_cap = ncap;
            }
        }
        if (len - (size_t)n + s->wq_len <= s->wq_cap) {
            memmove(s->wq + (len - (size_t)n), s->wq, s->wq_len);
            memcpy(s->wq, chunk + n, len - (size_t)n);
            s->wq_len += len - (size_t)n;
        }
        pthread_mutex_unlock(&s->mu);
    }
    free(chunk);
}

static bool sess_pending_write(hb_session *s)
{
    bool p;
    pthread_mutex_lock(&s->mu);
    p = s->wq_len > 0;
    pthread_mutex_unlock(&s->mu);
    return p;
}

static void *sess_thread(void *arg)
{
    hb_session *s = arg;
    char *buf = malloc(READ_CHUNK);
    bool eof = false;
    bool out_dirty = false;
    int code = -1;

    if (s->tr.connect) {
        int rc = s->tr.connect(s->tr.ctx, s);
        if (rc != 0) {
            /* transport 已经在 connect 里 emit 过具体 ERROR */
            pthread_mutex_lock(&s->mu);
            s->exited = true;
            s->exit_code = -1;
            pthread_mutex_unlock(&s->mu);
            watch_finish(s, -1);
            hb__session_emit(s, HB_SESS_EV_EXIT, 0, -1, "connect failed", NULL);
            free(buf);
            return NULL;
        }
    }

    for (;;) {
        struct pollfd pf[2];
        int nfd, tmo;
        char cmds[64];
        ssize_t nr;

        pf[0].fd = s->cmd_r;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = s->tr.fd(s->tr.ctx);
        pf[1].events = POLLIN | (sess_pending_write(s) ? POLLOUT : 0);
        pf[1].revents = 0;
        /* poll 超时 = min(传输层 tick, watch 建议的下个检查时刻) */
        tmo = s->tr.tick ? s->tr.tick(s->tr.ctx) : -1;
        pthread_mutex_lock(&s->mu);
        if (s->watch && s->watch_next >= 0) {
            int64_t wms = s->watch_next - hb_now_ms();
            int wt = wms <= 0 ? 0 : (wms > 60000 ? 60000 : (int)wms);
            tmo = (tmo < 0) ? wt : (wt < tmo ? wt : tmo);
        }
        pthread_mutex_unlock(&s->mu);
        nfd = poll(pf, 2, tmo);
        if (nfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pf[0].revents & POLLIN) {
            while ((nr = read(s->cmd_r, cmds, sizeof cmds)) > 0) {}
        }

        /* 处理控制命令 */
        pthread_mutex_lock(&s->mu);
        if (s->want_close) {
            pthread_mutex_unlock(&s->mu);
            break;
        }
        if (s->pend_rows > 0) {
            int r = s->pend_rows, c = s->pend_cols;
            s->pend_rows = 0;
            pthread_mutex_unlock(&s->mu);
            hb_vt_resize(s->vt, r, c);
            if (s->tr.resize)
                s->tr.resize(s->tr.ctx, r, c);
        } else {
            pthread_mutex_unlock(&s->mu);
        }

        /* 写冲刷 */
        if (pf[1].revents & (POLLOUT | POLLIN) || sess_pending_write(s))
            sess_flush_write(s);

        /* 读：pty 一次只吐几十字节，聚满一块再喂 vt，否则每次喂都是固定开销 */
        if (pf[1].revents & POLLIN) {
            size_t len = 0;
            for (;;) {
                ssize_t n = s->tr.read(s->tr.ctx, buf + len, READ_CHUNK - len);
                if (n > 0) {
                    len += (size_t)n;
                    if (len < READ_CHUNK)
                        continue;
                } else if (n == 0) {
                    eof = true;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    eof = true;     /* EIO 等 → 视为结束 */
                }
                break;
            }
            if (len) {
                hb_session_tap_fn tf;
                void *td;
                pthread_mutex_lock(&s->mu);
                tf = s->tap;
                td = s->tap_ud;
                pthread_mutex_unlock(&s->mu);
                if (tf)
                    tf(s, buf, len, td);
                watch_feed(s, buf, len);
                hb_vt_feed(s->vt, buf, len);
                out_dirty = true;
            }
        }
        if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            eof = true;

        if (s->tr.poll_exit && s->tr.poll_exit(s->tr.ctx, &code)) {
            /* 进程已死：可能还有残留输出，下一轮 POLLIN 读干后再退 */
            if (!eof && (pf[1].revents & POLLIN) == 0)
                eof = true;
        }
        /* 每轮 poll 通知一次；out_pending 事件侧再合并 */
        if (out_dirty) {
            hb__session_emit(s, HB_SESS_EV_OUTPUT, 0, 0, NULL, NULL);
            out_dirty = false;
        }
        watch_drain(s);
        if (eof)
            break;
    }

    /* 主动关闭 → 传输层挂断（SIGHUP 等价物） */
    {
        bool wc;
        pthread_mutex_lock(&s->mu);
        wc = s->want_close;
        pthread_mutex_unlock(&s->mu);
        if (wc && s->tr.close)
            s->tr.close(s->tr.ctx);
    }

    /* 收尾：EOF 前最后一批输出也通知 */
    if (out_dirty)
        hb__session_emit(s, HB_SESS_EV_OUTPUT, 0, 0, NULL, NULL);

    /* 取退出码 */
    if (code < 0 && s->tr.poll_exit) {
        int c = -1, i;
        for (i = 0; i < 100 && !s->tr.poll_exit(s->tr.ctx, &c); i++)
            usleep(1000);
        if (i < 100)
            code = c;
    }

    pthread_mutex_lock(&s->mu);
    s->exited = true;
    s->exit_code = code;
    pthread_mutex_unlock(&s->mu);
    watch_finish(s, code);            /* EOF hit 先于 EXIT */
    hb__session_emit(s, HB_SESS_EV_EXIT, 0, code,
                     s->tr.exit_text ? s->tr.exit_text(s->tr.ctx)
                                     : "process exited",
                     NULL);
    free(buf);
    return NULL;
}

/* ------------------------------ 构造/接口 ------------------------------ */

static int pty_fd_op(void *ctx) { return hb_pty_fd(ctx); }
static ssize_t pty_read_op(void *ctx, char *b, size_t n) { return read(hb_pty_fd(ctx), b, n); }
static ssize_t pty_write_op(void *ctx, const char *b, size_t n) { return write(hb_pty_fd(ctx), b, n); }
static int pty_resize_op(void *ctx, int r, int c) { return hb_pty_resize(ctx, r, c) == HB_OK ? 0 : -1; }
static bool pty_poll_exit_op(void *ctx, int *code) { return hb_pty_reap(ctx, code); }
static void pty_close_op(void *ctx) { hb_pty_hangup(ctx); }
static void pty_destroy_op(void *ctx) { hb_pty_free(ctx); }

hb_session *hb_session_new_transport(hb_session_transport *tr,
                                   int rows, int cols, int scrollback_lines,
                                   hb_vt *reuse_vt)
{
    hb_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->tr = *tr;
    if (reuse_vt) {
        s->vt = reuse_vt;
        hb_vt_resize(s->vt, rows, cols);
    } else {
        s->vt = hb_vt_new(rows, cols, scrollback_lines);
    }
    if (!s->vt) {
        free(s);
        return NULL;
    }
    hb_vt_set_output_fn(s->vt, sess_vt_out, s);
    hb_vt_set_event_fn(s->vt, sess_vt_event, s);

    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->reply_cv, NULL);
    s->pend_rows = 0;
    s->exit_code = -1;
    s->watch_next = -1;

    {
        int p[2];
        if (pipe(p) != 0)
            goto fail;
        s->cmd_r = p[0];
        s->cmd_w = p[1];
    }
    {
        int p[2];
        if (pipe(p) != 0) {
            close(s->cmd_r);
            close(s->cmd_w);
            goto fail;
        }
        s->ev_r = p[0];
        s->ev_w = p[1];
    }
    {
        int flags;
        flags = fcntl(s->cmd_r, F_GETFL);
        fcntl(s->cmd_r, F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(s->cmd_w, F_GETFL);
        fcntl(s->cmd_w, F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(s->ev_r, F_GETFL);
        fcntl(s->ev_r, F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(s->ev_w, F_GETFL);
        fcntl(s->ev_w, F_SETFL, flags | O_NONBLOCK);
    }

    if (pthread_create(&s->th, NULL, sess_thread, s) != 0) {
        close(s->cmd_r); close(s->cmd_w);
        close(s->ev_r);  close(s->ev_w);
        goto fail;
    }
    s->th_started = true;
    return s;

fail:
    /* 复用的 vt 不在失败时释放：所有权仍归调用方（摘掉指向本 s 的回调） */
    if (s->vt == reuse_vt) {
        hb_vt_set_output_fn(s->vt, NULL, NULL);
        hb_vt_set_event_fn(s->vt, NULL, NULL);
    } else {
        hb_vt_free(s->vt);
    }
    free(s);
    return NULL;
}

hb_session *hb_session_new_local_vt(const hb_pty_opts *opts, hb_vt *reuse,
                                    int scrollback_lines)
{
    hb_pty *p = NULL;
    hb_session_transport tr;
    int rows = 24, cols = 80;

    if (hb_pty_spawn(opts, &p) != HB_OK || !p)
        return NULL;
    if (opts) {
        if (opts->rows > 0) rows = opts->rows;
        if (opts->cols > 0) cols = opts->cols;
    }
    memset(&tr, 0, sizeof tr);
    tr.ctx = p;
    tr.fd = pty_fd_op;
    tr.read = pty_read_op;
    tr.write = pty_write_op;
    tr.resize = pty_resize_op;
    tr.poll_exit = pty_poll_exit_op;
    tr.close = pty_close_op;
    tr.destroy = pty_destroy_op;
    tr.connect = NULL;

    {
        hb_session *s = hb_session_new_transport(&tr, rows, cols,
                                                 scrollback_lines, reuse);
        if (!s) {
            hb_pty_free(p);
            return NULL;
        }
        return s;
    }
}

hb_session *hb_session_new_local(const hb_pty_opts *opts, int scrollback_lines)
{
    return hb_session_new_local_vt(opts, NULL, scrollback_lines);
}

hb_vt *hb_session_vt(hb_session *s) { return s ? s->vt : NULL; }

hb_vt *hb_session_steal_vt(hb_session *s)
{
    hb_vt *v;
    if (!s)
        return NULL;
    /* 仅 EXIT 之后允许：线程设置 exited 之后不再触碰 vt。
     * alive()==false 但 want_close 时线程可能仍在喂 vt —— 必须看 exited。 */
    pthread_mutex_lock(&s->mu);
    if (!s->exited) {
        pthread_mutex_unlock(&s->mu);
        return NULL;
    }
    v = s->vt;
    s->vt = NULL;
    pthread_mutex_unlock(&s->mu);
    /* 摘掉指向本 session 的回调，复用方会在 new_transport 里重新挂上。 */
    if (v) {
        hb_vt_set_output_fn(v, NULL, NULL);
        hb_vt_set_event_fn(v, NULL, NULL);
    }
    return v;
}

void hb_session_set_watch(hb_session *s, const hb_watch_cfg *cfg)
{
    hb_watch *nw, *old;
    char w = 'w';
    if (!s)
        return;
    /* 引擎构造（regex 编译）可在锁外做；替换在锁内是原子的。 */
    nw = cfg ? hb_watch_new(cfg) : NULL;
    pthread_mutex_lock(&s->mu);
    old = s->watch;
    s->watch = nw;
    s->watch_next = -1;
    pthread_mutex_unlock(&s->mu);
    if (old)
        hb_watch_free(old);
    /* 唤醒 session 线程重算 poll 超时 */
    if (nw && write(s->cmd_w, &w, 1) < 0) {}
}

void hb_session_write(hb_session *s, const char *data, size_t len)
{
    char w = 'w';
    if (!s || !data)
        return;
    watch_input(s);
    pthread_mutex_lock(&s->mu);
    if (s->wq_len + len > s->wq_cap) {
        size_t ncap = (s->wq_len + len) * 2 + 256;
        uint8_t *nb = realloc(s->wq, ncap);
        if (nb) {
            s->wq = nb;
            s->wq_cap = ncap;
        }
    }
    if (s->wq_len + len <= s->wq_cap) {
        memcpy(s->wq + s->wq_len, data, len);
        s->wq_len += len;
    }
    pthread_mutex_unlock(&s->mu);
    if (write(s->cmd_w, &w, 1) < 0) {}
}

void hb_session_key(hb_session *s, hb_key key, int mods)
{
    if (!s || !s->vt)
        return;
    watch_input(s);
    hb_vt_key(s->vt, key, mods);
}

void hb_session_unichar(hb_session *s, uint32_t c, int mods)
{
    if (!s || !s->vt)
        return;
    watch_input(s);
    hb_vt_unichar(s->vt, c, mods);
}

void hb_session_paste(hb_session *s, const char *utf8)
{
    if (!s || !s->vt)
        return;
    watch_input(s);
    hb_vt_paste(s->vt, utf8);
}

void hb_session_resize(hb_session *s, int rows, int cols)
{
    char w = 'r';
    pthread_mutex_lock(&s->mu);
    s->pend_rows = rows;
    s->pend_cols = cols;
    pthread_mutex_unlock(&s->mu);
    if (write(s->cmd_w, &w, 1) < 0) {}
}

bool hb_session_alive(hb_session *s)
{
    bool a;
    if (!s)
        return false;
    pthread_mutex_lock(&s->mu);
    a = !s->exited && !s->want_close;
    pthread_mutex_unlock(&s->mu);
    return a;
}

void hb_session_close(hb_session *s)
{
    char w = 'x';
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->want_close = true;
    pthread_cond_broadcast(&s->reply_cv);   /* 唤醒阻塞在应答等待的连接线程 */
    pthread_mutex_unlock(&s->mu);
    if (write(s->cmd_w, &w, 1) < 0) {}
}

void hb_session_set_tap(hb_session *s, hb_session_tap_fn fn, void *ud)
{
    pthread_mutex_lock(&s->mu);
    s->tap = fn;
    s->tap_ud = ud;
    pthread_mutex_unlock(&s->mu);
}

void hb_session_free(hb_session *s)
{
    ev_node *n;
    if (!s)
        return;
    if (s->th_started) {
        hb_session_close(s);
        pthread_join(s->th, NULL);
    }
    if (s->tr.destroy)
        s->tr.destroy(s->tr.ctx);
    /* 排空事件队列 */
    while (s->ev_head) {
        n = s->ev_head;
        s->ev_head = n->next;
        free(n->ev.text);
        free(n->ev.text2);
        free(n->ev.extra);
        if (n->ev.watch) {
            hb_watch_hit_clear(n->ev.watch);
            free(n->ev.watch);
        }
        free(n);
    }
    close(s->cmd_r); close(s->cmd_w);
    close(s->ev_r);  close(s->ev_w);
    free(s->wq);
    if (s->reply_secret)
        hb_secret_free(s->reply_secret);
    if (s->watch)
        hb_watch_free(s->watch);
    hb_vt_free(s->vt);
    pthread_cond_destroy(&s->reply_cv);
    pthread_mutex_destroy(&s->mu);
    free(s);
}
