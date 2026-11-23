/* test_rd.c — FR-07 rd 模块测试。
 * VNC 部分：进程内 libvncserver fixture（127.0.0.1:随机端口，VncAuth 密码 pw123）。
 * RDP 部分：环境变量 HB_TEST_RDP_PORT/HB_TEST_RDP_USER/HB_TEST_RDP_PASS 存在才跑
 * （配合 scripts/dev-desktop.sh 的 freerdp-shadow），否则整段跳过。 */

#include "harbor/rd.h"
#include "rd_priv.h" /* in_len/in_q 观察合并与 cap */
#include "harbor/session.h"
#include "harbor/pf.h"
#include "harbor/ssh.h"
#include "sshd_fixture.h"
#include "test.h"

#include <rfb/rfb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

static const char *VNC_PASS = "pw123";

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ======================= 事件收集 ======================= */

typedef struct {
    int n_conn, n_resize, n_frame, n_clip, n_cert, n_prompt, n_error,
        n_exit, n_stage, n_cursor;
    int conn_w, conn_h;
    int err_code, prompt_code, cert_code;
    char conn_name[128], conn_sec[128];
    char clip[2048], fp[256], cert_sum[512], err[512];
    int fx, fy, fw, fh; /* FRAME 损坏并集 */
    int have_frame;
} rdw;

static void rdw_init(rdw *w) { memset(w, 0, sizeof *w); }

static void rdw_take(rdw *w, hb_rd_event *ev)
{
    switch (ev->type) {
    case HB_RD_EV_CONNECTED:
        w->n_conn++;
        w->conn_w = ev->w;
        w->conn_h = ev->h;
        if (ev->text)
            snprintf(w->conn_name, sizeof w->conn_name, "%s", ev->text);
        if (ev->text2)
            snprintf(w->conn_sec, sizeof w->conn_sec, "%s", ev->text2);
        break;
    case HB_RD_EV_RESIZE:
        w->n_resize++;
        w->conn_w = ev->w;
        w->conn_h = ev->h;
        break;
    case HB_RD_EV_FRAME:
        w->n_frame++;
        if (!w->have_frame) {
            w->fx = ev->x;
            w->fy = ev->y;
            w->fw = ev->w;
            w->fh = ev->h;
            w->have_frame = 1;
        } else {
            int x2 = w->fx + w->fw, y2 = w->fy + w->fh;
            int nx2 = ev->x + ev->w, ny2 = ev->y + ev->h;
            w->fx = w->fx < ev->x ? w->fx : ev->x;
            w->fy = w->fy < ev->y ? w->fy : ev->y;
            w->fw = (x2 > nx2 ? x2 : nx2) - w->fx;
            w->fh = (y2 > ny2 ? y2 : ny2) - w->fy;
        }
        break;
    case HB_RD_EV_CLIPBOARD:
        w->n_clip++;
        if (ev->text)
            snprintf(w->clip, sizeof w->clip, "%s", ev->text);
        break;
    case HB_RD_EV_CERT:
        w->n_cert++;
        w->cert_code = ev->code;
        if (ev->text)
            snprintf(w->fp, sizeof w->fp, "%s", ev->text);
        if (ev->text2)
            snprintf(w->cert_sum, sizeof w->cert_sum, "%s", ev->text2);
        break;
    case HB_RD_EV_PROMPT:
        w->n_prompt++;
        w->prompt_code = ev->code;
        break;
    case HB_RD_EV_ERROR:
        w->n_error++;
        w->err_code = ev->code;
        if (ev->text)
            snprintf(w->err, sizeof w->err, "%s", ev->text);
        fprintf(stderr, "    [rd-error] code=%d text=%s detail=%s\n",
                ev->code, ev->text ? ev->text : "", ev->text2 ? ev->text2 : "");
        break;
    case HB_RD_EV_EXIT:
        w->n_exit++;
        break;
    case HB_RD_EV_CURSOR:
        w->n_cursor++;
        break;
    default:
        w->n_stage++;
        break;
    }
    hb_rd_event_clear(ev);
}

static void rd_pump(hb_rd *rd, rdw *w)
{
    hb_rd_event ev;
    while (hb_rd_next_event(rd, &ev))
        rdw_take(w, &ev);
}

/* 等谓词成立或超时 */
static int rd_wait(hb_rd *rd, rdw *w, int (*pred)(rdw *), int timeout_ms)
{
    int64_t dl = now_ms() + timeout_ms;
    while (now_ms() < dl) {
        struct pollfd pfd = { .fd = hb_rd_event_fd(rd), .events = POLLIN };
        if (pred(w))
            return 1;
        if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN))
            rd_pump(rd, w);
    }
    rd_pump(rd, w);
    return pred(w);
}

static int p_conn(rdw *w) { return w->n_conn > 0; }
static int p_exit(rdw *w) { return w->n_exit > 0; }
static int p_err(rdw *w) { return w->n_error > 0; }
static int p_frame(rdw *w) { return w->n_frame > 0; }
static int p_clip(rdw *w) { return w->n_clip > 0; }
static int p_resize(rdw *w) { return w->n_resize > 0; }
static int p_prompt(rdw *w) { return w->n_prompt > 0; }

/* ======================= VNC 服务端 fixture ======================= */

typedef struct {
    rfbScreenInfoPtr scr;
    pthread_t th;
    int port, w, h;
    int run;
    /* 输入观察 */
    pthread_mutex_t mu;
    int ptr_x, ptr_y, ptr_mask, n_ptr;
    uint32_t key_sym;
    int key_down, n_key;
    char cut[1024];
    int cutlen;
} vnc_fx;

static void fx_ptr(int mask, int x, int y, rfbClientPtr cl)
{
    vnc_fx *fx = (vnc_fx *)cl->screen->screenData;
    pthread_mutex_lock(&fx->mu);
    fx->ptr_x = x;
    fx->ptr_y = y;
    fx->ptr_mask = mask;
    fx->n_ptr++;
    pthread_mutex_unlock(&fx->mu);
}

static void fx_kbd(rfbBool down, rfbKeySym sym, rfbClientPtr cl)
{
    vnc_fx *fx = (vnc_fx *)cl->screen->screenData;
    pthread_mutex_lock(&fx->mu);
    fx->key_sym = (uint32_t)sym;
    fx->key_down = down;
    fx->n_key++;
    pthread_mutex_unlock(&fx->mu);
}

static void fx_cut(char *str, int len, rfbClientPtr cl)
{
    vnc_fx *fx = (vnc_fx *)cl->screen->screenData;
    pthread_mutex_lock(&fx->mu);
    int n = len < (int)sizeof fx->cut - 1 ? len : (int)sizeof fx->cut - 1;
    memcpy(fx->cut, str, n);
    fx->cutlen = n;
    fx->cut[n] = 0;
    pthread_mutex_unlock(&fx->mu);
}

/* 测试图样：像素(x,y) = R=x%256 G=y%256 B=(x+y)%256 → BGRX 字节序下
 * 内存字节为 [B][G][R][X] */
static uint32_t pattern(int x, int y)
{
    return ((uint32_t)0xFF << 24) | ((uint32_t)(x & 0xFF) << 16) |
           ((uint32_t)(y & 0xFF) << 8) | (uint32_t)((x + y) & 0xFF);
}

static void fx_log_quiet(const char *fmt, ...) { (void)fmt; }

static void fx_fill(vnc_fx *fx)
{
    uint32_t *px = (uint32_t *)fx->scr->frameBuffer;
    for (int y = 0; y < fx->h; y++)
        for (int x = 0; x < fx->w; x++)
            px[y * fx->w + x] = pattern(x, y);
}

static void *fx_loop(void *arg)
{
    vnc_fx *fx = arg;
    while (fx->run)
        rfbProcessEvents(fx->scr, 5000);
    return NULL;
}

static int fx_start(vnc_fx *fx, int w, int h)
{
    memset(fx, 0, sizeof *fx);
    fx->w = w;
    fx->h = h;
    pthread_mutex_init(&fx->mu, NULL);
    int argc = 0;
    char *argv[] = { NULL };
    fx->scr = rfbGetScreen(&argc, argv, w, h, 8, 3, 4);
    if (!fx->scr)
        return -1;
    /* pattern() 写的是 LE u32 0xFFRRGGBB（字节 B,G,R,A）。服务端默认
     * serverFormat 的 rshift=0 会把它读成 R=byte0——与客户端请求格式
     * 一致时碰巧原样透传，一旦客户端请求 BGRX（rshift=16）就会按错误
     * 语义做通道转换。显式声明服务端真实布局。 */
    fx->scr->serverFormat.redShift = 16;
    fx->scr->serverFormat.greenShift = 8;
    fx->scr->serverFormat.blueShift = 0;
    fx->scr->frameBuffer = malloc((size_t)w * h * 4);
    fx->scr->desktopName = "hb-rd-test";
    static char *pwlist[] = { "pw123", NULL };
    fx->scr->authPasswdData = pwlist;
    fx->scr->passwordCheck = rfbCheckPasswordByList;
    fx->scr->alwaysShared = TRUE;
    fx->scr->ptrAddEvent = fx_ptr;
    fx->scr->kbdAddEvent = fx_kbd;
    fx->scr->setXCutText = fx_cut;
    fx->scr->screenData = fx;
    rfbLog = fx_log_quiet;
    rfbErr = fx_log_quiet;

    /* 随机端口：先借内核挑一个，再放掉给 libvncserver 绑 */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET,
                                  .sin_port = 0,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        socklen_t sl = sizeof sa;
        if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 ||
            getsockname(ls, (struct sockaddr *)&sa, &sl) != 0) {
            if (ls >= 0)
                close(ls);
            return -1;
        }
        fx->port = ntohs(sa.sin_port);
        close(ls);
    }
    fx->scr->port = fx->port;
    fx->scr->ipv6port = -1; /* 禁用 IPv6 监听 */
    fx->scr->listenInterface = htonl(INADDR_LOOPBACK);

    fx_fill(fx);
    rfbInitServer(fx->scr);
    if (fx->scr->listenSock == RFB_INVALID_SOCKET)
        return -1;
    fx->run = 1;
    pthread_create(&fx->th, NULL, fx_loop, fx);
    return 0;
}

static void fx_stop(vnc_fx *fx)
{
    fx->run = 0;
    pthread_join(fx->th, NULL);
    free(fx->scr->frameBuffer);
    fx->scr->frameBuffer = NULL;
    rfbShutdownServer(fx->scr, TRUE);
    rfbScreenCleanup(fx->scr);
    pthread_mutex_destroy(&fx->mu);
}

static uint32_t rd_px(hb_rd *rd, int x, int y)
{
    const uint8_t *fb;
    int w, h, stride;
    if (!hb_rd_fb_lock(rd, &fb, &w, &h, &stride))
        return 0;
    uint32_t v = 0;
    memcpy(&v, fb + (size_t)y * stride + (size_t)x * 4, 4);
    hb_rd_fb_unlock(rd);
    return v;
}

static hb_rd *rd_vnc_connect(const char *pass, int port, hb_rd_quality q)
{
    hb_rd_opts o = { 0 };
    o.proto = HB_RD_VNC;
    o.host = "127.0.0.1";
    o.port = port;
    o.password = pass;
    o.quality = q;
    o.connect_timeout_s = 5;
    return hb_rd_new(&o);
}

/* ======================= 用例 ======================= */

/* 1+2：连接、图样、BGRX、增量更新 */
static void t_vnc_connect_frame(void)
{
    printf("  t_vnc_connect_frame\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(VNC_PASS, fx.port, HB_RD_Q_HIGH);
    rdw w;
    rdw_init(&w);
    CHECK(rd != NULL);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    CHECK_EQ_INT(w.conn_w, 320);
    CHECK_EQ_INT(w.conn_h, 240);
    CHECK_EQ_STR(w.conn_name, "hb-rd-test");
    CHECK(rd_wait(rd, &w, p_frame, 8000));
    /* BGRX：pattern 内存布局 R|G<<8|B<<16|A<<24？验证字节顺序：
     * pattern() 里我们把 R 放在 bits16-23，G bits8-15，B bits0-7（即 LE u32
     * 0xAARRGGBB = 字节 R,G,B,A）——服务端 32bpp 真彩默认就是这个内存序，
     * 客户端 BGRX 应得到字节 [B,G,R,X]。 */
    {
        const uint8_t *fb;
        int fw, fh, st;
        CHECK(hb_rd_fb_lock(rd, &fb, &fw, &fh, &st));
        if (fb) {
            /* 像素 (5,7)：R=5 G=7 B=12 → 字节 [12][7][5][X] */
            const uint8_t *p = fb + 7 * st + 5 * 4;
            CHECK_EQ_INT(p[0], 12);
            CHECK_EQ_INT(p[1], 7);
            CHECK_EQ_INT(p[2], 5);
            /* 全幅比对图样 */
            int bad = 0;
            for (int y = 0; y < fh && !bad; y++)
                for (int x = 0; x < fw && !bad; x++) {
                    const uint8_t *q = fb + y * st + x * 4;
                    if (q[0] != ((x + y) & 0xFF) || q[1] != (y & 0xFF) ||
                        q[2] != (x & 0xFF))
                        bad = 1;
                }
            CHECK(!bad);
        }
        hb_rd_fb_unlock(rd);
    }
    /* 服务端改一块 → FRAME + 新像素 */
    {
        uint32_t *px = (uint32_t *)fx.scr->frameBuffer;
        for (int y = 100; y < 140; y++)
            for (int x = 50; x < 90; x++)
                px[y * fx.w + x] = 0xFF00FF00u;
        rfbMarkRectAsModified(fx.scr, 50, 100, 90, 140);
        int64_t dl = now_ms() + 8000;
        int ok = 0;
        while (now_ms() < dl) {
            struct pollfd pfd = { .fd = hb_rd_event_fd(rd), .events = POLLIN };
            poll(&pfd, 1, 50);
            rd_pump(rd, &w);
            if (w.have_frame && w.fx <= 50 && w.fy <= 100 &&
                w.fx + w.fw >= 90 && w.fy + w.fh >= 140 &&
                (rd_px(rd, 60, 110) & 0xFFFFFFu) == 0x00FF00u) {
                ok = 1;
                break;
            }
        }
        CHECK(ok);
        CHECK(w.fx <= 50 && w.fy <= 100 && w.fx + w.fw >= 90 &&
              w.fy + w.fh >= 140);
    }
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 事件 fd 排干语义：事件取空后 fd 不得保持可读（通知管道须被抽干，
 * 否则 UI 的 fd 源永远触发、空转）。 */
static void t_evfd_drain(void)
{
    printf("  t_evfd_drain\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(VNC_PASS, fx.port, HB_RD_Q_HIGH);
    rdw w;
    rdw_init(&w);
    CHECK(rd != NULL);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    /* 队列取空后 fd 必须归于不可读；允许晚到事件，收敛轮询 2s。 */
    {
        int64_t dl = now_ms() + 2000;
        int quiet = 0;
        while (now_ms() < dl) {
            struct pollfd pfd = { .fd = hb_rd_event_fd(rd),
                                  .events = POLLIN, .revents = 0 };
            rd_pump(rd, &w);
            if (poll(&pfd, 1, 0) == 0) {
                quiet = 1;
                break;
            }
            usleep(5000);
        }
        CHECK(quiet);
    }
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 3：输入回环 */
static void t_vnc_input(void)
{
    printf("  t_vnc_input\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(VNC_PASS, fx.port, HB_RD_Q_HIGH);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    rd_wait(rd, &w, p_frame, 8000);

    hb_rd_pointer(rd, 111, 222, 0x01); /* left down */
    hb_rd_pointer(rd, 120, 230, 0x00); /* release */
    hb_rd_key(rd, 0x61, 38, true);     /* 'a' keysym 0x61 */
    hb_rd_key(rd, 0x61, 38, false);

    int64_t dl = now_ms() + 5000;
    int okp = 0, okk = 0;
    while (now_ms() < dl && !(okp && okk)) {
        pthread_mutex_lock(&fx.mu);
        if (fx.n_ptr > 0 && fx.ptr_x == 120 && fx.ptr_y == 230 &&
            fx.ptr_mask == 0)
            okp = 1;
        if (fx.n_key >= 2 && fx.key_sym == 0x61 && fx.key_down == 0)
            okk = 1;
        pthread_mutex_unlock(&fx.mu);
        usleep(20000);
        rd_pump(rd, &w);
    }
    pthread_mutex_lock(&fx.mu);
    CHECK(fx.n_ptr >= 2 && fx.ptr_mask == 0);
    CHECK(fx.n_key >= 2 && fx.key_sym == 0x61);
    pthread_mutex_unlock(&fx.mu);
    (void)okp;
    (void)okk;
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 3b：空闲服务端下指针输入延迟 —— worker 必须被 cmd 管道唤醒，
 * 不能等 RFB 消息（旧 WaitForMessage 路径中位数 ~25ms）。20 样本中位 <20ms。 */
static int cmp_i64(const void *a, const void *b)
{
    int64_t d = *(const int64_t *)a - *(const int64_t *)b;
    return d < 0 ? -1 : d > 0;
}

static void t_vnc_input_latency(void)
{
    printf("  t_vnc_input_latency\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(VNC_PASS, fx.port, HB_RD_Q_BALANCED);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    rd_wait(rd, &w, p_frame, 8000);

    /* 预热一次，排除 connect 期残留 */
    hb_rd_pointer(rd, 5, 5, 0);
    usleep(30000);

    int64_t lat[20];
    int n = 0;
    for (int i = 0; i < 20; i++) {
        int x = 30 + i, y = 40 + i;
        pthread_mutex_lock(&fx.mu);
        int base = fx.n_ptr;
        pthread_mutex_unlock(&fx.mu);
        int64_t t0 = now_us();
        hb_rd_pointer(rd, x, y, 0);
        int64_t el = -1;
        while (now_us() - t0 < 3000000) {
            pthread_mutex_lock(&fx.mu);
            int hit = fx.n_ptr > base && fx.ptr_x == x && fx.ptr_y == y;
            pthread_mutex_unlock(&fx.mu);
            if (hit) {
                el = now_us() - t0;
                break;
            }
            usleep(200);
        }
        CHECK(el >= 0);
        if (el >= 0)
            lat[n++] = el;
    }
    if (n > 0) {
        qsort(lat, n, sizeof *lat, cmp_i64);
        printf("    ptr latency: median %.2f ms  min %.2f  max %.2f  (n=%d)\n",
               lat[n / 2] / 1000.0, lat[0] / 1000.0, lat[n - 1] / 1000.0, n);
        CHECK(lat[n / 2] < 20000); /* 中位 <20ms */
    }
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 3c：输入队列合并 + cap。worker 停在 PROMPT（不 drain），可确定性观察队列。 */
static void t_vnc_input_queue(void)
{
    printf("  t_vnc_input_queue\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(NULL, fx.port, HB_RD_Q_BALANCED); /* 无密码 → PROMPT */
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_prompt, 8000));

    /* 合并：10k 同按钮 motion 全部合并进队尾——队列恒为 ~1 条；
     * 按钮变化的 click 绝不合并，成为第 2 条。 */
    int lx = 10 + 9999 % 300, ly = 10 + (9999 * 7) % 220;
    for (int i = 0; i < 10000; i++)
        hb_rd_pointer(rd, 10 + i % 300, 10 + (i * 7) % 220, 0);
    hb_rd_pointer(rd, lx, ly, 1); /* 最后移动位置上的 click */
    pthread_mutex_lock(&rd->in_mu);
    rd_in tail = rd->in_q[rd->in_len - 1];
    size_t len1 = rd->in_len;
    pthread_mutex_unlock(&rd->in_mu);
    CHECK(len1 <= 2); /* 10k motion 合并成 1 条 + click = 至多 2 条，远低于 cap */
    CHECK(tail.t == RD_IN_POINTER && tail.buttons == 1 && tail.x == lx &&
          tail.y == ly);

    /* cap：交错 KEY 让 POINTER 不可合并；纯移动（buttons 与上条 POINTER 相同）
     * 在队满后被丢弃——POINTER 条目数封顶 4096，KEY 仍全部放行 */
    int max_np = 0;
    size_t max_len = 0;
    for (int i = 0; i < 2500; i++) {
        hb_rd_key(rd, 'a', 38, true);
        hb_rd_pointer(rd, i % 320, (i * 3) % 240, 0);
        if (i % 50)
            continue;
        pthread_mutex_lock(&rd->in_mu);
        int np = 0;
        for (size_t j = 0; j < rd->in_len; j++)
            np += rd->in_q[j].t == RD_IN_POINTER;
        if (np > max_np)
            max_np = np;
        if (rd->in_len > max_len)
            max_len = rd->in_len;
        pthread_mutex_unlock(&rd->in_mu);
    }
    printf("    queue cap: ptr entries <= %d (cap 4096), len max %zu\n",
           max_np, max_len);
    CHECK(max_np <= 4096);
    /* 队满后 click（按钮转移）仍可入队 */
    hb_rd_pointer(rd, lx, ly, 1);
    pthread_mutex_lock(&rd->in_mu);
    rd_in tail2 = rd->in_q[rd->in_len - 1];
    pthread_mutex_unlock(&rd->in_mu);
    CHECK(tail2.t == RD_IN_POINTER && tail2.buttons == 1);

    /* 放行 → server 最终看到 click @(lx,ly)，mask=1，顺序不被交换 */
    hb_rd_prompt_reply(rd, NULL, VNC_PASS, NULL);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    int ok = 0;
    for (int64_t dl = now_ms() + 8000; now_ms() < dl && !ok;) {
        pthread_mutex_lock(&fx.mu);
        ok = fx.ptr_mask == 1 && fx.ptr_x == lx && fx.ptr_y == ly;
        pthread_mutex_unlock(&fx.mu);
        if (!ok)
            usleep(5000);
        rd_pump(rd, &w);
    }
    if (!ok) {
        pthread_mutex_lock(&fx.mu);
        fprintf(stderr, "    [dbg] n_ptr=%d last=(%d,%d,mask=%d) want=(%d,%d,1)"
                " err=%d/%d exit=%d\n", fx.n_ptr, fx.ptr_x, fx.ptr_y,
                fx.ptr_mask, lx, ly, w.n_error, w.err_code, w.n_exit);
        pthread_mutex_unlock(&fx.mu);
    }
    CHECK(ok);
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 4：剪贴板双向（Latin-1 可表示的 "café"） */
static void t_vnc_clipboard(void)
{
    printf("  t_vnc_clipboard\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(VNC_PASS, fx.port, HB_RD_Q_HIGH);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    rd_wait(rd, &w, p_frame, 8000);

    /* client→server：UTF-8 "café" → Latin-1 "caf\xE9" */
    hb_rd_clipboard(rd, "caf\xC3\xA9");
    int64_t dl = now_ms() + 5000;
    int got = 0;
    while (now_ms() < dl) {
        pthread_mutex_lock(&fx.mu);
        got = fx.cutlen > 0;
        pthread_mutex_unlock(&fx.mu);
        if (got)
            break;
        usleep(20000);
    }
    pthread_mutex_lock(&fx.mu);
    CHECK(got);
    CHECK(fx.cutlen == 4 && memcmp(fx.cut, "caf\xE9", 4) == 0);
    pthread_mutex_unlock(&fx.mu);

    /* server→client：Latin-1 "thé" → UTF-8 */
    rfbSendServerCutText(fx.scr, (char *)"th\xE9", 3);
    CHECK(rd_wait(rd, &w, p_clip, 5000));
    CHECK_EQ_STR(w.clip, "th\xC3\xA9");
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 5：错误密码 → AUTH；无密码 → PROMPT → 答复连接；NULL 答复 → CANCELLED */
static void t_vnc_auth(void)
{
    printf("  t_vnc_auth\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;

    /* wrong password */
    hb_rd *rd = rd_vnc_connect("wrong", fx.port, HB_RD_Q_HIGH);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_err, 8000));
    CHECK_EQ_INT(w.err_code, HB_RD_ERR_AUTH);
    rd_wait(rd, &w, p_exit, 5000);
    hb_rd_free(rd);

    /* missing password → PROMPT → 答对 */
    rd = rd_vnc_connect(NULL, fx.port, HB_RD_Q_HIGH);
    rdw_init(&w);
    int64_t dl = now_ms() + 8000;
    while (now_ms() < dl && !w.n_prompt) {
        struct pollfd pfd = { .fd = hb_rd_event_fd(rd), .events = POLLIN };
        poll(&pfd, 1, 50);
        rd_pump(rd, &w);
    }
    CHECK(w.n_prompt == 1);
    CHECK_EQ_INT(w.prompt_code, 0);
    hb_rd_prompt_reply(rd, NULL, VNC_PASS, NULL);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    hb_rd_free(rd);

    /* missing password → PROMPT → NULL → CANCELLED */
    rd = rd_vnc_connect(NULL, fx.port, HB_RD_Q_HIGH);
    rdw_init(&w);
    dl = now_ms() + 8000;
    while (now_ms() < dl && !w.n_prompt) {
        struct pollfd pfd = { .fd = hb_rd_event_fd(rd), .events = POLLIN };
        poll(&pfd, 1, 50);
        rd_pump(rd, &w);
    }
    CHECK(w.n_prompt == 1);
    hb_rd_prompt_reply(rd, NULL, NULL, NULL);
    CHECK(rd_wait(rd, &w, p_err, 8000));
    CHECK_EQ_INT(w.err_code, HB_RD_ERR_CANCELLED);
    hb_rd_free(rd);
    fx_stop(&fx);
}

/* 6：服务端 resize → RESIZE */
static void t_vnc_resize(void)
{
    printf("  t_vnc_resize\n");
    vnc_fx fx;
    CHECK(fx_start(&fx, 320, 240) == 0);
    if (!fx.scr)
        return;
    hb_rd *rd = rd_vnc_connect(VNC_PASS, fx.port, HB_RD_Q_HIGH);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 8000));
    rd_wait(rd, &w, p_frame, 8000);

    char *nfb = malloc((size_t)400 * 300 * 4);
    rfbNewFramebuffer(fx.scr, nfb, 400, 300, 8, 3, 4);
    CHECK(rd_wait(rd, &w, p_resize, 8000));
    CHECK_EQ_INT(w.conn_w, 400);
    CHECK_EQ_INT(w.conn_h, 300);
    hb_rd_free(rd);
    fx_stop(&fx); /* rfbNewFramebuffer 的 nfb 归 screen 所有，cleanup 释放 */
}

/* 9：连接中 close（接受但不说话的端口）→ 2s 内 EXIT */
static void t_vnc_close_during_connect(void)
{
    printf("  t_vnc_close_during_connect\n");
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_port = 0,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0);
    CHECK(listen(ls, 4) == 0);
    socklen_t sl = sizeof sa;
    getsockname(ls, (struct sockaddr *)&sa, &sl);
    int port = ntohs(sa.sin_port);

    hb_rd *rd = rd_vnc_connect(VNC_PASS, port, HB_RD_Q_BALANCED);
    rdw w;
    rdw_init(&w);
    usleep(300000); /* 让 worker 进入握手读 */
    int64_t t0 = now_ms();
    hb_rd_close(rd);
    CHECK(rd_wait(rd, &w, p_exit, 2000));
    CHECK(now_ms() - t0 < 2000);
    hb_rd_free(rd);
    close(ls);
}

/* 7：SSH 隧道 → VNC，释放后端口归还 */
static void t_vnc_tunnel(void)
{
    printf("  t_vnc_tunnel\n");
    hb_sshd fx;
    memset(&fx, 0, sizeof fx);
    if (hb_sshd_start(&fx) != 0)
        exit(77);

    vnc_fx vfx;
    CHECK(fx_start(&vfx, 320, 240) == 0);
    if (!vfx.scr) {
        hb_sshd_stop(&fx);
        return;
    }

    hb_ssh_opts o;
    memset(&o, 0, sizeof o);
    o.target.host = "127.0.0.1";
    o.target.port = fx.port;
    o.target.user = fx.user;
    o.target.auth = HB_SSH_AUTH_KEY;
    o.target.privkey = fx.key;
    const char *kh[] = { fx.khline };
    o.known_lines = kh;
    o.nknown = 1;
    o.connect_timeout_s = 10;
    o.mode = HB_SSH_MODE_FORWARD;

    hb_pf_rule rule = { .kind = HB_PF_LOCAL,
                        .bind_addr = "127.0.0.1",
                        .bind_port = 0,
                        .dest_addr = "127.0.0.1",
                        .dest_port = vfx.port };
    int bound = 0;
    char *err = NULL;
    hb_session *s = hb_session_new_forward(&o, &rule, &bound, &err);
    CHECK(s != NULL);
    CHECK(bound > 0);
    if (!s) {
        fprintf(stderr, "  forward failed: %s\n", err);
        free(err);
        fx_stop(&vfx);
        hb_sshd_stop(&fx);
        return;
    }
    /* 泵 session 事件到 LISTENING/CONNECTED */
    int64_t dl = now_ms() + 15000;
    int live = 0;
    while (now_ms() < dl && !live) {
        hb_sess_event ev;
        struct pollfd pfd = { .fd = hb_session_event_fd(s), .events = POLLIN };
        poll(&pfd, 1, 50);
        while (hb_session_next_event(s, &ev)) {
            if (ev.type == HB_SESS_EV_FORWARD && ev.pf &&
                ev.pf->kind == HB_PF_EV_LISTENING)
                live = 1;
            if (ev.type == HB_SESS_EV_ERROR)
                fprintf(stderr, "  sess err: %s detail=%s\n", ev.text,
                        ev.text2 ? ev.text2 : "");
            hb_sess_event_clear(&ev);
        }
    }
    CHECK(live);

    hb_rd *rd = rd_vnc_connect(VNC_PASS, bound, HB_RD_Q_BALANCED);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 10000));
    CHECK_EQ_INT(w.conn_w, 320);
    hb_rd_free(rd);
    hb_session_free(s);

    /* 5s 内端口应可重新绑定 */
    int free_ok = 0;
    dl = now_ms() + 5000;
    while (now_ms() < dl) {
        int ts = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in b2 = { .sin_family = AF_INET,
                                  .sin_port = htons(bound),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        if (bind(ts, (struct sockaddr *)&b2, sizeof b2) == 0) {
            free_ok = 1;
            close(ts);
            break;
        }
        close(ts);
        usleep(100000);
    }
    CHECK(free_ok);
    fx_stop(&vfx);
    hb_sshd_stop(&fx);
}

/* 8：吞吐 —— 1920x1080 持续全屏重绘，3s 测 FRAME/s（经 SSH 隧道） */
static void t_vnc_throughput(void)
{
    printf("  t_vnc_throughput\n");
    hb_sshd fx;
    memset(&fx, 0, sizeof fx);
    if (hb_sshd_start(&fx) != 0)
        exit(77);

    vnc_fx vfx;
    CHECK(fx_start(&vfx, 1920, 1080) == 0);
    if (!vfx.scr) {
        hb_sshd_stop(&fx);
        return;
    }

    hb_ssh_opts o;
    memset(&o, 0, sizeof o);
    o.target.host = "127.0.0.1";
    o.target.port = fx.port;
    o.target.user = fx.user;
    o.target.auth = HB_SSH_AUTH_KEY;
    o.target.privkey = fx.key;
    const char *kh[] = { fx.khline };
    o.known_lines = kh;
    o.nknown = 1;
    o.connect_timeout_s = 10;
    o.mode = HB_SSH_MODE_FORWARD;

    hb_pf_rule rule = { .kind = HB_PF_LOCAL,
                        .bind_addr = "127.0.0.1",
                        .bind_port = 0,
                        .dest_addr = "127.0.0.1",
                        .dest_port = vfx.port };
    int bound = 0;
    hb_session *s = hb_session_new_forward(&o, &rule, &bound, NULL);
    CHECK(s != NULL);
    if (!s) {
        fx_stop(&vfx);
        hb_sshd_stop(&fx);
        return;
    }
    int64_t dl = now_ms() + 15000;
    int live = 0;
    while (now_ms() < dl && !live) {
        hb_sess_event ev;
        struct pollfd pfd = { .fd = hb_session_event_fd(s), .events = POLLIN };
        poll(&pfd, 1, 50);
        while (hb_session_next_event(s, &ev)) {
            if (ev.type == HB_SESS_EV_FORWARD && ev.pf &&
                ev.pf->kind == HB_PF_EV_LISTENING)
                live = 1;
            hb_sess_event_clear(&ev);
        }
    }

    hb_rd *rd = rd_vnc_connect(VNC_PASS, bound, HB_RD_Q_BALANCED);
    rdw w;
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 10000));

    int64_t t0 = now_ms();
    int repaints = 0;
    while (now_ms() - t0 < 3000) {
        /* 全屏改色 + 标记 */
        uint32_t *px = (uint32_t *)vfx.scr->frameBuffer;
        uint32_t c = 0xFF000000u | (uint32_t)(repaints & 0xFF) << 8;
        for (int i = 0; i < 1920 * 1080; i += 97)
            px[i] = c;
        rfbMarkRectAsModified(vfx.scr, 0, 0, 1920, 1080);
        repaints++;
        rd_pump(rd, &w);
        usleep(16000); /* ~60fps 服务端重绘上限 */
    }
    rd_pump(rd, &w);
    double secs = (now_ms() - t0) / 1000.0;
    printf("    throughput: %d FRAME events in %.1fs = %.1f FRAME/s "
           "(%d server repaints)\n",
           w.n_frame, secs, w.n_frame / secs, repaints);
    CHECK(w.n_frame > 0);
    hb_rd_free(rd);
    hb_session_free(s);
    fx_stop(&vfx);
    hb_sshd_stop(&fx);
}

/* ======================= RDP（env-gated） ======================= */

static int rdp_probe(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int ok = connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
    close(fd);
    return ok;
}

static hb_rd *rd_rdp_connect(int port, const char *user, const char *pass,
                             const char *cert_fp, const char *cfgdir)
{
    hb_rd_opts o = { 0 };
    o.proto = HB_RD_RDP;
    o.host = "127.0.0.1";
    o.port = port;
    o.user = user;
    o.password = pass;
    o.quality = HB_RD_Q_BALANCED;
    o.width = 1280;
    o.height = 720;
    o.cert_fp = cert_fp;
    o.tls_hostname = "harbor-dev";
    o.freerdp_config_dir = cfgdir;
    o.connect_timeout_s = 10;
    return hb_rd_new(&o);
}

static void t_rdp(void)
{
    const char *ps = getenv("HB_TEST_RDP_PORT");
    const char *user = getenv("HB_TEST_RDP_USER");
    const char *pass = getenv("HB_TEST_RDP_PASS");
    if (!ps || !user || !pass) {
        printf("  t_rdp: skipped (HB_TEST_RDP_* unset)\n");
        return;
    }
    int port = atoi(ps);
    if (!rdp_probe(port)) {
        printf("  t_rdp: skipped (no RDP on 127.0.0.1:%d)\n", port);
        return;
    }
    printf("  t_rdp (127.0.0.1:%d user=%s)\n", port, user);
    char cfg[256];
    snprintf(cfg, sizeof cfg, "/tmp/hb-rd-test-freerdp-%d", getpid());
    if (mkdir(cfg, 0700) != 0 && errno != EEXIST)
        CHECK(0);

    /* 首次连接：CERT → accept → CONNECTED */
    hb_rd *rd = rd_rdp_connect(port, user, pass, NULL, cfg);
    rdw w;
    rdw_init(&w);
    int64_t dl = now_ms() + 20000;
    while (now_ms() < dl && !w.n_cert && !w.n_error && !w.n_conn) {
        struct pollfd pfd = { .fd = hb_rd_event_fd(rd), .events = POLLIN };
        poll(&pfd, 1, 100);
        rd_pump(rd, &w);
    }
    CHECK(w.n_cert >= 1);
    CHECK_EQ_INT(w.cert_code, 0);
    char fp[256] = { 0 };
    snprintf(fp, sizeof fp, "%s", w.fp);
    CHECK(fp[0] != 0);
    printf("    cert fp: %s\n", fp);
    printf("    cert sum: %s\n", w.cert_sum);
    hb_rd_cert_reply(rd, true);
    CHECK(rd_wait(rd, &w, p_conn, 20000));
    CHECK(rd_wait(rd, &w, p_frame, 10000));
    printf("    connected %dx%d sec=%s frames=%d\n", w.conn_w, w.conn_h,
           w.conn_sec, w.n_frame);

    /* clipboard out：远端→本地不易驱动（shadow 无 GUI），
     * 至少把本地→远端的路径跑通（发格式列表不崩） */
    hb_rd_clipboard(rd, "hello-rdp");
    usleep(500000);
    rd_pump(rd, &w);

    /* request_size → Display Control（shadow 未必支持） */
    w.n_resize = 0;
    hb_rd_request_size(rd, 1366, 768);
    int resized = rd_wait(rd, &w, p_resize, 8000);
    printf("    display-control resize %s (w=%d h=%d)\n",
           resized ? "OK" : "not supported/no RESIZE", w.conn_w, w.conn_h);
    hb_rd_free(rd);

    /* 重连 + cert_fp → 无 CERT */
    rd = rd_rdp_connect(port, user, pass, fp, cfg);
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_conn, 20000));
    CHECK_EQ_INT(w.n_cert, 0);
    hb_rd_free(rd);

    /* 不同 fp → CERT code 1 */
    rd = rd_rdp_connect(port, user, pass,
                        "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                        cfg);
    rdw_init(&w);
    dl = now_ms() + 20000;
    while (now_ms() < dl && !w.n_cert && !w.n_error) {
        struct pollfd pfd = { .fd = hb_rd_event_fd(rd), .events = POLLIN };
        poll(&pfd, 1, 100);
        rd_pump(rd, &w);
    }
    CHECK(w.n_cert >= 1);
    CHECK_EQ_INT(w.cert_code, 1); /* 已存指纹且不符 → changed（vault 为准） */
    hb_rd_cert_reply(rd, false);
    CHECK(rd_wait(rd, &w, p_err, 10000));
    CHECK_EQ_INT(w.err_code, HB_RD_ERR_CERT_REJECTED);
    hb_rd_free(rd);

    /* 错误密码 → AUTH */
    rd = rd_rdp_connect(port, user, "wrong-pw", fp, cfg);
    rdw_init(&w);
    CHECK(rd_wait(rd, &w, p_err, 20000));
    CHECK_EQ_INT(w.err_code, HB_RD_ERR_AUTH);
    hb_rd_free(rd);
}

/* ======================= main ======================= */

int main(void)
{
    hb_core_init();
    printf("test_rd\n");

    /* 9 用例先做（不依赖 fixture 成功） */
    t_vnc_close_during_connect();

    vnc_fx probe;
    if (fx_start(&probe, 320, 240) != 0) {
        fprintf(stderr, "  libvncserver fixture failed; skipping VNC tests\n");
        TEST_RETURN();
    }
    fx_stop(&probe);

    t_vnc_connect_frame();
    t_evfd_drain();
    t_vnc_input();
    t_vnc_input_latency();
    t_vnc_input_queue();
    t_vnc_clipboard();
    t_vnc_auth();
    t_vnc_resize();
    t_vnc_tunnel();
    t_vnc_throughput();
    t_rdp();

    printf("rd: %d checks, %d failures\n", hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
