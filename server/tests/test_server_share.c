/* harbor-server share relay 测试（docs/api/share-protocol.md §2/§4/§6）：
 * 真实 spawn 服务端，libsoup + 裸 socket 客户端驱动。
 * 覆盖：登记 REST、路由重写与类型过滤、PAKE 锁定/兜底、容量、生命周期、
 * 背压丢帧、以及 core 引擎经真实 relay 的端到端。 */

#include <harbor/session.h>
#include <harbor/share.h>
#include <harbor/vt.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <sodium.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int t_failures, t_checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        t_checks++;                                                       \
        if (!(cond)) {                                                    \
            t_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,        \
                    __LINE__, #cond);                                     \
        }                                                                 \
    } while (0)
#define CHECK_EQ_INT(a, b)                                                \
    do {                                                                  \
        t_checks++;                                                       \
        long long _a = (long long)(a), _b = (long long)(b);               \
        if (_a != _b) {                                                   \
            t_failures++;                                                 \
            fprintf(stderr,                                               \
                    "%s:%d: CHECK_EQ_INT failed: %s=%lld %s=%lld\n",      \
                    __FILE__, __LINE__, #a, _a, #b, _b);                  \
        }                                                                 \
    } while (0)
#define CHECK_EQ_STR(a, b)                                                \
    do {                                                                  \
        t_checks++;                                                       \
        const char *_a = (a), *_b = (b);                                  \
        if ((_a == NULL) != (_b == NULL) ||                               \
            (_a && _b && strcmp(_a, _b))) {                               \
            t_failures++;                                                 \
            fprintf(stderr,                                               \
                    "%s:%d: CHECK_EQ_STR failed: %s='%s' %s='%s'\n",      \
                    __FILE__, __LINE__, #a, _a, #b, _b);                  \
        }                                                                 \
    } while (0)

static int64_t now_ms(void) { return g_get_monotonic_time() / 1000; }
static void msleep(int ms) { g_usleep((guint)ms * 1000); }

static SoupSession *g_sess;
static char *g_base, *g_wsbase, *g_dir, *g_dump;
static int g_port;
static GPid g_pid = -1;

/* ============================ helpers ============================ */

static char *b64e(const uint8_t *d, size_t n)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t outn = (n / 3) * 4 + (n % 3 ? n % 3 + 1 : 0), i;
    char *o = malloc(outn + 1), *p = o;
    for (i = 0; i + 3 <= n; i += 3) {
        uint32_t w = d[i] << 16 | d[i + 1] << 8 | d[i + 2];
        *p++ = T[w >> 18]; *p++ = T[w >> 12 & 63];
        *p++ = T[w >> 6 & 63]; *p++ = T[w & 63];
    }
    if (n % 3 == 1) {
        *p++ = T[d[i] >> 2]; *p++ = T[(d[i] & 3) << 4];
    } else if (n % 3 == 2) {
        uint32_t w = d[i] << 16 | d[i + 1] << 8;
        *p++ = T[w >> 18]; *p++ = T[w >> 12 & 63]; *p++ = T[w >> 6 & 63];
    }
    *p = 0;
    return o;
}

typedef struct {
    guint status;
    char *body;
    JsonObject *obj;
} Resp;

static void resp_free(Resp *r)
{
    free(r->body);
    if (r->obj) json_object_unref(r->obj);
}

static Resp http(const char *method, const char *path, const char *body,
                 const char *token)
{
    Resp r = {0, NULL, NULL};
    g_autofree char *url = g_strconcat(g_base, path, NULL);
    SoupMessage *m = soup_message_new(method, url);
    GBytes *rb;
    gsize bn;
    if (token) {
        g_autofree char *a = g_strdup_printf("Bearer %s", token);
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", a);
    }
    if (body) {
        GBytes *bb = g_bytes_new(body, strlen(body));
        soup_message_set_request_body_from_bytes(m, "application/json", bb);
        g_bytes_unref(bb);
    }
    rb = soup_session_send_and_read(g_sess, m, NULL, NULL);
    r.status = soup_message_get_status(m);
    if (rb) {
        const char *d = g_bytes_get_data(rb, &bn);
        r.body = g_strndup(d, bn);
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, r.body, bn, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_OBJECT(root))
                r.obj = json_object_ref(json_node_get_object(root));
        }
        g_object_unref(p);
        g_bytes_unref(rb);
    }
    g_object_unref(m);
    return r;
}

static Resp post(const char *path, const char *body, const char *tok)
{
    return http(SOUP_METHOD_POST, path, body, tok);
}

static Resp get(const char *path, const char *tok)
{
    return http(SOUP_METHOD_GET, path, NULL, tok);
}

static const char *jstr(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_STRING)
               ? json_node_get_string(n)
               : NULL;
}

static const char *jerr(JsonObject *o) { return jstr(o, "error"); }

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    socklen_t l = sizeof a;
    int port;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    getsockname(fd, (struct sockaddr *)&a, &l);
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static void start_server(const char *dir)
{
    char **env, *listen_a, **argv;
    GError *err = NULL;
    g_free(g_dir);
    g_dir = strdup(dir);
    g_port = free_port();
    CHECK(g_port > 0);
    g_base = g_strdup_printf("http://127.0.0.1:%d", g_port);
    g_wsbase = g_strdup_printf("ws://127.0.0.1:%d", g_port);
    listen_a = g_strdup_printf("127.0.0.1:%d", g_port);
    env = g_environ_setenv(g_get_environ(), "HARBOR_SERVER_TEST_LOCK_MS",
                           "200", TRUE);
    env = g_environ_setenv(env, "HARBOR_SERVER_TEST_IP_LIMIT", "10000",
                           TRUE);
    env = g_environ_setenv(env, "HARBOR_SERVER_FRAME_DUMP", g_dump, TRUE);
    argv = g_new0(char *, 9);
    argv[0] = (char *)SERVER_BIN;
    argv[1] = "--listen";
    argv[2] = listen_a;
    argv[3] = "--data";
    argv[4] = g_dir;
    argv[5] = "--log-level";
    argv[6] = "warn";
    if (!g_spawn_async(NULL, argv, env, G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &g_pid, &err)) {
        fprintf(stderr, "spawn: %s\n", err->message);
        CHECK(0);
    }
    for (int i = 0; i < 100; i++) {
        Resp r = post("/v1/prelogin", "{\"email\":\"w@u.p\"}", NULL);
        if (r.status) {
            resp_free(&r);
            break;
        }
        msleep(50);
    }
    g_free(listen_a);
    g_free(argv);
    g_strfreev(env);
}

static void stop_server(void)
{
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        g_spawn_close_pid(g_pid);
        g_pid = -1;
    }
}

/* ---- 账号 ---- */

typedef struct {
    char *account_id, *device_id, *token;
} Acct;

static void acct_free(Acct *a)
{
    free(a->account_id); free(a->device_id); free(a->token);
}

static bool do_register(Acct *a, const char *email, uint8_t seed)
{
    Resp r;
    char *body, *salt;
    {
        uint8_t s[16];
        memset(s, seed + 2, 16);
        salt = b64e(s, 16);
    }
    {
        uint8_t ak[32], ra[32];
        char *akb, *rab, *wp, *wr;
        memset(ak, seed, 32); memset(ra, seed + 1, 32);
        akb = b64e(ak, 32); rab = b64e(ra, 32);
        wp = b64e((uint8_t *)"wrapped-pw", 10);
        wr = b64e((uint8_t *)"wrapped-rc", 10);
        body = g_strdup_printf(
            "{\"email\":\"%s\",\"auth_key\":\"%s\",\"rc_auth\":\"%s\","
            "\"kdf\":{\"salt\":\"%s\",\"ops\":2,\"mem\":67108864},"
            "\"wrapped_vk_pw\":\"%s\",\"wrapped_vk_rc\":\"%s\","
            "\"device\":{\"name\":\"dev-%d\",\"platform\":\"linux\"}}",
            email, akb, rab, salt, wp, wr, seed);
        free(akb); free(rab); free(wp); free(wr);
    }
    r = post("/v1/register", body, NULL);
    g_free(body);
    free(salt);
    if (r.status != 200) {
        fprintf(stderr, "register %s -> %u %s\n", email, r.status,
                r.body ? r.body : "?");
        resp_free(&r);
        return false;
    }
    a->account_id = g_strdup(jstr(r.obj, "account_id"));
    a->device_id = g_strdup(jstr(r.obj, "device_id"));
    a->token = g_strdup(jstr(r.obj, "token"));
    resp_free(&r);
    return a->account_id && a->device_id && a->token;
}

/* 同账号再登录一台设备（device 名 devname）。 */
static bool do_login(Acct *a, const char *email, const char *devname,
                     uint8_t seed)
{
    Resp r;
    bool ok = false;
    char *body;
    {
        uint8_t s[16], ak[32], ra[32];
        char *salt, *akb, *rab;
        memset(s, seed + 2, 16);
        memset(ak, seed, 32); memset(ra, seed + 1, 32);
        salt = b64e(s, 16); akb = b64e(ak, 32); rab = b64e(ra, 32);
        body = g_strdup_printf(
            "{\"email\":\"%s\",\"auth_key\":\"%s\",\"rc_auth\":\"%s\","
            "\"kdf\":{\"salt\":\"%s\",\"ops\":2,\"mem\":67108864},"
            "\"device\":{\"name\":\"%s\",\"platform\":\"linux\"}}",
            email, akb, rab, salt, devname);
        free(salt); free(akb); free(rab);
    }
    r = post("/v1/login", body, NULL);
    g_free(body);
    if (r.status == 200) {
        a->account_id = g_strdup(jstr(r.obj, "account_id"));
        a->device_id = g_strdup(jstr(r.obj, "device_id"));
        a->token = g_strdup(jstr(r.obj, "token"));
        ok = a->account_id && a->device_id && a->token;
    } else {
        fprintf(stderr, "login %s -> %u %s\n", email, r.status,
                r.body ? r.body : "?");
    }
    resp_free(&r);
    return ok;
}

/* ============================ WS 客户端 ============================ */

typedef struct Ws Ws;
struct Ws {
    SoupWebsocketConnection *conn;
    GError                  *err;
    bool                     done, closed;
    guint                    close_code;
    guint                    hs_status;   /* 握手失败时的 HTTP status */
    GPtrArray               *texts;       /* char* */
    GPtrArray               *bins;        /* GBytes* */
    /* e2e：挂引擎直接喂 */
    hb_share_owner          *o;
    hb_share_peer           *p;
};

static void ws_cb(GObject *src, GAsyncResult *res, gpointer ud)
{
    Ws *w = ud;
    SoupMessage *m;
    w->conn = soup_session_websocket_connect_finish(SOUP_SESSION(src),
                                                    res, &w->err);
    m = soup_session_get_async_result_message(SOUP_SESSION(src), res);
    if (m)
        w->hs_status = soup_message_get_status(m);
    w->done = true;
}

static void ws_on_msg(SoupWebsocketConnection *c, int type, GBytes *msg,
                      gpointer ud)
{
    Ws *w = ud;
    gsize n;
    const uint8_t *d = g_bytes_get_data(msg, &n);
    (void)c;
    if (type == SOUP_WEBSOCKET_DATA_TEXT)
        g_ptr_array_add(w->texts, g_strndup((const char *)d, n));
    else
        g_ptr_array_add(w->bins, g_bytes_ref(msg));
    if (w->o)
        hb_share_owner_incoming(w->o, d, n,
                                type == SOUP_WEBSOCKET_DATA_TEXT);
    else if (w->p)
        hb_share_peer_incoming(w->p, d, n,
                               type == SOUP_WEBSOCKET_DATA_TEXT);
}

static void ws_on_closed(SoupWebsocketConnection *c, gpointer ud)
{
    Ws *w = ud;
    w->closed = true;
    w->close_code = soup_websocket_connection_get_close_code(c);
}

static void pump(int ms);

static Ws *ws_open(const char *rel, const char *token)
{
    Ws *w = g_new0(Ws, 1);
    g_autofree char *url = g_strconcat(g_wsbase, rel, NULL);
    SoupMessage *m = soup_message_new(SOUP_METHOD_GET, url);
    int i;
    w->texts = g_ptr_array_new_with_free_func(g_free);
    w->bins = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_bytes_unref);
    if (token) {
        g_autofree char *ah = g_strdup_printf("Bearer %s", token);
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", ah);
    }
    soup_session_websocket_connect_async(g_sess, m, NULL, NULL, 0, NULL,
                                         ws_cb, w);
    for (i = 0; i < 400 && !w->done; i++)
        g_main_context_iteration(NULL, TRUE);
    g_object_unref(m);
    if (w->conn) {
        /* 协议帧可达 4 MiB；libsoup 默认入站上限会断大包 */
        soup_websocket_connection_set_max_incoming_payload_size(
            w->conn, (gsize)8 << 20);
        g_signal_connect(w->conn, "message", G_CALLBACK(ws_on_msg), w);
        g_signal_connect(w->conn, "closed", G_CALLBACK(ws_on_closed), w);
    }
    return w;
}

static void ws_free(Ws *w)
{
    if (w->conn) {
        if (soup_websocket_connection_get_state(w->conn) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(w->conn, 1000, NULL);
        g_object_unref(w->conn);
    }
    g_clear_error(&w->err);
    g_ptr_array_free(w->texts, TRUE);
    g_ptr_array_free(w->bins, TRUE);
    g_free(w);
}

static const char *ws_last_text(Ws *w)
{
    return w->texts->len
               ? g_ptr_array_index(w->texts, w->texts->len - 1)
               : NULL;
}

/* 越界安全取帧：返回空帧（type 0），调用处配合 len CHECK。 */
static const uint8_t *bin_at(Ws *w, guint i, gsize *n)
{
    static const uint8_t z[1] = {0};
    gsize sz;
    const uint8_t *d;
    if (i >= w->bins->len) {
        if (n) *n = 0;
        return z;
    }
    d = g_bytes_get_data(g_ptr_array_index(w->bins, i), &sz);
    if (n) *n = sz;
    return d;
}

static int bin_type_at(Ws *w, guint i) { return bin_at(w, i, NULL)[0]; }

static bool ws_texts_has(Ws *w, const char *sub)
{
    guint i;
    for (i = 0; i < w->texts->len; i++)
        if (strstr(g_ptr_array_index(w->texts, i), sub))
            return true;
    return false;
}

/* 文本消息 id 字段提取（"id":N）。 */
static int text_id(const char *t)
{
    const char *p = t ? strstr(t, "\"id\":") : NULL;
    return p ? atoi(p + 5) : -1;
}

/* ---- 裸 socket WS 客户端（不读 → 服务端背压/心跳场景） ---- */

static int raw_tcp(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(g_port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 完成 WS 握手返回 fd；失败 -1。 */
static int raw_ws(const char *rel, const char *token)
{
    int fd = raw_tcp(), n = 0, off = 0;
    char req[1024], buf[4096], *p;
    int rn;
    if (fd < 0) return -1;
    rn = snprintf(req, sizeof req,
                  "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                  "Sec-WebSocket-Version: 13\r\n%s"
                  "\r\n",
                  rel,
                  token ? "" : "");
    if (token) {
        /* 重带 Authorization 重写请求 */
        rn = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "Authorization: Bearer %s\r\n\r\n",
                      rel, token);
    }
    if (write(fd, req, rn) != rn) { close(fd); return -1; }
    /* 读响应头直到 \r\n\r\n */
    while (off < (int)sizeof buf - 1) {
        n = read(fd, buf + off, sizeof buf - 1 - off);
        if (n <= 0) { close(fd); return -1; }
        off += n;
        buf[off] = 0;
        if ((p = strstr(buf, "\r\n\r\n"))) {
            if (!strstr(buf, " 101")) { close(fd); return -1; }
            return fd;
        }
    }
    close(fd);
    return -1;
}

/* 解析 server→client 未掩码帧；返回消耗字节，0=不完整。 */
static size_t ws_parse(const uint8_t *b, size_t n, uint8_t *op,
                       const uint8_t **pl, size_t *plen)
{
    uint64_t l;
    size_t h;
    if (n < 2) return 0;
    *op = b[0] & 0x0f;
    l = b[1] & 0x7f;
    h = 2;
    if (l == 126) {
        if (n < 4) return 0;
        l = (b[2] << 8) | b[3];
        h = 4;
    } else if (l == 127) {
        int i;
        if (n < 10) return 0;
        l = 0;
        for (i = 0; i < 8; i++) l = l << 8 | b[2 + i];
        h = 10;
    }
    if (n < h + l) return 0;
    *pl = b + h;
    *plen = (size_t)l;
    return h + (size_t)l;
}

/* ============================ 帧构造 ============================ */

/* 17B 头 + payload；to/from/seq 任意填（relay 改写参与者的）。 */
static GBytes *mk_frame(uint8_t type, uint32_t to, uint32_t from,
                        uint64_t seq, const void *pl, size_t plen)
{
    uint8_t *b = malloc(17 + plen);
    int i;
    b[0] = type;
    for (i = 0; i < 4; i++) b[1 + i] = (to >> (24 - i * 8)) & 0xff;
    for (i = 0; i < 4; i++) b[5 + i] = (from >> (24 - i * 8)) & 0xff;
    for (i = 0; i < 8; i++) b[9 + i] = (seq >> (56 - i * 8)) & 0xff;
    memcpy(b + 17, pl, plen);
    {
        GBytes *g = g_bytes_new_take(b, 17 + plen);
        return g;
    }
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* ============================ sqlite 直查 ============================ */

static bool db_share(const char *id, int *fails, int *locked, int *ended)
{
    g_autofree char *path = g_build_filename(g_dir, "harbor-server.db",
                                             NULL);
    sqlite3 *db = NULL;
    sqlite3_stmt *st;
    bool ok = false;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return false;
    st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT pake_fails,locked,ended FROM shares WHERE share_id=?",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (fails) *fails = sqlite3_column_int(st, 0);
        if (locked) *locked = sqlite3_column_int(st, 1);
        if (ended) *ended = sqlite3_column_int(st, 2);
        ok = true;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}

/* ============================ 引擎 pump（e2e） ============================ */

static void eng_frame(Ws *w, hb_share_frame *f)
{
    if (f->is_text) {
        char *t = g_strndup((const char *)f->data, f->len);
        soup_websocket_connection_send_text(w->conn, t);
        g_free(t);
    } else {
        soup_websocket_connection_send_binary(w->conn, f->data, f->len);
    }
}

static void eng_drain(Ws *w)
{
    hb_share_frame f;
    if (w->o)
        while (hb_share_owner_next_frame(w->o, &f)) {
            eng_frame(w, &f);
            hb_share_frame_clear(&f);
        }
    if (w->p)
        while (hb_share_peer_next_frame(w->p, &f)) {
            eng_frame(w, &f);
            hb_share_frame_clear(&f);
        }
}

static Ws *g_engs[16];
static int g_nengs;

static void pump(int ms)
{
    int64_t end = now_ms() + ms;
    int i;
    while (now_ms() < end) {
        g_main_context_iteration(NULL, FALSE);
        for (i = 0; i < g_nengs; i++) {
            eng_drain(g_engs[i]);
            if (g_engs[i]->o)
                hb_share_owner_tick(g_engs[i]->o);
            if (g_engs[i]->p)
                hb_share_peer_tick(g_engs[i]->p);
        }
        g_usleep(1500);
    }
}

static bool wait_for(bool (*fn)(void *), void *ud, int ms)
{
    int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        pump(30);
        if (fn(ud))
            return true;
    }
    return fn(ud);
}

/* ============================ 1. 登记 REST ============================ */

static void t_registry(void)
{
    Acct a = {0}, b = {0}, a2 = {0};
    Resp r;
    char *meta = b64e((uint8_t *)"sealed-meta", 11);
    g_autofree char *body = NULL, *bad = NULL;
    const char *sid = "11111111-2222-3333-4444-555555555555";
    Ws *ntf;
    int64_t t0;

    CHECK(do_register(&a, "sa@x.y", 0x61));
    CHECK(do_register(&b, "sb@x.y", 0x71));
    CHECK(do_login(&a2, "sa@x.y", "dev-a2", 0x61));

    /* shares 通知：账号 WS 挂上后 create → {"t":"shares"} <1s */
    ntf = ws_open("/v1/ws", a.token);
    CHECK(ntf->conn != NULL);
    body = g_strdup_printf(
        "{\"share_id\":\"%s\",\"meta\":\"%s\",\"guest_ttl\":300}", sid,
        meta);
    t0 = now_ms();
    r = post("/v1/shares", body, a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    while (!ws_texts_has(ntf, "\"shares\"") && now_ms() - t0 < 1500)
        pump(30);
    CHECK(ws_texts_has(ntf, "\"shares\""));
    fprintf(stderr, "  shares notify: %lld ms\n",
            (long long)(now_ms() - t0));

    /* 重复 → 409 share_exists */
    r = post("/v1/shares", body, a.token);
    CHECK_EQ_INT(r.status, 409);
    CHECK_EQ_STR(jerr(r.obj), "share_exists");
    resp_free(&r);

    /* GET：本账号看到，其它账号看不到 */
    r = get("/v1/shares", a.token);
    CHECK_EQ_INT(r.status, 200);
    {
        JsonArray *arr =
            r.obj ? json_object_get_array_member(r.obj, "shares") : NULL;
        CHECK(arr && json_array_get_length(arr) == 1);
        if (arr && json_array_get_length(arr)) {
            JsonObject *s0 = json_array_get_object_element(arr, 0);
            CHECK_EQ_STR(json_object_get_string_member(s0, "share_id"),
                         sid);
            CHECK_EQ_STR(json_object_get_string_member(s0, "meta"),
                         meta);
            CHECK_EQ_STR(
                json_object_get_string_member(s0, "owner_device"),
                a.device_id);
        }
    }
    resp_free(&r);
    r = get("/v1/shares", b.token);
    CHECK_EQ_INT(r.status, 200);
    {
        JsonArray *arr =
            r.obj ? json_object_get_array_member(r.obj, "shares") : NULL;
        CHECK(arr && json_array_get_length(arr) == 0);
    }
    resp_free(&r);

    /* PUT：owner device → 200；同账号其它设备 → 403；其它账号 → 403 */
    r = http(SOUP_METHOD_PUT, "/v1/shares/11111111-2222-3333-4444-555555555555",
             "{\"meta\":\"bmV3\"}", a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    r = http(SOUP_METHOD_PUT, "/v1/shares/11111111-2222-3333-4444-555555555555",
             "{\"meta\":\"bmV3\"}", a2.token);
    CHECK_EQ_INT(r.status, 403);
    resp_free(&r);
    r = http(SOUP_METHOD_PUT, "/v1/shares/11111111-2222-3333-4444-555555555555",
             "{\"meta\":\"bmV3\"}", b.token);
    CHECK_EQ_INT(r.status, 403);
    resp_free(&r);

    /* DELETE：非 owner → 403；owner → 200 且列表清空 */
    r = http(SOUP_METHOD_DELETE,
             "/v1/shares/11111111-2222-3333-4444-555555555555", NULL,
             a2.token);
    CHECK_EQ_INT(r.status, 403);
    resp_free(&r);
    r = http(SOUP_METHOD_DELETE,
             "/v1/shares/11111111-2222-3333-4444-555555555555", NULL,
             a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    r = get("/v1/shares", a.token);
    {
        JsonArray *arr =
            r.obj ? json_object_get_array_member(r.obj, "shares") : NULL;
        CHECK(arr && json_array_get_length(arr) == 0);
    }
    resp_free(&r);

    /* 校验：meta >64KiB / 坏 b64 / 坏 id / 坏 ttl → 400 */
    {
        char *big = malloc(90000);
        memset(big, 'A', 89999);
        big[89999] = 0;
        bad = g_strdup_printf(
            "{\"share_id\":\"%s\",\"meta\":\"%s\"}",
            "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", big);
        free(big);
    }
    r = post("/v1/shares", bad, a.token);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    r = post("/v1/shares",
             "{\"share_id\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
             "\"meta\":\"not!!b64\"}",
             a.token);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    r = post("/v1/shares", "{\"share_id\":\"bad id!\",\"meta\":\"bmV0\"}",
             a.token);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    r = post("/v1/shares",
             "{\"share_id\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
             "\"meta\":\"bmV0\",\"guest_ttl\":-5}",
             a.token);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    /* 无认证 → 401 */
    r = post("/v1/shares", body, NULL);
    CHECK_EQ_INT(r.status, 401);
    resp_free(&r);

    /* POST /guest：owner → 200 且 db 字段复位；非 owner → 403 */
    {
        const char *sid2 = "99999999-8888-7777-6666-555555555555";
        int fails = -1, locked = -1;
        sqlite3 *db = NULL;
        sqlite3_stmt *st;
        g_autofree char *path =
            g_build_filename(g_dir, "harbor-server.db", NULL);
        g_autofree char *body2 = g_strdup_printf(
            "{\"share_id\":\"%s\",\"meta\":\"%s\"}", sid2, meta);
        r = post("/v1/shares", body2, a.token);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
        /* 预置 pake_fails/locked 验证复位 */
        sqlite3_open(path, &db);
        sqlite3_prepare_v2(db,
            "UPDATE shares SET pake_fails=3,locked=1 WHERE share_id=?",
            -1, &st, NULL);
        sqlite3_bind_text(st, 1, sid2, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        sqlite3_close(db);
        r = post("/v1/shares/99999999-8888-7777-6666-555555555555/guest",
                 "{\"guest_ttl\":60}", a2.token);
        CHECK_EQ_INT(r.status, 403);
        resp_free(&r);
        r = post("/v1/shares/99999999-8888-7777-6666-555555555555/guest",
                 "{\"guest_ttl\":60}", a.token);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
        CHECK(db_share(sid2, &fails, &locked, NULL));
        CHECK_EQ_INT(fails, 0);
        CHECK_EQ_INT(locked, 0);
    }
    ws_free(ntf);
    acct_free(&a); acct_free(&b); acct_free(&a2);
    free(meta);
}

/* ============================ 2. 路由 ============================ */

static void t_routing(void)
{
    Acct a = {0}, a2 = {0};
    Resp r;
    const char *sid = "22222222-3333-4444-5555-666666666666";
    char *body;
    Ws *ow, *mw, *gw;
    int mid = -1, gid = -1;
    GBytes *f;
    gsize bn;
    const uint8_t *d;
    int64_t t0;

    CHECK(do_register(&a, "ra@x.y", 0x62));
    CHECK(do_login(&a2, "ra@x.y", "dev-mem", 0x62));
    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}", sid);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);

    {
        g_autofree char *po = g_strdup_printf("/v1/share/%s/ws?role=owner", sid);
        g_autofree char *pm = g_strdup_printf("/v1/share/%s/ws?role=member", sid);
        g_autofree char *pg = g_strdup_printf("/v1/share/%s/ws?role=guest", sid);
        ow = ws_open(po, a.token);
        CHECK(ow->conn != NULL);
        mw = ws_open(pm, a2.token);
        CHECK(mw->conn != NULL);
        gw = ws_open(pg, NULL);
        CHECK(gw->conn != NULL);
        /* hello：owner id=1，member/guest 各自的 id */
        t0 = now_ms();
        while ((!ws_last_text(ow) || !ws_last_text(mw) ||
                !ws_last_text(gw)) && now_ms() - t0 < 3000)
            pump(30);
        CHECK(ws_texts_has(ow, "\"hello\"") &&
              text_id(ws_last_text(mw)) > 1);
        mid = text_id(ws_last_text(mw));
        gid = text_id(ws_last_text(gw));
        CHECK(mid > 1 && gid > 1 && mid != gid);
        /* owner 收到两条 joined（member device 名、guest device ""） */
        while (!ws_texts_has(ow, "\"joined\"") && now_ms() - t0 < 3000)
            pump(30);
        CHECK(ws_texts_has(ow, "\"role\":\"member\""));
        CHECK(ws_texts_has(ow, "\"device\":\"dev-mem\""));
        CHECK(ws_texts_has(ow, "\"role\":\"guest\""));
        CHECK(ws_texts_has(ow, "\"device\":\"\""));

        /* member 发 INPUT（乱填 to/from）→ owner 收到 to=1 from=mid */
        f = mk_frame(0x10, 5, 99, 1, "INP", 3);
        {
            const void *pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(mw->conn, pd, bn);
        }
        g_bytes_unref(f);
        t0 = now_ms();
        while (!ow->bins->len && now_ms() - t0 < 3000) pump(30);
        CHECK(ow->bins->len == 1);
        d = bin_at(ow, 0, &bn);
        CHECK_EQ_INT(d[0], 0x10);
        CHECK_EQ_INT(be32(d + 1), 1);
        CHECK_EQ_INT(be32(d + 5), mid);

        /* member 尝试广播 to=0 → 仍改写 to=1 只发 owner；guest 收不到 */
        f = mk_frame(0x11, 0, 7, 2, NULL, 0);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(mw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        t0 = now_ms();
        while (ow->bins->len < 2 && now_ms() - t0 < 3000) pump(30);
        CHECK(ow->bins->len == 2);
        d = bin_at(ow, 1, NULL);
        CHECK_EQ_INT(be32(d + 1), 1);
        CHECK_EQ_INT(be32(d + 5), mid);
        CHECK(gw->bins->len == 0);

        /* guest 未授权：0x10 丢，0x11 丢，0x20/0x22 到 owner */
        f = mk_frame(0x10, 1, gid, 3, "X", 1);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        f = mk_frame(0x11, 1, gid, 4, NULL, 0);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        f = mk_frame(0x20, 1, gid, 5, "PAKE1DATA", 9);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        f = mk_frame(0x22, 1, gid, 6, "P3", 2);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        t0 = now_ms();
        while (ow->bins->len < 4 && now_ms() - t0 < 3000) pump(30);
        CHECK(ow->bins->len == 4);      /* 只有 PAKE1+PAKE3 */
        d = bin_at(ow, 2, NULL);
        CHECK_EQ_INT(d[0], 0x20);
        CHECK_EQ_INT(be32(d + 5), gid);
        d = bin_at(ow, 3, NULL);
        CHECK_EQ_INT(d[0], 0x22);

        /* owner 广播 0x01 → member 收、未授权 guest 不收 */
        f = mk_frame(0x01, 0, 1, 10, "SNAP", 4);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
        g_bytes_unref(f);
        /* owner 定向 0x21 → guest 收（pake_ok 前唯一可收之一） */
        f = mk_frame(0x21, gid, 1, 11, "P2", 2);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
        g_bytes_unref(f);
        /* owner 定向 0x13 → guest 不收 */
        f = mk_frame(0x13, gid, 1, 12, "G", 1);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
        g_bytes_unref(f);
        /* owner 定向 0x05 给 member → member 收 */
        f = mk_frame(0x05, mid, 1, 13, "NOTICE", 6);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
        g_bytes_unref(f);
        t0 = now_ms();
        while ((mw->bins->len < 2 || gw->bins->len < 1) &&
               now_ms() - t0 < 3000)
            pump(30);
        CHECK(mw->bins->len == 2);      /* SNAPSHOT + NOTICE */
        CHECK_EQ_INT(bin_type_at(mw, 0), 0x01);
        CHECK_EQ_INT(bin_type_at(mw, 1), 0x05);
        CHECK(gw->bins->len == 1);      /* 只有 PAKE2 */
        CHECK_EQ_INT(bin_type_at(gw, 0), 0x21);

        /* owner 文本 pake_ok → guest 授权后：广播可达、可发 0x11、
         * 0x10 仍丢 */
        {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "{\"t\":\"pake_ok\",\"id\":%d}",
                     gid);
            soup_websocket_connection_send_text(ow->conn, tmp);
        }
        f = mk_frame(0x02, 0, 1, 14, "D1", 2);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
        g_bytes_unref(f);
        /* 等 DELTA 到达 guest：证明服务端已处理 pake_ok（跨 socket
         * 无次序保证），再发 guest 帧，否则 0x11 会被当未授权丢 */
        t0 = now_ms();
        while (gw->bins->len < 2 && now_ms() - t0 < 3000) pump(30);
        f = mk_frame(0x10, 1, gid, 7, "Y", 1);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        f = mk_frame(0x11, 1, gid, 8, NULL, 0);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        t0 = now_ms();
        while ((gw->bins->len < 2 || ow->bins->len < 5) &&
               now_ms() - t0 < 3000)
            pump(30);
        CHECK_EQ_INT(gw->bins->len, 2);
        CHECK_EQ_INT(bin_type_at(gw, 1), 0x02);
        CHECK_EQ_INT(ow->bins->len, 5); /* 只有 SNAP_REQ */
        CHECK_EQ_INT(bin_type_at(ow, 4), 0x11);

        /* kick：member 收到 ended(kicked) 并关闭 */
        {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "{\"t\":\"kick\",\"id\":%d}",
                     mid);
            soup_websocket_connection_send_text(ow->conn, tmp);
        }
        t0 = now_ms();
        while (!mw->closed && now_ms() - t0 < 3000) pump(30);
        CHECK(mw->closed);
        CHECK(ws_texts_has(mw, "\"kicked\""));
        /* guest left → owner 收 left */
        soup_websocket_connection_close(gw->conn, 1000, NULL);
        t0 = now_ms();
        while (!ws_texts_has(ow, "\"left\"") && now_ms() - t0 < 3000)
            pump(30);
        CHECK(ws_texts_has(ow, "\"left\""));
    }
    ws_free(ow); ws_free(mw); ws_free(gw);
    acct_free(&a); acct_free(&a2);
}

/* ============================ 3. PAKE 锁定 ============================ */

static void t_lockout(void)
{
    Acct a = {0};
    Resp r;
    const char *sid = "33333333-4444-5555-6666-777777777777";
    char *body;
    Ws *ow, *gw, *gw2;
    g_autofree char *po = NULL, *pg = NULL;
    int fails = -1, locked = -1, gid;
    GBytes *f;
    gsize bn;
    int64_t t0;
    int i;

    CHECK(do_register(&a, "la@x.y", 0x63));
    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}", sid);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    po = g_strdup_printf("/v1/share/%s/ws?role=owner", sid);
    pg = g_strdup_printf("/v1/share/%s/ws?role=guest", sid);
    ow = ws_open(po, a.token);
    CHECK(ow->conn != NULL);

    /* 兜底计数：guest 发 PAKE1 后断开 → pake_fails=1；owner 再报
     * pake_fail 不重复计。 */
    gw = ws_open(pg, NULL);
    CHECK(gw->conn != NULL);
    t0 = now_ms();
    while (!ws_last_text(gw) && now_ms() - t0 < 3000) pump(30);
    gid = text_id(ws_last_text(gw));
    CHECK(gid > 1);
    f = mk_frame(0x20, 1, gid, 1, "P1", 2);
    {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
    g_bytes_unref(f);
    pump(300);
    soup_websocket_connection_close(gw->conn, 1000, NULL);
    t0 = now_ms();
    while (db_share(sid, &fails, NULL, NULL) && fails < 1 &&
           now_ms() - t0 < 3000)
        pump(50);
    CHECK_EQ_INT(fails, 1);
    /* owner 迟到的 pake_fail 报告 → 不双计 */
    {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "{\"t\":\"pake_fail\",\"id\":%d}", gid);
        soup_websocket_connection_send_text(ow->conn, tmp);
    }
    pump(300);
    CHECK(db_share(sid, &fails, NULL, NULL));
    CHECK_EQ_INT(fails, 1);
    ws_free(gw);

    /* 同一时刻仅 1 个未完成 PAKE：第二个 pending guest → 4009 且不计 */
    gw = ws_open(pg, NULL);
    gw2 = ws_open(pg, NULL);
    t0 = now_ms();
    while (!gw2->done || (!gw2->conn && !gw2->err)) {
        if (now_ms() - t0 > 3000) break;
        pump(30);
    }
    t0 = now_ms();
    while (gw2->conn && !gw2->closed && now_ms() - t0 < 3000)
        pump(30);
    if (gw2->conn)
        CHECK_EQ_INT(gw2->close_code, 4009);
    else
        CHECK(0);                   /* 应能升级再被 4009 关 */
    ws_free(gw2);
    ws_free(gw);                    /* 未发 PAKE1 → 不计 */
    pump(300);
    CHECK(db_share(sid, &fails, NULL, NULL));
    CHECK_EQ_INT(fails, 1);

    /* owner 报 4 次 pake_fail（对 4 个不同 guest）→ 共 5 → locked */
    for (i = 0; i < 4; i++) {
        char tmp[64];
        gw = ws_open(pg, NULL);
        CHECK(gw->conn != NULL);
        t0 = now_ms();
        while (!ws_last_text(gw) && now_ms() - t0 < 3000) pump(30);
        gid = text_id(ws_last_text(gw));
        f = mk_frame(0x20, 1, gid, 1, "P1", 2);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(gw->conn, _pd, bn);
        }
        g_bytes_unref(f);
        pump(200);
        snprintf(tmp, sizeof tmp, "{\"t\":\"pake_fail\",\"id\":%d}", gid);
        soup_websocket_connection_send_text(ow->conn, tmp);
        t0 = now_ms();
        while (!gw->closed && now_ms() - t0 < 3000) pump(30);
        CHECK(gw->closed);          /* owner 报失败 → relay 踢出 */
        ws_free(gw);
    }
    t0 = now_ms();
    while (db_share(sid, &fails, &locked, NULL) && locked < 1 &&
           now_ms() - t0 < 3000)
        pump(50);
    CHECK_EQ_INT(fails, 5);
    CHECK_EQ_INT(locked, 1);

    /* locked → 新 guest 收到 {"t":"locked"} 后关闭 */
    gw = ws_open(pg, NULL);
    CHECK(gw->conn != NULL);
    t0 = now_ms();
    while (!gw->closed && now_ms() - t0 < 3000) pump(30);
    CHECK(gw->closed);
    CHECK(ws_texts_has(gw, "\"locked\""));
    ws_free(gw);

    /* POST /guest 解锁 → guest 又能连 */
    {
        g_autofree char *gp =
            g_strdup_printf("/v1/shares/%s/guest", sid);
        r = post(gp, "{\"guest_ttl\":600}", a.token);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
    }
    CHECK(db_share(sid, &fails, &locked, NULL));
    CHECK_EQ_INT(locked, 0);
    gw = ws_open(pg, NULL);
    CHECK(gw->conn != NULL);
    t0 = now_ms();
    while (!ws_last_text(gw) && now_ms() - t0 < 3000) pump(30);
    CHECK(ws_texts_has(gw, "\"hello\""));
    ws_free(gw);
    ws_free(ow);
    acct_free(&a);
}

/* guest_expires 过期 → 拒绝新 guest。 */
static void t_guest_expired(void)
{
    Acct a = {0};
    Resp r;
    const char *sid = "44444444-5555-6666-7777-888888888888";
    char *body;
    Ws *gw;
    g_autofree char *pg = NULL;
    int64_t t0;

    CHECK(do_register(&a, "ga@x.y", 0x64));
    body = g_strdup_printf(
        "{\"share_id\":\"%s\",\"meta\":\"bmV0\",\"guest_ttl\":1}", sid);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    msleep(1200);                   /* 等 guest_expires 过 */
    pg = g_strdup_printf("/v1/share/%s/ws?role=guest", sid);
    gw = ws_open(pg, NULL);
    CHECK(gw->conn != NULL);
    t0 = now_ms();
    while (!gw->closed && now_ms() - t0 < 3000) pump(30);
    CHECK(gw->closed);
    CHECK(ws_texts_has(gw, "\"expired\""));
    ws_free(gw);
    acct_free(&a);
}

/* ============================ 4. 容量与角色 ============================ */

static void t_caps(void)
{
    Acct a = {0}, o = {0};
    Acct devs[6] = {{0}};
    Resp r;
    const char *sid = "55555555-6666-7777-8888-999999999999";
    char *body;
    Ws *ow, *ow2, *mw[6] = {0}, *bad;
    g_autofree char *po = NULL, *pm = NULL;
    int64_t t0;
    int i;

    CHECK(do_register(&a, "ca@x.y", 0x65));
    CHECK(do_register(&o, "cb@x.y", 0x75));
    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}", sid);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    po = g_strdup_printf("/v1/share/%s/ws?role=owner", sid);
    pm = g_strdup_printf("/v1/share/%s/ws?role=member", sid);

    ow = ws_open(po, a.token);
    CHECK(ow->conn != NULL);
    /* 第二个 owner socket → 拒 */
    ow2 = ws_open(po, a.token);
    CHECK(ow2->conn == NULL);
    ws_free(ow2);
    /* owner 设备以 member 角色 → 403；其它账号 member → 403 */
    bad = ws_open(pm, a.token);
    CHECK(bad->conn == NULL);
    CHECK_EQ_INT(bad->hs_status, 403);
    ws_free(bad);
    bad = ws_open(pm, o.token);
    CHECK(bad->conn == NULL);
    CHECK_EQ_INT(bad->hs_status, 403);
    ws_free(bad);
    /* 无 token member → 401 */
    bad = ws_open(pm, NULL);
    CHECK(bad->conn == NULL);
    CHECK_EQ_INT(bad->hs_status, 401);
    ws_free(bad);

    /* 5 member → OK；第 6 个 → 4008 */
    for (i = 0; i < 6; i++) {
        char nm[24];
        snprintf(nm, sizeof nm, "dev-m%d", i);
        CHECK(do_login(&devs[i], "ca@x.y", nm, 0x65));
        mw[i] = ws_open(pm, devs[i].token);
        CHECK(mw[i]->conn != NULL);
        if (i < 5) {
            t0 = now_ms();
            while (!ws_last_text(mw[i]) && now_ms() - t0 < 3000)
                pump(30);
            CHECK(ws_texts_has(mw[i], "\"hello\""));
        }
    }
    t0 = now_ms();
    while (!mw[5]->closed && now_ms() - t0 < 3000)
        pump(30);
    CHECK(mw[5]->closed);
    CHECK_EQ_INT(mw[5]->close_code, 4008);

    for (i = 0; i < 6; i++) {
        ws_free(mw[i]);
        acct_free(&devs[i]);
    }
    ws_free(ow);
    acct_free(&a); acct_free(&o);
}

/* ============================ 5. 生命周期 ============================ */

static void t_lifecycle(void)
{
    Acct a = {0}, a2 = {0};
    Resp r;
    const char *sid = "66666666-7777-8888-9999-aaaaaaaaaaaa";
    const char *sid2 = "77777777-8888-9999-aaaa-bbbbbbbbbbbb";
    char *body;
    Ws *ow, *mw, *gw;
    g_autofree char *po = NULL, *pm = NULL, *pg = NULL, *dp = NULL;
    int64_t t0;
    int ended = -1;

    CHECK(do_register(&a, "la2@x.y", 0x66));
    CHECK(do_login(&a2, "la2@x.y", "dev-mm", 0x66));
    po = g_strdup_printf("/v1/share/%s/ws?role=owner", sid);
    pm = g_strdup_printf("/v1/share/%s/ws?role=member", sid);
    pg = g_strdup_printf("/v1/share/%s/ws?role=guest", sid);
    dp = g_strdup_printf("/v1/shares/%s", sid);

    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}", sid);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    ow = ws_open(po, a.token);
    mw = ws_open(pm, a2.token);
    gw = ws_open(pg, NULL);
    CHECK(ow->conn && mw->conn && gw->conn);
    pump(500);

    /* DELETE → 全员 ended(stopped) + 3 s 内关闭 */
    t0 = now_ms();
    r = http(SOUP_METHOD_DELETE, dp, NULL, a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    while ((!mw->closed || !gw->closed) && now_ms() - t0 < 4000)
        pump(30);
    CHECK(mw->closed && gw->closed);
    CHECK(now_ms() - t0 < 3000);
    fprintf(stderr, "  delete→closed: %lld ms\n",
            (long long)(now_ms() - t0));
    CHECK(ws_texts_has(mw, "\"stopped\""));
    CHECK(ws_texts_has(gw, "\"stopped\""));
    ws_free(ow); ws_free(mw); ws_free(gw);

    /* owner socket 关闭 → ended(owner_gone) <1s */
    r = post("/v1/shares", body =
             g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}",
                             sid2),
             a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    {
        g_autofree char *po2 =
            g_strdup_printf("/v1/share/%s/ws?role=owner", sid2);
        g_autofree char *pm2 =
            g_strdup_printf("/v1/share/%s/ws?role=member", sid2);
        ow = ws_open(po2, a.token);
        mw = ws_open(pm2, a2.token);
        CHECK(ow->conn && mw->conn);
        pump(300);
        t0 = now_ms();
        soup_websocket_connection_close(ow->conn, 1000, NULL);
        while (!mw->closed && now_ms() - t0 < 3000)
            pump(20);
        CHECK(mw->closed);
        CHECK(ws_texts_has(mw, "\"owner_gone\""));
        CHECK(now_ms() - t0 < 1000);
        fprintf(stderr, "  owner close→member ended: %lld ms\n",
                (long long)(now_ms() - t0));
        ws_free(ow); ws_free(mw);
    }

    /* owner 假死（裸 socket 完成握手后不处理任何帧，等价 SIGSTOP：
     * TCP 保持但无应用层 pong）→ ~10-12 s ended */
    {
        g_autofree char *po3 = g_strdup_printf(
            "/v1/share/%s/ws?role=owner",
            "eeeeeeee-ffff-0000-1111-222222222222");
        g_autofree char *pm3 = g_strdup_printf(
            "/v1/share/%s/ws?role=member",
            "eeeeeeee-ffff-0000-1111-222222222222");
        int rfd;
        r = post("/v1/shares", body = g_strdup_printf(
                     "{\"share_id\":\"%s\",\"meta\":\"bmV0\"}",
                     "eeeeeeee-ffff-0000-1111-222222222222"),
                 a.token);
        g_free(body);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
        rfd = raw_ws(po3, a.token);
        CHECK(rfd >= 0);
        mw = ws_open(pm3, a2.token);
        CHECK(mw->conn != NULL);
        pump(300);
        t0 = now_ms();
        while (!mw->closed && now_ms() - t0 < 16000)
            pump(50);
        CHECK(mw->closed);
        CHECK(ws_texts_has(mw, "\"owner_gone\""));
        fprintf(stderr, "  dead owner→ended: %lld ms\n",
                (long long)(now_ms() - t0));
        CHECK(now_ms() - t0 >= 9000 && now_ms() - t0 <= 14000);
        close(rfd);
        ws_free(mw);
    }
    /* 重启 → 进行中共享全部 ended */
    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}",
                           "dddddddd-eeee-ffff-0000-111111111111");
    r = post("/v1/shares", body, a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    stop_server();
    {
        char *dir = g_strdup(g_dir);
        start_server(dir);
        g_free(dir);
    }
    CHECK(db_share("dddddddd-eeee-ffff-0000-111111111111", NULL, NULL,
                   &ended));
    CHECK_EQ_INT(ended, 1);
    r = get("/v1/shares", a.token);
    {
        JsonArray *arr =
            r.obj ? json_object_get_array_member(r.obj, "shares") : NULL;
        CHECK(arr && json_array_get_length(arr) == 0);
    }
    resp_free(&r);
    acct_free(&a); acct_free(&a2);       /* ow 已在前述块释放 */
}

/* ============================ 6. 背压 ============================ */

static void t_backpressure(void)
{
    Acct a = {0}, a2 = {0}, a3 = {0};
    Resp r;
    const char *sid = "88888888-9999-aaaa-bbbb-cccccccccccc";
    char *body;
    Ws *ow, *m2;
    g_autofree char *po = NULL, *pm = NULL;
    int rfd = -1;
    int i;
    const int NSEND = 40;           /* 40 × 256 KiB = 10 MiB DELTA */
    uint8_t *pl;
    GBytes *f;
    gsize bn;
    uint8_t *rbuf = NULL;
    size_t rlen = 0, rcap = 0;
    int deltas = 0, snaps = 0;
    int64_t t0;

    CHECK(do_register(&a, "ba@x.y", 0x67));
    CHECK(do_login(&a2, "ba@x.y", "dev-slow", 0x67));
    CHECK(do_login(&a3, "ba@x.y", "dev-fast", 0x67));
    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"bmV0\"}", sid);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    po = g_strdup_printf("/v1/share/%s/ws?role=owner", sid);
    pm = g_strdup_printf("/v1/share/%s/ws?role=member", sid);
    ow = ws_open(po, a.token);
    CHECK(ow->conn);
    /* 慢 member：裸 socket 握手后永不读 */
    rfd = raw_ws(pm, a2.token);
    CHECK(rfd >= 0);
    /* 正常 member 作对照 */
    m2 = ws_open(pm, a3.token);
    CHECK(m2->conn);
    pump(500);

    pl = malloc(256 << 10);
    memset(pl, 'D', 256 << 10);
    t0 = now_ms();
    for (i = 0; i < NSEND; i++) {
        f = mk_frame(0x02, 0, 1, i + 1, pl, 256 << 10);
        {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
        g_bytes_unref(f);
        pump(5);
    }
    /* 慢 member 的服务端队列此时应已触发 DELTA 丢弃；发 SNAPSHOT */
    f = mk_frame(0x01, 0, 1, 999, "BP-SNAPSHOT-MARK", 16);
    {
            const void *_pd = g_bytes_get_data(f, &bn);
            soup_websocket_connection_send_binary(ow->conn, _pd, bn);
        }
    g_bytes_unref(f);
    pump(800);

    /* 慢 member 开始读：应收到部分 DELTA + 最后的 SNAPSHOT。
     * 裸 socket 不应答 ping，~10 s 会被 liveness 断开 — 必须在
     * 其存活窗口内读，且读间穿插 pump 维持本进程 ws 心跳。 */
    {
        int fl = fcntl(rfd, F_GETFL, 0);
        int64_t dead = now_ms() + 4000;
        fcntl(rfd, F_SETFL, fl | O_NONBLOCK);
        while (now_ms() < dead) {
            uint8_t tmp[65536];
            ssize_t n;
            for (;;) {
                n = read(rfd, tmp, sizeof tmp);
                if (n <= 0)
                    break;
                if (rlen + (size_t)n > rcap) {
                    rcap = (rlen + n) * 2 + 4096;
                    rbuf = realloc(rbuf, rcap);
                }
                memcpy(rbuf + rlen, tmp, n);
                rlen += n;
            }
            pump(20);
        }
    }
    /* 正常 member 应收到全部 DELTA + SNAPSHOT（不受慢者影响） */
    t0 = now_ms();
    while (m2->bins->len < (guint)NSEND + 1 && now_ms() - t0 < 30000)
        pump(50);
    CHECK_EQ_INT(m2->bins->len, NSEND + 1);
    /* 解析帧统计 */
    {
        size_t off = 0;
        while (off < rlen) {
            uint8_t op;
            const uint8_t *fr;
            size_t flen, u = ws_parse(rbuf + off, rlen - off, &op, &fr,
                                      &flen);
            if (!u)
                break;
            if (op == 2 && flen >= 17) {
                if (fr[0] == 0x02)
                    deltas++;
                else if (fr[0] == 0x01 &&
                         flen >= 17 + 16 &&
                         !memcmp(fr + 17, "BP-SNAPSHOT-MARK", 16))
                    snaps++;
            }
            off += u;
        }
    }
    CHECK(deltas < NSEND);          /* 有 DELTA 被丢 */
    CHECK(snaps == 1);              /* SNAPSHOT 完整送达 */
    fprintf(stderr, "  backpressure: sent %d deltas, slow got %d\n",
            NSEND, deltas);
    close(rfd);
    free(pl);
    free(rbuf);
    ws_free(ow);
    ws_free(m2);
    acct_free(&a); acct_free(&a2); acct_free(&a3);
}

/* ============================ 7. 端到端（真实引擎×真实 relay） ======== */

static int vt_has(hb_vt *v, const char *s)
{
    int rows, cols, r, found = 0;
    hb_vt_lock(v);
    hb_vt_size(v, &rows, &cols);
    for (r = -hb_vt_scrollback_count(v); r < rows && !found; r++) {
        char *ln = hb_vt_get_text(v, r, 0, r, cols, false);
        if (ln && strstr(ln, s))
            found = 1;
        free(ln);
    }
    hb_vt_unlock(v);
    return found;
}

struct has_ctx { hb_vt *v; const char *s; };
static bool has_cb(void *ud) { return vt_has(((struct has_ctx *)ud)->v,
                                            ((struct has_ctx *)ud)->s)
                                       != 0; }

static bool file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    bool found = false;
    size_t nl = strlen(needle), i;
    if (!f) return false;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return false; }
    fclose(f);
    for (i = 0; nl && i + nl <= (size_t)n; i++)
        if (!memcmp(b + i, needle, nl)) { found = true; break; }
    free(b);
    return found;
}

static void t_e2e(void)
{
    Acct a = {0}, a2 = {0};
    Resp r;
    hb_pty_opts po = { .shell = "/bin/sh", .rows = 10, .cols = 60 };
    hb_session *sess;
    hb_share_owner *o;
    hb_share_peer *m, *g;
    hb_err e = HB_OK;
    uint8_t vk[32];
    char *meta, *link, *body;
    const char *sid = "99999999-aaaa-bbbb-cccc-dddddddddddd";
    Ws *ow, *mw, *gw;
    g_autofree char *ourl = NULL, *murl = NULL, *gurl = NULL;
    g_autofree char *spath = NULL;
    char *meta_b64 = NULL;
    int64_t t0;
    int i;
    struct has_ctx hc;

    randombytes_buf(vk, 32);
    CHECK(do_register(&a, "ea@x.y", 0x68));
    CHECK(do_login(&a2, "ea@x.y", "dev-e2e-m", 0x68));

    sess = hb_session_new_local(&po, 500);
    CHECK(sess != NULL);
    if (!sess) return;
    o = hb_share_owner_new(sid, vk, HB_SHARE_COLLAB);
    hb_share_owner_attach(o, sess);
    meta = hb_share_owner_meta(o, "E2E-SECRET-TITLE", "sec-host-x",
                               "devA");
    link = hb_share_owner_link(o, "http://127.0.0.1:9");
    CHECK(meta && link);

    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"%s\"}", sid,
                           meta);
    r = post("/v1/shares", body, a.token);
    g_free(body);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    ourl = g_strdup_printf("/v1/share/%s/ws?role=owner", sid);
    murl = g_strdup_printf("/v1/share/%s/ws?role=member", sid);
    gurl = g_strdup_printf("/v1/share/%s/ws?role=guest", sid);

    ow = ws_open(ourl, a.token);
    CHECK(ow->conn);
    ow->o = o;
    g_engs[g_nengs++] = ow;
    pump(300);
    CHECK_EQ_INT(text_id(ws_last_text(ow)), 1);

    /* member：GET /shares 取 meta → 构造引擎 → ws */
    r = get("/v1/shares", a2.token);
    CHECK_EQ_INT(r.status, 200);
    {
        JsonArray *arr =
            r.obj ? json_object_get_array_member(r.obj, "shares") : NULL;
        CHECK(arr && json_array_get_length(arr) == 1);
        meta_b64 = g_strdup(json_object_get_string_member(
            json_array_get_object_element(arr, 0), "meta"));
    }
    resp_free(&r);
    m = hb_share_peer_new_member(sid, vk, meta_b64, "devB", "memberB",
                                 &e);
    CHECK(m && e == HB_OK);
    mw = ws_open(murl, a2.token);
    CHECK(mw->conn);
    mw->p = m;
    g_engs[g_nengs++] = mw;

    /* guest：link+code → PAKE */
    g = hb_share_peer_new_guest(link, hb_share_owner_code(o), &e);
    CHECK(g && e == HB_OK);
    gw = ws_open(gurl, NULL);
    CHECK(gw->conn);
    gw->p = g;
    g_engs[g_nengs++] = gw;

    /* 等两边拿到快照（owner 输出行 "READY-E2E" 广播） */
    hb_session_write(sess, "echo READY-E2E\n", 15);
    hc.v = hb_share_peer_vt(m); hc.s = "READY-E2E";
    CHECK(wait_for(has_cb, &hc, 15000));
    hc.v = hb_share_peer_vt(g);
    CHECK(wait_for(has_cb, &hc, 15000));

    /* member 协作输入 → owner 与两个 peer 都看到 */
    CHECK_EQ_INT(hb_share_peer_input(m, "echo MK-INPUT-7\n",
                                     strlen("echo MK-INPUT-7\n")),
                 HB_OK);
    hc.v = hb_share_peer_vt(g); hc.s = "MK-INPUT-7";
    CHECK(wait_for(has_cb, &hc, 15000));
    hc.v = hb_share_peer_vt(m);
    CHECK(wait_for(has_cb, &hc, 15000));
    hc.v = hb_session_vt(sess);
    CHECK(wait_for(has_cb, &hc, 5000));

    /* guest 输入被拒（只读） */
    CHECK_EQ_INT(hb_share_peer_input(g, "x", 1), HB_ERR_INVALID);

    /* 5 次错误配对码 → locked */
    for (i = 0; i < 5; i++) {
        hb_share_peer *wg;
        Ws *w;
        hb_share_event ev;
        bool saw_fail = false;
        const char *wc =
            !strcmp(hb_share_owner_code(o), "000000") ? "000001"
                                                      : "000000";
        wg = hb_share_peer_new_guest(link, wc, &e);
        CHECK(wg && e == HB_OK);
        w = ws_open(gurl, NULL);
        CHECK(w->conn);
        w->p = wg;
        g_engs[g_nengs++] = w;
        /* 等 guest 侧 tag_o 校验失败事件或 relay 踢出 */
        t0 = now_ms();
        while (!saw_fail && !w->closed && now_ms() - t0 < 8000) {
            pump(60);
            while (hb_share_peer_next_event(wg, &ev)) {
                if (ev.type == HB_SH_EV_PAKE_FAIL)
                    saw_fail = true;
                hb_share_event_clear(&ev);
            }
        }
        CHECK(saw_fail || w->closed);
        /* guest 断开（wrong code 中止）→ relay/owner 计一次失败 */
        if (w->conn && !w->closed)
            soup_websocket_connection_close(w->conn, 1000, NULL);
        pump(300);
        g_nengs--;
        w->p = NULL;
        ws_free(w);
        hb_share_peer_free(wg);
    }
    {
        int fails = -1, locked = -1;
        t0 = now_ms();
        while (db_share(sid, &fails, &locked, NULL) && locked < 1 &&
               now_ms() - t0 < 5000)
            pump(100);
        CHECK_EQ_INT(fails, 5);
        CHECK_EQ_INT(locked, 1);
    }
    /* 第 6 个 guest → locked 文本 + 关闭 */
    {
        Ws *w = ws_open(gurl, NULL);
        CHECK(w->conn);
        t0 = now_ms();
        while (!w->closed && now_ms() - t0 < 3000) pump(30);
        CHECK(w->closed);
        CHECK(ws_texts_has(w, "\"locked\""));
        ws_free(w);
    }

    /* owner stop → 两端 ended */
    spath = g_strdup_printf("/v1/shares/%s", sid);
    r = http(SOUP_METHOD_DELETE, spath, NULL, a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    {
        bool me = false, ge = false;
        hb_share_event ev;
        t0 = now_ms();
        while (!(me && ge) && now_ms() - t0 < 5000) {
            pump(50);
            while (hb_share_peer_next_event(m, &ev)) {
                if (ev.type == HB_SH_EV_ENDED) me = true;
                hb_share_event_clear(&ev);
            }
            while (hb_share_peer_next_event(g, &ev)) {
                if (ev.type == HB_SH_EV_ENDED) ge = true;
                hb_share_event_clear(&ev);
            }
        }
        CHECK(me && ge);
    }

    /* 零知识：dump 的每一帧无命令/输出明文；db meta 无明文 */
    CHECK(!file_has(g_dump, "READY-E2E"));
    CHECK(!file_has(g_dump, "MK-INPUT-7"));
    CHECK(!file_has(g_dump, "E2E-SECRET-TITLE"));
    CHECK(!file_has(g_dump, "sec-host-x"));
    {
        g_autofree char *dbp =
            g_build_filename(g_dir, "harbor-server.db", NULL);
        CHECK(!file_has(dbp, "E2E-SECRET-TITLE"));
        CHECK(!file_has(dbp, "sec-host-x"));
    }

    g_nengs -= 3;
    ow->o = NULL; mw->p = NULL; gw->p = NULL;
    ws_free(ow); ws_free(mw); ws_free(gw);
    hb_share_peer_free(m);
    hb_share_peer_free(g);
    hb_share_owner_free(o);
    hb_session_free(sess);
    free(meta); free(link); free(meta_b64);
    acct_free(&a); acct_free(&a2);
}

/* ============================ main ============================ */

static const char *g_only;

#define RUN(fn) do { if (!g_only || !strcmp(g_only, #fn)) fn(); } while (0)

int main(int argc, char **argv)
{
    char dir[] = "/tmp/hbsrv-share-XXXXXX";
    CHECK(sodium_init() >= 0);
    g_sess = soup_session_new();
    g_dump = strdup("/tmp/hbsrv-frames-dump.bin");
    unlink(g_dump);
    if (!mkdtemp(dir)) { CHECK(0); return 1; }
    g_only = argc > 1 ? argv[1] : NULL;
    start_server(dir);

    RUN(t_registry);
    RUN(t_routing);
    RUN(t_lockout);
    RUN(t_guest_expired);
    RUN(t_caps);
    RUN(t_lifecycle);
    RUN(t_backpressure);
    RUN(t_e2e);

    stop_server();
    printf("server-share: %d checks, %d failures\n", t_checks,
           t_failures);
    return t_failures ? 1 : 0;
}
