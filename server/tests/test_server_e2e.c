/* harbor-server ↔ harbor-core 互通测试：用真实 hb_sync_* 客户端加密栈
 * 对真实服务端做 register/login/push/pull 往返。 */

#include <harbor/sync.h>
#include <harbor/vault.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <sodium.h>
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

static SoupSession *g_sess;
static char *g_base;
static GPid g_pid = -1;
static char *g_dir;

static char *http_req(const char *method, const char *path,
                      const char *body, const char *token, guint *status)
{
    g_autofree char *url = g_strconcat(g_base, path, NULL);
    SoupMessage *m = soup_message_new(method, url);
    GBytes *rb;
    gsize bn;
    char *out = NULL;
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
    *status = soup_message_get_status(m);
    if (rb) {
        const char *d = g_bytes_get_data(rb, &bn);
        out = g_malloc(bn + 1);
        memcpy(out, d, bn);
        out[bn] = 0;
        g_bytes_unref(rb);
    }
    g_object_unref(m);
    return out;
}

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    socklen_t l = sizeof a;
    int port;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&a, sizeof a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static void start_server(void)
{
    int port = free_port();
    char **env, *listen_a, **argv;
    char dir[] = "/tmp/hbsrv-e2e-XXXXXX";
    CHECK(port > 0);
    if (!mkdtemp(dir)) { CHECK(0); return; }
    g_dir = strdup(dir);
    g_base = g_strdup_printf("http://127.0.0.1:%d", port);
    listen_a = g_strdup_printf("127.0.0.1:%d", port);
    env = g_environ_setenv(g_get_environ(), "HARBOR_SERVER_TEST_LOCK_MS",
                           "200", TRUE);
    argv = g_new0(char *, 9);
    argv[0] = (char *)SERVER_BIN;
    argv[1] = "--listen";
    argv[2] = listen_a;
    argv[3] = "--data";
    argv[4] = g_dir;
    argv[5] = "--log-level";
    argv[6] = "warn";
    if (!g_spawn_async(NULL, argv, env, G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &g_pid, NULL)) {
        CHECK(0);
        return;
    }
    for (int i = 0; i < 100; i++) {
        guint st;
        char *r = http_req(SOUP_METHOD_POST, "/v1/prelogin",
                           "{\"email\":\"w@u.p\"}", NULL, &st);
        if (r) { g_free(r); break; }
        g_usleep(50000);
    }
    g_free(listen_a);
    g_free(argv);
    g_strfreev(env);
}

int main(void)
{
    hb_vault *va = NULL, *vb = NULL;
    hb_sync *sa = NULL, *sb = NULL;
    uint8_t dek_a[32] = {0}, dek_b[32] = {0}, vk[32];
    uint8_t salt[16];
    uint64_t ops, mem;
    char *body = NULL, *words = NULL, *resp;
    char *acct_id = NULL, *dev_id = NULL, *token = NULL;
    guint st;
    hb_sync_push *push = NULL;
    bool retry;
    char tmpa[] = "/tmp/hbv-a-XXXXXX.db";
    char tmpb[] = "/tmp/hbv-b-XXXXXX.db";
    {
        int fd = mkstemps(tmpa, 3);
        close(fd); unlink(tmpa);
        fd = mkstemps(tmpb, 3);
        close(fd); unlink(tmpb);
    }

    CHECK(sodium_init() >= 0);
    g_sess = soup_session_new();
    start_server();

    /* ---- register：core 生成 body（含 auth_key/rc_auth/VK 封装） ---- */
    CHECK_EQ_INT(hb_sync_register_body("e2e@x.y", "pw-hunter2", "devA",
                                     "linux", 2, 67108864, &body, &words,
                                     vk),
                 HB_OK);
    CHECK(body && words && strrchr(words, ' '));
    resp = http_req(SOUP_METHOD_POST, "/v1/register", body, NULL, &st);
    CHECK_EQ_INT(st, 200);
    CHECK_EQ_INT(hb_sync_auth_resp_parse(resp, &acct_id, &dev_id, &token),
                 HB_OK);
    CHECK(acct_id && dev_id && token);
    g_free(resp);

    /* vault A：建 host，set VK + account，build push → POST → apply */
    memset(dek_a, 0xa1, 32);
    CHECK_EQ_INT(hb_vault_open(tmpa, dek_a, &va), HB_OK);
    sa = hb_sync_new(va);
    CHECK_EQ_INT(hb_sync_set_vk(sa, vk), HB_OK);
    CHECK_EQ_INT(hb_sync_set_account(sa, acct_id, "e2e@x.y", g_base, dev_id),
                 HB_OK);
    {
        hb_obj *o = hb_obj_new("host");
        hb_obj_set_str(o, "name", "e2e-host");
        hb_obj_set_str(o, "address", "10.0.0.7");
        hb_obj_set_str(o, "user", "ops");
        CHECK_EQ_INT(hb_vault_put(va, o), HB_OK);
        hb_obj_free(o);
    }
    CHECK_EQ_INT(hb_sync_build_push(sa, &push), HB_OK);
    CHECK(push != NULL);
    resp = http_req(SOUP_METHOD_POST, "/v1/objects",
                    hb_sync_push_body(push), token, &st);
    CHECK_EQ_INT(st, 200);
    CHECK_EQ_INT(hb_sync_apply_push(sa, push, resp, &retry), HB_OK);
    CHECK(!retry);
    g_free(resp);
    hb_sync_push_free(push);

    /* ---- device B：login → wrapped_vk_pw 解出 VK → pull ---- */
    {
        uint8_t wrap_key[32], vkb[32];
        char *acct_b = NULL, *dev_b = NULL, *token_b = NULL;
        bool totp_en;
        free(body);
        body = NULL;
        /* prelogin → kdf */
        CHECK_EQ_INT(hb_sync_prelogin_body("e2e@x.y", &body), HB_OK);
        resp = http_req(SOUP_METHOD_POST, "/v1/prelogin", body, NULL, &st);
        CHECK_EQ_INT(st, 200);
        CHECK_EQ_INT(hb_sync_prelogin_parse(resp, salt, &ops, &mem), HB_OK);
        CHECK_EQ_INT(mem, 67108864);
        g_free(resp);
        free(body);
        body = NULL;
        CHECK_EQ_INT(hb_sync_login_body("e2e@x.y", "pw-hunter2", salt, ops,
                                        mem, NULL, "devB", "linux", &body,
                                        wrap_key),
                     HB_OK);
        resp = http_req(SOUP_METHOD_POST, "/v1/login", body, NULL, &st);
        CHECK_EQ_INT(st, 200);
        CHECK_EQ_INT(hb_sync_login_parse(resp, wrap_key, vkb, &acct_b,
                                         &dev_b, &token_b, &totp_en),
                     HB_OK);
        g_free(resp);
        CHECK_EQ_INT(memcmp(vk, vkb, 32), 0); /* 同一把 VK */
        sodium_memzero(wrap_key, 32);

        memset(dek_b, 0xb2, 32);
        CHECK_EQ_INT(hb_vault_open(tmpb, dek_b, &vb), HB_OK);
        sb = hb_sync_new(vb);
        CHECK_EQ_INT(hb_sync_set_vk(sb, vkb), HB_OK);
        CHECK_EQ_INT(hb_sync_set_account(sb, acct_b, "e2e@x.y", g_base,
                                         dev_b),
                     HB_OK);
        sodium_memzero(vkb, 32);
        {
            char *ppath = NULL;
            hb_sync_pull_stats ps;
            CHECK_EQ_INT(hb_sync_pull_path(sb, &ppath), HB_OK);
            resp = http_req(SOUP_METHOD_GET, ppath, NULL, token_b, &st);
            CHECK_EQ_INT(st, 200);
            CHECK_EQ_INT(hb_sync_apply_pull(sb, resp, &ps), HB_OK);
            CHECK_EQ_INT(ps.applied, 1);
            g_free(resp);
            free(ppath);
        }
        /* B 看到同一对象 */
        {
            /* 取对象 id：A vault 里的 host */
            hb_obj *ob = NULL;
            /* 通过 objects 表查：直接遍历不可行，用已知的 data 比对：
             * 让 B pull 后 get 同一 id —— 从 A 的对象列表里取。 */
            {
                /* 从 A vault rows 拿不到 id（公开 API 无 list），
                 * 用 pull 的响应 blob id —— 重新 GET 记录 id */
                char *ppath2 = NULL;
                char *rid = NULL;
                JsonParser *jp;
                CHECK_EQ_INT(hb_sync_pull_path(sa, &ppath2), HB_OK);
                /* A 的游标已推进；直接查服务端拿 id */
                g_free(ppath2);
                ppath2 = g_strdup("/v1/objects?since=0&limit=10");
                resp = http_req(SOUP_METHOD_GET, ppath2, NULL, token, &st);
                CHECK_EQ_INT(st, 200);
                jp = json_parser_new();
                json_parser_load_from_data(jp, resp, -1, NULL);
                JsonArray *os = json_object_get_array_member(
                    json_node_get_object(json_parser_get_root(jp)),
                    "objects");
                CHECK_EQ_INT(json_array_get_length(os), 1);
                JsonObject *o0 = json_array_get_object_element(os, 0);
                rid = g_strdup(json_object_get_string_member(o0, "id"));
                g_object_unref(jp);
                g_free(resp);
                g_free(ppath2);
                /* B 取该对象验证字段 */
                CHECK_EQ_INT(hb_vault_get(vb, rid, &ob), HB_OK);
                CHECK_EQ_STR(hb_obj_get_str(ob, "name"), "e2e-host");
                CHECK_EQ_STR(hb_obj_get_str(ob, "address"), "10.0.0.7");
                CHECK_EQ_STR(hb_obj_get_str(ob, "user"), "ops");
                hb_obj_free(ob);
                g_free(rid);
            }
        }
        free(acct_b); free(dev_b); free(token_b);
    }

    hb_sync_free(sa);
    hb_sync_free(sb);
    hb_vault_close(va);
    hb_vault_close(vb);
    free(acct_id); free(dev_id); free(token);
    free(body); free(words);
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        g_spawn_close_pid(g_pid);
    }
    printf("server-e2e: %d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
