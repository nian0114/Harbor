#include "harbor/session.h"

#include "harbor/sftp.h"
#include "harbor/pf.h"
#include "session_priv.h"

#include <errno.h>
#include <stdio.h>
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

typedef struct run_node {
    hb_session_run_fn fn;
    void *ud;
    struct run_node *next;
} run_node;

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

    /* run_on_thread 队列：UI 追加、线程取干 */
    struct run_node *run_head, *run_tail;

    void *udata;            /* transport 私有数据（SFTP 引擎） */

    /* 完成监控引擎（FR-13）：仅 session 线程在 mu 下触碰；
     * set_watch 换引擎也在 mu 内完成。watch_next = poll 建议的下个检查时刻。 */
    hb_watch *watch;
    int64_t   watch_next;

    /* Shell History 命令采集（FR-08）。
     * hist_on 由 UI 线程在 mu 下设置；
     * osc_* 仅 session 线程触碰（vt 事件置标志，feed 后提取屏幕文本）；
     * hl_* 仅 UI 线程输入路径触碰，echo 判定标志例外地在 mu 下同步。 */
    bool hist_on;
    bool osc133_seen;            /* 见过任一 'B' → OSC 133 路径接管 */
    bool osc_b_valid;
    int  osc_b_row, osc_b_col;
    uint64_t osc_b_pushed;
    bool osc_c_flag;
    int  osc_c_row, osc_c_col;
    uint64_t osc_c_pushed;
    /* C 到达时快照的 B 位置：同一 feed 块里下一个提示符的 B
     * 会覆盖 osc_b_*，待提取的区间必须有自己的副本。 */
    int  osc_pb_row, osc_pb_col;
    uint64_t osc_pb_pushed;

    char  *hl; size_t hl_len, hl_cap;  /* 启发式累积的输入行（utf8） */
    bool   hl_bad;                     /* 方向/历史/Tab/控制键 → 行不可信 */
    bool   hl_typed;                   /* 行内有可打印输入 */
    int    hl_srow, hl_scol;           /* 首次输入的屏幕位置（=提示行尾） */
    uint64_t hl_pushed;
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
    if (ev->sftp) {
        hb_sftp_result_free(ev->sftp);
        ev->sftp = NULL;
    }
    if (ev->pf) {
        hb_pf_status_free(ev->pf);
        ev->pf = NULL;
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

/* ------------------------------ Shell History（FR-08） ------------------------------
 * 两条采集路径：
 *  - OSC 133：见过 'B' 后，命令 = 屏幕上 B→C 之间的回显文本（取 vt 已有解析，
 *    不复制转义状态机）。位置按 total_pushed 换算回滚屏差。
 *  - 回退启发式：用户输入拼行；Enter 时在 密码提示/备用屏/不可信行/无前导空格/
 *    no-echo 等条件下才 emit。 */

static void hist_line_reset(hb_session *s)
{
    s->hl_len = 0;
    if (s->hl)
        s->hl[0] = 0;
    s->hl_bad = false;
    s->hl_typed = false;
    s->hl_srow = -1;
}

static void hist_sample_pos(hb_session *s)
{
    int r, c, shape;
    bool vis, bl;
    hb_vt_cursor(s->vt, &r, &c, &vis, &bl, &shape);
    s->hl_srow = r;
    s->hl_scol = c;
    s->hl_pushed = hb_vt_total_pushed(s->vt);
}

static void hist_put(hb_session *s, const char *utf8, size_t n)
{
    if (s->hl_len == 0)
        hist_sample_pos(s);
    if (s->hl_len + n + 1 > s->hl_cap) {
        size_t nc = s->hl_cap ? s->hl_cap * 2 : 256;
        char *nb;
        while (s->hl_len + n + 1 > nc)
            nc *= 2;
        nb = realloc(s->hl, nc);
        if (!nb)
            return;
        s->hl = nb;
        s->hl_cap = nc;
    }
    memcpy(s->hl + s->hl_len, utf8, n);
    s->hl_len += n;
    s->hl[s->hl_len] = 0;
    s->hl_typed = true;
}

/* 删最后一个 utf8 codepoint */
static void hist_bs(hb_session *s)
{
    if (!s->hl_len)
        return;
    do {
        s->hl_len--;
    } while (s->hl_len &&
         ((unsigned char)s->hl[s->hl_len] & 0xc0) == 0x80);
    s->hl[s->hl_len] = 0;
}

/* 密码/敏感提示检测：输入起点之前的屏幕行文本含关键词且以 :/： 结尾。
 * （read -p "[sudo] password for x:" 这类有回显的提示也覆盖） */
static int ci_has(const char *hay, const char *needle)
{
    size_t hl = strlen(hay), nl = strlen(needle), i, j;
    if (!nl || nl > hl)
        return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b)
                break;
        }
        if (j == nl)
            return 1;
    }
    return 0;
}

static bool hist_secret_prompt(hb_session *s)
{
    static const char *KWS[] = {
        "pass", "password", "passphrase", "密码", "口令", "pin",
        "token", "otp", "verification code"
    };
    char *t;
    bool sec = false;
    size_t n, i;
    long r;

    if (s->hl_srow < 0)
        return false;
    r = s->hl_srow - (long)(hb_vt_total_pushed(s->vt) - s->hl_pushed);
    t = hb_vt_get_text(s->vt, (int)r, 0, (int)r, s->hl_scol, true);
    if (!t)
        return false;
    n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t'))
        n--;
    /* 必须以 ':' 或 '：'(EF BC 9A) 结尾 */
    if ((n >= 1 && t[n - 1] == ':') ||
        (n >= 3 && !memcmp(t + n - 3, "\xef\xbc\x9a", 3))) {
        for (i = 0; i < sizeof KWS / sizeof KWS[0] && !sec; i++)
            sec = ci_has(t, KWS[i]);
    }
    free(t);
    return sec;
}

/* no-echo 判定（密码/隐藏输入）：键入期间光标没动 = 输入未被回显。
 * read -s / stty -echo 之类；比"等输出字节"更直接（Enter 自身换行不算）。 */
static bool hist_noecho(hb_session *s)
{
    int r, c, shape;
    bool vis, bl;
    if (!s->hl_typed || s->hl_srow < 0)
        return false;
    hb_vt_cursor(s->vt, &r, &c, &vis, &bl, &shape);
    return (hb_vt_total_pushed(s->vt) + (uint64_t)r) ==
               (s->hl_pushed + (uint64_t)s->hl_srow) &&
           c == s->hl_scol;
}

/* Enter：按回退启发式决定是否上报命令（OSC 133 见过 'B' 后交给 OSC 路径）。 */
static void hist_finish_line(hb_session *s)
{
    bool emit_it;

    if (!s->hist_on)
        return;
    emit_it = !s->osc133_seen && !s->hl_bad && s->hl_len &&
              s->hl[0] != ' ' && !hist_noecho(s) &&
              !hb_vt_altscreen(s->vt) && !hist_secret_prompt(s);
    if (emit_it)
        hb__session_emit(s, HB_SESS_EV_COMMAND, 0, 0, s->hl, NULL);
    hist_line_reset(s);
}

/* write/paste 路径的原始字节流（IME commit / 粘贴文本） */
static void hist_bytes(hb_session *s, const char *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\r' || c == '\n')
            hist_finish_line(s);
        else if (c == 0x08 || c == 0x7f)
            hist_bs(s);
        else if (c == 0x03 || c == 0x15)        /* ^C ^U */
            hist_line_reset(s);
        else if (c == 0x09)                    /* Tab 补全 → 行不可信 */
            s->hl_bad = true;
        else if (c < 0x20 || c == 0x1b)
            s->hl_bad = true;
        else
            hist_put(s, data + i, 1);
    }
}

/* session 线程：feed 之后处理 OSC 133 C 提取 */
static void hist_after_feed(hb_session *s)
{
    bool c;
    int cr, cc;
    uint64_t cp;

    int br, bc;
    uint64_t bp;

    pthread_mutex_lock(&s->mu);
    c = s->hist_on && s->osc_c_flag;
    s->osc_c_flag = false;
    cr = s->osc_c_row;
    cc = s->osc_c_col;
    cp = s->osc_c_pushed;
    br = s->osc_pb_row;
    bc = s->osc_pb_col;
    bp = s->osc_pb_pushed;
    pthread_mutex_unlock(&s->mu);
    if (!c)
        return;
    {
        uint64_t pn = hb_vt_total_pushed(s->vt);
        long r0 = br - (long)(pn - bp);
        long r1 = cr - (long)(pn - cp);
        char *txt = hb_vt_get_text(s->vt, (int)r0, bc,
                                   (int)r1, cc, false);
        if (txt) {
            char *b = txt, *e;
            while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')
                b++;
            e = b + strlen(b);
            while (e > b && (e[-1] == ' ' || e[-1] == '\t' ||
                             e[-1] == '\r' || e[-1] == '\n'))
                *--e = 0;
            if (*b && !hb_vt_altscreen(s->vt))
                hb__session_emit(s, HB_SESS_EV_COMMAND, 0, 0, b, NULL);
            free(txt);
        }
    }
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

/* SFTP 结果事件：r 的所有权移交事件队列 */
void hb__session_emit_sftp(hb_session *s, struct hb_sftp_result *r)
{
    ev_node *n;
    char w = 'e';
    if (!s || !r)
        return;
    n = calloc(1, sizeof *n);
    if (!n) {
        hb_sftp_result_free(r);
        return;
    }
    n->ev.type = HB_SESS_EV_SFTP;
    n->ev.sftp = r;
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

/* 转发状态事件：st 的所有权移交事件队列 */
void hb__session_emit_pf(hb_session *s, struct hb_pf_status *st)
{
    ev_node *n;
    char w = 'e';
    if (!s || !st)
        return;
    n = calloc(1, sizeof *n);
    if (!n) {
        hb_pf_status_free(st);
        return;
    }
    n->ev.type = HB_SESS_EV_FORWARD;
    n->ev.pf = st;
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

void hb__session_set_udata(hb_session *s, void *p)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->udata = p;
    pthread_mutex_unlock(&s->mu);
}

void *hb__session_udata(hb_session *s)
{
    void *p;
    if (!s)
        return NULL;
    pthread_mutex_lock(&s->mu);
    p = s->udata;
    pthread_mutex_unlock(&s->mu);
    return p;
}

void hb__session_wake(hb_session *s)
{
    char w = 'w';
    if (!s)
        return;
    if (write(s->cmd_w, &w, 1) < 0) {}
}

int hb__session_cmd_fd(hb_session *s)
{
    return s ? s->cmd_r : -1;
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
        if (s->hist_on) {
            if (ev->mark == 'B') {
                s->osc133_seen = true;
                s->osc_b_valid = true;
                s->osc_b_row = ev->row;
                s->osc_b_col = ev->col;
                s->osc_b_pushed = ev->pushed;
            } else if (ev->mark == 'C' && s->osc_b_valid) {
                s->osc_c_flag = true;
                s->osc_c_row = ev->row;
                s->osc_c_col = ev->col;
                s->osc_c_pushed = ev->pushed;
                s->osc_pb_row = s->osc_b_row;
                s->osc_pb_col = s->osc_b_col;
                s->osc_pb_pushed = s->osc_b_pushed;
                s->osc_b_valid = false;
            }
        }
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

/* session 线程：取干并执行 run 队列（read→tap→feed 间隙调用） */
static void sess_run_queued(hb_session *s)
{
    for (;;) {
        run_node *n;
        pthread_mutex_lock(&s->mu);
        n = s->run_head;
        if (n) {
            s->run_head = n->next;
            if (!s->run_head)
                s->run_tail = NULL;
        }
        pthread_mutex_unlock(&s->mu);
        if (!n)
            break;
        n->fn(s, n->ud);
        free(n);
    }
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

    /* 自定义 run 循环（SFTP）：接管线程直到关闭/断线，返回值即 EXIT code */
    if (s->tr.run) {
        int rc = s->tr.run(s->tr.ctx, s);
        pthread_mutex_lock(&s->mu);
        s->exited = true;
        s->exit_code = rc;
        pthread_mutex_unlock(&s->mu);
        watch_finish(s, rc);
        hb__session_emit(s, HB_SESS_EV_EXIT, 0, rc,
                         s->tr.exit_text ? s->tr.exit_text(s->tr.ctx)
                                         : "session ended",
                         NULL);
        free(buf);
        return NULL;
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

        /* run_on_thread 队列：此刻处于两次 read→tap→feed 之间，
         * 回调取到的 vt 状态与下一个 tap 的 dseq 严格同点。 */
        sess_run_queued(s);

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
                hist_after_feed(s);
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

void hb_session_set_history(hb_session *s, bool on)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->hist_on = on;
    if (!on) {
        s->osc_c_flag = false;
        s->osc_b_valid = false;
        s->osc133_seen = false;
    }
    pthread_mutex_unlock(&s->mu);
    if (!on) {
        s->hl_len = 0;
        if (s->hl) s->hl[0] = 0;
        s->hl_bad = s->hl_typed = false;
        s->hl_srow = -1;
    }
}

void hb_session_write(hb_session *s, const char *data, size_t len)
{
    char w = 'w';
    if (!s || !data)
        return;
    watch_input(s);
    if (s->hist_on)
        hist_bytes(s, data, len);
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
    if (s->hist_on) {
        if (mods)
            s->hl_bad = true;
        else if (key == HB_KEY_ENTER)
            hist_finish_line(s);
        else if (key == HB_KEY_BACKSPACE)
            hist_bs(s);
        else
            s->hl_bad = true;
    }
    hb_vt_key(s->vt, key, mods);
}

void hb_session_unichar(hb_session *s, uint32_t c, int mods)
{
    char u8[4];
    if (!s || !s->vt)
        return;
    watch_input(s);
    if (s->hist_on) {
        if ((mods & ~HB_MOD_SHIFT) == 0 && c >= 0x20 && c != 0x7f) {
            u8[0] = 0;
            {
                /* utf8 编码（与 vt.c 同款） */
                size_t n;
                if (c < 0x80) {
                    u8[0] = (char)c;
                    n = 1;
                } else if (c < 0x800) {
                    u8[0] = (char)(0xc0 | (c >> 6));
                    u8[1] = (char)(0x80 | (c & 0x3f));
                    n = 2;
                } else if (c < 0x10000) {
                    u8[0] = (char)(0xe0 | (c >> 12));
                    u8[1] = (char)(0x80 | ((c >> 6) & 0x3f));
                    u8[2] = (char)(0x80 | (c & 0x3f));
                    n = 3;
                } else {
                    u8[0] = (char)(0xf0 | (c >> 18));
                    u8[1] = (char)(0x80 | ((c >> 12) & 0x3f));
                    u8[2] = (char)(0x80 | ((c >> 6) & 0x3f));
                    u8[3] = (char)(0x80 | (c & 0x3f));
                    n = 4;
                }
                hist_put(s, u8, n);
            }
        } else if ((c & 0x7f) == 0x03 || (c & 0x7f) == 0x15 ||
                   ((mods & HB_MOD_CTRL) &&
                    ((c & ~0x20u) == 'C' || (c & ~0x20u) == 'U')))
            /* ^C/^U：控制字节或 Ctrl+字母两种入径都要清行重录 */
            hist_line_reset(s);
        else
            s->hl_bad = true;                  /* Ctrl/Meta 编辑/搜索等 */
    }
    hb_vt_unichar(s->vt, c, mods);
}

void hb_session_paste(hb_session *s, const char *utf8)
{
    if (!s || !s->vt)
        return;
    watch_input(s);
    if (s->hist_on && utf8)
        hist_bytes(s, utf8, strlen(utf8));
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

void hb_session_run_on_thread(hb_session *s, hb_session_run_fn fn, void *ud)
{
    run_node *n;
    char w = 'r';
    bool run_now = false;
    if (!s || !fn)
        return;
    pthread_mutex_lock(&s->mu);
    run_now = !s->th_started || s->exited;
    if (!run_now) {
        n = calloc(1, sizeof *n);
        if (n) {
            n->fn = fn;
            n->ud = ud;
            if (s->run_tail)
                s->run_tail->next = n;
            else
                s->run_head = n;
            s->run_tail = n;
        }
    }
    pthread_mutex_unlock(&s->mu);
    if (run_now) {
        /* 线程已退出：vt 不再被喂，调用方线程持锁运行即可（语义同点）。 */
        if (s->vt)
            hb_vt_lock(s->vt);
        fn(s, ud);
        if (s->vt)
            hb_vt_unlock(s->vt);
        return;
    }
    if (write(s->cmd_w, &w, 1) < 0) {}
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
        if (n->ev.sftp)
            hb_sftp_result_free(n->ev.sftp);
        free(n);
    }
    close(s->cmd_r); close(s->cmd_w);
    close(s->ev_r);  close(s->ev_w);
    while (s->run_head) {
        run_node *n = s->run_head;
        s->run_head = n->next;
        free(n);
    }
    free(s->wq);
    if (s->reply_secret)
        hb_secret_free(s->reply_secret);
    if (s->watch)
        hb_watch_free(s->watch);
    free(s->hl);
    hb_vt_free(s->vt);
    pthread_cond_destroy(&s->reply_cv);
    pthread_mutex_destroy(&s->mu);
    free(s);
}
