/* rd.c — FR-07 远程桌面公共管线：事件队列、帧缓冲、输入队列、答复通道、worker 管理 */

#include "rd_priv.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sodium.h>

/* ---------- 小工具 ---------- */

static int mkpipe_nonblock(int fds[2])
{
    if (pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFL);
        fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }
    return 0;
}

static char *xstrdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* ---------- options 深拷贝 ---------- */

bool hb__rd_opts_copy(hb_rd *rd, const hb_rd_opts *o)
{
    memset(&rd->o, 0, sizeof rd->o);
    rd->o.proto = o->proto;
    rd->o.port = o->port;
    rd->o.quality = o->quality;
    rd->o.view_only = o->view_only;
    rd->o.width = o->width;
    rd->o.height = o->height;
    rd->o.connect_timeout_s = o->connect_timeout_s > 0 ? o->connect_timeout_s : 15;
    rd->o.host = xstrdup(o->host);
    rd->o.user = xstrdup(o->user);
    rd->o.domain = xstrdup(o->domain);
    rd->o.cert_fp = xstrdup(o->cert_fp);
    rd->o.tls_hostname = xstrdup(o->tls_hostname);
    rd->o.freerdp_config_dir = xstrdup(o->freerdp_config_dir);
    if (o->password)
        rd->password = hb_secret_strdup(o->password);
    return true;
}

void hb__rd_opts_free(hb_rd *rd)
{
    free((char *)rd->o.host);
    free((char *)rd->o.user);
    free((char *)rd->o.domain);
    free((char *)rd->o.cert_fp);
    free((char *)rd->o.tls_hostname);
    free((char *)rd->o.freerdp_config_dir);
    hb__rd_wipe_password(rd);
}

void hb__rd_wipe_password(hb_rd *rd)
{
    if (rd->password) {
        sodium_free(rd->password);
        rd->password = NULL;
    }
    rd->o.password = NULL;
}

/* ---------- 事件队列 ---------- */

static void ev_notify(hb_rd *rd)
{
    uint8_t b = 'e';
    ssize_t r = write(rd->ev_w, &b, 1);
    (void)r;
}

/* 在 ev_mu 下调用：把 ev 挂队尾。ev 内 malloc 字段由队列接管。 */
static void ev_push_locked(hb_rd *rd, hb_rd_event *ev)
{
    rd_ev_node *n = calloc(1, sizeof *n);
    if (!n)
        return;
    n->ev = *ev;
    if (ev->type == HB_RD_EV_FRAME) {
        if (rd->ev_frame) {
            /* 合并损坏并集 */
            hb_rd_event *f = &rd->ev_frame->ev;
            int x2 = f->x + f->w, y2 = f->y + f->h;
            int nx2 = ev->x + ev->w, ny2 = ev->y + ev->h;
            f->x = f->x < ev->x ? f->x : ev->x;
            f->y = f->y < ev->y ? f->y : ev->y;
            f->w = (x2 > nx2 ? x2 : nx2) - f->x;
            f->h = (y2 > ny2 ? y2 : ny2) - f->y;
            free(n);
            return;
        }
        rd->ev_frame = n;
    }
    if (rd->ev_tail)
        rd->ev_tail->next = n;
    else
        rd->ev_head = n;
    rd->ev_tail = n;
}

void hb__rd_push_owned(hb_rd *rd, hb_rd_event *ev)
{
    pthread_mutex_lock(&rd->ev_mu);
    ev_push_locked(rd, ev);
    pthread_mutex_unlock(&rd->ev_mu);
    ev_notify(rd);
}

void hb__rd_push(hb_rd *rd, const hb_rd_event *ev)
{
    hb_rd_event c = *ev;
    c.px = NULL;
    c.text = c.text ? strdup(c.text) : NULL;
    c.text2 = c.text2 ? strdup(c.text2) : NULL;
    hb__rd_push_owned(rd, &c);
}

void hb__rd_stage(hb_rd *rd, const char *text)
{
    hb_rd_event ev = { .type = HB_RD_EV_STAGE, .text = (char *)text };
    hb__rd_push(rd, &ev);
}

void hb__rd_error(hb_rd *rd, hb_rd_err code, const char *cn, const char *detail)
{
    hb_rd_event ev = { .type = HB_RD_EV_ERROR,
                       .code = code,
                       .text = (char *)cn,
                       .text2 = (char *)detail };
    hb__rd_push(rd, &ev);
}

void hb__rd_connected(hb_rd *rd, int w, int h, const char *name, const char *sec)
{
    hb_rd_event ev = { .type = HB_RD_EV_CONNECTED,
                       .w = w,
                       .h = h,
                       .text = (char *)name,
                       .text2 = (char *)sec };
    hb__rd_push(rd, &ev);
}

void hb__rd_resize(hb_rd *rd, int w, int h)
{
    hb_rd_event ev = { .type = HB_RD_EV_RESIZE, .w = w, .h = h };
    hb__rd_push(rd, &ev);
}

void hb__rd_frame(hb_rd *rd, int x, int y, int w, int h)
{
    hb_rd_event ev = { .type = HB_RD_EV_FRAME, .x = x, .y = y, .w = w, .h = h };
    hb__rd_push_owned(rd, &ev);
}

void hb__rd_clipboard(hb_rd *rd, const char *utf8)
{
    hb_rd_event ev = { .type = HB_RD_EV_CLIPBOARD, .text = (char *)utf8 };
    hb__rd_push(rd, &ev);
}

void hb__rd_cursor(hb_rd *rd, int w, int h, int hx, int hy, const uint8_t *bgra)
{
    hb_rd_event ev = { .type = HB_RD_EV_CURSOR, .w = w, .h = h, .hx = hx, .hy = hy };
    ev.px = malloc((size_t)w * h * 4);
    if (!ev.px)
        return;
    memcpy(ev.px, bgra, (size_t)w * h * 4);
    hb__rd_push_owned(rd, &ev);
}

void hb__rd_exit(hb_rd *rd)
{
    hb_rd_event ev = { .type = HB_RD_EV_EXIT };
    pthread_mutex_lock(&rd->ev_mu);
    rd->exited = true;
    ev_push_locked(rd, &ev);
    pthread_mutex_unlock(&rd->ev_mu);
    ev_notify(rd);
}

int hb_rd_event_fd(hb_rd *rd)
{
    return rd->ev_r;
}

bool hb_rd_next_event(hb_rd *rd, hb_rd_event *ev)
{
    pthread_mutex_lock(&rd->ev_mu);
    rd_ev_node *n = rd->ev_head;
    if (!n) {
        pthread_mutex_unlock(&rd->ev_mu);
        return false;
    }
    rd->ev_head = n->next;
    if (rd->ev_tail == n)
        rd->ev_tail = NULL;
    if (rd->ev_frame == n)
        rd->ev_frame = NULL;
    *ev = n->ev;
    free(n);
    pthread_mutex_unlock(&rd->ev_mu);
    /* 队列可能空了：管道字节无妨，UI 反正 next_event 到 false 为止 */
    return true;
}

void hb_rd_event_clear(hb_rd_event *ev)
{
    if (!ev)
        return;
    free(ev->text);
    free(ev->text2);
    free(ev->px);
    memset(ev, 0, sizeof *ev);
}

/* ---------- 帧缓冲 ---------- */

bool hb__rd_fb_update(hb_rd *rd, const uint8_t *src, int src_stride,
                      int w, int h, int x, int y, int dw, int dh)
{
    pthread_mutex_lock(&rd->fb_mu);
    if (w != rd->fb_w || h != rd->fb_h) {
        int stride = w * 4;
        uint8_t *nb = realloc(rd->fb, (size_t)stride * h);
        if (!nb) {
            pthread_mutex_unlock(&rd->fb_mu);
            return false;
        }
        rd->fb = nb;
        rd->fb_w = w;
        rd->fb_h = h;
        rd->fb_stride = stride;
    }
    for (int row = 0; row < dh; row++) {
        const uint8_t *s = src + (size_t)(y + row) * src_stride + (size_t)x * 4;
        uint8_t *d = rd->fb + (size_t)(y + row) * rd->fb_stride + (size_t)x * 4;
        memcpy(d, s, (size_t)dw * 4);
    }
    rd->fb_ready = true;
    pthread_mutex_unlock(&rd->fb_mu);
    hb__rd_frame(rd, x, y, dw, dh);
    return true;
}

bool hb_rd_fb_lock(hb_rd *rd, const uint8_t **px, int *w, int *h, int *stride)
{
    pthread_mutex_lock(&rd->fb_mu);
    if (!rd->fb_ready || !rd->fb) {
        pthread_mutex_unlock(&rd->fb_mu);
        return false;
    }
    if (px)
        *px = rd->fb;
    if (w)
        *w = rd->fb_w;
    if (h)
        *h = rd->fb_h;
    if (stride)
        *stride = rd->fb_stride;
    return true;
}

void hb_rd_fb_unlock(hb_rd *rd)
{
    pthread_mutex_unlock(&rd->fb_mu);
}

/* ---------- 输入队列 ---------- */

#define RD_IN_CAP 4096 /* 指针事件上限（软上限：按钮转移/键/剪贴板仍可超限） */

static void in_enqueue(hb_rd *rd, const rd_in *in)
{
    pthread_mutex_lock(&rd->in_mu);
    /* 指针移动合并：队尾同为 POINTER 且 buttons 相同 → 只更新坐标。
     * 按钮变化绝不合并（每次按钮转移都必须到达对端）。 */
    if (in->t == RD_IN_POINTER && rd->in_len > 0) {
        rd_in *tail = &rd->in_q[rd->in_len - 1];
        if (tail->t == RD_IN_POINTER && tail->buttons == in->buttons) {
            tail->x = in->x;
            tail->y = in->y;
            pthread_mutex_unlock(&rd->in_mu);
            return;
        }
    }
    /* 满：纯移动（buttons 与上一条入队 POINTER 相同）丢弃；
     * 按钮转移、键、剪贴板仍放行——它们稀少且不可丢。 */
    if (rd->in_len >= RD_IN_CAP && in->t == RD_IN_POINTER &&
        in->buttons == rd->in_last_ptr_buttons) {
        pthread_mutex_unlock(&rd->in_mu);
        return;
    }
    if (rd->in_len == rd->in_cap) {
        size_t nc = rd->in_cap ? rd->in_cap * 2 : 64;
        rd_in *nq = realloc(rd->in_q, nc * sizeof *nq);
        if (!nq) {
            pthread_mutex_unlock(&rd->in_mu);
            return;
        }
        rd->in_q = nq;
        rd->in_cap = nc;
    }
    if (in->t == RD_IN_POINTER)
        rd->in_last_ptr_buttons = in->buttons;
    rd->in_q[rd->in_len++] = *in;
    pthread_mutex_unlock(&rd->in_mu);
    hb__rd_wake(rd);
}

void hb__rd_wake(hb_rd *rd)
{
    uint8_t b = 'c';
    ssize_t r = write(rd->cmd_w, &b, 1);
    (void)r;
}

int hb__rd_in_wait_fd(hb_rd *rd)
{
    return rd->cmd_r;
}

void hb__rd_in_drain_fd(hb_rd *rd)
{
    uint8_t buf[256];
    while (read(rd->cmd_r, buf, sizeof buf) > 0)
        ;
}

size_t hb__rd_in_pending(hb_rd *rd)
{
    pthread_mutex_lock(&rd->in_mu);
    size_t n = rd->in_len;
    pthread_mutex_unlock(&rd->in_mu);
    return n;
}

size_t hb__rd_in_drain(hb_rd *rd, rd_in *out, size_t cap)
{
    pthread_mutex_lock(&rd->in_mu);
    size_t n = rd->in_len < cap ? rd->in_len : cap;
    memcpy(out, rd->in_q, n * sizeof *out);
    if (n < rd->in_len)
        memmove(rd->in_q, rd->in_q + n, (rd->in_len - n) * sizeof *rd->in_q);
    rd->in_len -= n;
    pthread_mutex_unlock(&rd->in_mu);
    return n;
}

void hb_rd_pointer(hb_rd *rd, int x, int y, unsigned buttons)
{
    if (rd->o.view_only)
        return;
    rd_in in = { .t = RD_IN_POINTER, .x = x, .y = y, .buttons = buttons };
    in_enqueue(rd, &in);
}

void hb_rd_key(hb_rd *rd, uint32_t keysym, uint32_t x11_keycode, bool down)
{
    if (rd->o.view_only)
        return;
    rd_in in = { .t = RD_IN_KEY, .keysym = keysym, .x11kc = x11_keycode, .down = down };
    in_enqueue(rd, &in);
}

void hb_rd_clipboard(hb_rd *rd, const char *utf8)
{
    if (rd->o.view_only || !utf8)
        return;
    rd_in in = { .t = RD_IN_CLIPBOARD, .text = strdup(utf8) };
    in_enqueue(rd, &in);
}

void hb_rd_request_size(hb_rd *rd, int w, int h)
{
    if (rd->o.view_only || w <= 0 || h <= 0)
        return;
    rd_in in = { .t = RD_IN_SIZE, .w = w, .h = h };
    in_enqueue(rd, &in);
}

void hb_rd_set_quality(hb_rd *rd, hb_rd_quality q)
{
    rd->o.quality = q;
    rd_in in = { .t = RD_IN_QUALITY, .x = q };
    in_enqueue(rd, &in);
}

/* ---------- CERT / PROMPT 答复 ---------- */

void hb_rd_cert_reply(hb_rd *rd, bool accept)
{
    pthread_mutex_lock(&rd->rp_mu);
    rd->cert_accept = accept ? 1 : 0;
    pthread_cond_signal(&rd->rp_cv);
    pthread_mutex_unlock(&rd->rp_mu);
}

void hb_rd_prompt_reply(hb_rd *rd, const char *user, const char *password, const char *domain)
{
    pthread_mutex_lock(&rd->rp_mu);
    free(rd->rp_user);
    free(rd->rp_domain);
    if (rd->rp_password) {
        sodium_free(rd->rp_password);
        rd->rp_password = NULL;
    }
    rd->rp_user = xstrdup(user);
    rd->rp_domain = xstrdup(domain);
    if (password) {
        rd->rp_password = hb_secret_strdup(password);
        rd->rp_cancel = false;
    } else
        rd->rp_cancel = true;
    rd->rp_done = true;
    pthread_cond_signal(&rd->rp_cv);
    pthread_mutex_unlock(&rd->rp_mu);
}

void hb__rd_arm_cert(hb_rd *rd)
{
    pthread_mutex_lock(&rd->rp_mu);
    rd->cert_accept = -1;
    pthread_mutex_unlock(&rd->rp_mu);
}

bool hb__rd_wait_cert(hb_rd *rd)
{
    pthread_mutex_lock(&rd->rp_mu);
    while (rd->cert_accept < 0 && !rd->closing)
        pthread_cond_wait(&rd->rp_cv, &rd->rp_mu);
    int a = rd->cert_accept;
    pthread_mutex_unlock(&rd->rp_mu);
    return a == 1;
}

void hb__rd_arm_prompt(hb_rd *rd)
{
    pthread_mutex_lock(&rd->rp_mu);
    rd->rp_done = false;
    rd->rp_cancel = false;
    free(rd->rp_user);
    rd->rp_user = NULL;
    free(rd->rp_domain);
    rd->rp_domain = NULL;
    if (rd->rp_password) {
        sodium_free(rd->rp_password);
        rd->rp_password = NULL;
    }
    pthread_mutex_unlock(&rd->rp_mu);
}

bool hb__rd_wait_prompt(hb_rd *rd, char **user, char **password, char **domain)
{
    pthread_mutex_lock(&rd->rp_mu);
    while (!rd->rp_done && !rd->closing)
        pthread_cond_wait(&rd->rp_cv, &rd->rp_mu);
    if (rd->rp_cancel || rd->closing) {
        pthread_mutex_unlock(&rd->rp_mu);
        return false;
    }
    *user = rd->rp_user;
    rd->rp_user = NULL;
    *domain = rd->rp_domain;
    rd->rp_domain = NULL;
    *password = rd->rp_password;
    rd->rp_password = NULL;
    rd->rp_done = false;
    pthread_mutex_unlock(&rd->rp_mu);
    return true;
}

bool hb__rd_closing(hb_rd *rd)
{
    return rd->closing;
}

/* ---------- 生命周期 ---------- */

void hb_rd_close(hb_rd *rd)
{
    if (!rd)
        return;
    rd->closing = true;
    /* 唤醒可能在 CERT/PROMPT 等待中的 worker */
    pthread_mutex_lock(&rd->rp_mu);
    pthread_cond_broadcast(&rd->rp_cv);
    pthread_mutex_unlock(&rd->rp_mu);
    hb__rd_wake(rd);
    /* 后端打断阻塞的系统调用（关 socket / abort） */
    hb__rd_be_close(rd);
}

void hb__rd_be_close(hb_rd *rd)
{
    if (rd->o.proto == HB_RD_VNC)
        hb__rd_vnc_abort(rd);
    else
        hb__rd_rdp_abort(rd);
}

static void *rd_thread(void *arg)
{
    hb_rd *rd = arg;
    if (rd->o.proto == HB_RD_VNC)
        hb__rd_vnc_run(rd);
    else
        hb__rd_rdp_run(rd);
    hb__rd_wipe_password(rd);
    hb__rd_exit(rd);
    return NULL;
}

hb_rd *hb_rd_new(const hb_rd_opts *o)
{
    if (!o || !o->host || o->port <= 0 || o->port > 65535)
        return NULL;
    if (o->proto != HB_RD_VNC && o->proto != HB_RD_RDP)
        return NULL;

    hb_rd *rd = calloc(1, sizeof *rd);
    if (!rd)
        return NULL;
    hb_core_init();
    if (!hb__rd_opts_copy(rd, o))
        goto fail;
    if (o->password)
        rd->o.password = rd->password; /* 公开副本指向 sodium 内存 */

    pthread_mutex_init(&rd->ev_mu, NULL);
    pthread_mutex_init(&rd->fb_mu, NULL);
    pthread_mutex_init(&rd->in_mu, NULL);
    pthread_mutex_init(&rd->rp_mu, NULL);
    pthread_cond_init(&rd->rp_cv, NULL);
    rd->ev_r = rd->ev_w = rd->cmd_r = rd->cmd_w = -1;
    int p[2];
    if (mkpipe_nonblock(p) != 0)
        goto fail;
    rd->ev_r = p[0];
    rd->ev_w = p[1];
    if (mkpipe_nonblock(p) != 0)
        goto fail;
    rd->cmd_r = p[0];
    rd->cmd_w = p[1];

    if (pthread_create(&rd->th, NULL, rd_thread, rd) == 0)
        rd->th_started = true;
    else {
        hb__rd_error(rd, HB_RD_ERR_PROTOCOL, "无法启动连接线程", strerror(errno));
        rd->exited = true;
    }
    return rd;

fail:
    hb_rd_free(rd);
    return NULL;
}

void hb_rd_free(hb_rd *rd)
{
    if (!rd)
        return;
    hb_rd_close(rd);
    if (rd->th_started) {
        /* 有界等待：多数情况下立即汇合；后端自身负责打断阻塞 I/O */
        pthread_join(rd->th, NULL);
        rd->th_started = false;
    }
    /* 清空事件队列 */
    pthread_mutex_lock(&rd->ev_mu);
    rd_ev_node *n = rd->ev_head;
    while (n) {
        rd_ev_node *nx = n->next;
        hb_rd_event_clear(&n->ev);
        free(n);
        n = nx;
    }
    rd->ev_head = rd->ev_tail = rd->ev_frame = NULL;
    pthread_mutex_unlock(&rd->ev_mu);

    /* 剩余输入队列 */
    pthread_mutex_lock(&rd->in_mu);
    for (size_t i = 0; i < rd->in_len; i++)
        free(rd->in_q[i].text);
    free(rd->in_q);
    pthread_mutex_unlock(&rd->in_mu);

    pthread_mutex_lock(&rd->rp_mu);
    free(rd->rp_user);
    free(rd->rp_domain);
    if (rd->rp_password)
        sodium_free(rd->rp_password);
    pthread_mutex_unlock(&rd->rp_mu);

    if (rd->ev_r >= 0) close(rd->ev_r);
    if (rd->ev_w >= 0) close(rd->ev_w);
    if (rd->cmd_r >= 0) close(rd->cmd_r);
    if (rd->cmd_w >= 0) close(rd->cmd_w);
    free(rd->fb);
    hb__rd_opts_free(rd);
    if (rd->be) {
        if (rd->o.proto == HB_RD_VNC)
            hb__rd_vnc_destroy(rd);
        else
            hb__rd_rdp_destroy(rd);
    }
    pthread_mutex_destroy(&rd->ev_mu);
    pthread_mutex_destroy(&rd->fb_mu);
    pthread_mutex_destroy(&rd->in_mu);
    pthread_mutex_destroy(&rd->rp_mu);
    pthread_cond_destroy(&rd->rp_cv);
    free(rd);
}
