/* harbor-server 协议测试：真实 spawn 服务端进程，libsoup 客户端驱动。
 * 覆盖 docs/api/sync-protocol.md §6：账号流、CAS、限流/锁定、TOTP、
 * 设备吊销、WebSocket 通知、恢复、零知识审计、输入校验。 */

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <sodium.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
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

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static uint8_t *b64d(const char *s, size_t *n_out)
{
    size_t n = strlen(s), outn = (n / 4) * 3, i;
    uint8_t *o;
    if (n % 4 == 2) outn += 1;
    else if (n % 4 == 3) outn += 2;
    o = malloc(outn);
    for (i = 0; i < n; i += 4) {
        int v0 = b64v(s[i]), v1 = i + 1 < n ? b64v(s[i + 1]) : 0;
        int v2 = i + 2 < n ? b64v(s[i + 2]) : 0;
        int v3 = i + 3 < n ? b64v(s[i + 3]) : 0;
        uint32_t w = v0 << 18 | v1 << 12 | v2 << 6 | v3;
        size_t oi = i / 4 * 3;
        if (oi < outn) o[oi] = w >> 16;
        if (oi + 1 < outn) o[oi + 1] = w >> 8;
        if (oi + 2 < outn) o[oi + 2] = w;
    }
    *n_out = outn;
    return o;
}

static int b32v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

static uint8_t *b32d(const char *s, size_t *n_out)
{
    size_t n = strlen(s), cap = n * 5 / 8 + 1;
    uint8_t *o = calloc(cap, 1);
    uint64_t acc = 0;
    int bits = 0;
    size_t oi = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b32v(s[i]);
        if (v < 0) { free(o); return NULL; }
        acc = (acc << 5) | v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            o[oi++] = (acc >> bits) & 0xff;
        }
    }
    *n_out = oi;
    return o;
}

/* 独立实现 SHA1+HMAC+RFC 6238（6 位、30 s）——不用服务端代码。
 * libsodium 无 SHA1；服务端用 GHmac，此处完全独立。 */
static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 80; i++) {
        uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = x << 1 | x >> 31;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;          k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);
                         k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;          k = 0xca62c1d6; }
        t = (a << 5 | a >> 27) + f + e + k + w[i];
        e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *data, size_t n, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
                      0xc3d2e1f0 };
    uint64_t bits = (uint64_t)n * 8;
    uint8_t tail[128];
    size_t tn;
    int i;
    while (n >= 64) { sha1_block(h, data); data += 64; n -= 64; }
    memcpy(tail, data, n);
    tail[n] = 0x80;
    tn = n + 1;
    if (tn % 64 > 56) tn += 64 - tn % 64;
    while (tn % 64 != 56) tail[tn++] = 0;
    for (i = 0; i < 8; i++) tail[tn++] = (uint8_t)(bits >> ((7 - i) * 8));
    for (size_t off = 0; off < tn; off += 64) sha1_block(h, tail + off);
    for (i = 0; i < 5; i++) {
        out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16;
        out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
}

static void hmac_sha1(const uint8_t *key, size_t kn, const uint8_t *msg,
                      size_t mn, uint8_t out[20])
{
    uint8_t k[64] = {0}, buf[64 + 64], inner[20], outer_in[84];
    size_t i;
    if (kn > 64) {
        sha1(key, kn, k);
    } else {
        memcpy(k, key, kn);
    }
    for (i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;
    memcpy(buf + 64, msg, mn);
    sha1(buf, 64 + mn, inner);
    for (i = 0; i < 64; i++) outer_in[i] = k[i] ^ 0x5c;
    memcpy(outer_in + 64, inner, 20);
    sha1(outer_in, 84, out);
}

static uint32_t totp_sha1(const uint8_t *sec, size_t n, int64_t step)
{
    uint8_t ctr[8], mac[20];
    int off;
    uint32_t code;
    for (int i = 7; i >= 0; i--) ctr[i] = (uint8_t)(step >> ((7 - i) * 8));
    hmac_sha1(sec, n, ctr, 8, mac);
    off = mac[19] & 0xf;
    code = ((uint32_t)(mac[off] & 0x7f) << 24) |
           ((uint32_t)mac[off + 1] << 16) |
           ((uint32_t)mac[off + 2] << 8) | mac[off + 3];
    return code % 1000000;
}

static int64_t now_ms(void) { return g_get_real_time() / 1000; }
static void msleep(int ms) { g_usleep(ms * 1000); }

/* ============================ http client ============================ */

static SoupSession *g_sess;
static char *g_base;
static GPid g_pid = -1;
static char *g_dir;

typedef struct {
    guint       status;
    char       *body;      /* 原始响应文本（调用方 free） */
    JsonObject *obj;       /* 解析出的 JSON object 或 NULL（调用方 unref） */
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

static int64_t jint(JsonObject *o, const char *k, int64_t fb)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_INT64)
               ? json_node_get_int(n)
               : fb;
}

static bool jbool(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_BOOLEAN) &&
           json_node_get_boolean(n);
}

static const char *jerr(JsonObject *o) { return jstr(o, "error"); }

/* 随机循环端口 */
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

static void start_server(void)
{
    int port = free_port();
    char **env, *listen_a, **argv;
    GError *err = NULL;
    char dir[] = "/tmp/hbsrv-test-XXXXXX";
    CHECK(port > 0);
    if (!mkdtemp(dir)) { CHECK(0); return; }
    g_dir = strdup(dir);
    g_base = g_strdup_printf("http://127.0.0.1:%d", port);
    listen_a = g_strdup_printf("127.0.0.1:%d", port);
    env = g_environ_setenv(g_get_environ(), "HARBOR_SERVER_TEST_LOCK_MS",
                           "200", TRUE);
    env = g_environ_setenv(env, "HARBOR_SERVER_TEST_IP_LIMIT", "10000",
                           TRUE);
    env = g_environ_setenv(env, "G_MESSAGES_DEBUG", "", TRUE);
    argv = g_new0(char *, 12);
    argv[0] = (char *)SERVER_BIN;
    argv[1] = "--listen";
    argv[2] = listen_a;
    argv[3] = "--data";
    argv[4] = g_dir;
    argv[5] = "--log-level";
    argv[6] = "debug";
    argv[7] = "--hash-threads";
    argv[8] = "2";
    argv[9] = "--hash-queue";
    argv[10] = "2";
    if (!g_spawn_async(NULL, argv, env, G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &g_pid, &err)) {
        fprintf(stderr, "spawn: %s\n", err->message);
        CHECK(0);
    }
    /* 等待端口就绪 */
    for (int i = 0; i < 100; i++) {
        Resp r = post("/v1/prelogin", "{\"email\":\"w@u.p\"}", NULL);
        if (r.status) {
            resp_free(&r);
            break;
        }
        msleep(50);
    }
    g_free(listen_a);
    g_free(argv);          /* 元素非 g_strdup 全部归属外部，只释放数组 */
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

/* ============================ 注册/登录 helper ============================ */

typedef struct {
    char *account_id, *device_id, *token;
    uint8_t auth_key[32], rc_auth[32];
    char *kdf_salt;
    char *email;
} Acct;

static void acct_free(Acct *a)
{
    free(a->account_id); free(a->device_id); free(a->token);
    free(a->kdf_salt); free(a->email);
}

/* 用固定字节当 auth_key/rc_auth（零知识测试能认出来） */
static bool do_register(Acct *a, const char *email, uint8_t seed)
{
    Resp r;
    char *body, *salt;
    memset(a->auth_key, seed, 32);
    memset(a->rc_auth, seed + 1, 32);
    {
        uint8_t s[16];
        memset(s, seed + 2, 16);
        salt = b64e(s, 16);
    }
    {
        char *ak = b64e(a->auth_key, 32), *ra = b64e(a->rc_auth, 32);
        char *wp = b64e((uint8_t *)"wrapped-pw", 10);
        char *wr = b64e((uint8_t *)"wrapped-rc", 10);
        body = g_strdup_printf(
            "{\"email\":\"%s\",\"auth_key\":\"%s\",\"rc_auth\":\"%s\","
            "\"kdf\":{\"salt\":\"%s\",\"ops\":2,\"mem\":67108864},"
            "\"wrapped_vk_pw\":\"%s\",\"wrapped_vk_rc\":\"%s\","
            "\"device\":{\"name\":\"dev-%d\",\"platform\":\"linux\"}}",
            email, ak, ra, salt, wp, wr, seed);
        free(ak); free(ra); free(wp); free(wr);
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
    a->account_id = strdup(jstr(r.obj, "account_id"));
    a->device_id = strdup(jstr(r.obj, "device_id"));
    a->token = strdup(jstr(r.obj, "token"));
    a->email = strdup(email);
    resp_free(&r);
    return a->account_id && a->token;
}

static Resp do_login(Acct *a, const char *totp)
{
    char *ak = b64e(a->auth_key, 32);
    char *body = g_strdup_printf(
        "{\"email\":\"%s\",\"auth_key\":\"%s\",\"device\":{\"name\":\"l\","
        "\"platform\":\"linux\"}%s%s%s}",
        a->email, ak, totp ? ",\"totp\":\"" : "", totp ? totp : "",
        totp ? "\"" : "");
    Resp r = post("/v1/login", body, NULL);
    free(ak);
    g_free(body);
    return r;
}

/* ============================ tests ============================ */

/* 1. register→login→objects 往返；未知邮箱 prelogin 伪造参数稳定 */
static void t_basic(void)
{
    Acct a = {0};
    Resp r1, r2;
    CHECK(do_register(&a, "u1@x.y", 0x11));

    /* 未知邮箱 prelogin：两次同 salt，形状与真的一致 */
    r1 = post("/v1/prelogin", "{\"email\":\"ghost@x.y\"}", NULL);
    msleep(20);
    r2 = post("/v1/prelogin", "{\"email\":\"ghost@x.y\"}", NULL);
    CHECK_EQ_INT(r1.status, 200);
    {
        JsonObject *k1 = json_object_get_object_member(r1.obj, "kdf");
        JsonObject *k2 = json_object_get_object_member(r2.obj, "kdf");
        CHECK_EQ_STR(jstr(k1, "salt"), jstr(k2, "salt"));
        CHECK(jstr(k1, "salt") != NULL);
        CHECK(jint(k1, "ops", -1) > 0 && jint(k1, "mem", -1) > 0);
        size_t sn;
        uint8_t *sd = b64d(jstr(k1, "salt"), &sn);
        CHECK_EQ_INT(sn, 16);
        free(sd);
    }
    resp_free(&r1);
    resp_free(&r2);

    /* 已注册邮箱 prelogin → 注册时的 salt */
    r1 = post("/v1/prelogin", "{\"email\":\"u1@x.y\"}", NULL);
    CHECK_EQ_INT(r1.status, 200);
    resp_free(&r1);

    /* objects push + pull */
    {
        char *b1 = b64e((uint8_t *)"cipher-1", 8);
        char *b2 = b64e((uint8_t *)"cipher-2", 8);
        char *body = g_strdup_printf(
            "{\"changes\":[{\"id\":\"h-1\",\"type\":\"host\",\"base_seq\":0,"
            "\"blob\":\"%s\"},{\"id\":\"k-1\",\"type\":\"key\",\"base_seq\":0,"
            "\"blob\":\"%s\"}]}",
            b1, b2);
        Resp r = post("/v1/objects", body, a.token);
        CHECK_EQ_INT(r.status, 200);
        {
            JsonArray *res = json_object_get_array_member(r.obj, "results");
            CHECK_EQ_INT(json_array_get_length(res), 2);
            CHECK_EQ_INT(
                json_object_get_int_member(
                    json_array_get_object_element(res, 0), "seq"),
                1);
            CHECK_EQ_INT(
                json_object_get_int_member(
                    json_array_get_object_element(res, 1), "seq"),
                2);
        }
        resp_free(&r);
        free(b1); free(b2); g_free(body);
    }
    {
        Resp r = get("/v1/objects?since=0&limit=500", a.token);
        CHECK_EQ_INT(r.status, 200);
        JsonArray *os = json_object_get_array_member(r.obj, "objects");
        CHECK_EQ_INT(json_array_get_length(os), 2);
        CHECK(!jbool(r.obj, "more"));
        CHECK_EQ_INT(jint(r.obj, "next", -1), 2);
        resp_free(&r);
        r = get("/v1/objects?since=1&limit=1", a.token);
        os = json_object_get_array_member(r.obj, "objects");
        CHECK_EQ_INT(json_array_get_length(os), 1);
        resp_free(&r);
    }
    /* 登录拿 wrapped_vk_pw */
    {
        Resp r = do_login(&a, NULL);
        CHECK_EQ_INT(r.status, 200);
        CHECK(jstr(r.obj, "wrapped_vk_pw") != NULL);
        CHECK(jstr(r.obj, "token") != NULL);
        CHECK(jbool(r.obj, "totp_enabled") == false);
        resp_free(&r);
    }
    acct_free(&a);
}

/* 2. CAS 语义 + seq 递增 + 账号隔离 */
static void t_cas(void)
{
    Acct a = {0}, b = {0};
    char *blob = b64e((uint8_t *)"v1-blob", 7);
    char *blob2 = b64e((uint8_t *)"v2-blob", 7);
    CHECK(do_register(&a, "cas-a@x.y", 0x21));
    CHECK(do_register(&b, "cas-b@x.y", 0x31));
    {
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"o-1\",\"type\":\"host\",\"base_seq\":0,"
            "\"blob\":\"%s\"}]}",
            blob);
        Resp r = post("/v1/objects", bd, a.token);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
        g_free(bd);
    }
    /* stale base_seq → conflict 带当前 blob+seq+type */
    {
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"o-1\",\"type\":\"host\",\"base_seq\":0,"
            "\"blob\":\"%s\"}]}",
            blob2);
        Resp r = post("/v1/objects", bd, a.token);
        CHECK_EQ_INT(r.status, 200);
        JsonObject *res =
            json_array_get_object_element(
                json_object_get_array_member(r.obj, "results"), 0);
        CHECK(!jbool(res, "ok"));
        JsonObject *cf = json_object_get_object_member(res, "conflict");
        CHECK_EQ_INT(jint(cf, "seq", -1), 1);
        CHECK_EQ_STR(jstr(cf, "type"), "host");
        CHECK(jstr(cf, "blob") != NULL);
        resp_free(&r);
        g_free(bd);
    }
    /* 正确 base_seq=1 → seq=2 递增 */
    {
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"o-1\",\"type\":\"host\",\"base_seq\":1,"
            "\"blob\":\"%s\"}]}",
            blob2);
        Resp r = post("/v1/objects", bd, a.token);
        JsonObject *res =
            json_array_get_object_element(
                json_object_get_array_member(r.obj, "results"), 0);
        CHECK(jbool(res, "ok"));
        CHECK_EQ_INT(jint(res, "seq", -1), 2);
        resp_free(&r);
        g_free(bd);
    }
    /* 新 id + base_seq≠0 → conflict（对象不存在，seq:0） */
    {
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"o-x\",\"type\":\"host\",\"base_seq\":7,"
            "\"blob\":\"%s\"}]}",
            blob2);
        Resp r = post("/v1/objects", bd, a.token);
        JsonObject *res =
            json_array_get_object_element(
                json_object_get_array_member(r.obj, "results"), 0);
        CHECK(!jbool(res, "ok"));
        CHECK_EQ_INT(
            jint(json_object_get_object_member(res, "conflict"), "seq", -1),
            0);
        resp_free(&r);
        g_free(bd);
    }
    /* 账号隔离：B 看不到 A 的对象 */
    {
        Resp r = get("/v1/objects?since=0&limit=500", b.token);
        JsonArray *os = json_object_get_array_member(r.obj, "objects");
        CHECK_EQ_INT(json_array_get_length(os), 0);
        resp_free(&r);
    }
    free(blob); free(blob2);
    acct_free(&a);
    acct_free(&b);
}

/* 3. 错误 auth_key 不泄露数据；5 次失败锁定；锁内正确密码也 429 */
static void t_lockout(void)
{
    Acct a = {0};
    CHECK(do_register(&a, "lock@x.y", 0x41));
    /* 错误 auth_key → 401 invalid_credentials，响应无任何 vault 数据 */
    {
        uint8_t bad[32];
        char *bad64, *body;
        memset(bad, 0x99, 32);
        bad64 = b64e(bad, 32);
        body = g_strdup_printf(
            "{\"email\":\"%s\",\"auth_key\":\"%s\","
            "\"device\":{\"name\":\"x\",\"platform\":\"y\"}}",
            a.email, bad64);
        Resp r = post("/v1/login", body, NULL);
        CHECK_EQ_INT(r.status, 401);
        CHECK_EQ_STR(jerr(r.obj), "invalid_credentials");
        CHECK(jstr(r.obj, "wrapped_vk_pw") == NULL);
        CHECK(jstr(r.obj, "token") == NULL);
        CHECK(!strstr(r.body, "wrapped_vk"));
        resp_free(&r);
        /* 余下 4 次失败 → 锁定 */
        for (int i = 0; i < 4; i++) {
            r = post("/v1/login", body, NULL);
            resp_free(&r);
        }
        free(bad64); g_free(body);
        /* 第 6 次（锁内）：即使密码对也 rate_limited */
        r = do_login(&a, NULL);
        CHECK_EQ_INT(r.status, 429);
        CHECK_EQ_STR(jerr(r.obj), "rate_limited");
        CHECK(jint(r.obj, "retry_after", 0) >= 1);
        resp_free(&r);
        /* 锁 200ms 后 → 成功且清零 */
        msleep(260);
        r = do_login(&a, NULL);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
    }
    acct_free(&a);
}

/* 4. TOTP：RFC 向量、setup/enable、required vs invalid、重放拒绝 */
static void t_totp(void)
{
    Acct a = {0};
    /* RFC 6238 向量：secret "12345678901234567890"，T=59 → 8 位 94287082
     * → 6 位截断 287082 */
    CHECK_EQ_INT(totp_sha1((const uint8_t *)"12345678901234567890", 20,
                         59 / 30),
                 287082);
    CHECK(do_register(&a, "totp@x.y", 0x51));
    /* setup */
    Resp r = post("/v1/totp/setup", "{}", a.token);
    CHECK_EQ_INT(r.status, 200);
    const char *sec32 = jstr(r.obj, "secret");
    const char *uri = jstr(r.obj, "uri");
    CHECK(sec32 && uri);
    char *sec_copy = sec32 ? strdup(sec32) : NULL;
    CHECK(uri && strstr(uri, "otpauth://totp/Harbor:totp%40x.y"));
    resp_free(&r);
    /* enable：用当前步长的合法 code */
    size_t sn = 0;
    uint8_t *sec = b32d(sec_copy, &sn);
    CHECK(sec && sn == 20);
    char code[8];
    snprintf(code, sizeof code, "%06u",
             totp_sha1(sec, sn, now_ms() / 30000));
    {
        char *bd = g_strdup_printf("{\"code\":\"%s\"}", code);
        r = post("/v1/totp/enable", bd, a.token);
        g_free(bd);
    }
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    /* 无 code 登录 → totp_required（不计失败） */
    r = do_login(&a, NULL);
    CHECK_EQ_INT(r.status, 401);
    CHECK_EQ_STR(jerr(r.obj), "totp_required");
    resp_free(&r);
    /* 错 code → totp_invalid */
    r = do_login(&a, "000000");
    CHECK_EQ_INT(r.status, 401);
    CHECK_EQ_STR(jerr(r.obj), "totp_invalid");
    /* 000000 万一是真 code（概率 1e-6）：换 111111 */
    if (r.status != 401) {
        resp_free(&r);
        r = do_login(&a, "111111");
        CHECK_EQ_STR(jerr(r.obj), "totp_invalid");
    }
    resp_free(&r);
    /* 下一步长 code（已在 ±1 容忍内且未用过）→ 200 */
    snprintf(code, sizeof code, "%06u",
             totp_sha1(sec, sn, now_ms() / 30000 + 1));
    r = do_login(&a, code);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    /* 重放同 code → 拒绝（窗口内防重放） */
    r = do_login(&a, code);
    CHECK_EQ_INT(r.status, 401);
    CHECK_EQ_STR(jerr(r.obj), "totp_invalid");
    resp_free(&r);
    free(sec);
    free(sec_copy);
    acct_free(&a);
}

/* ============================ websocket client ============================ */

typedef struct {
    SoupWebsocketConnection *conn;
    GError                  *err;
    bool                     done;
    char                    *last_msg;   /* 最近一条文本消息 */
    int                      n_msgs;
    bool                     closed;
} WsConn;

static void ws_connect_cb(GObject *src, GAsyncResult *res, gpointer ud)
{
    WsConn *w = ud;
    w->conn = soup_session_websocket_connect_finish(SOUP_SESSION(src), res,
                                                    &w->err);
    w->done = true;
}

static void ws_on_msg(SoupWebsocketConnection *c, int type, GBytes *msg,
                      gpointer ud)
{
    WsConn *w = ud;
    (void)c;
    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        gsize n;
        const char *d = g_bytes_get_data(msg, &n);
        g_free(w->last_msg);
        w->last_msg = g_strndup(d, n);
        w->n_msgs++;
    }
}

static void ws_on_closed(SoupWebsocketConnection *c, gpointer ud)
{
    (void)c;
    ((WsConn *)ud)->closed = true;
}

static WsConn *ws_connect(const char *token)
{
    WsConn *w = g_new0(WsConn, 1);
    g_autofree char *url = g_strdup_printf("%s/v1/ws", g_base);
    g_autofree char *ah = g_strdup_printf("Bearer %s", token);
    SoupMessage *m = soup_message_new(SOUP_METHOD_GET, url);
    int i;
    soup_message_headers_append(soup_message_get_request_headers(m),
                                "Authorization", ah);
    soup_session_websocket_connect_async(g_sess, m, NULL, NULL, 0, NULL,
                                         ws_connect_cb, w);
    for (i = 0; i < 300 && !w->done; i++) {
        g_main_context_iteration(NULL, FALSE);
        msleep(10);
    }
    g_object_unref(m);
    if (w->conn) {
        g_signal_connect(w->conn, "message", G_CALLBACK(ws_on_msg), w);
        g_signal_connect(w->conn, "closed", G_CALLBACK(ws_on_closed), w);
    }
    return w;
}

/* 泵主循环直到条件满足或超时（ms）。 */
static bool ws_wait(WsConn *w, int timeout_ms)
{
    int64_t until = now_ms() + timeout_ms;
    while (now_ms() < until) {
        g_main_context_iteration(NULL, FALSE);
        if (w->last_msg || w->closed) return true;
        msleep(10);
    }
    return w->last_msg != NULL || w->closed;
}

static void ws_free(WsConn *w)
{
    if (w->conn) {
        if (!w->closed)
            soup_websocket_connection_close(w->conn, 1000, NULL);
        g_object_unref(w->conn);
    }
    g_clear_error(&w->err);
    g_free(w->last_msg);
    g_free(w);
}

/* ============================ 5/6. 设备与 WS ============================ */

static void t_devices_ws(void)
{
    Acct a = {0};
    WsConn *ws;
    Resp r;
    CHECK(do_register(&a, "devs@x.y", 0x61));
    r = do_login(&a, NULL);
    CHECK_EQ_INT(r.status, 200);
    char *tok_b = jstr(r.obj, "token") ? strdup(jstr(r.obj, "token")) : NULL;
    char *dev_b =
        jstr(r.obj, "device_id") ? strdup(jstr(r.obj, "device_id")) : NULL;
    CHECK(tok_b && dev_b);
    resp_free(&r);

    /* GET /devices：两台、恰一个 current */
    r = get("/v1/devices", a.token);
    CHECK_EQ_INT(r.status, 200);
    {
        JsonArray *ds = json_object_get_array_member(r.obj, "devices");
        int cur = 0;
        CHECK_EQ_INT(json_array_get_length(ds), 2);
        for (guint i = 0; i < json_array_get_length(ds); i++)
            if (json_object_get_boolean_member(
                    json_array_get_object_element(ds, i), "current"))
                cur++;
        CHECK_EQ_INT(cur, 1);
    }
    resp_free(&r);

    /* 无效 token 的 ws → 拒绝（conn 为 NULL 或非 101） */
    {
        WsConn *bad = ws_connect("AAAA");
        CHECK(bad->conn == NULL);
        ws_free(bad);
    }

    ws = ws_connect(tok_b);
    if (!ws->conn)
        fprintf(stderr, "[ws] connect failed: %s\n",
                ws->err ? ws->err->message : "(timeout)");
    CHECK(ws->conn != NULL);
    if (!ws->conn) { ws_free(ws); acct_free(&a); free(tok_b); free(dev_b); return; }

    /* A push 一条 → B 的 ws 1 s 内收到 changed */
    {
        char *b1 = b64e((uint8_t *)"blob", 4);
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"o-9\",\"type\":\"host\",\"base_seq\":0,"
            "\"blob\":\"%s\"}]}",
            b1);
        int64_t t0 = now_ms();
        r = post("/v1/objects", bd, a.token);
        CHECK_EQ_INT(r.status, 200);
        CHECK(ws_wait(ws, 1000));
        int64_t dt = now_ms() - t0;
        fprintf(stderr, "[ws] changed latency %lld ms\n", (long long)dt);
        CHECK(ws->last_msg && strstr(ws->last_msg, "\"changed\""));
        CHECK(dt < 1000);
        resp_free(&r);
        free(b1); g_free(bd);
    }

    /* A 吊销 B（wipe） → B 的 ws 收 revoked 并关闭；之后的请求 401
     * device_revoked+wipe；GET /devices 只剩一台 */
    {
        char *url = g_strdup_printf("/v1/devices/%s/revoke", dev_b);
        g_free(ws->last_msg);
        ws->last_msg = NULL;
        int64_t t0 = now_ms();
        r = post(url, "{\"wipe\":true}", a.token);
        CHECK_EQ_INT(r.status, 200);
        CHECK(ws_wait(ws, 3000));
        fprintf(stderr, "[ws] revoked latency %lld ms\n",
                (long long)(now_ms() - t0));
        CHECK(ws->last_msg && strstr(ws->last_msg, "\"revoked\"") &&
              strstr(ws->last_msg, "true"));
        /* 等 closed */
        for (int i = 0; i < 200 && !ws->closed; i++) {
            g_main_context_iteration(NULL, FALSE);
            msleep(10);
        }
        CHECK(ws->closed);
        resp_free(&r);
        g_free(url);
    }
    r = get("/v1/devices", tok_b);
    CHECK_EQ_INT(r.status, 401);
    CHECK_EQ_STR(jerr(r.obj), "device_revoked");
    CHECK(jbool(r.obj, "wipe"));
    resp_free(&r);
    r = get("/v1/devices", a.token);
    CHECK_EQ_INT(json_array_get_length(
                     json_object_get_array_member(r.obj, "devices")),
                 1);
    resp_free(&r);
    /* 吊销自己 → 400 */
    {
        char *url = g_strdup_printf("/v1/devices/%s/revoke", a.device_id);
        r = post(url, "{\"wipe\":false}", a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        g_free(url);
    }
    ws_free(ws);
    free(tok_b); free(dev_b);
    acct_free(&a);
}

/* 7. 恢复流：rc_auth → recovery_token + wrapped_vk_rc；finish 改密；
 *    新密码可登录，旧密码失败，旧设备 token 仍有效 */
static void t_recovery(void)
{
    Acct a = {0};
    char *rtok = NULL;
    Resp r;
    CHECK(do_register(&a, "rec@x.y", 0x71));
    {
        char *ra = b64e(a.rc_auth, 32);
        char *bd = g_strdup_printf(
            "{\"email\":\"%s\",\"rc_auth\":\"%s\"}", a.email, ra);
        r = post("/v1/recover/start", bd, NULL);
        CHECK_EQ_INT(r.status, 200);
        CHECK(jstr(r.obj, "recovery_token") != NULL);
        CHECK_EQ_STR(jstr(r.obj, "wrapped_vk_rc"),
                     "d3JhcHBlZC1yYw"); /* b64("wrapped-rc") */
        rtok = strdup(jstr(r.obj, "recovery_token"));
        resp_free(&r);
        free(ra); g_free(bd);
    }
    /* 错误 recovery_token → invalid_credentials */
    {
        uint8_t bogus[32]; memset(bogus, 9, 32);
        char *bt = b64e(bogus, 32);
        uint8_t nk[32]; memset(nk, 0x55, 32);
        char *nk64 = b64e(nk, 32);
        char *salt = b64e((uint8_t *)"saltsaltsaltsalt", 16);
        char *wp = b64e((uint8_t *)"nw-pw", 5);
        char *bd = g_strdup_printf(
            "{\"recovery_token\":\"%s\",\"auth_key\":\"%s\",\"kdf\":{"
            "\"salt\":\"%s\",\"ops\":2,\"mem\":67108864},"
            "\"wrapped_vk_pw\":\"%s\",\"device\":{\"name\":\"r\",\"platform\":\"l\"}}",
            bt, nk64, salt, wp);
        r = post("/v1/recover/finish", bd, NULL);
        CHECK_EQ_INT(r.status, 401);
        resp_free(&r);
        free(bt); free(nk64); free(salt); free(wp); g_free(bd);
    }
    /* finish：换 auth_key */
    uint8_t new_k[32];
    memset(new_k, 0x55, 32);
    {
        char *nk64 = b64e(new_k, 32);
        char *salt = b64e((uint8_t *)"saltsaltsaltsalt", 16);
        char *wp = b64e((uint8_t *)"nw-pw", 5);
        char *bd = g_strdup_printf(
            "{\"recovery_token\":\"%s\",\"auth_key\":\"%s\",\"kdf\":{"
            "\"salt\":\"%s\",\"ops\":2,\"mem\":67108864},"
            "\"wrapped_vk_pw\":\"%s\",\"device\":{\"name\":\"r\",\"platform\":\"l\"}}",
            rtok, nk64, salt, wp);
        r = post("/v1/recover/finish", bd, NULL);
        CHECK_EQ_INT(r.status, 200);
        CHECK(jstr(r.obj, "token") != NULL);
        resp_free(&r);
        free(nk64); free(salt); free(wp); g_free(bd);
    }
    /* 新密码登录 OK */
    {
        char *nk64 = b64e(new_k, 32);
        char *bd = g_strdup_printf(
            "{\"email\":\"%s\",\"auth_key\":\"%s\","
            "\"device\":{\"name\":\"n\",\"platform\":\"l\"}}",
            a.email, nk64);
        r = post("/v1/login", bd, NULL);
        CHECK_EQ_INT(r.status, 200);
        CHECK_EQ_STR(jstr(r.obj, "wrapped_vk_pw"), "bnctcHc");
        resp_free(&r);
        free(nk64); g_free(bd);
    }
    /* 旧密码登录失败 */
    r = do_login(&a, NULL);
    CHECK_EQ_INT(r.status, 401);
    resp_free(&r);
    /* 旧设备 token 仍有效 */
    r = get("/v1/devices", a.token);
    CHECK_EQ_INT(r.status, 200);
    resp_free(&r);
    free(rtok);
    acct_free(&a);
}

/* 8. 零知识审计：sqlite dump 中不得出现 auth_key/rc_auth 原始字节
 *    及其 b64url；token 只存 hash。 */
static void t_zero_knowledge(void)
{
    Acct a = {0};
    CHECK(do_register(&a, "zk@x.y", 0x81));
    /* 存几条 blob */
    {
        char *b1 = b64e((uint8_t *)"super-secret-blob-1", 19);
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"z-1\",\"type\":\"host\",\"base_seq\":0,"
            "\"blob\":\"%s\"}]}",
            b1);
        Resp r = post("/v1/objects", bd, a.token);
        CHECK_EQ_INT(r.status, 200);
        resp_free(&r);
        free(b1); g_free(bd);
    }
    msleep(100); /* 让 WAL 落盘 */
    /* dump 全库 */
    sqlite3 *db = NULL;
    char *dbp;
    GByteArray *dump = g_byte_array_new();
    asprintf(&dbp, "%s/harbor-server.db", g_dir);
    CHECK_EQ_INT(sqlite3_open(dbp, &db), SQLITE_OK);
    free(dbp);
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table'", -1, &st,
            NULL);
        GPtrArray *tables = g_ptr_array_new_with_free_func(g_free);
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(tables,
                            g_strdup((const char *)sqlite3_column_text(st,
                                                                       0)));
        sqlite3_finalize(st);
        for (guint t = 0; t < tables->len; t++) {
            char *q = g_strdup_printf("SELECT * FROM \"%s\"",
                                      (char *)tables->pdata[t]);
            sqlite3_prepare_v2(db, q, -1, &st, NULL);
            g_free(q);
            int nc = sqlite3_column_count(st);
            while (sqlite3_step(st) == SQLITE_ROW)
                for (int c = 0; c < nc; c++) {
                    int ty = sqlite3_column_type(st, c);
                    if (ty == SQLITE_BLOB)
                        g_byte_array_append(
                            dump, sqlite3_column_blob(st, c),
                            sqlite3_column_bytes(st, c));
                    else if (ty == SQLITE_TEXT || ty == SQLITE_INTEGER) {
                        const char *tx =
                            (const char *)sqlite3_column_text(st, c);
                        if (tx)
                            g_byte_array_append(dump, (uint8_t *)tx,
                                                strlen(tx) + 1);
                    }
                }
            sqlite3_finalize(st);
        }
        g_ptr_array_free(tables, TRUE);
    }
    sqlite3_close(db);
    /* 原始字节 */
    CHECK(memmem(dump->data, dump->len, a.auth_key, 32) == NULL);
    CHECK(memmem(dump->data, dump->len, a.rc_auth, 32) == NULL);
    /* b64url 形式 */
    {
        char *ak64 = b64e(a.auth_key, 32), *rc64 = b64e(a.rc_auth, 32);
        CHECK(memmem(dump->data, dump->len, ak64, strlen(ak64)) == NULL);
        CHECK(memmem(dump->data, dump->len, rc64, strlen(rc64)) == NULL);
        free(ak64); free(rc64);
    }
    /* token 明文不出现；其 sha256 hex 出现（证明只存 hash） */
    {
        CHECK(memmem(dump->data, dump->len, a.token, strlen(a.token)) ==
              NULL);
        uint8_t tbin[64]; size_t tn;
        uint8_t *td = b64d(a.token, &tn);
        char *th = malloc(65);
        uint8_t dig[crypto_hash_sha256_BYTES];
        memcpy(tbin, td, tn < 64 ? tn : 64);
        crypto_hash_sha256(dig, td, tn);
        sodium_bin2hex(th, 65, dig, 32);
        CHECK(memmem(dump->data, dump->len, th, 64) != NULL);
        free(td); free(th);
    }
    /* email 是明文元数据（协议允许），不必检查 */
    g_byte_array_free(dump, TRUE);
    acct_free(&a);
}

/* 9. 输入校验 */
static void t_validation(void)
{
    Acct a = {0};
    Resp r;
    CHECK(do_register(&a, "val@x.y", 0x91));
    /* 超大 blob（>1MiB 解码后） */
    {
        size_t n = (1 << 20) + 1;
        uint8_t *big = malloc(n);
        char *b64;
        memset(big, 0x42, n);
        b64 = b64e(big, n);
        char *bd = g_strdup_printf(
            "{\"changes\":[{\"id\":\"v-1\",\"type\":\"host\",\"base_seq\":0,"
            "\"blob\":\"%s\"}]}",
            b64);
        r = post("/v1/objects", bd, a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        free(big); free(b64); g_free(bd);
    }
    /* 非法 id / type / base64 / 缺字段 / 空 changes / >200 */
    {
        r = post("/v1/objects",
                 "{\"changes\":[{\"id\":\"bad id!\",\"type\":\"host\","
                 "\"base_seq\":0,\"blob\":\"AAAA\"}]}",
                 a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        r = post("/v1/objects",
                 "{\"changes\":[{\"id\":\"ok-1\",\"type\":\"Bad\","
                 "\"base_seq\":0,\"blob\":\"AAAA\"}]}",
                 a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        r = post("/v1/objects",
                 "{\"changes\":[{\"id\":\"ok-1\",\"type\":\"host\","
                 "\"base_seq\":0,\"blob\":\"!!notb64!!\"}]}",
                 a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        r = post("/v1/objects",
                 "{\"changes\":[{\"id\":\"ok-1\",\"type\":\"host\","
                 "\"base_seq\":0,\"blob\":\"AA==\"}]}",
                 a.token); /* 带填充的 base64 也拒 */
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        r = post("/v1/objects", "{\"changes\":[]}", a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
        r = post("/v1/objects",
                 "{\"changes\":[{\"id\":\"ok-1\",\"type\":\"host\"}]}",
                 a.token);
        CHECK_EQ_INT(r.status, 400);
        resp_free(&r);
    }
    /* 未授权 */
    r = get("/v1/objects?since=0", NULL);
    CHECK_EQ_INT(r.status, 401);
    resp_free(&r);
    r = get("/v1/objects?since=0", "bogus-token");
    CHECK_EQ_INT(r.status, 401);
    resp_free(&r);
    /* register 缺字段 / login 畸形 */
    r = post("/v1/register", "{\"email\":\"x@y\"}", NULL);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    r = post("/v1/login", "{\"email\":\"x@y\"}", NULL);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    r = post("/v1/prelogin", "not json", NULL);
    CHECK_EQ_INT(r.status, 400);
    resp_free(&r);
    acct_free(&a);
}

/* ---- 有界 Argon2 池：并发认证不阻塞主循环；超队列上限 → 429 ---- */

typedef struct {
    char  *body;
    guint  status;
} LoginJob;

static gpointer storm_thread(gpointer p)
{
    LoginJob *j = p;
    SoupSession *s = soup_session_new();
    g_autofree char *url = g_strconcat(g_base, "/v1/login", NULL);
    SoupMessage *m = soup_message_new(SOUP_METHOD_POST, url);
    GBytes *bb = g_bytes_new(j->body, strlen(j->body));
    soup_message_set_request_body_from_bytes(m, "application/json", bb);
    g_bytes_unref(bb);
    {
        GBytes *rb = soup_session_send_and_read(s, m, NULL, NULL);
        if (rb) g_bytes_unref(rb);
    }
    j->status = soup_message_get_status(m);
    g_object_unref(m);
    g_object_unref(s);
    return NULL;
}

static void t_hash_pool(void)
{
    enum { N = 12 };
    Acct a = {0};
    GThread *ths[N];
    LoginJob jobs[N];
    int i, n401 = 0, n429 = 0, other = 0;
    char *tok;

    CHECK(do_register(&a, "pool@x.y", 0x91));
    tok = strdup(a.token);
    /* 12 个并发 login（不存在邮箱 → dummy 校验仍跑 Argon2，
     * 且不触发按账号锁定）。hash-threads=2 + hash-queue=2 →
     * 同时最多 2 个在途，其余立即 429。 */
    for (i = 0; i < N; i++) {
        jobs[i].body =
            g_strdup_printf("{\"email\":\"storm-%d@x.y\","
                            "\"auth_key\":\"%s\","
                            "\"device\":{\"name\":\"s\",\"platform\":\"t\"}}",
                            i, b64e(a.auth_key, 32));
        jobs[i].status = 0;
        ths[i] = g_thread_new("storm", storm_thread, &jobs[i]);
    }
    /* 等池饱和（各线程建连+发送后，2 个 worker 忙 Argon2 ~数百 ms） */
    msleep(300);
    /* 池饱和期间，已认证的非认证类请求必须仍快（主循环未被阻塞） */
    {
        gint64 t0 = g_get_monotonic_time();
        Resp r = get("/v1/objects?since=0", tok);
        gint64 dt = g_get_monotonic_time() - t0;
        CHECK_EQ_INT(r.status, 200);
        CHECK(dt < 200 * 1000);
        if (dt >= 200 * 1000)
            fprintf(stderr, "objects during hash storm: %lld us\n",
                    (long long)dt);
        resp_free(&r);
    }
    for (i = 0; i < N; i++) {
        g_thread_join(ths[i]);
        g_free(jobs[i].body);
        if (jobs[i].status == 401) n401++;
        else if (jobs[i].status == 429) n429++;
        else other++;
    }
    CHECK_EQ_INT(other, 0);
    CHECK(n401 >= 1);          /* 池排空，完成的真实校验 */
    CHECK(n429 >= 1);          /* 并发 > 2 → 超限拒绝 */
    fprintf(stderr, "[pool] storm: %d x401, %d x429\n", n401, n429);
    free(tok);
    acct_free(&a);
}

int main(void)
{
    if (sodium_init() < 0) return 1;
    g_sess = soup_session_new();
    start_server();
    t_basic();
    t_cas();
    t_lockout();
    t_totp();
    t_devices_ws();
    t_recovery();
    t_zero_knowledge();
    t_validation();
    t_hash_pool();
    stop_server();
    printf("server: %d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
