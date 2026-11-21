/* rd_vnc.c — VNC 后端（libvncclient）。所有 libvncclient 调用只发生在 rd worker 线程；
 * hb_rd_close 仅通过 be->sock 的 shutdown 打断阻塞中的 I/O。 */

#include "rd_priv.h"

#include <rfb/rfbclient.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sodium.h>

typedef struct {
    rfbClient *cl;
    pthread_mutex_t mu;
    int sock;           /* 我们预连的 fd；abort 用，-1 无 */
    bool auth_attempted; /* 到过密码阶段 → init 失败映射 AUTH */
    bool cancelled;      /* PROMPT 被取消 → ERROR CANCELLED */
    bool connected;
    bool latin1_warned;
} rd_vnc;

static rd_vnc *beptr(hb_rd *rd) { return rd->be; }

static void vnc_log_quiet(const char *fmt, ...)
{
    (void)fmt;
}

/* ---------- Latin-1 ↔ UTF-8（VNC ClientCutText 语义） ---------- */

static char *latin1_to_utf8(const char *s, int len)
{
    /* 每个 Latin-1 字节 → 1 或 2 个 UTF-8 字节 */
    char *out = malloc((size_t)len * 2 + 1);
    if (!out)
        return NULL;
    char *p = out;
    for (int i = 0; i < len; i++) {
        unsigned c = (unsigned char)s[i];
        if (c < 0x80)
            *p++ = (char)c;
        else {
            *p++ = (char)(0xC0 | (c >> 6));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p = 0;
    return out;
}

static char *utf8_to_latin1(const char *u, int *out_len)
{
    size_t n = strlen(u);
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    int j = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)u[i];
        uint32_t cp;
        size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((c & 0x1F) << 6) | (u[i + 1] & 0x3F); adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((c & 0x0F) << 12) | ((u[i + 1] & 0x3F) << 6) | (u[i + 2] & 0x3F); adv = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = 0x110000; adv = 4; /* >Latin-1 一律 ? */
        } else { cp = 0x110000; adv = 1; }
        out[j++] = cp <= 0xFF ? (char)cp : '?';
        i += adv;
    }
    out[j] = 0;
    if (out_len)
        *out_len = j;
    return out;
}

/* ---------- libvncclient 回调 ---------- */

static char *vnc_get_password(rfbClient *client)
{
    hb_rd *rd = rfbClientGetClientData(client, (void *)vnc_get_password);
    rd_vnc *be = beptr(rd);
    be->auth_attempted = true;
    if (rd->password)
        return strdup(rd->password); /* libvncclient 用后 free() */
    hb__rd_arm_prompt(rd);
    hb_rd_event ev = { .type = HB_RD_EV_PROMPT, .code = 0,
                       .text = (char *)"服务器要求 VNC 密码" };
    hb__rd_push(rd, &ev);
    char *user = NULL, *pass = NULL, *dom = NULL;
    if (!hb__rd_wait_prompt(rd, &user, &pass, &dom)) {
        free(user);
        free(dom);
        be->cancelled = true;
        return NULL;
    }
    free(user);
    free(dom);
    if (!pass) {
        be->cancelled = true;
        return NULL;
    }
    char *ret = strdup(pass); /* 给 libvncclient 的普通 malloc 副本 */
    sodium_free(pass);
    return ret;
}

static rfbBool vnc_malloc_fb(rfbClient *client)
{
    hb_rd *rd = rfbClientGetClientData(client, (void *)vnc_get_password);
    rd_vnc *be = beptr(rd);
    uint8_t *nb = realloc(client->frameBuffer,
                          (size_t)client->width * client->height * 4);
    if (!nb)
        return FALSE;
    client->frameBuffer = nb;
    if (be->connected)
        hb__rd_resize(rd, client->width, client->height);
    return TRUE;
}

static void vnc_got_update(rfbClient *client, int x, int y, int w, int h)
{
    hb_rd *rd = rfbClientGetClientData(client, (void *)vnc_get_password);
    hb__rd_fb_update(rd, client->frameBuffer, client->width * 4,
                     client->width, client->height, x, y, w, h);
}

static void vnc_got_cut(rfbClient *client, const char *text, int len)
{
    hb_rd *rd = rfbClientGetClientData(client, (void *)vnc_get_password);
    char *u = latin1_to_utf8(text, len);
    if (u) {
        hb__rd_clipboard(rd, u);
        free(u);
    }
}

static void vnc_got_cursor(rfbClient *client, int xhot, int yhot,
                           int w, int h, int bpp)
{
    hb_rd *rd = rfbClientGetClientData(client, (void *)vnc_get_password);
    if (bpp != 4 || !client->rcSource || w <= 0 || h <= 0)
        return;
    size_t n = (size_t)w * h;
    uint8_t *bgra = calloc(n, 4);
    if (!bgra)
        return;
    for (size_t i = 0; i < n; i++) {
        /* rcMask 每像素 1 bit，MSB first */
        int mb = (client->rcMask[i / 8] >> (7 - (i % 8))) & 1;
        if (mb) {
            memcpy(bgra + i * 4, client->rcSource + i * 4, 4);
            bgra[i * 4 + 3] = 0xFF;
        }
    }
    hb__rd_cursor(rd, w, h, xhot, yhot, bgra);
    free(bgra);
}

/* ---------- 画质预设 ---------- */

static void vnc_apply_quality(hb_rd *rd, rfbClient *cl)
{
    switch (rd->o.quality) {
    case HB_RD_Q_FAST:
        cl->appData.encodingsString = "copyrect tight";
        cl->appData.qualityLevel = 2;
        cl->appData.compressLevel = 1;
        break;
    case HB_RD_Q_HIGH:
        cl->appData.encodingsString = "copyrect zrle hextile raw";
        cl->appData.qualityLevel = 9;
        cl->appData.compressLevel = 9;
        break;
    case HB_RD_Q_BALANCED:
    default:
        cl->appData.encodingsString = "copyrect tight zrle";
        cl->appData.qualityLevel = 6;
        cl->appData.compressLevel = 6;
        break;
    }
}

/* ---------- 自管理 TCP 连接（精确的错误映射 + 可中止） ---------- */

static int tcp_connect(hb_rd *rd, int timeout_s, hb_rd_err *err, char *det, size_t dlen)
{
    char port[16];
    snprintf(port, sizeof port, "%d", rd->o.port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC,
                              .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(rd->o.host, port, &hints, &ai);
    if (rc != 0) {
        snprintf(det, dlen, "getaddrinfo: %s", gai_strerror(rc));
        *err = HB_RD_ERR_REFUSED;
        return -1;
    }
    int fd = -1;
    *err = HB_RD_ERR_REFUSED;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, p->ai_addr, p->ai_addrlen);
        if (r == 0)
            break;
        if (errno != EINPROGRESS) {
            snprintf(det, dlen, "connect: %s", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr;
        do {
            pr = poll(&pfd, 1, timeout_s * 1000);
        } while (pr < 0 && errno == EINTR && !hb__rd_closing(rd));
        if (pr == 0) {
            snprintf(det, dlen, "connect timeout %ds", timeout_s);
            *err = HB_RD_ERR_TIMEOUT;
        } else if (pr < 0) {
            snprintf(det, dlen, "poll: %s", strerror(errno));
        } else {
            int so = 0;
            socklen_t sl = sizeof so;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
            if (so == 0)
                break;
            snprintf(det, dlen, "connect: %s", strerror(so));
            *err = so == ETIMEDOUT ? HB_RD_ERR_TIMEOUT : HB_RD_ERR_REFUSED;
        }
        close(fd);
        fd = -1;
        if (hb__rd_closing(rd)) {
            *err = HB_RD_ERR_CANCELLED;
            break;
        }
    }
    freeaddrinfo(ai);
    if (fd >= 0) {
        /* 连接完成，回到阻塞模式便于库内语义一致 */
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    return fd;
}

void hb__rd_vnc_abort(hb_rd *rd)
{
    rd_vnc *be = beptr(rd);
    if (!be)
        return;
    pthread_mutex_lock(&be->mu);
    if (be->sock >= 0)
        shutdown(be->sock, SHUT_RDWR);
    if (be->cl && be->cl->sock != RFB_INVALID_SOCKET)
        shutdown(be->cl->sock, SHUT_RDWR);
    pthread_mutex_unlock(&be->mu);
}

/* ---------- worker ---------- */

void hb__rd_vnc_run(hb_rd *rd)
{
    rd_vnc *be = calloc(1, sizeof *be);
    rd->be = be;
    pthread_mutex_init(&be->mu, NULL);
    be->sock = -1;
    char det[512] = { 0 };
    hb_rd_err err;

    hb__rd_stage(rd, "正在连接…");
    int fd = tcp_connect(rd, rd->o.connect_timeout_s, &err, det, sizeof det);
    if (fd < 0) {
        const char *cn = err == HB_RD_ERR_TIMEOUT   ? "连接超时"
                         : err == HB_RD_ERR_CANCELLED ? "已取消"
                                                      : "无法连接到服务器";
        hb__rd_error(rd, err, cn, det);
        return;
    }
    pthread_mutex_lock(&be->mu);
    be->sock = fd;
    pthread_mutex_unlock(&be->mu);

    hb__rd_stage(rd, "RFB 握手…");
    /* 静默 libvncclient 日志（可能含协议细节；口令永不经此路径） */
    rfbClientLog = vnc_log_quiet;
    rfbClientErr = vnc_log_quiet;
    rfbClient *cl = rfbGetClient(8, 3, 4);
    cl->programName = "harbor";
    cl->serverHost = strdup(rd->o.host);
    cl->serverPort = rd->o.port;
    cl->sock = fd; /* 预连的 socket：libvncclient 0.9.14 跳过自连 */
    cl->canHandleNewFBSize = TRUE;
    cl->connectTimeout = rd->o.connect_timeout_s;
    vnc_apply_quality(rd, cl);
    cl->appData.useRemoteCursor = TRUE;
    cl->GetPassword = vnc_get_password;
    cl->MallocFrameBuffer = vnc_malloc_fb;
    cl->GotFrameBufferUpdate = vnc_got_update;
    cl->GotXCutText = vnc_got_cut;
    cl->GotCursorShape = vnc_got_cursor;
    rfbClientSetClientData(cl, (void *)vnc_get_password, rd);
    pthread_mutex_lock(&be->mu);
    be->cl = cl;
    pthread_mutex_unlock(&be->mu);

    if (!rfbInitClient(cl, NULL, NULL)) {
        pthread_mutex_lock(&be->mu);
        be->sock = -1;
        be->cl = NULL;
        pthread_mutex_unlock(&be->mu);
        if (hb__rd_closing(rd) || be->cancelled)
            hb__rd_error(rd, HB_RD_ERR_CANCELLED, "已取消", NULL);
        else if (be->auth_attempted)
            hb__rd_error(rd, HB_RD_ERR_AUTH, "密码错误", "vncAuth failed");
        else
            hb__rd_error(rd, HB_RD_ERR_PROTOCOL, "RFB 握手失败",
                         "server does not speak RFB or auth type mismatch");
        return;
    }
    pthread_mutex_lock(&be->mu);
    be->cl = cl;
    if (cl->sock != fd) {
        /* libvncclient 自己连的：跟踪实际 socket 供 abort 用，关掉我们的探测 fd */
        close(fd);
        be->sock = cl->sock;
    }
    pthread_mutex_unlock(&be->mu);

    be->connected = true;
    const char *sec = rd->o.quality == HB_RD_Q_HIGH   ? "VncAuth · ZRLE"
                      : rd->o.quality == HB_RD_Q_FAST ? "VncAuth · Tight"
                                                      : "VncAuth · Tight+ZRLE";
    hb__rd_connected(rd, cl->width, cl->height,
                     cl->desktopName ? cl->desktopName : "", sec);
    /* 已认证：擦掉主存里的口令副本 */
    hb__rd_wipe_password(rd);

    int cmdfd = hb__rd_in_wait_fd(rd);
    while (!hb__rd_closing(rd)) {
        /* poll(cl->sock + cmd_r)：输入经 cmd 管道立刻唤醒，不等 RFB 消息。
         * WaitForMessage 只看 socket，输入会排在最长 50ms 后——鼠标延迟感明显。
         * cl->buffered > 0 说明 libvncclient 缓冲里已有（部分）报文，直接交给
         * HandleRFBServerMessage 消化，不必等 socket 再有数据。
         * TLS（VeNCrypt）会话 gnutls 可能有内部明文凭缓冲我们看不见——但我们
         * 不设 GetCredential，协商不到 TLS 安全类型，tlsSession 恒为 NULL。 */
        struct pollfd pfd[2] = {
            { .fd = cl->sock, .events = POLLIN },
            { .fd = cmdfd, .events = POLLIN },
        };
        /* 队列还有积压 → 非阻塞轮询，继续排空（500ms 等待会漏掉 drain 后剩余） */
        int i = cl->buffered > 0 ? 1
                               : poll(pfd, 2, hb__rd_in_pending(rd) ? 0 : 500);
        if (i < 0) {
            if (errno == EINTR)
                continue;
            if (!hb__rd_closing(rd))
                hb__rd_error(rd, HB_RD_ERR_DISCONNECTED, "连接中断",
                             "poll failed");
            break;
        }
        if (cl->buffered == 0 && (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) &&
            !(pfd[0].revents & POLLIN)) {
            if (!hb__rd_closing(rd))
                hb__rd_error(rd, HB_RD_ERR_DISCONNECTED, "连接中断",
                             "socket closed");
            break;
        }
        /* 输入队列 */
        rd_in in[64];
        size_t n = hb__rd_in_drain(rd, in, 64);
        for (size_t k = 0; k < n; k++) {
            switch (in[k].t) {
            case RD_IN_POINTER:
                SendPointerEvent(cl, in[k].x, in[k].y, in[k].buttons);
                break;
            case RD_IN_KEY:
                SendKeyEvent(cl, in[k].keysym, in[k].down ? TRUE : FALSE);
                break;
            case RD_IN_CLIPBOARD: {
                int l1len = 0;
                char *l1 = utf8_to_latin1(in[k].text, &l1len);
                if (l1) {
                    SendClientCutText(cl, l1, l1len);
                    free(l1);
                }
                free(in[k].text);
                break;
            }
            case RD_IN_QUALITY:
                vnc_apply_quality(rd, cl);
                SetFormatAndEncodings(cl);
                break;
            case RD_IN_SIZE:
                break; /* VNC 无客户端请求 resize 的标准机制 */
            }
        }
        hb__rd_in_drain_fd(rd);
        bool sock_ready = cl->buffered > 0 || (pfd[0].revents & POLLIN);
        if (sock_ready && !HandleRFBServerMessage(cl)) {
            if (!hb__rd_closing(rd))
                hb__rd_error(rd, HB_RD_ERR_DISCONNECTED, "连接中断",
                             "HandleRFBServerMessage failed");
            break;
        }
    }

    pthread_mutex_lock(&be->mu);
    be->cl = NULL;
    be->sock = -1;
    pthread_mutex_unlock(&be->mu);
    rfbClientCleanup(cl);
}

void hb__rd_vnc_destroy(hb_rd *rd)
{
    rd_vnc *be = beptr(rd);
    pthread_mutex_destroy(&be->mu);
    free(be);
    rd->be = NULL;
}
