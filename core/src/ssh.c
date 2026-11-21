/*
 * SSH 传输：libssh2（OpenSSL 后端）。一个 hb_session 一条 SSH 连接，
 * 支持 ProxyJump 跳板链（direct-tcpip + socketpair 泵线程）、TOFU/MITM
 * 主机密钥确认、agent/密钥/密码/keyboard-interactive 认证、keepalive。
 * 连接在 session 线程上跑，全程可经 hb_session_close 取消。
 */

#include "harbor/ssh.h"

#include "ssh_priv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pthread.h>
#include <sodium.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HANDSHAKE_MS  30000   /* 握手 / 认证 / channel 阶段死线 */
#define WAIT_SLICE_MS 100     /* 等待切片：期间检查取消 */

/* ------------------------------ 配置拷贝 ------------------------------ */

static char *xstrdup(const char *s)
{
    char *p;
    size_t n;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void hop_cfg_free(c_hop *c)
{
    free(c->host);
    free(c->user);
    if (c->password)
        hb_secret_free(c->password);
    if (c->privkey)
        hb_secret_free(c->privkey);
    free(c->identity_file);
    if (c->passphrase)
        hb_secret_free(c->passphrase);
    free(c->label);
}

/* ------------------------------ 小工具 ------------------------------ */

void hb__ssh_set_err(hb_ssh *h, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->errbuf, sizeof h->errbuf, fmt, ap);
    va_end(ap);
}

void hb__ssh_set_lib_err(hb_ssh *h, LIBSSH2_SESSION *sess)
{
    char *msg = NULL;
    int len = 0;
    if (sess &&
        libssh2_session_last_error(sess, &msg, &len, 0) == 0 && msg && len)
        snprintf(h->errbuf, sizeof h->errbuf, "libssh2: %.*s", len, msg);
    else
        snprintf(h->errbuf, sizeof h->errbuf, "libssh2 error %d",
                 sess ? libssh2_session_last_errno(sess) : 0);
}

void hb__ssh_emit_error(hb_ssh *h, int code, const char *text)
{
    hb__session_emit(h->s, HB_SESS_EV_ERROR, 0, code, text,
                     h->errbuf[0] ? h->errbuf : NULL);
}

/* 把等待码翻成错误类别 + 发事件；返回 -code */
static int fail_wait(hb_ssh *h, int w, int phase_err)
{
    if (w == W_CANCEL) {
        hb__ssh_emit_error(h, HB_SSH_ERR_CANCELLED, "连接已取消");
        return -HB_SSH_ERR_CANCELLED;
    }
    if (w == W_TMOUT) {
        hb__ssh_emit_error(h, HB_SSH_ERR_TIMEOUT, "主机不可达或超时");
        return -HB_SSH_ERR_TIMEOUT;
    }
    hb__ssh_emit_error(h, phase_err,
               phase_err == HB_SSH_ERR_DISCONNECTED ? "连接已断开"
                                                    : "连接失败");
    return -phase_err;
}

static void stage(hb_ssh *h, hop *hp, int st, const char *fmt, ...)
{
    char msg[384], line[560];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%s%s", hp->pfx, msg);
    hb__session_emit(h->s, HB_SESS_EV_STAGE, hp->mark, st, line, NULL);
}

/* ------------------------------ DNS（线程 + 超时/取消） ------------------------------ */

typedef struct {
    char             host[256];
    char             port[8];
    struct addrinfo *res;
    int              err;
    pthread_t        th;
    pthread_mutex_t  mu;
    bool             done;
    bool             abandoned;
} dns_job;

static void *dns_run(void *arg)
{
    dns_job *j = arg;
    struct addrinfo hints, *res = NULL;
    int e;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    e = getaddrinfo(j->host, j->port, &hints, &res);
    pthread_mutex_lock(&j->mu);
    if (j->abandoned) {
        pthread_mutex_unlock(&j->mu);
        if (res)
            freeaddrinfo(res);
        free(j);
        return NULL;
    }
    j->res = res;
    j->err = e;
    j->done = true;
    pthread_mutex_unlock(&j->mu);
    return NULL;
}

/* 0 成功；W_* 负码（导出给 pf.c：remote 规则本地解析 dest） */
int hb__ssh_resolve(hb_ssh *h, const char *host, int port,
                    int timeout_ms, struct addrinfo **res_out)
{
    dns_job *j;
    int64_t dl;
    int r = W_TMOUT;

    *res_out = NULL;
    j = calloc(1, sizeof *j);
    if (!j)
        return W_SOCK;
    snprintf(j->host, sizeof j->host, "%s", host);
    snprintf(j->port, sizeof j->port, "%d", port);
    pthread_mutex_init(&j->mu, NULL);
    if (pthread_create(&j->th, NULL, dns_run, j) != 0) {
        pthread_mutex_destroy(&j->mu);
        free(j);
        return W_SOCK;
    }
    dl = hb_now_ms() + timeout_ms;
    for (;;) {
        bool done;
        pthread_mutex_lock(&j->mu);
        done = j->done;
        pthread_mutex_unlock(&j->mu);
        if (done)
            break;
        if (hb__session_cancelled(h->s)) {
            r = W_CANCEL;
            goto abandon;
        }
        if (hb_now_ms() >= dl)
            goto abandon;
        usleep(3000);
    }
    pthread_join(j->th, NULL);
    if (j->err != 0 || !j->res) {
        hb__ssh_set_err(h, "getaddrinfo: %s",
                j->err ? gai_strerror(j->err) : "no result");
        r = W_SOCK;
    } else {
        *res_out = j->res;
        r = 0;
    }
    pthread_mutex_destroy(&j->mu);
    free(j);
    return r;

abandon:
    pthread_mutex_lock(&j->mu);
    j->abandoned = true;
    pthread_mutex_unlock(&j->mu);
    pthread_detach(j->th);   /* 线程结束时自行 free(j) */
    return r;
}

/* ------------------------------ TCP 非阻塞连接 ------------------------------ */

/* 0 成功；W_* 负码，-4 拒绝 */
static int tcp_connect(hb_ssh *h, struct addrinfo *res, int timeout_ms,
                       int *fd_out)
{
    int64_t dl = hb_now_ms() + timeout_ms;
    bool any_refused = false, any_other = false;

    for (; res; res = res->ai_next) {
        int fd = socket(res->ai_family,
                        res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        res->ai_protocol);
        int rc;
        if (fd < 0)
            continue;
        rc = connect(fd, res->ai_addr, res->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            if (errno == ECONNREFUSED)
                any_refused = true;
            else
                any_other = true;
            close(fd);
            continue;
        }
        if (rc != 0) {
            /* EINPROGRESS：poll 可写，切片内查取消 */
            int st = W_SOCK;
            for (;;) {
                struct pollfd p = { fd, POLLOUT, 0 };
                int64_t left = dl - hb_now_ms();
                int pr;
                if (hb__session_cancelled(h->s)) {
                    st = W_CANCEL;
                    break;
                }
                if (left <= 0) {
                    st = W_TMOUT;
                    break;
                }
                if (left > WAIT_SLICE_MS)
                    left = WAIT_SLICE_MS;
                pr = poll(&p, 1, (int)left);
                if (pr > 0) {
                    int e = 0;
                    socklen_t el = sizeof e;
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0) {
                        if (e == 0)
                            st = 0;
                        else if (e == ECONNREFUSED) {
                            any_refused = true;
                            st = W_SOCK;
                        } else {
                            any_other = true;
                            st = W_SOCK;
                        }
                    }
                    break;
                }
                if (pr < 0 && errno != EINTR) {
                    any_other = true;
                    st = W_SOCK;
                    break;
                }
            }
            if (st == 0) {
                *fd_out = fd;
                return 0;
            }
            close(fd);
            if (st == W_CANCEL || st == W_TMOUT)
                return st;
            continue;
        }
        *fd_out = fd;   /* 立即成功（回环常见） */
        return 0;
    }
    if (any_refused && !any_other)
        return -4;
    return W_SOCK;
}

/* ------------------------------ libssh2 EAGAIN 等待 ------------------------------ */

/* 0 ok；W_* 负码 */
int hb__ssh_wait_dir(hb_ssh *h, hop *hp, int64_t deadline_ms)
{
    int dir = libssh2_session_block_directions(hp->sess);
    short ev = 0;
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        ev |= POLLIN;
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        ev |= POLLOUT;
    if (!ev)
        ev = POLLIN;
    for (;;) {
        struct pollfd p = { hp->sock, ev, 0 };
        int64_t left;
        int r;
        if (hb__session_cancelled(h->s))
            return W_CANCEL;
        left = deadline_ms - hb_now_ms();
        if (left <= 0)
            return W_TMOUT;
        if (left > WAIT_SLICE_MS)
            left = WAIT_SLICE_MS;
        r = poll(&p, 1, (int)left);
        if (r > 0) {
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
                return W_SOCK;
            return 0;
        }
        if (r < 0 && errno != EINTR)
            return W_SOCK;
    }
}

/* int rc 表达式的 EAGAIN 重试：w 置 W_* 码；调用方 `if (w) ...` */
#define SSH_LOOP(h, hp, dl, expr, out, w)                                      \
    do {                                                                     \
        (w) = 0;                                                             \
        for (;;) {                                                           \
            (out) = (expr);                                                  \
            if ((out) != LIBSSH2_ERROR_EAGAIN)                               \
                break;                                                       \
            (w) = hb__ssh_wait_dir((h), (hp), (dl));                                 \
            if (w)                                                           \
                break;                                                       \
        }                                                                    \
    } while (0)

/* 指针表达式版本：NULL + errno==EAGAIN → 重试 */
#define SSH_LOOP_PTR(h, hp, dl, expr, out, w)                                  \
    do {                                                                     \
        (w) = 0;                                                             \
        for (;;) {                                                           \
            (out) = (expr);                                                  \
            if ((out) != NULL ||                                             \
                libssh2_session_last_errno((hp)->sess) !=                    \
                    LIBSSH2_ERROR_EAGAIN)                                    \
                break;                                                       \
            (w) = hb__ssh_wait_dir((h), (hp), (dl));                                 \
            if (w)                                                           \
                break;                                                       \
        }                                                                    \
    } while (0)

/* ------------------------------ 主机密钥 ------------------------------ */

static const char *hk_type_name(int t)
{
    switch (t) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:        return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_DSS:        return "ssh-dss";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:  return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:  return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:  return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:    return "ssh-ed25519";
    }
    return "unknown";
}

/* randomart 标题标签："ED25519 256" / "RSA 2048" / "ECDSA 256"… */
static void hk_label(int type, const unsigned char *key, size_t klen,
                     char *out, size_t cap)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        snprintf(out, cap, "ED25519 256");
        return;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        snprintf(out, cap, "ECDSA 256");
        return;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        snprintf(out, cap, "ECDSA 384");
        return;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        snprintf(out, cap, "ECDSA 521");
        return;
    case LIBSSH2_HOSTKEY_TYPE_RSA: {
        /* blob: string "ssh-rsa", mpint e, mpint n → n 的位数 */
        unsigned bits = 0;
        size_t off = 0;
        int i;
        for (i = 0; i < 3; i++) {
            uint32_t l;
            if (off + 4 > klen)
                break;
            memcpy(&l, key + off, 4);
            l = ntohl(l);
            off += 4;
            if (off + l > klen)
                break;
            if (i == 2) {   /* n */
                bits = l * 8;
                if (l > 0 && key[off] == 0)
                    bits -= 8;
            }
            off += l;
        }
        snprintf(out, cap, "RSA %u", bits ? bits : 2048);
        return;
    }
    default:
        snprintf(out, cap, "UNKNOWN");
    }
}

static char *b64_encode(const unsigned char *bin, size_t len, bool strip_pad)
{
    size_t cap = sodium_base64_ENCODED_LEN(len,
                                           sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(cap);
    size_t n;
    if (!b64)
        return NULL;
    sodium_bin2base64(b64, cap, bin, len, sodium_base64_VARIANT_ORIGINAL);
    if (strip_pad) {
        n = strlen(b64);
        while (n && b64[n - 1] == '=')
            b64[--n] = 0;
    }
    return b64;
}

char *hb_ssh_fingerprint_sha256(const unsigned char *key, size_t len)
{
    unsigned char d[32];
    char *b64, *out;
    unsigned int dl = 0;
    if (hb_core_init() != 0)
        return NULL;
    if (!EVP_Digest(key, len, d, &dl, EVP_sha256(), NULL) || dl != 32)
        return NULL;
    b64 = b64_encode(d, 32, true);
    if (!b64)
        return NULL;
    out = malloc(strlen(b64) + 8);
    if (out)
        sprintf(out, "SHA256:%s", b64);
    free(b64);
    return out;
}

/* ------------------------------ randomart ------------------------------ */

#define RA_X 17
#define RA_Y 9

static void ra_border(char *out, const char *title)
{
    /* "+--[title]--+"：标题在 17 字符内居中，多出的 '-' 放右边（OpenSSH 同款） */
    size_t tl = strlen(title);
    size_t pad = tl < RA_X ? RA_X - tl : 0;
    size_t left = pad / 2, right = pad - left, i;
    char *p = out;
    *p++ = '+';
    for (i = 0; i < left; i++)
        *p++ = '-';
    memcpy(p, title, tl);
    p += tl;
    for (i = 0; i < right; i++)
        *p++ = '-';
    *p++ = '+';
    *p++ = '\n';
    *p = 0;
}

char *hb_ssh_randomart(const char *keytype_label,
                       const unsigned char *sha256, size_t len)
{
    static const char aug[] = " .o+=*BOX@%&#/^";
    const int alen = (int)sizeof(aug) - 1;
    unsigned char field[RA_X][RA_Y];
    int x, y, i;
    char title[48], *out, *p;

    if (!sha256 || len == 0)
        return NULL;
    memset(field, 0, sizeof field);
    x = RA_X / 2;
    y = RA_Y / 2;
    for (i = 0; i < (int)len; i++) {
        int input = sha256[i];
        int bit;
        for (bit = 0; bit < 4; bit++) {
            x += (input & 0x1) ? 1 : -1;
            y += (input & 0x2) ? 1 : -1;
            if (x < 0) x = 0;
            if (x > RA_X - 1) x = RA_X - 1;
            if (y < 0) y = 0;
            if (y > RA_Y - 1) y = RA_Y - 1;
            if (field[x][y] < alen - 2)
                field[x][y]++;
            input >>= 2;
        }
    }
    field[RA_X / 2][RA_Y / 2] = (unsigned char)(alen - 1);   /* S */
    field[x][y] = (unsigned char)alen;                     /* E */

    snprintf(title, sizeof title, "[%s]", keytype_label ? keytype_label : "?");
    out = malloc((RA_X + 4) * (RA_Y + 2) + 8);
    if (!out)
        return NULL;
    p = out;
    ra_border(p, title);
    p += strlen(p);
    for (y = 0; y < RA_Y; y++) {
        *p++ = '|';
        for (x = 0; x < RA_X; x++) {
            int v = field[x][y];
            if (v == alen - 1)
                *p++ = 'S';
            else if (v == alen)
                *p++ = 'E';
            else
                *p++ = aug[v > alen ? alen : v];
        }
        *p++ = '|';
        *p++ = '\n';
    }
    ra_border(p, "[SHA256]");
    return out;
}

/* ------------------------------ parse_target / err_name ------------------------------ */

hb_err hb_ssh_parse_target(const char *s, char **user, char **host, int *port)
{
    const char *at, *h;
    char *hp = NULL;
    long p = 0;
    char *endp;

    if (!s || !*s || !user || !host || !port)
        return HB_ERR_INVALID;
    *user = NULL;
    *host = NULL;
    *port = 0;

    at = strrchr(s, '@');
    h = s;
    if (at) {
        if (at == s || !at[1])
            return HB_ERR_INVALID;
        *user = strndup(s, (size_t)(at - s));
        h = at + 1;
    }
    if (*h == '[') {
        const char *cb = strchr(h, ']');
        const char *rest;
        if (!cb)
            goto bad;
        hp = strndup(h + 1, (size_t)(cb - h - 1));
        rest = cb + 1;
        if (*rest == ':') {
            p = strtol(rest + 1, &endp, 10);
            if (*endp || endp == rest + 1)
                goto bad;
        } else if (*rest)
            goto bad;
    } else {
        const char *c1 = strchr(h, ':');
        if (!c1) {
            hp = xstrdup(h);
        } else if (strchr(c1 + 1, ':')) {
            hp = xstrdup(h);   /* 裸 IPv6，无端口 */
        } else {
            hp = strndup(h, (size_t)(c1 - h));
            p = strtol(c1 + 1, &endp, 10);
            if (*endp || endp == c1 + 1)
                goto bad;
        }
    }
    if (!hp || !*hp || p < 0 || p > 65535)
        goto bad;
    *host = hp;
    *port = (int)p;
    return HB_OK;
bad:
    free(hp);
    free(*user);
    *user = NULL;
    return HB_ERR_INVALID;
}

const char *hb_ssh_err_name(int code)
{
    switch (code) {
    case HB_SSH_ERR_RESOLVE:
    case HB_SSH_ERR_REFUSED:
    case HB_SSH_ERR_UNREACHABLE:      return "unreachable";
    case HB_SSH_ERR_TIMEOUT:          return "timeout";
    case HB_SSH_ERR_AUTH:             return "auth_failed";
    case HB_SSH_ERR_HOSTKEY_CHANGED:  return "hostkey_changed";
    case HB_SSH_ERR_HOSTKEY_REJECTED: return "hostkey_rejected";
    case HB_SSH_ERR_CANCELLED:        return "cancelled";
    case HB_SSH_ERR_FORWARD:          return "forward_failed";
    default:                          return "error";
    }
}

/* ------------------------------ 主机密钥校验 ------------------------------ */

/* 构造可写入 known_hosts 的一行 */
static char *kh_make_line(const char *host, int port, const char *type_name,
                          const unsigned char *key, size_t klen)
{
    char *b64 = b64_encode(key, klen, false);
    char *line;
    if (!b64)
        return NULL;
    line = malloc(strlen(host) + strlen(type_name) + strlen(b64) + 32);
    if (line) {
        if (port == 22)
            sprintf(line, "%s %s %s", host, type_name, b64);
        else
            sprintf(line, "[%s]:%d %s %s", host, port, type_name, b64);
    }
    free(b64);
    return line;
}

static unsigned hk_key_bit(int t)
{
    switch (t) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:        return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:        return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:  return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:  return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:  return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:    return LIBSSH2_KNOWNHOST_KEY_ED25519;
    }
    return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
}

/* 0 = 可继续；<0 = -hb_ssh_err */
static int hostkey_verify(hb_ssh *h, hop *hp)
{
    LIBSSH2_KNOWNHOSTS *kh;
    const char *key;
    size_t klen;
    int type = 0, chk;
    size_t i;

    kh = libssh2_knownhost_init(hp->sess);
    if (!kh) {
        hb__ssh_set_err(h, "knownhost init failed");
        hb__ssh_emit_error(h, HB_SSH_ERR_HANDSHAKE, "主机密钥校验初始化失败");
        return -HB_SSH_ERR_HANDSHAKE;
    }
    for (i = 0; i < h->nknown; i++)
        libssh2_knownhost_readline(kh, h->known[i], strlen(h->known[i]),
                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    key = libssh2_session_hostkey(hp->sess, &klen, &type);
    if (!key || !klen) {
        libssh2_knownhost_free(kh);
        hb__ssh_set_lib_err(h, hp->sess);
        hb__ssh_emit_error(h, HB_SSH_ERR_HANDSHAKE, "无法读取主机密钥");
        return -HB_SSH_ERR_HANDSHAKE;
    }
    chk = libssh2_knownhost_checkp(
        kh, hp->cfg.host, hp->cfg.port, key, klen,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
            hk_key_bit(type),
        NULL);
    libssh2_knownhost_free(kh);

    if (chk == LIBSSH2_KNOWNHOST_CHECK_MATCH)
        return 0;
    if (chk == LIBSSH2_KNOWNHOST_CHECK_FAILURE) {
        hb__ssh_set_err(h, "knownhost check failure");
        hb__ssh_emit_error(h, HB_SSH_ERR_HANDSHAKE, "主机密钥校验失败");
        return -HB_SSH_ERR_HANDSHAKE;
    }

    /* NOTFOUND（code 0 未知）/ MISMATCH（code 1 变更）→ 等 UI 裁决 */
    {
        int changed = (chk == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) ? 1 : 0;
        const char *tn = hk_type_name(type);
        char *fp = hb_ssh_fingerprint_sha256((const unsigned char *)key, klen);
        const char *hash =
            libssh2_hostkey_hash(hp->sess, LIBSSH2_HOSTKEY_HASH_SHA256);
        char label[40];
        char *art = NULL;
        char *khline =
            kh_make_line(hp->cfg.host, hp->cfg.port, tn,
                         (const unsigned char *)key, klen);
        char hostport[320], text2[128];
        char *extra = NULL;
        int accept = 0, r;

        hk_label(type, (const unsigned char *)key, klen, label, sizeof label);
        if (hash)
            art = hb_ssh_randomart(label, (const unsigned char *)hash, 32);
        snprintf(hostport, sizeof hostport, "%s:%d", hp->cfg.host, hp->cfg.port);
        snprintf(text2, sizeof text2, "%s %s", tn, fp ? fp : "?");
        if (khline && art) {
            size_t n = strlen(khline) + strlen(art) + 2;
            extra = malloc(n);
            if (extra)
                snprintf(extra, n, "%s\n%s", khline, art);
        } else if (khline) {
            extra = xstrdup(khline);
        }
        free(fp);
        free(art);

        hb__session_reply_reset(h->s, HB__REPLY_HOSTKEY);
        hb__session_emit_ex(h->s, HB_SESS_EV_HOSTKEY, hp->mark, changed,
                            hostport, text2, extra);
        free(extra);
        free(khline);
        r = hb__session_reply_wait(h->s, &accept, NULL);
        if (r <= 0) {
            hb__ssh_emit_error(h, HB_SSH_ERR_CANCELLED, "连接已取消");
            return -HB_SSH_ERR_CANCELLED;
        }
        if (!accept) {
            int code = changed ? HB_SSH_ERR_HOSTKEY_CHANGED
                               : HB_SSH_ERR_HOSTKEY_REJECTED;
            hb__ssh_emit_error(h, code,
                       changed ? "主机密钥已变更，可能存在中间人攻击"
                               : "主机密钥未被信任，连接已取消");
            return -code;
        }
    }
    return 0;
}

/* ------------------------------ 认证 ------------------------------ */

static const char *auth_name(hb_ssh_auth a)
{
    switch (a) {
    case HB_SSH_AUTH_PASSWORD: return "password";
    case HB_SSH_AUTH_KEY:      return "publickey";
    case HB_SSH_AUTH_AGENT:    return "agent";
    case HB_SSH_AUTH_NONE:     return "none";
    default:                   return "auto";
    }
}

static int ssh_authenticated(hop *hp)
{
    return libssh2_userauth_authenticated(hp->sess) == 1;
}

/* 取服务器认证方式列表；NULL = 已认证(none 通过)或真错误（当全允许处理）。
 * *w 置 W_* 等待码。 */
static char *auth_methods(hb_ssh *h, hop *hp, int64_t dl, int *w)
{
    *w = 0;
    for (;;) {
        char *m = libssh2_userauth_list(hp->sess, hp->cfg.user,
                                        strlen(hp->cfg.user));
        if (m || libssh2_userauth_authenticated(hp->sess))
            return m;
        if (libssh2_session_last_errno(hp->sess) != LIBSSH2_ERROR_EAGAIN)
            return NULL;   /* 真错误 → 后续当全部允许处理 */
        *w = hb__ssh_wait_dir(h, hp, dl);
        if (*w)
            return NULL;
    }
}

/* 0 成功；1 无 agent / 无可用身份（换下一方式）；W_* 等待码 */
static int auth_agent(hb_ssh *h, hop *hp, int64_t dl)
{
    LIBSSH2_AGENT *ag;
    struct libssh2_agent_publickey *id = NULL, *prev = NULL;
    int rc = -1, w = 0, ok = 1;
    const char *sock = getenv("SSH_AUTH_SOCK");

    if (!sock || !*sock)
        return 1;
    ag = libssh2_agent_init(hp->sess);
    if (!ag)
        return 1;
    SSH_LOOP(h, hp, dl, libssh2_agent_connect(ag), rc, w);
    if (w) {
        ok = w;
        goto out;
    }
    if (rc)
        goto out;
    SSH_LOOP(h, hp, dl, libssh2_agent_list_identities(ag), rc, w);
    if (w) {
        ok = w;
        goto out;
    }
    if (rc)
        goto out;
    while (ok != 0) {
        SSH_LOOP(h, hp, dl, libssh2_agent_get_identity(ag, &id, prev), rc, w);
        if (w) {
            ok = w;
            goto out;
        }
        if (rc)
            break;      /* 遍历完 */
        SSH_LOOP(h, hp, dl,
                 libssh2_agent_userauth(ag, hp->cfg.user, id), rc, w);
        if (w) {
            ok = w;
            goto out;
        }
        if (rc == 0) {
            ok = 0;
            break;
        }
        prev = id;
    }
out:
    libssh2_agent_disconnect(ag);
    libssh2_agent_free(ag);
    return ok;
}

/* 单次密钥尝试；返回 0 或 W_*；libssh2 rc 存 *rc_out */
static int key_try(hb_ssh *h, hop *hp, const char *priv_text,
                   const char *idfile, const char *pp, int64_t dl, int *rc_out)
{
    int w;
    if (priv_text) {
        SSH_LOOP(h, hp, dl,
                 libssh2_userauth_publickey_frommemory(
                     hp->sess, hp->cfg.user, strlen(hp->cfg.user),
                     NULL, 0, priv_text, strlen(priv_text), pp),
                 *rc_out, w);
    } else {
        SSH_LOOP(h, hp, dl,
                 libssh2_userauth_publickey_fromfile_ex(
                     hp->sess, hp->cfg.user,
                     (unsigned int)strlen(hp->cfg.user),
                     NULL, idfile, pp),
                 *rc_out, w);
    }
    return w;
}

static char *prompt_secret(hb_ssh *h, hop *hp, int code, const char *msg)
{
    char *got = NULL;
    char idx[8];   /* text2 = hop 下标：UI 凭它归属"记住口令"到正确的跳/目标 */
    hb__session_reply_reset(h->s, HB__REPLY_PROMPT);
    snprintf(idx, sizeof idx, "%d", (int)(hp - h->hops));
    hb__session_emit(h->s, HB_SESS_EV_PROMPT, hp->mark, code, msg, idx);
    if (hb__session_reply_wait(h->s, NULL, &got) <= 0) {
        hb_secret_free(got);
        return NULL;
    }
    return got;   /* NULL = 用户取消 */
}

/* 尝试一个密钥源（内存文本或文件），加密时按需 PROMPT 口令。
 * 0 成功；1 源不可用/服务器拒绝（换源）；<0 -hb_ssh_err */
static int auth_key_source(hb_ssh *h, hop *hp, const char *priv_text,
                           const char *idfile, int64_t *dl)
{
    const char *pp = hp->cfg.passphrase;
    char *got = NULL;
    int tries = 0, rc = -1, w;

    if (idfile && access(idfile, R_OK) != 0) {
        hb__ssh_set_err(h, "%s: %s", idfile, strerror(errno));
        return 1;
    }
    while (tries < 3) {
        w = key_try(h, hp, priv_text, idfile, pp, *dl, &rc);
        if (w) {
            hb_secret_free(got);
            return fail_wait(h, w, HB_SSH_ERR_AUTH);
        }
        if (rc == 0) {
            hb_secret_free(got);
            return 0;
        }
        if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED) {
            hb_secret_free(got);
            hb__ssh_set_lib_err(h, hp->sess);
            return 1;   /* 密钥解析正常但服务器不认 → 换源 */
        }
        /* 解析/解密失败 → 大概率要口令；PROMPT code 1 */
        {
            char msg[320];
            if (tries > 0)
                snprintf(msg, sizeof msg, "口令错误，请重试");
            else if (idfile)
                snprintf(msg, sizeof msg, "私钥 %s 的口令", idfile);
            else
                snprintf(msg, sizeof msg, "私钥口令");
            hb_secret_free(got);
            got = prompt_secret(h, hp, 1, msg);
            if (!got) {
                hb__ssh_emit_error(h, HB_SSH_ERR_CANCELLED, "连接已取消");
                return -HB_SSH_ERR_CANCELLED;
            }
            *dl = hb_now_ms() + HANDSHAKE_MS;   /* 用户输入时间不计死线 */
        }
        pp = got;
        tries++;
    }
    hb_secret_free(got);
    hb__ssh_set_lib_err(h, hp->sess);
    return 1;
}

/* keyboard-interactive：用密码填满所有挑战项 */
static void kbi_cb(const char *name, int name_len,
                   const char *instruction, int instruction_len,
                   int num_prompts,
                   const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                   LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                   void **abstract)
{
    const char *pw = (abstract && *abstract) ? *abstract : NULL;
    int i;
    (void)name; (void)name_len; (void)instruction; (void)instruction_len;
    (void)prompts;
    if (!pw)
        return;
    for (i = 0; i < num_prompts; i++) {
        responses[i].text = strdup(pw);
        responses[i].length = (unsigned int)strlen(pw);
    }
}

/* 0 成功；<0 -hb_ssh_err（已发事件） */
static int auth_password(hb_ssh *h, hop *hp, const char *methods, int64_t *dl)
{
    const char *use = hp->cfg.password;
    char *got = NULL;
    int tries = 0, w = 0;
    bool allow_kbi = !methods || strstr(methods, "keyboard-interactive");
    bool allow_pw = !methods || strstr(methods, "password");

    if (!allow_pw && !allow_kbi) {
        hb__ssh_set_err(h, "server does not offer password/keyboard-interactive");
        return -HB_SSH_ERR_AUTH;
    }
    while (tries < 3) {
        int rc = -1;
        if (!use) {
            char msg[320];
            if (tries > 0)
                snprintf(msg, sizeof msg, "密码错误，请重试");
            else
                snprintf(msg, sizeof msg, "%s@%s:%d 的密码",
                         hp->cfg.user, hp->cfg.host, hp->cfg.port);
            hb_secret_free(got);
            got = prompt_secret(h, hp, 0, msg);
            if (!got) {
                hb__ssh_emit_error(h, HB_SSH_ERR_CANCELLED, "连接已取消");
                return -HB_SSH_ERR_CANCELLED;
            }
            use = got;
            *dl = hb_now_ms() + HANDSHAKE_MS;
        }
        if (allow_pw) {
            SSH_LOOP(h, hp, *dl,
                     libssh2_userauth_password_ex(
                         hp->sess, hp->cfg.user,
                         (unsigned int)strlen(hp->cfg.user),
                         use, (unsigned int)strlen(use), NULL),
                     rc, w);
            if (w) {
                hb_secret_free(got);
                return fail_wait(h, w, HB_SSH_ERR_AUTH);
            }
            if (rc == 0) {
                hb_secret_free(got);
                return 0;
            }
        }
        if (allow_kbi && !ssh_authenticated(hp)) {
            *libssh2_session_abstract(hp->sess) = (void *)use;
            SSH_LOOP(h, hp, *dl,
                     libssh2_userauth_keyboard_interactive_ex(
                         hp->sess, hp->cfg.user,
                         (unsigned int)strlen(hp->cfg.user), &kbi_cb),
                     rc, w);
            *libssh2_session_abstract(hp->sess) = NULL;
            if (w) {
                hb_secret_free(got);
                return fail_wait(h, w, HB_SSH_ERR_AUTH);
            }
            if (rc == 0) {
                hb_secret_free(got);
                return 0;
            }
        }
        hb_secret_free(got);
        got = NULL;
        use = NULL;
        tries++;
        if (hp->cfg.password)
            break;   /* 显式给的密码错了 → 不重试 */
    }
    hb_secret_free(got);
    hb__ssh_set_lib_err(h, hp->sess);
    return -HB_SSH_ERR_AUTH;
}

/* 0 成功；<0 -hb_ssh_err（错误事件已发或待末尾统一发 AUTH） */
static int do_auth(hb_ssh *h, hop *hp, int64_t dl)
{
    c_hop *c = &hp->cfg;
    char *methods;
    int w = 0, r;

    methods = auth_methods(h, hp, dl, &w);
    if (w)
        return fail_wait(h, w, HB_SSH_ERR_AUTH);
    if (!methods && ssh_authenticated(hp))
        return 0;   /* none 直接通过 */
    if (c->auth == HB_SSH_AUTH_NONE)
        goto auth_fail;

    /* agent */
    if (c->auth == HB_SSH_AUTH_AGENT || c->auth == HB_SSH_AUTH_AUTO) {
        r = auth_agent(h, hp, dl);
        if (r == 0)
            return 0;
        if (IS_WAIT(r))
            return fail_wait(h, r, HB_SSH_ERR_AUTH);
        if (r < 0)
            return r;
        if (c->auth == HB_SSH_AUTH_AGENT)
            goto auth_fail;
    }

    /* 密钥：KEY → privkey → identity_file；
     * AUTO → identity_file → privkey → 默认 ~/.ssh/id_* */
    if ((c->auth == HB_SSH_AUTH_KEY || c->auth == HB_SSH_AUTH_AUTO) &&
        (!methods || strstr(methods, "publickey"))) {
        const char *src_priv[2] = { NULL, NULL };
        const char *src_file[2] = { NULL, NULL };
        int nsrc = 0, si;
        if (c->auth == HB_SSH_AUTH_KEY) {
            src_priv[nsrc++] = c->privkey;
            src_file[nsrc++] = c->identity_file;
        } else {
            src_file[nsrc++] = c->identity_file;
            src_priv[nsrc++] = c->privkey;
        }
        for (si = 0; si < nsrc; si++) {
            if (!src_priv[si] && !src_file[si])
                continue;
            r = auth_key_source(h, hp, src_priv[si], src_file[si], &dl);
            if (r == 0)
                return 0;
            if (r < 0)
                return r;
        }
        /* AUTO：未配置的默认身份文件 ~/.ssh/id_{ed25519,ecdsa,rsa} */
        if (c->auth == HB_SSH_AUTH_AUTO) {
            static const char *defs[] = {
                "id_ed25519", "id_ecdsa", "id_rsa"
            };
            const char *home = getenv("HOME");
            size_t di;
            for (di = 0; home && di < sizeof defs / sizeof defs[0]; di++) {
                char path[1080];
                snprintf(path, sizeof path, "%s/.ssh/%s", home, defs[di]);
                if (access(path, R_OK) != 0)
                    continue;
                r = auth_key_source(h, hp, NULL, path, &dl);
                if (r == 0)
                    return 0;
                if (r < 0)
                    return r;
            }
        }
        if (c->auth == HB_SSH_AUTH_KEY)
            goto auth_fail;
    } else if (c->auth == HB_SSH_AUTH_KEY) {
        hb__ssh_set_err(h, "server does not offer publickey");
        goto auth_fail;
    }

    /* 密码 */
    if (c->auth == HB_SSH_AUTH_PASSWORD || c->auth == HB_SSH_AUTH_AUTO) {
        r = auth_password(h, hp, methods, &dl);
        if (r == 0)
            return 0;
        if (r == -HB_SSH_ERR_AUTH)
            goto auth_fail;
        return r;   /* CANCELLED 等（事件已发） */
    }

auth_fail:
    if (!h->errbuf[0])
        hb__ssh_set_err(h, "no supported auth method / credentials rejected");
    hb__ssh_emit_error(h, HB_SSH_ERR_AUTH, "认证失败：服务器拒绝了提供的凭据");
    return -HB_SSH_ERR_AUTH;
}

/* ------------------------------ ProxyJump 泵 ------------------------------ */

/* libssh2 ≥1.11 会话级只有一个出站 packet 槽：有包未发完时，别的
 * transport_send 会替它续发并把成功错记给调用方（调用方负载根本没
 * 入包，真正的属主还会把同一负载再发一遍 → 内层密文流坏）。泵在
 * 外层 session 上只有三个生产者，按属主门控即可。 */
enum { PTX_NONE = 0, PTX_W, PTX_RD, PTX_EOF };

static int pump_pend(pump_t *p)
{
    if (!(libssh2_session_block_directions(p->sess) &
          LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
        p->txop = PTX_NONE;
        return 0;
    }
    return 1;
}

/* 该生产者可否发起会产生 SSH 包的调用：无挂起或属主同参。 */
static int pump_try(pump_t *p, int op)
{
    return !pump_pend(p) || p->txop == op;
}

/* 调用后登记属主；属主重试未排完不改写。 */
static void pump_note(pump_t *p, int op)
{
    if (pump_pend(p) && p->txop == PTX_NONE)
        p->txop = op;
}

static void *pump_run(void *arg)
{
    pump_t *p = arg;
    int tmo = 100;
    for (;;) {
        struct pollfd pf[2];
        int r;
        if (p->stop)
            break;
        /* 挂起包排空：RD 属主或无属主时，0 字节 read 触发内部
         * window-adjust 的同参续发（无属主则 mismatch-resume 代偿）。 */
        if (pump_pend(p) && p->txop != PTX_W && p->txop != PTX_EOF) {
            char j;
            (void)libssh2_channel_read(p->ch, &j, 0);
            pump_note(p, PTX_RD);
        }
        pf[0].fd = p->hop_fd;
        pf[0].events = (short)(POLLIN |
                               ((p->clen || pump_pend(p)) ? POLLOUT : 0));
        pf[0].revents = 0;
        pf[1].fd = p->pair;
        pf[1].events = (short)((p->chan_eof ? 0 : POLLIN) |
                               (p->plen ? POLLOUT : 0));
        pf[1].revents = 0;
        r = poll(pf, 2, tmo);
        tmo = 100;
        if (p->stop)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* pair → channel：先暂存，再尽量 channel_write */
        if ((pf[1].revents & POLLIN) && p->clen < sizeof p->cbuf) {
            ssize_t n = read(p->pair, p->cbuf + p->clen,
                             sizeof p->cbuf - p->clen);
            if (n > 0)
                p->clen += (size_t)n;
            else if (n == 0)
                p->pair_eof = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                     errno != EINTR)
                p->pair_eof = true;
        }
        if (p->clen && pump_try(p, PTX_W)) {
            ssize_t n = libssh2_channel_write(p->ch, (const char *)p->cbuf,
                                              p->clen);
            pump_note(p, PTX_W);
            if (n > 0) {
                memmove(p->cbuf, p->cbuf + n, p->clen - (size_t)n);
                p->clen -= (size_t)n;
            } else if (n != LIBSSH2_ERROR_EAGAIN) {
                p->chan_eof = true;
            }
        }

        /* channel → pair：channel_read 到 pbuf，再尽量写 pair。
         * read 内部可能发 window-adjust，同样受挂起门控。 */
        if (!p->chan_eof && p->plen < sizeof p->pbuf && pump_try(p, PTX_RD)) {
            ssize_t n = libssh2_channel_read(p->ch, (char *)p->pbuf + p->plen,
                                             sizeof p->pbuf - p->plen);
            pump_note(p, PTX_RD);
            if (n > 0) {
                p->plen += (size_t)n;
                /* libssh2 内部可能还压着已解密数据而 socket 不可读 →
                 * 下一轮 poll 立即返回，尽快再读 */
                tmo = 0;
            } else if (n == 0 && libssh2_channel_eof(p->ch))
                p->chan_eof = true;
            else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN)
                p->chan_eof = true;
        }
        if (p->plen && (pf[1].revents & POLLOUT)) {
            ssize_t n = send(p->pair, p->pbuf, p->plen, MSG_NOSIGNAL);
            if (n > 0) {
                memmove(p->pbuf, p->pbuf + n, p->plen - (size_t)n);
                p->plen -= (size_t)n;
                /* pbuf 排空后 channel 队列里可能还有明文 →
                 * 下轮 poll 不等待，尽快 channel_read 续上 */
                tmo = 0;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR) {
                p->pair_eof = true;   /* 内层已死 → channel 也无意义 */
                p->chan_eof = true;
                p->plen = 0;          /* 下行缓存无人能收，丢弃 */
            }
        }

        if (p->chan_eof) {
            p->clen = 0;                  /* channel 已死，上行缓存丢弃 */
            /* 下行缓存必须写完再 shutdown，否则截断尾包 */
            if (p->plen == 0 && !p->pair_eof) {
                shutdown(p->pair, SHUT_WR);   /* 通知内层：下行已尽 */
                p->pair_eof = true;
            }
        }
        /* 上行缓存必须先发完，再向 channel 宣告 EOF，否则截断尾包 */
        if (p->pair_eof && p->clen == 0 && !p->sent_eof &&
            pump_try(p, PTX_EOF)) {
            int rc = libssh2_channel_send_eof(p->ch);
            pump_note(p, PTX_EOF);
            if (rc != LIBSSH2_ERROR_EAGAIN)
                p->sent_eof = true;
        }
        if (p->chan_eof && p->pair_eof && p->plen == 0 && p->clen == 0)
            break;
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            p->chan_eof = true;
    }
    return NULL;
}

static int pump_start(hb_ssh *h, hop *inner)
{
    hop *outer = inner - 1;   /* hops[i-1] */
    int pr[2];
    pump_t *p = &inner->pump;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   pr) != 0) {
        hb__ssh_set_err(h, "socketpair: %s", strerror(errno));
        return W_SOCK;
    }
    p->sess = outer->sess;
    p->ch = outer->chan;
    p->hop_fd = outer->sock;
    p->pair = pr[0];
    inner->sock = pr[1];
    if (pthread_create(&p->th, NULL, pump_run, p) != 0) {
        close(pr[0]);
        close(pr[1]);
        inner->sock = -1;
        hb__ssh_set_err(h, "pthread_create failed");
        return W_SOCK;
    }
    p->started = true;
    inner->pump_on = true;
    return 0;
}

/* ------------------------------ hop 连接 ------------------------------ */

static int hop_up(hb_ssh *h, size_t i)
{
    hop *hp = &h->hops[i];
    int64_t dl;
    int w, rc;

    if (i == 0) {
        struct addrinfo *res = NULL;
        stage(h, hp, 0, "解析 %s…", hp->cfg.host);
        w = hb__ssh_resolve(h, hp->cfg.host, hp->cfg.port, h->timeout_ms, &res);
        if (w) {
            if (w == W_CANCEL)
                return fail_wait(h, w, HB_SSH_ERR_RESOLVE);
            if (w == W_TMOUT) {
                hb__ssh_emit_error(h, HB_SSH_ERR_TIMEOUT, "主机不可达或超时");
                return -HB_SSH_ERR_TIMEOUT;
            }
            {
                char msg[384];
                snprintf(msg, sizeof msg, "无法解析主机名 %s", hp->cfg.host);
                hb__ssh_emit_error(h, HB_SSH_ERR_RESOLVE, msg);
            }
            return -HB_SSH_ERR_RESOLVE;
        }
        stage(h, hp, 1, "TCP 连接 %s:%d…", hp->cfg.host, hp->cfg.port);
        w = tcp_connect(h, res, h->timeout_ms, &hp->sock);
        freeaddrinfo(res);
        if (w) {
            if (w == -4) {
                hb__ssh_set_err(h, "ECONNREFUSED");
                {
                    char msg[384];
                    snprintf(msg, sizeof msg, "无法连接：%s:%d 拒绝连接",
                             hp->cfg.host, hp->cfg.port);
                    hb__ssh_emit_error(h, HB_SSH_ERR_REFUSED, msg);
                }
                return -HB_SSH_ERR_REFUSED;
            }
            if (w == W_SOCK) {
                hb__ssh_set_err(h, "connect failed");
                hb__ssh_emit_error(h, HB_SSH_ERR_UNREACHABLE, "主机不可达或超时");
                return -HB_SSH_ERR_UNREACHABLE;
            }
            return fail_wait(h, w, HB_SSH_ERR_UNREACHABLE);
        }
    } else {
        /* 经上一跳 session 转发到本跳；地址由上一跳的服务端解析 */
        hop *out = &h->hops[i - 1];
        int64_t fdl = hb_now_ms() + HANDSHAKE_MS;
        stage(h, hp, 0, "解析 %s（远端）…", hp->cfg.host);
        stage(h, hp, 1, "经跳板 %zu 转发 %s:%d…", i, hp->cfg.host,
              hp->cfg.port);
        SSH_LOOP_PTR(h, out, fdl,
                     libssh2_channel_direct_tcpip_ex(
                         out->sess, hp->cfg.host, hp->cfg.port,
                         "127.0.0.1", 22),
                     out->chan, w);
        if (w)
            return fail_wait(h, w, HB_SSH_ERR_DISCONNECTED);
        if (!out->chan) {
            hb__ssh_set_lib_err(h, out->sess);
            hb__ssh_emit_error(h, HB_SSH_ERR_CHANNEL,
                       "无法建立跳板转发通道");
            return -HB_SSH_ERR_CHANNEL;
        }
        w = pump_start(h, hp);
        if (w)
            return fail_wait(h, w, HB_SSH_ERR_DISCONNECTED);
    }

    /* SSH 握手（非阻塞 + EAGAIN 重试） */
    stage(h, hp, 2, "SSH 握手…");
    hp->sess = libssh2_session_init();
    if (!hp->sess) {
        hb__ssh_set_err(h, "session_init failed");
        hb__ssh_emit_error(h, HB_SSH_ERR_HANDSHAKE, "SSH 握手失败");
        return -HB_SSH_ERR_HANDSHAKE;
    }
    libssh2_session_set_blocking(hp->sess, 0);
    dl = hb_now_ms() + HANDSHAKE_MS;
    SSH_LOOP(h, hp, dl, libssh2_session_handshake(hp->sess, hp->sock), rc, w);
    if (w)
        return fail_wait(h, w, HB_SSH_ERR_HANDSHAKE);
    if (rc) {
        hb__ssh_set_lib_err(h, hp->sess);
        hb__ssh_emit_error(h, HB_SSH_ERR_HANDSHAKE, "SSH 握手失败");
        return -HB_SSH_ERR_HANDSHAKE;
    }
    {
        const char *kex = libssh2_session_methods(hp->sess, LIBSSH2_METHOD_KEX);
        const char *hk =
            libssh2_session_methods(hp->sess, LIBSSH2_METHOD_HOSTKEY);
        stage(h, hp, 2, "SSH 握手 · kex %s · %s", kex ? kex : "?",
              hk ? hk : "?");
    }

    w = hostkey_verify(h, hp);
    if (w)
        return w;

    stage(h, hp, 3, "认证 %s (%s)…", hp->cfg.user,
          auth_name(hp->cfg.auth));
    w = do_auth(h, hp, hb_now_ms() + HANDSHAKE_MS);
    if (w)
        return w;   /* 错误事件已在 do_auth 内发出 */
    /* 认证完成 → 即擦密钥材料（sodium_free 清零） */
    if (hp->cfg.password) {
        hb_secret_free(hp->cfg.password);
        hp->cfg.password = NULL;
    }
    if (hp->cfg.passphrase) {
        hb_secret_free(hp->cfg.passphrase);
        hp->cfg.passphrase = NULL;
    }
    if (hp->cfg.privkey) {
        hb_secret_free(hp->cfg.privkey);
        hp->cfg.privkey = NULL;
    }
    return 0;
}

/* 目标跳：开 channel + pty + shell/exec */
static int target_shell(hb_ssh *h, hop *hp, int64_t dl)
{
    int w, rc;
    size_t i;

    stage(h, hp, 4, "启动 shell (%s)…", h->term);
    SSH_LOOP_PTR(h, hp, dl,
                 libssh2_channel_open_ex(hp->sess, "session", 7,
                                         LIBSSH2_CHANNEL_WINDOW_DEFAULT,
                                         LIBSSH2_CHANNEL_PACKET_DEFAULT,
                                         NULL, 0),
                 hp->chan, w);
    if (w)
        return fail_wait(h, w, HB_SSH_ERR_CHANNEL);
    if (!hp->chan) {
        hb__ssh_set_lib_err(h, hp->sess);
        hb__ssh_emit_error(h, HB_SSH_ERR_CHANNEL, "无法打开 SSH channel");
        return -HB_SSH_ERR_CHANNEL;
    }

    if (h->env) {
        for (i = 0; h->env[i]; i++) {
            const char *eq = strchr(h->env[i], '=');
            if (eq && eq != h->env[i]) {
                char k[128];
                size_t kl = (size_t)(eq - h->env[i]);
                if (kl >= sizeof k)
                    kl = sizeof k - 1;
                memcpy(k, h->env[i], kl);
                k[kl] = 0;
                libssh2_channel_setenv_ex(hp->chan, k, kl, eq + 1,
                                          strlen(eq + 1));   /* 忽略失败 */
            }
        }
    }

    SSH_LOOP(h, hp, dl,
             libssh2_channel_request_pty_ex(
                 hp->chan, h->term, (unsigned int)strlen(h->term),
                 NULL, 0, h->cols, h->rows, 0, 0),
             rc, w);
    if (w)
        return fail_wait(h, w, HB_SSH_ERR_CHANNEL);
    if (rc) {
        hb__ssh_set_lib_err(h, hp->sess);
        hb__ssh_emit_error(h, HB_SSH_ERR_CHANNEL, "PTY 请求被拒绝");
        return -HB_SSH_ERR_CHANNEL;
    }

    if (h->command) {
        SSH_LOOP(h, hp, dl,
                 libssh2_channel_process_startup(
                     hp->chan, "exec", 4, h->command, strlen(h->command)),
                 rc, w);
    } else {
        SSH_LOOP(h, hp, dl,
                 libssh2_channel_process_startup(
                     hp->chan, "shell", 5, NULL, 0),
                 rc, w);
    }
    if (w)
        return fail_wait(h, w, HB_SSH_ERR_CHANNEL);
    if (rc) {
        hb__ssh_set_lib_err(h, hp->sess);
        hb__ssh_emit_error(h, HB_SSH_ERR_CHANNEL, "无法启动远程 shell/命令");
        return -HB_SSH_ERR_CHANNEL;
    }

    if (h->keepalive_s > 0) {
        libssh2_keepalive_config(hp->sess, 1,
                                 (unsigned int)h->keepalive_s);
        h->ka_next = hb_now_ms() + (int64_t)h->keepalive_s * 1000;
        h->ka_last_in = hb_now_ms();
    }
    hb__session_emit(h->s, HB_SESS_EV_CONNECTED, hp->mark, 0, NULL, NULL);
    return 0;
}

/* ---- SHELL 目标会话出站包属主门控 ----
 * 与 ProxyJump 泵同一原因：libssh2 会话级只有一个挂起出站 packet 槽。
 * channel_write EAGAIN 留下 write_state=created 的半成品；此刻
 * channel_read 的内部 window-adjust / keepalive_send / pty-size
 * request / wait_closed 的 close 包续发同一槽位会被错记成功，随后
 * write 重试把同一负载再组包重发 → 终端输入重复（大粘贴必现）。
 * 目标会话上能产包的入口全部按属主门控；不可重试属主在 tick 里
 * 用 0 字节 channel_read 的 mismatch-resume 代偿排空。 */
enum {
    STX_NONE = 0,
    STX_W,      /* channel_write（可同参重试，只它自己排） */
    STX_RD,     /* channel_read 内部 window-adjust */
    STX_EOF,    /* channel_send_eof */
    STX_RSZ,    /* channel_request_pty_size */
    STX_CLS,    /* channel_wait_closed 的 close 包 */
    STX_KA,     /* keepalive_send */
    STX_ORPHAN  /* 无登记属主（如 connect 尾声遗留） */
};

/* 目标会话有挂起出站包？顺带在排空后复位属主。 */
static int shell_pend(hb_ssh *h)
{
    hop *hp = hb__ssh_target(h);
    if (!(libssh2_session_block_directions(hp->sess) &
          LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
        h->stx = STX_NONE;
        return 0;
    }
    return 1;
}

/* 该生产者可否发起会产生包的调用。 */
static int shell_try(hb_ssh *h, int op)
{
    return !shell_pend(h) || h->stx == op;
}

/* 调用后登记属主（仅当尚无属主）。 */
static void shell_note(hb_ssh *h, int op)
{
    if (shell_pend(h) && h->stx == STX_NONE)
        h->stx = op;
}

/* ------------------------------ transport ops ------------------------------ */

hop *hb__ssh_target(hb_ssh *h)
{
    return &h->hops[h->nhops - 1];
}

static int ssh_fd_op(void *ctx)
{
    hb_ssh *h = ctx;
    return hb__ssh_target(h)->sock;
}

static ssize_t ssh_read_op(void *ctx, char *buf, size_t len)
{
    hb_ssh *h = ctx;
    hop *hp = hb__ssh_target(h);
    ssize_t n;

    /* 能进这里说明 fd 可读了 —— 有包从对端过来（包括 libssh2 内部吃掉的
     * keepalive 回复），是半开 TCP 检测的"存活"信号。 */
    h->ka_last_in = hb_now_ms();
    if (!hp->chan) {
        errno = EIO;
        return -1;
    }
    /* 挂起包属主不是 read 时不能调：read 内部的 window-adjust 会替
     * 挂起包（如 channel_write 半成品）续发并被错记，属主重试时再
     * 发一遍 → 负载重复。挂起态等属主续发排完再来。 */
    if (!shell_try(h, STX_RD)) {
        errno = EAGAIN;
        return -1;
    }
    n = libssh2_channel_read(hp->chan, buf, len);
    shell_note(h, STX_RD);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        n = libssh2_channel_read_stderr(hp->chan, buf, len);
        shell_note(h, STX_RD);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            errno = EAGAIN;
            return -1;
        }
    }
    if (n > 0)
        return n;
    if (n == 0) {
        if (libssh2_channel_eof(hp->chan))
            return 0;
        errno = EAGAIN;
        return -1;
    }
    /* 真错误 */
    if (!libssh2_channel_eof(hp->chan))
        h->disconnected = true;
    errno = EIO;
    return -1;
}

static ssize_t ssh_write_op(void *ctx, const char *buf, size_t len)
{
    hb_ssh *h = ctx;
    hop *hp = hb__ssh_target(h);
    ssize_t n;

    if (!hp->chan) {
        errno = EIO;
        return -1;
    }
    /* 同理：别人（read 的 adjust / keepalive / resize / close）的包
     * 还挂着时不能写 —— write 的半成品若被外人续发错记，这里重试
     * 会把同一段输入再发一遍（大粘贴必坏）。 */
    if (!shell_try(h, STX_W)) {
        errno = EAGAIN;
        return -1;
    }
    n = libssh2_channel_write(hp->chan, buf, len);
    shell_note(h, STX_W);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (n < 0) {
        h->disconnected = true;
        errno = EIO;
        return -1;
    }
    return n;
}

static int ssh_resize_op(void *ctx, int rows, int cols)
{
    hb_ssh *h = ctx;
    hop *hp = hb__ssh_target(h);
    int rc, i;
    if (!hp->chan)
        return -1;
    /* 记下最新尺寸：被门控/超时没发出去时由 tick 补发 ——
     * session.c 对 resize 失败直接丢弃，不存这里会丢尺寸。 */
    h->rsz_rows = rows;
    h->rsz_cols = cols;
    /* 挂起包属主不是 resize → 本次不发（不能让这个包替别人续发
     * 错记），存下即返回，由 tick 补发。 */
    if (!shell_try(h, STX_RSZ))
        return 0;
    for (i = 0; i < 20; i++) {
        rc = libssh2_channel_request_pty_size_ex(hp->chan, cols, rows, 0, 0);
        shell_note(h, STX_RSZ);
        if (rc != LIBSSH2_ERROR_EAGAIN)
            break;
        /* 自己的包挂着只能同参重试（mismatch-resume 也行：发出去的
         * 仍是同一个尺寸请求，重试重复发幂等无害） */
        usleep(20000);
    }
    if (rc == LIBSSH2_ERROR_EAGAIN)
        return 0;             /* 仍未发完 → 留存，tick 补发 */
    h->rsz_rows = h->rsz_cols = 0;  /* 送达或对端明确拒绝，不再补 */
    return rc == 0 ? 0 : -1;
}

static bool ssh_poll_exit_op(void *ctx, int *code)
{
    hb_ssh *h = ctx;
    hop *hp = hb__ssh_target(h);

    if (h->disconnected && !h->disconnect_emitted) {
        h->disconnect_emitted = true;
        hb__ssh_set_err(h, "session/channel terminated unexpectedly");
        hb__ssh_emit_error(h, HB_SSH_ERR_DISCONNECTED, "连接已断开");
    }
    if (h->disconnected) {
        *code = -1;
        return true;
    }
    if (!hp->chan)
        return false;
    if (!libssh2_channel_eof(hp->chan))
        return false;
    /* wait_closed 内部发 close 包 —— 别的属主的包还挂着时本轮先不
     * 收尾（属主重试或 tick 代偿排空后下一轮再来），否则续发错记。 */
    if (!shell_try(h, STX_CLS))
        return false;
    /* exit-status 分组可能比 eof 晚几个包到达：EOF 后在非阻塞模式下
     * 再 drain 一小段时间（≤500ms）等 channel close，保证
     * get_exit_status 拿到的是真实退出码而非默认 0。 */
    {
        int64_t dl = hb_now_ms() + 500;
        int rc;
        while ((rc = libssh2_channel_wait_closed(hp->chan)) ==
               LIBSSH2_ERROR_EAGAIN && hb_now_ms() < dl)
            usleep(10000);
        shell_note(h, STX_CLS);
        (void)rc;
    }
    *code = libssh2_channel_get_exit_status(hp->chan);
    return true;
}

static void ssh_close_op(void *ctx)
{
    hb_ssh *h = ctx;
    hop *hp = hb__ssh_target(h);
    if (hp->sock >= 0)
        shutdown(hp->sock, SHUT_RDWR);
}

static const char *ssh_exit_text_op(void *ctx)
{
    hb_ssh *h = ctx;
    return h->disconnected ? "连接已断开" : "连接已关闭 · SSH channel EOF";
}

/* keepalive/半开检测节拍：返回值同 ssh_tick_op。SFTP 引擎复用。 */
int hb__ssh_ka_tick(hb_ssh *h, int may_send)
{
    int64_t now, left;
    int next = 0;

    if (!h->ka_next)
        return -1;
    now = hb_now_ms();
    if (now >= h->ka_next) {
        /* libssh2 keepalive 是 fire-and-forget：send() 打进内核缓冲就成功，
         * 拔线型半开连接要靠 want_reply 的回复来检测 —— 判据是"上次发出
         * keepalive 之后没有任何入站包（reply 也算）"，连续 3 次 → 死连接。
         * 不能拿 ka_last_in 比 keepalive_s：空闲连接上最后一个入站包就是
         * 上一个 reply，距今≈发送间隔，间隔大于 keepalive_s 时会误判。 */
        if (h->ka_last_out && h->ka_last_in < h->ka_last_out) {
            if (++h->ka_miss >= 3) {
                h->disconnected = true;
                return 0;
            }
        } else {
            h->ka_miss = 0;
        }
        if (may_send &&
            libssh2_keepalive_send(hb__ssh_target(h)->sess, &next) == 0) {
            h->ka_last_out = now;
            if (next > 0)
                h->ka_next = now + (int64_t)next * 1000;
            else
                h->ka_next = now + (int64_t)h->keepalive_s * 1000;
        } else {
            h->ka_next = now + (int64_t)h->keepalive_s * 1000;
        }
        if (h->disconnected)
            return 0;   /* 立刻唤醒 poll → read/poll_exit 走断线收尾 */
    }
    left = h->ka_next - now;
    if (left < 50)
        left = 50;
    if (left > 3600000)
        left = 3600000;
    return (int)left;
}

static int ssh_tick_op(void *ctx)
{
    hb_ssh *h = ctx;
    hop *hp = hb__ssh_target(h);
    int may, r;

    /* 排空挂起出站包：W 属主留给 flush_write 同参重试（read 替它续发
     * 正是要修的错记→重复）；其余属主（RD 的 adjust / resize / close /
     * keepalive / 无主遗留）用 0 字节 channel_read 代偿 —— 其内部
     * window-adjust 以 channel 内缓冲同参续发（RD），或对一次性包
     * mismatch-resume 直接发掉，负载本身都是想发的那个包，无害。 */
    if (shell_pend(h) && h->stx != STX_W && hp->chan) {
        char j;
        (void)libssh2_channel_read(hp->chan, &j, 0);
        shell_note(h, STX_RD);
    }
    /* 补发留存的 resize：槽位一空就发；仍 EAGAIN 则下次再来。 */
    if (h->rsz_rows > 0 && hp->chan && shell_try(h, STX_RSZ)) {
        int rc = libssh2_channel_request_pty_size_ex(
                     hp->chan, h->rsz_cols, h->rsz_rows, 0, 0);
        shell_note(h, STX_RSZ);
        if (rc != LIBSSH2_ERROR_EAGAIN)
            h->rsz_rows = h->rsz_cols = 0;
    }
    /* keepalive 也产包：挂起期间（除自身重发/无主遗留可代偿）不发；
     * keepalive 关闭时本节拍仍须运行（上面两段的排空/补发靠它）。 */
    r = -1;
    if (h->keepalive_s > 0) {
        may = !shell_pend(h) || h->stx == STX_NONE || h->stx == STX_KA;
        r = hb__ssh_ka_tick(h, may);
        if (may)
            shell_note(h, STX_KA);
    }
    /* 仍有挂起或待补发 → 短超时兜底：非 W 属主只有本函数代偿；
     * POLLIN 久不来也要保证 50ms 内再排一次，否则 write 一直被门住。 */
    if ((shell_pend(h) || h->rsz_rows > 0) && (r < 0 || r > 50))
        r = 50;
    return r;
}

/* ------------------------------ 释放 ------------------------------ */

static void pump_stop(pump_t *p)
{
    if (!p->started)
        return;
    p->stop = true;
    shutdown(p->pair, SHUT_RDWR);
    pthread_join(p->th, NULL);
    p->started = false;
    close(p->pair);
    p->pair = -1;
}

static void hop_teardown(hb_ssh *h, size_t i)
{
    hop *hp = &h->hops[i];
    (void)h;
    if (hp->pump_on)
        pump_stop(&hp->pump);
    if (hp->chan) {
        libssh2_channel_close(hp->chan);
        libssh2_channel_free(hp->chan);
        hp->chan = NULL;
    }
    if (hp->sess) {
        libssh2_session_disconnect_ex(hp->sess, SSH_DISCONNECT_BY_APPLICATION,
                                      "", "");
        libssh2_session_free(hp->sess);
        hp->sess = NULL;
    }
    if (hp->sock >= 0) {
        close(hp->sock);
        hp->sock = -1;
    }
}

static void ssh_destroy_op(void *ctx)
{
    hb_ssh *h = ctx;
    size_t i;
    if (!h)
        return;
    if (h->sftp_eng) {
        hb__sftp_eng_free(h->sftp_eng);
        h->sftp_eng = NULL;
    }
    if (h->pf_eng) {
        hb__pf_eng_free(h->pf_eng);
        h->pf_eng = NULL;
    }
    /* 内 → 外依次拆除（先停靠外的泵） */
    for (i = h->nhops; i > 0; i--)
        hop_teardown(h, i - 1);
    for (i = 0; i < h->nhops; i++)
        hop_cfg_free(&h->hops[i].cfg);
    for (i = 0; i < h->nknown; i++)
        free(h->known[i]);
    free(h->known);
    free(h->term);
    free(h->command);
    if (h->env) {
        for (i = 0; h->env[i]; i++)
            free(h->env[i]);
        free(h->env);
    }
    free(h->hops);
    free(h);
}

/* ------------------------------ connect ------------------------------ */

/* 解析一行 "K=V"（去引号/CR）到 out；无该行返回 false */
static bool osr_field(const char *txt, const char *key, char *out, size_t cap)
{
    const char *p = txt;
    size_t kl = strlen(key);
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && !memcmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = ll - kl - 1;
            if (vl && v[vl - 1] == '\r')
                vl--;
            if (vl >= 2 && (*v == '"' || *v == '\'') &&
                v[vl - 1] == *v) {
                v++;
                vl -= 2;
            }
            if (vl >= cap)
                vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return true;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return false;
}

/* 认证后 exec `cat /etc/os-release`（3s 预算、非致命）→ OSINFO。
 * 只有 W_CANCEL 传播（中止连接）；超时/失败一律静默跳过。 */
static int os_detect(hb_ssh *h, hop *hp)
{
    LIBSSH2_CHANNEL *ch = NULL;
    char buf[8192];
    char id[64] = "", pretty[256] = "";
    size_t len = 0;
    int64_t dl = hb_now_ms() + 3000;
    int w, rc = -1;

    SSH_LOOP_PTR(h, hp, dl,
                 libssh2_channel_open_ex(hp->sess, "session", 7,
                                         LIBSSH2_CHANNEL_WINDOW_DEFAULT,
                                         LIBSSH2_CHANNEL_PACKET_DEFAULT,
                                         NULL, 0),
                 ch, w);
    if (w || !ch)
        goto out;
    {
        const char *cmd = "cat /etc/os-release 2>/dev/null";
        SSH_LOOP(h, hp, dl,
                 libssh2_channel_process_startup(
                     ch, "exec", 4, cmd, strlen(cmd)),
                 rc, w);
    }
    if (w)
        goto out;
    if (!rc) {
        while (len < sizeof buf - 1) {
            ssize_t n = libssh2_channel_read(ch, buf + len,
                                             sizeof buf - 1 - len);
            if (n > 0) {
                len += (size_t)n;
                continue;
            }
            if (n == 0 && libssh2_channel_eof(ch))
                break;
            if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
                /* EAGAIN，或 libssh2 在 EOF 前偶发的 0 → 继续等到预算尽 */
                w = hb__ssh_wait_dir(h, hp, dl);
                if (w == W_CANCEL)
                    goto out;
                if (w)
                    break;   /* 超时等非致命 */
                continue;
            }
            break;   /* 真错误 → 非致命 */
        }
    }
    buf[len] = 0;
    if (len && osr_field(buf, "ID", id, sizeof id))
        osr_field(buf, "PRETTY_NAME", pretty, sizeof pretty);
out:
    if (ch)
        libssh2_channel_free(ch);
    if (id[0])
        hb__session_emit(h->s, HB_SESS_EV_OSINFO, 'T', 0, id,
                         pretty[0] ? pretty : NULL);
    return w == W_CANCEL ? w : 0;
}

/* SFTP 模式收尾：sftp_init → 引擎接管（CONNECT 后 tr.run 跑请求循环）。 */
static int target_sftp(hb_ssh *h, hop *hp, int64_t dl)
{
    LIBSSH2_SFTP *sf = NULL;
    int w = 0;

    stage(h, hp, 4, "启动 SFTP 子系统…");
    SSH_LOOP_PTR(h, hp, dl, libssh2_sftp_init(hp->sess), sf, w);
    if (w)
        return fail_wait(h, w, HB_SSH_ERR_CHANNEL);
    if (!sf) {
        hb__ssh_set_lib_err(h, hp->sess);
        hb__ssh_emit_error(h, HB_SSH_ERR_CHANNEL, "无法启动 SFTP 子系统");
        return -HB_SSH_ERR_CHANNEL;
    }
    if (hb__sftp_eng_bind(h->sftp_eng, h->s, sf) != 0) {
        libssh2_sftp_shutdown(sf);
        hb__ssh_emit_error(h, HB_SSH_ERR_CHANNEL, "无法启动 SFTP 引擎");
        return -HB_SSH_ERR_CHANNEL;
    }
    stage(h, hp, 4, "已连接");
    if (h->keepalive_s > 0) {
        libssh2_keepalive_config(hp->sess, 1,
                                 (unsigned int)h->keepalive_s);
        h->ka_next = hb_now_ms() + (int64_t)h->keepalive_s * 1000;
        h->ka_last_in = hb_now_ms();
    }
    hb__session_emit(h->s, HB_SESS_EV_CONNECTED, hp->mark, 0, NULL, NULL);
    return 0;
}

static int ssh_connect_op(void *ctx, hb_session *s)
{
    hb_ssh *h = ctx;
    size_t i;
    int rc;

    h->s = s;
    for (i = 0; i < h->nhops; i++) {
        rc = hop_up(h, i);
        if (rc)
            return -1;
    }
    if (h->detect_os) {
        rc = os_detect(h, hb__ssh_target(h));
        if (rc)
            return fail_wait(h, rc, HB_SSH_ERR_CHANNEL);
    }
    if (h->mode == HB_SSH_MODE_SFTP)
        return target_sftp(h, hb__ssh_target(h),
                           hb_now_ms() + HANDSHAKE_MS) ? -1 : 0;
    if (h->mode == HB_SSH_MODE_FORWARD)
        return hb__pf_connect(h->pf_eng, h, hb__ssh_target(h),
                              hb_now_ms() + HANDSHAKE_MS) ? -1 : 0;
    return target_shell(h, hb__ssh_target(h),
                        hb_now_ms() + HANDSHAKE_MS) ? -1 : 0;
}

/* ------------------------------ 构造 ------------------------------ */

static bool hop_cfg_copy(c_hop *dst, const hb_ssh_hop *src)
{
    memset(dst, 0, sizeof *dst);
    dst->host = xstrdup(src->host);
    dst->port = src->port > 0 ? src->port : 22;
    dst->user = xstrdup(src->user);
    dst->auth = src->auth;
    dst->password = hb_secret_strdup(src->password);
    dst->privkey = hb_secret_strdup(src->privkey);
    dst->identity_file = xstrdup(src->identity_file);
    dst->passphrase = hb_secret_strdup(src->passphrase);
    dst->label = xstrdup(src->label);
    if (!dst->host || !dst->user)
        return false;
    return true;
}

/* pf_eng：仅 FORWARD 模式由调用方预建并移交（local/dynamic 监听器在
 * 调用线程上已绑好），本函数无条件接管（失败路径经 destroy 释放）。 */
static hb_session *new_ssh_impl(const hb_ssh_opts *opts, int rows, int cols,
                                int scrollback_lines, void *pf_eng)
{
    hb_ssh *h;
    hb_session_transport tr;
    size_t i;

    if (!opts || !opts->target.host || !opts->target.user) {
        hb__pf_eng_free(pf_eng);
        return NULL;
    }
    h = calloc(1, sizeof *h);
    if (!h) {
        hb__pf_eng_free(pf_eng);
        return NULL;
    }
    h->nhops = opts->njumps + 1;
    h->hops = calloc(h->nhops, sizeof *h->hops);
    if (!h->hops) {
        hb__pf_eng_free(pf_eng);
        free(h);
        return NULL;
    }
    h->pf_eng = pf_eng;
    if (opts->mode == HB_SSH_MODE_FORWARD && !h->pf_eng)
        goto fail;
    h->timeout_ms = (opts->connect_timeout_s > 0 ? opts->connect_timeout_s : 15)
                    * 1000;
    h->keepalive_s = opts->keepalive_s;
    h->mode = opts->mode;
    h->term = xstrdup(opts->term ? opts->term : "xterm-256color");
    h->command = xstrdup(opts->command);
    h->detect_os = opts->detect_os && opts->mode == HB_SSH_MODE_SHELL;
    h->rows = rows;
    h->cols = cols;
    for (i = 0; i < h->nhops; i++)
        h->hops[i].sock = -1;

    /* 跳板（外→内）+ 目标 */
    for (i = 0; i < opts->njumps; i++) {
        hop *hp = &h->hops[i];
        if (!hop_cfg_copy(&hp->cfg, &opts->jumps[i]))
            goto fail;
        hp->mark = 'J';
        snprintf(hp->pfx, sizeof hp->pfx, "[跳板 %zu/%zu %s] ", i + 1,
                 opts->njumps,
                 opts->jumps[i].label ? opts->jumps[i].label
                                      : opts->jumps[i].host);
    }
    if (!hop_cfg_copy(&h->hops[h->nhops - 1].cfg, &opts->target))
        goto fail;
    h->hops[h->nhops - 1].mark = 'T';

    /* known_hosts 行拷贝 */
    h->nknown = opts->nknown;
    if (h->nknown) {
        h->known = calloc(h->nknown, sizeof *h->known);
        if (!h->known)
            goto fail;
        for (i = 0; i < h->nknown; i++) {
            h->known[i] = xstrdup(opts->known_lines[i]);
            if (!h->known[i])
                goto fail;
        }
    }
    /* env 数组拷贝 */
    if (opts->env) {
        size_t n = 0;
        while (opts->env[n])
            n++;
        h->env = calloc(n + 1, sizeof *h->env);
        if (!h->env)
            goto fail;
        for (i = 0; i < n; i++)
            h->env[i] = xstrdup(opts->env[i]);
    }

    if (h->mode == HB_SSH_MODE_SFTP) {
        h->sftp_eng = hb__sftp_eng_new();
        if (!h->sftp_eng)
            goto fail;
    }

    memset(&tr, 0, sizeof tr);
    tr.ctx = h;
    tr.fd = ssh_fd_op;
    tr.read = ssh_read_op;
    tr.write = ssh_write_op;
    tr.resize = ssh_resize_op;
    tr.poll_exit = ssh_poll_exit_op;
    tr.close = ssh_close_op;
    tr.destroy = ssh_destroy_op;
    /* tick 必须始终装上：keepalive 关时它也是挂起包排空/resize 补发
     * 的唯一驱动者（无事时返回 -1，不产生空转唤醒）。 */
    tr.tick = ssh_tick_op;
    tr.exit_text = ssh_exit_text_op;
    tr.connect = ssh_connect_op;
    if (h->mode == HB_SSH_MODE_SFTP)
        tr.run = hb__sftp_run;
    else if (h->mode == HB_SSH_MODE_FORWARD)
        tr.run = hb__pf_run;

    {
        hb_session *s = hb_session_new_transport(&tr, rows, cols,
                                                 scrollback_lines,
                                                 opts->reuse_vt);
        if (!s)
            goto fail;
        return s;
    }
fail:
    ssh_destroy_op(h);
    return NULL;
}

hb_session *hb_session_new_ssh(const hb_ssh_opts *opts,
                               int rows, int cols, int scrollback_lines)
{
    return new_ssh_impl(opts, rows, cols, scrollback_lines, NULL);
}

hb_session *hb__ssh_new_forward(const hb_ssh_opts *opts, void *pf_eng)
{
    return new_ssh_impl(opts, 0, 0, 0, pf_eng);
}

hb_session *hb_session_new_sftp(const hb_ssh_opts *opts)
{
    hb_ssh_opts o;
    if (!opts)
        return NULL;
    o = *opts;
    o.mode = HB_SSH_MODE_SFTP;
    return hb_session_new_ssh(&o, 0, 0, 0);
}
