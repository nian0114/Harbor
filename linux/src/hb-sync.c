/* FR-11 账号同步服务（HbSyncService，应用单例）。
 * - 拥有 hb_sync 引擎（绑定 hb_store_default 的 vault）、SoupSession、
 *   WebSocket 与同步状态机。vault 只在主线程访问。
 * - Argon2 派生（注册/登录/恢复/改密/轮换）经专用 GThreadPool 跑
 *   hb_sync_*_body —— 这些函数只做 crypto+JSON，不碰 vault；rotate/change
 *   只读 s->vk（op 串行保证期间无 set/clear）。
 * - VK + 登录 token 存 libsecret "dev.harbor.Harbor.Sync"（purpose+data_dir
 *   属性），不可用时退 data_dir/sync.key（0600）。日志绝不记录密钥。 */

#include "hb-sync.h"
#include "hb-store.h"
#include "hb-notify.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libsecret/secret.h>
#include <sodium.h>

#include <fcntl.h>
#include <unistd.h>

#define POLL_SEC         30
#define PUSH_DEBOUNCE_MS 1000
#define PUSH_RETRY_MAX   3
#define HASH_THREADS     2
#define WATCHDOG_SEC   60      /* 单轮同步最长时限 */
#define WS_CHECK_SEC   15      /* WS 存活检测周期 */
#define WS_PING_SEC    15      /* 客户端 WS ping 间隔 */
#define WS_SILENCE_SEC 75      /* 超过此时长无任何帧 → 判定半开 */

struct _HbSyncService {
    GObject       parent;
    HbStore      *store;
    hb_sync      *sync;
    SoupSession  *sess;
    GThreadPool  *pool;
    GNetworkMonitor *mon;

    HbSyncState   state;
    char         *status;
    char         *last_error;
    gint64        last_sync;
    gboolean      totp_enabled;
    int           op_count;

    uint8_t      *vk;           /* sodium_malloc'd；登出/销毁即擦 */
    gboolean      has_vk;
    char         *token;
    char         *server_url;
    gboolean      merge_pending;
    guint         gen;          /* 代次：登出递增，worker 结果防陈旧 */

    gboolean      busy;
    gboolean      kick_pending;
    int           push_round;
    guint         debounce_src;
    guint         poll_src;

    SoupWebsocketConnection *ws;
    gboolean      ws_open;
    int           ws_backoff;
    guint         ws_retry_src;
    gboolean      ws_connecting;
    gint64        ws_last_rx;    /* 最近收到任何 WS 帧的时间（含服务端 ping） */
    guint         ws_rx_src;     /* 裸 socket G_IO_IN 存活侦测 */
    guint         ws_check_src;  /* WS 沉默检测定时器 */

    gint64        round_start;   /* 本轮同步开始（单钟） */
    guint         watchdog_src;  /* 同步看门狗：一轮 >60s 强制下线 */
    GCancellable *round_cancel;  /* 本轮 HTTP 的取消句柄 */
};

G_DEFINE_TYPE(HbSyncService, hb_sync_service, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];
static HbSyncService *g_svc;
static const int BACKOFF[] = { 1, 2, 4, 8, 15, 30 };

static void sync_kick(HbSyncService *s);
static void start_triggers(HbSyncService *s);
static void loop_offline(HbSyncService *s, const char *why);
static void ws_close_conn(HbSyncService *s);
static void set_status_txt(HbSyncService *s, const char *txt);
static void logout_ignore_cb(HbSyncService *s, guint st, const char *b,
                             JsonObject *o, gpointer u);
static void ws_connect(HbSyncService *s);
static void sign_out_local(HbSyncService *s, gboolean wipe, const char *why);

static void emit_changed(HbSyncService *s)
{
    g_signal_emit(s, sigs[SIG_CHANGED], 0);
}

/* 擦除并释放普通堆字符串（token/密码/词等机密）。 */
static void wfree(char **p)
{
    if (*p) {
        memset(*p, 0, strlen(*p));
        g_free(*p);
        *p = NULL;
    }
}

static uint8_t *ensure_vk(HbSyncService *s)
{
    if (!s->vk)
        s->vk = sodium_malloc(32);
    return s->vk;
}

static void set_state(HbSyncService *s, HbSyncState st)
{
    s->state = st;
    emit_changed(s);
}

static char *norm_email(const char *email)
{
    char *t = g_strdup(email ? email : "");
    char *l;
    g_strstrip(t);
    l = g_ascii_strdown(t, -1);
    g_free(t);
    return l;
}

static char *nfkc(const char *pw)
{
    return pw ? g_utf8_normalize(pw, -1, G_NORMALIZE_NFKC) : NULL;
}

/* ============================ 凭据存储 ============================ */

static const SecretSchema SYNC_SCHEMA = {
    "dev.harbor.Harbor.Sync", SECRET_SCHEMA_NONE,
    {
        { "purpose",  SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "data_dir", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "NULL", 0 },
    },
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

static char *creds_path(HbSyncService *s)
{
    return g_build_filename(hb_store_data_dir(s->store), "sync.key", NULL);
}

/* 负载：b64(vk).<token> */
static void creds_save(HbSyncService *s, const uint8_t vk[32],
                       const char *token)
{
    GError *err = NULL;
    char *b64, *payload, *path;
    gboolean stored;
    b64 = g_base64_encode(vk, 32);
    payload = g_strdup_printf("%s.%s", b64, token);
    stored = secret_password_store_sync(
        &SYNC_SCHEMA, SECRET_COLLECTION_DEFAULT,
        "Harbor sync credentials", payload, NULL, &err,
        "purpose", "sync-creds",
        "data_dir", hb_store_data_dir(s->store), NULL);
    if (!stored) {
        if (err) { g_error_free(err); }
        path = creds_path(s);
        {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                if (write(fd, payload, strlen(payload)) < 0)
                    g_warning("sync.key write failed");
                close(fd);
            }
        }
        g_free(path);
    }
    sodium_memzero(payload, strlen(payload));
    sodium_memzero(b64, strlen(b64));
    g_free(payload);
    g_free(b64);
}

static gboolean creds_load(HbSyncService *s, uint8_t vk[32],
                           char **token_out)
{
    GError *err = NULL;
    gchar *payload;

    payload = secret_password_lookup_sync(&SYNC_SCHEMA, NULL, &err,
                                        "purpose", "sync-creds",
                                        "data_dir",
                                        hb_store_data_dir(s->store),
                                        NULL);
    if (err) { g_error_free(err); err = NULL; }
    {
        gboolean from_secret = TRUE;
        if (!payload) {
            g_autofree char *path = creds_path(s);
            gsize n = 0;
            if (g_file_get_contents(path, &payload, &n, NULL) &&
                payload && *payload) {
                g_strstrip(payload);
                from_secret = FALSE;
            } else {
                g_free(payload);
                payload = NULL;
            }
        }
        if (!payload)
            return FALSE;
        {
            size_t plen = strlen(payload);   /* 切分前取全长：擦除须覆盖 token 段 */
            char *dot = strchr(payload, '.');
            gsize vn = 0;
            guchar *raw = NULL;
            gboolean ok = FALSE;
            if (dot) {
                *dot = 0;
                raw = g_base64_decode(payload, &vn);
                if (raw && vn == 32 && dot[1]) {
                    memcpy(vk, raw, 32);
                    *token_out = g_strdup(dot + 1);
                    ok = TRUE;
                }
            }
            if (raw) { sodium_memzero(raw, vn); g_free(raw); }
            sodium_memzero(payload, plen);
            if (from_secret)
                secret_password_free(payload);
            else
                g_free(payload);
            return ok;
        }
    }
}

static void creds_wipe(HbSyncService *s)
{
    g_autofree char *path = creds_path(s);
    secret_password_clear_sync(&SYNC_SCHEMA, NULL, NULL,
                               "purpose", "sync-creds",
                               "data_dir", hb_store_data_dir(s->store),
                               NULL);
    unlink(path);
}

/* ============================ HTTP ============================ */

typedef struct {
    HbSyncService *s;
    SoupMessage   *msg;
    void (*cb)(HbSyncService *s, guint status, const char *body,
               JsonObject *obj, gpointer ud);
    gpointer     ud;
    gboolean     authed;
} ReqCtx;

static void req_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ReqCtx *r = ud;
    HbSyncService *s = r->s;
    GError *err = NULL;
    GBytes *b;
    guint status;
    char *txt = NULL;
    JsonObject *obj = NULL;
    const char *code = NULL;

    b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (!b) {
        r->cb(s, 0, NULL, NULL, r->ud);
        g_free(r);
        return;
    }
    status = soup_message_get_status(r->msg);
    {
        gsize n = 0;
        const char *d = g_bytes_get_data(b, &n);
        txt = g_strndup(d ? d : "", n);
    }
    g_bytes_unref(b);
    {
        g_autoptr(JsonParser) p = json_parser_new();
        if (json_parser_load_from_data(p, txt, -1, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_OBJECT(root))
                obj = json_object_ref(json_node_get_object(root));
        }
    }
    if (r->authed && obj) {
        JsonNode *en = json_object_get_member(obj, "error");
        code = en && JSON_NODE_HOLDS_VALUE(en) &&
                       json_node_get_value_type(en) == G_TYPE_STRING
                   ? json_node_get_string(en) : NULL;
        if (code && !strcmp(code, "device_revoked")) {
            gboolean wipe = FALSE;
            JsonNode *wn = json_object_get_member(obj, "wipe");
            if (wn && JSON_NODE_HOLDS_VALUE(wn) &&
                json_node_get_value_type(wn) == G_TYPE_BOOLEAN)
                wipe = json_node_get_boolean(wn);
            json_object_unref(obj);
            g_free(txt);
            sign_out_local(s, wipe, NULL);
            hb_notify_post("err", "此设备已被吊销",
                           wipe ? "账号已退出，本地数据已擦除" : "账号已退出",
                           NULL);
            g_free(r);
            return;
        }
        if (code && !strcmp(code, "unauthorized")) {
            json_object_unref(obj);
            g_free(txt);
            sign_out_local(s, FALSE, "登录已失效，请重新登录");
            g_free(r);
            return;
        }
    }
    r->cb(s, status, txt, obj, r->ud);
    if (obj) json_object_unref(obj);
    g_free(txt);
    g_free(r);
}

static void http(HbSyncService *s, const char *server, const char *method,
                 const char *path, const char *body, gboolean authed,
                 GCancellable *cancel,
                 void (*cb)(HbSyncService *, guint, const char *,
                            JsonObject *, gpointer),
                 gpointer ud)
{
    g_autofree char *url = NULL;
    SoupMessage *m;
    if (!server || !*server)
        return;
    url = g_strconcat(server, path, NULL);
    m = soup_message_new(method, url);
    ReqCtx *r;
    if (s->token) {
        g_autofree char *a = g_strdup_printf("Bearer %s", s->token);
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", a);
    }
    if (body) {
        GBytes *bb = g_bytes_new(body, strlen(body));
        soup_message_set_request_body_from_bytes(m, "application/json", bb);
        g_bytes_unref(bb);
    }
    r = g_new0(ReqCtx, 1);
    r->s = s;
    r->msg = m;
    r->cb = cb;
    r->ud = ud;
    r->authed = authed;
    soup_session_send_and_read_async(s->sess, m, G_PRIORITY_DEFAULT,
                                     cancel, req_done, r);
}

static const char *jstr(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) &&
                   json_node_get_value_type(n) == G_TYPE_STRING
               ? json_node_get_string(n) : NULL;
}

/* ============================ 操作框架 ============================ */

enum {
    OP_LOGIN, OP_REGISTER, OP_RECOVER, OP_ROTATE, OP_PASSWORD,
};

typedef struct OpCtx OpCtx;
struct OpCtx {
    HbSyncService *svc;
    int        kind;
    char      *server, *email, *password, *totp, *new_password;
    char      *words_in;
    uint8_t    salt[16];
    uint64_t   ops, mem;
    uint8_t    wrap_key[32];
    char      *body;
    char      *account_id, *dev_id, *token;
    gboolean   totp_enabled;
    char      *recovery_token;
    char      *out_words;
    uint8_t   *vk_in;      /* sodium：主线程快照的 VK 副本 */
    uint8_t   *vk_out;     /* sodium：worker 产出的 VK（注册/解析） */
    guint      gen;
    hb_err     err;
    HbSyncOpCb cb;
    gpointer   ud;
};

static void op_free(OpCtx *o)
{
    g_free(o->server); g_free(o->email);
    wfree(&o->password); wfree(&o->totp); wfree(&o->new_password);
    wfree(&o->words_in); wfree(&o->body); wfree(&o->account_id);
    g_free(o->dev_id);
    wfree(&o->token); wfree(&o->recovery_token);
    if (o->out_words) wfree(&o->out_words);
    if (o->vk_in) { sodium_memzero(o->vk_in, 32); sodium_free(o->vk_in); }
    if (o->vk_out) { sodium_memzero(o->vk_out, 32); sodium_free(o->vk_out); }
    sodium_memzero(o->wrap_key, sizeof o->wrap_key);
    sodium_memzero(o->salt, sizeof o->salt);
    g_free(o);
}

static void op_result(OpCtx *o, gboolean ok, const char *code,
                      const char *msg, int retry_after)
{
    HbSyncOpResult r = {
        .ok = ok, .code = code, .message = msg,
        .retry_after = retry_after,
        .words = o->out_words,
    };
    o->out_words = NULL;
    if (o->cb) o->cb(o->svc, &r, o->ud);
}

static void op_end(OpCtx *o)
{
    o->svc->op_count--;
    emit_changed(o->svc);
    op_free(o);
}

static OpCtx *op_new(HbSyncService *s, int kind, HbSyncOpCb cb,
                     gpointer ud)
{
    OpCtx *o = g_new0(OpCtx, 1);
    o->svc = s;
    o->kind = kind;
    o->cb = cb;
    o->ud = ud;
    o->gen = s->gen;
    s->op_count++;
    emit_changed(s);
    return o;
}

/* worker：Argon2 重的 body 构建（线程池；不碰 vault） */
typedef struct {
    OpCtx *o;
    void (*then)(OpCtx *o);
} Work;

static void op_worker(OpCtx *o)
{
    switch (o->kind) {
    case OP_LOGIN:
        o->err = hb_sync_login_body(o->email, o->password, o->salt, o->ops,
                                    o->mem, o->totp, g_get_host_name(),
                                    "linux", &o->body, o->wrap_key);
        break;
    case OP_REGISTER:
        o->err = hb_sync_register_body(o->email, o->password,
                                       g_get_host_name(), "linux", 0, 0,
                                       &o->body, &o->out_words, o->vk_out);
        break;
    case OP_RECOVER:
        o->err = hb_sync_recover_finish_body(o->recovery_token,
                                             o->new_password, 0, 0,
                                             g_get_host_name(), "linux",
                                             o->vk_out, &o->body);
        break;
    case OP_ROTATE:
        o->err = hb_sync_recovery_rotate_body_vk(o->vk_in, o->password,
                                                 o->salt, o->ops, o->mem,
                                                 &o->body, &o->out_words);
        break;
    case OP_PASSWORD:
        o->err = hb_sync_change_password_body(o->password, o->salt, o->ops,
                                              o->mem, o->new_password, 0, 0,
                                              o->vk_in, &o->body);
        break;
    }
}

static gboolean work_then(gpointer p)
{
    Work *w = p;
    void (*then)(OpCtx *) = w->then;
    OpCtx *o = w->o;
    g_free(w);
    then(o);
    return G_SOURCE_REMOVE;
}

static void work_runner(gpointer p, gpointer u)
{
    Work *w = p;
    (void)u;
    op_worker(w->o);
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, work_then, w,
                               NULL);
}

static void schedule_work(OpCtx *o, void (*then)(OpCtx *))
{
    Work *w = g_new(Work, 1);
    w->o = o;
    w->then = then;
    g_thread_pool_push(o->svc->pool, w, NULL);
}

/* ============================ 会话建立 ============================ */

static gboolean has_local_scope_data(HbSyncService *s)
{
    static const char *types[] = {
        "host", "group", "key", "snippet", "forward", "known", "settings",
    };
    size_t i, n;
    for (i = 0; i < G_N_ELEMENTS(types); i++) {
        hb_obj **list;
        n = 0;
        list = hb_store_list(s->store, types[i], &n);
        if (n) {
            hb_obj_list_free(list, n);
            return TRUE;
        }
        if (list) hb_obj_list_free(list, n);
    }
    return FALSE;
}

static void finish_signin(OpCtx *o)
{
    HbSyncService *s = o->svc;
    gboolean need_merge;
    if (o->gen != s->gen || !o->vk_out) {
        op_end(o);
        return;
    }
    memcpy(ensure_vk(s), o->vk_out, 32);
    s->has_vk = TRUE;
    hb_sync_set_vk(s->sync, o->vk_out);
    sodium_memzero(o->vk_out, 32);
    sodium_free(o->vk_out);
    o->vk_out = NULL;
    g_free(s->token);
    s->token = g_steal_pointer(&o->token);
    g_free(s->server_url);
    s->server_url = g_steal_pointer(&o->server);
    hb_sync_set_account(s->sync, o->account_id, o->email, s->server_url,
                        o->dev_id);
    s->totp_enabled = o->totp_enabled;
    creds_save(s, s->vk, s->token);
    set_state(s, HB_SYNC_IDLE);
    need_merge = (o->kind == OP_LOGIN || o->kind == OP_RECOVER) &&
                 has_local_scope_data(s);
    if (need_merge) {
        s->merge_pending = TRUE;
        {
            HbSyncOpResult r = { .ok = TRUE, .need_merge = TRUE };
            if (o->cb) o->cb(s, &r, o->ud);
        }
    } else {
        start_triggers(s);
        ws_connect(s);
        sync_kick(s);
        {
            HbSyncOpResult r = { .ok = TRUE };
            if (o->cb) o->cb(s, &r, o->ud);
        }
    }
    op_end(o);
}

/* ---- 错误映射 ---- */
static void op_fail_from_resp(OpCtx *o, guint st, JsonObject *obj)
{
    const char *code = jstr(obj, "error");
    const char *msg = jstr(obj, "message");
    int ra = 0;
    JsonNode *n;
    if (st == 0) {
        op_result(o, FALSE, "network", "无法连接服务器", 0);
        return;
    }
    if (st == 429) {
        n = obj ? json_object_get_member(obj, "retry_after") : NULL;
        if (n && JSON_NODE_HOLDS_VALUE(n))
            ra = (int)json_node_get_int(n);
        msg = msg ? msg : "请求过于频繁，请稍后再试";
        op_result(o, FALSE, "rate_limited", msg, ra);
        return;
    }
    if (code && !strcmp(code, "totp_required")) {
        op_result(o, FALSE, "totp_required", "需要两步验证码", 0);
        return;
    }
    if (code && !strcmp(code, "totp_invalid")) {
        op_result(o, FALSE, "totp_invalid", "两步验证码错误", 0);
        return;
    }
    if (code && !strcmp(code, "invalid_credentials")) {
        op_result(o, FALSE, "invalid_credentials", "邮箱、密码或恢复码错误",
                  0);
        return;
    }
    if (code && !strcmp(code, "email_taken")) {
        op_result(o, FALSE, "email_taken", "该邮箱已注册", 0);
        return;
    }
    op_result(o, FALSE, code ? code : "server_error",
              msg ? msg : "服务器错误", 0);
}

/* ============================ login ============================ */

static void login_post_done(HbSyncService *s, guint st, const char *body,
                            JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    hb_err e;
    (void)s;
    if (st == 200) {
        if (!o->vk_out)
            o->vk_out = sodium_malloc(32);
        {
            bool te = FALSE;
            e = hb_sync_login_parse(body, o->wrap_key, o->vk_out,
                                    &o->account_id, &o->dev_id, &o->token,
                                    &te);
            o->totp_enabled = te;
        }
        if (e == HB_OK && o->gen == s->gen) {
            finish_signin(o);
            return;
        }
        op_result(o, FALSE, "server_error", "服务器响应异常", 0);
    } else {
        op_fail_from_resp(o, st, obj);
    }
    op_end(o);
}

static void login_after_work(OpCtx *o)
{
    if (o->err != HB_OK || o->gen != o->svc->gen) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(o->svc, o->server, SOUP_METHOD_POST, "/v1/login", o->body,
         FALSE, NULL, login_post_done, o);
}

static void login_prelogin_done(HbSyncService *s, guint st, const char *body,
                                JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    (void)s;
    if (st == 200 &&
        hb_sync_prelogin_parse(body, o->salt, &o->ops, &o->mem) == HB_OK) {
        schedule_work(o, login_after_work);
        return;
    }
    op_fail_from_resp(o, st, obj);
    op_end(o);
}

void hb_sync_service_login(HbSyncService *s, const char *server,
                           const char *email, const char *password,
                           const char *totp, HbSyncOpCb cb, gpointer ud)
{
    OpCtx *o = op_new(s, OP_LOGIN, cb, ud);
    char *em = norm_email(email);
    char *body;
    o->server = g_strdup(server);
    o->email = em;
    o->password = nfkc(password);
    o->totp = totp && *totp ? g_strdup(totp) : NULL;
    body = NULL;
    if (hb_sync_prelogin_body(em, &body) != HB_OK) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(s, o->server, SOUP_METHOD_POST, "/v1/prelogin", body, FALSE, NULL,
         login_prelogin_done, o);
    free(body);
}

/* ============================ register ============================ */

static void register_post_done(HbSyncService *s, guint st, const char *body,
                               JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    (void)s;
    if (st == 200 &&
        hb_sync_auth_resp_parse(body, &o->account_id, &o->dev_id,
                                &o->token) == HB_OK) {
        finish_signin(o);
        return;
    }
    if (st == 200)
        op_result(o, FALSE, "server_error", "服务器响应异常", 0);
    else
        op_fail_from_resp(o, st, obj);
    op_end(o);
}

static void register_after_work(OpCtx *o)
{
    if (o->err != HB_OK || o->gen != o->svc->gen) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(o->svc, o->server, SOUP_METHOD_POST, "/v1/register", o->body,
         FALSE, NULL, register_post_done, o);
}

void hb_sync_service_register(HbSyncService *s, const char *server,
                              const char *email, const char *password,
                              HbSyncOpCb cb, gpointer ud)
{
    OpCtx *o = op_new(s, OP_REGISTER, cb, ud);
    o->vk_out = sodium_malloc(32);
    o->server = g_strdup(server);
    o->email = norm_email(email);
    o->password = nfkc(password);
    schedule_work(o, register_after_work);
}

/* ============================ recover ============================ */

static void recover_finish_done(HbSyncService *s, guint st, const char *body,
                                JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    (void)s;
    if (st == 200 &&
        hb_sync_auth_resp_parse(body, &o->account_id, &o->dev_id,
                                &o->token) == HB_OK) {
        finish_signin(o);
        return;
    }
    if (st == 200)
        op_result(o, FALSE, "server_error", "服务器响应异常", 0);
    else
        op_fail_from_resp(o, st, obj);
    op_end(o);
}

static void recover_after_work(OpCtx *o)
{
    if (o->err != HB_OK || o->gen != o->svc->gen) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(o->svc, o->server, SOUP_METHOD_POST, "/v1/recover/finish", o->body,
         FALSE, NULL, recover_finish_done, o);
}

static void recover_start_done(HbSyncService *s, guint st, const char *body,
                               JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    (void)s;
    if (st == 200) {
        if (!o->vk_out)
            o->vk_out = sodium_malloc(32);
        if (hb_sync_recover_start_parse(body, o->words_in, o->vk_out,
                                        &o->recovery_token) == HB_OK) {
            schedule_work(o, recover_after_work);
            return;
        }
    }
    if (st == 200)
        op_result(o, FALSE, "server_error", "服务器响应异常", 0);
    else
        op_fail_from_resp(o, st, obj);
    op_end(o);
}

void hb_sync_service_recover(HbSyncService *s, const char *server,
                             const char *email, const char *words,
                             const char *new_password,
                             HbSyncOpCb cb, gpointer ud)
{
    OpCtx *o = op_new(s, OP_RECOVER, cb, ud);
    char *em = norm_email(email);
    char *body = NULL;
    o->server = g_strdup(server);
    o->email = em;
    o->words_in = g_strdup(words);
    o->new_password = nfkc(new_password);
    if (hb_sync_recover_start_body(em, words, &body) != HB_OK) {
        op_result(o, FALSE, "invalid_credentials", "恢复码无效", 0);
        op_end(o);
        return;
    }
    http(s, o->server, SOUP_METHOD_POST, "/v1/recover/start", body, FALSE, NULL,
         recover_start_done, o);
    free(body);
}

/* ============================ rotate / password ============================ */

static void simple_post_done(HbSyncService *s, guint st, const char *body,
                             JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    (void)s; (void)body;
    if (st == 200) {
        op_result(o, TRUE, NULL, NULL, 0);
    } else {
        op_fail_from_resp(o, st, obj);
    }
    op_end(o);
}

static void rotate_after_work(OpCtx *o)
{
    if (o->err != HB_OK || o->gen != o->svc->gen) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(o->svc, o->svc->server_url, SOUP_METHOD_POST, "/v1/recovery",
         o->body, TRUE, NULL, simple_post_done, o);
}

static void password_after_work(OpCtx *o)
{
    if (o->err != HB_OK || o->gen != o->svc->gen) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(o->svc, o->svc->server_url, SOUP_METHOD_POST, "/v1/password",
         o->body, TRUE, NULL, simple_post_done, o);
}

static void prelogin_then_done(HbSyncService *s, guint st, const char *body,
                               JsonObject *obj, gpointer ud)
{
    OpCtx *o = ud;
    void (*then)(OpCtx *) = o->kind == OP_ROTATE ? rotate_after_work
                                               : password_after_work;
    (void)s;
    if (st == 200 &&
        hb_sync_prelogin_parse(body, o->salt, &o->ops, &o->mem) == HB_OK) {
        schedule_work(o, then);
        return;
    }
    op_fail_from_resp(o, st, obj);
    op_end(o);
}

static void authed_prelogin(OpCtx *o, const char *email)
{
    char *body = NULL;
    if (hb_sync_prelogin_body(email, &body) != HB_OK) {
        op_result(o, FALSE, "server_error", "内部错误", 0);
        op_end(o);
        return;
    }
    http(o->svc, o->svc->server_url, SOUP_METHOD_POST, "/v1/prelogin", body,
         FALSE, NULL, prelogin_then_done, o);
    free(body);
}

void hb_sync_service_rotate_recovery(HbSyncService *s, const char *password,
                                     HbSyncOpCb cb, gpointer ud)
{
    OpCtx *o = op_new(s, OP_ROTATE, cb, ud);
    char *em = NULL, *aid = NULL, *srv = NULL, *dev = NULL;
    o->password = nfkc(password);
    if (s->vk) {
        o->vk_in = sodium_malloc(32);
        memcpy(o->vk_in, s->vk, 32);
    }
    hb_sync_get_account(s->sync, &aid, &em, &srv, &dev);
    if (!em) {
        g_free(aid); g_free(srv); g_free(dev);
        op_result(o, FALSE, "server_error", "未登录", 0);
        op_end(o);
        return;
    }
    o->email = em;
    g_free(aid); g_free(srv); g_free(dev);
    authed_prelogin(o, em);
}

void hb_sync_service_change_password(HbSyncService *s, const char *old_pw,
                                     const char *new_pw,
                                     HbSyncOpCb cb, gpointer ud)
{
    OpCtx *o = op_new(s, OP_PASSWORD, cb, ud);
    char *em = NULL, *aid = NULL, *srv = NULL, *dev = NULL;
    o->password = nfkc(old_pw);
    o->new_password = nfkc(new_pw);
    if (s->vk) {
        o->vk_in = sodium_malloc(32);
        memcpy(o->vk_in, s->vk, 32);
    }
    hb_sync_get_account(s->sync, &aid, &em, &srv, &dev);
    if (!em) {
        g_free(aid); g_free(srv); g_free(dev);
        op_result(o, FALSE, "server_error", "未登录", 0);
        op_end(o);
        return;
    }
    o->email = em;
    g_free(aid); g_free(srv); g_free(dev);
    authed_prelogin(o, em);
}

/* ============================ 简单 authed ops ============================ */

typedef struct {
    HbSyncService *svc;
    int        what;     /* 0 setup 1 enable 2 disable 3 devices 4 revoke */
    char      *code;
    char      *device_id;
    gboolean   wipe;
    HbSyncOpCb cb;
    gpointer   ud;
    char      *totp_secret, *totp_uri;
    GPtrArray *devices;
} SimpOp;

static void device_free_pub(HbSyncDevice *d)
{
    g_free(d->id); g_free(d->name); g_free(d->platform); g_free(d);
}

static void simp_free(SimpOp *o)
{
    wfree(&o->code);
    g_free(o->device_id);
    if (o->totp_secret) wfree(&o->totp_secret);
    g_free(o->totp_uri);
    if (o->devices) {
        for (guint _i = 0; _i < o->devices->len; _i++)
            device_free_pub(g_ptr_array_index(o->devices, _i));
        g_ptr_array_free(o->devices, TRUE);
    }
    g_free(o);
}

static void simp_end(SimpOp *o, gboolean ok, const char *code,
                     const char *msg, int ra)
{
    HbSyncOpResult r = {
        .ok = ok, .code = code, .message = msg, .retry_after = ra,
        .totp_secret = o->totp_secret, .totp_uri = o->totp_uri,
        .devices = o->devices,
    };
    o->totp_secret = o->totp_uri = NULL;
    o->devices = NULL;
    if (o->cb) o->cb(o->svc, &r, o->ud);
    o->svc->op_count--;
    emit_changed(o->svc);
    simp_free(o);
}

static void simp_done(HbSyncService *s, guint st, const char *body,
                      JsonObject *obj, gpointer ud)
{
    SimpOp *o = ud;
    (void)body;
    if (st == 0) { simp_end(o, FALSE, "network", "无法连接服务器", 0); return; }
    if (o->what == 0 && st == 200) {
        o->totp_secret = g_strdup(jstr(obj, "secret"));
        o->totp_uri = g_strdup(jstr(obj, "uri"));
        simp_end(o, TRUE, NULL, NULL, 0);
        return;
    }
    if (o->what == 3 && st == 200) {
        JsonNode *dn = obj ? json_object_get_member(obj, "devices") : NULL;
        o->devices = g_ptr_array_new();
        if (dn && JSON_NODE_HOLDS_ARRAY(dn)) {
            JsonArray *arr = json_node_get_array(dn);
            guint i, n = json_array_get_length(arr);
            for (i = 0; i < n; i++) {
                JsonObject *d = json_array_get_object_element(arr, i);
                HbSyncDevice *dev = g_new0(HbSyncDevice, 1);
                JsonNode *cn;
                dev->id = g_strdup(jstr(d, "id"));
                dev->name = g_strdup(jstr(d, "name"));
                dev->platform = g_strdup(jstr(d, "platform"));
                cn = json_object_get_member(d, "created");
                if (cn) dev->created = json_node_get_int(cn);
                cn = json_object_get_member(d, "last_seen");
                if (cn) dev->last_seen = json_node_get_int(cn);
                cn = json_object_get_member(d, "current");
                if (cn && JSON_NODE_HOLDS_VALUE(cn) &&
                    json_node_get_value_type(cn) == G_TYPE_BOOLEAN)
                    dev->current = json_node_get_boolean(cn);
                g_ptr_array_add(o->devices, dev);
            }
        }
        simp_end(o, TRUE, NULL, NULL, 0);
        return;
    }
    if (st == 200) {
        if (o->what == 1) s->totp_enabled = TRUE;
        if (o->what == 2) s->totp_enabled = FALSE;
        simp_end(o, TRUE, NULL, NULL, 0);
        return;
    }
    {
        const char *code = jstr(obj, "error");
        const char *msg = jstr(obj, "message");
        int ra = 0;
        JsonNode *n;
        if (st == 429) {
            n = obj ? json_object_get_member(obj, "retry_after") : NULL;
            if (n) ra = (int)json_node_get_int(n);
        }
        simp_end(o, FALSE, code ? code : "server_error",
                 msg ? msg : "操作失败", ra);
    }
}

static void simp_go(HbSyncService *s, SimpOp *o)
{
    const char *method = SOUP_METHOD_POST;
    char *path = NULL;
    const char *body = NULL;
    g_autofree char *owned_path = NULL, *owned_body = NULL;
    s->op_count++;
    emit_changed(s);
    switch (o->what) {
    case 0: path = "/v1/totp/setup"; body = "{}"; break;
    case 1:
        path = "/v1/totp/enable";
        owned_body = g_strdup_printf("{\"code\":\"%s\"}", o->code);
        body = owned_body;
        break;
    case 2:
        path = "/v1/totp/disable";
        owned_body = g_strdup_printf("{\"code\":\"%s\"}", o->code);
        body = owned_body;
        break;
    case 3: method = SOUP_METHOD_GET; path = "/v1/devices"; break;
    case 4:
        owned_path = g_strdup_printf("/v1/devices/%s/revoke", o->device_id);
        path = owned_path;
        owned_body = g_strdup_printf("{\"wipe\":%s}",
                                     o->wipe ? "true" : "false");
        body = owned_body;
        break;
    }
    http(s, s->server_url, method, path, body, TRUE, NULL, simp_done, o);
}

void hb_sync_service_totp_setup(HbSyncService *s, HbSyncOpCb cb,
                                gpointer ud)
{
    SimpOp *o = g_new0(SimpOp, 1);
    o->svc = s; o->what = 0; o->cb = cb; o->ud = ud;
    simp_go(s, o);
}

void hb_sync_service_totp_enable(HbSyncService *s, const char *code,
                                 HbSyncOpCb cb, gpointer ud)
{
    SimpOp *o = g_new0(SimpOp, 1);
    o->svc = s; o->what = 1; o->cb = cb; o->ud = ud;
    o->code = g_strdup(code);
    simp_go(s, o);
}

void hb_sync_service_totp_disable(HbSyncService *s, const char *code,
                                  HbSyncOpCb cb, gpointer ud)
{
    SimpOp *o = g_new0(SimpOp, 1);
    o->svc = s; o->what = 2; o->cb = cb; o->ud = ud;
    o->code = g_strdup(code);
    simp_go(s, o);
}

void hb_sync_service_devices(HbSyncService *s, HbSyncOpCb cb,
                             gpointer ud)
{
    SimpOp *o = g_new0(SimpOp, 1);
    o->svc = s; o->what = 3; o->cb = cb; o->ud = ud;
    simp_go(s, o);
}

void hb_sync_service_revoke(HbSyncService *s, const char *device_id,
                            gboolean wipe, HbSyncOpCb cb, gpointer ud)
{
    SimpOp *o = g_new0(SimpOp, 1);
    o->svc = s; o->what = 4; o->cb = cb; o->ud = ud;
    o->device_id = g_strdup(device_id);
    o->wipe = wipe;
    simp_go(s, o);
}

/* ============================ 同步循环 ============================ */

static void pull_step(HbSyncService *s);
static void push_round(HbSyncService *s, int round);

static void watchdog_disarm(HbSyncService *s)
{
    if (s->watchdog_src) {
        g_source_remove(s->watchdog_src);
        s->watchdog_src = 0;
    }
}

static gboolean watchdog_cb(gpointer p)
{
    HbSyncService *s = p;
    s->watchdog_src = 0;
    if (!s->busy)
        return G_SOURCE_REMOVE;
    if (s->round_cancel)
        g_cancellable_cancel(s->round_cancel);
    loop_offline(s, "同步超时");
    return G_SOURCE_REMOVE;
}

static void loop_offline(HbSyncService *s, const char *why)
{
    watchdog_disarm(s);
    s->busy = FALSE;
    s->kick_pending = TRUE;
    g_free(s->last_error);
    s->last_error = g_strdup(why);
    set_state(s, HB_SYNC_OFFLINE);
    set_status_txt(s, "离线 · 重试中");
}

static void finish_round(HbSyncService *s)
{
    watchdog_disarm(s);
    s->busy = FALSE;
    s->last_sync = g_get_real_time() / G_USEC_PER_SEC;
    g_free(s->last_error);
    s->last_error = NULL;
    set_state(s, HB_SYNC_IDLE);
    if (s->kick_pending) {
        s->kick_pending = FALSE;
        sync_kick(s);
    } else {
        emit_changed(s);
    }
}

static void pull_done(HbSyncService *s, guint st, const char *body,
                      JsonObject *obj, gpointer ud)
{
    hb_sync_pull_stats ps;
    (void)ud; (void)obj;
    if (s->state == HB_SYNC_SIGNED_OUT)
        return;
    if (st == 200 && hb_sync_apply_pull(s->sync, body, &ps) == HB_OK) {
        if (ps.more) {
            pull_step(s);
        } else {
            push_round(s, 0);
        }
        return;
    }
    if (st == 0) {
        loop_offline(s, "网络错误");
    } else {
        loop_offline(s, "服务器错误");
    }
}

static void pull_step(HbSyncService *s)
{
    char *path = NULL;
    if (hb_sync_pull_path(s->sync, &path) != HB_OK) {
        loop_offline(s, "内部错误");
        return;
    }
    http(s, s->server_url, SOUP_METHOD_GET, path, NULL, TRUE,
         s->round_cancel, pull_done, NULL);
    free(path);
}

static void push_done(HbSyncService *s, guint st, const char *body,
                      JsonObject *obj, gpointer ud)
{
    hb_sync_push *p = ud;
    bool retry = FALSE;
    (void)obj;
    if (s->state == HB_SYNC_SIGNED_OUT) {
        hb_sync_push_free(p);
        return;
    }
    if (st == 200 && hb_sync_apply_push(s->sync, p, body, &retry) == HB_OK) {
        hb_sync_push_free(p);
        if (retry && s->push_round < PUSH_RETRY_MAX) {
            push_round(s, s->push_round + 1);
        } else {
            finish_round(s);
        }
        return;
    }
    hb_sync_push_free(p);
    if (st == 0) loop_offline(s, "网络错误");
    else loop_offline(s, "服务器错误");
}

static void push_round(HbSyncService *s, int round)
{
    hb_sync_push *p = NULL;
    s->push_round = round;
    if (hb_sync_build_push(s->sync, &p) != HB_OK || !p) {
        if (p) hb_sync_push_free(p);
        finish_round(s);
        return;
    }
    http(s, s->server_url, SOUP_METHOD_POST, "/v1/objects",
         hb_sync_push_body(p), TRUE, s->round_cancel, push_done, p);
}

static void sync_kick(HbSyncService *s)
{
    if (s->state == HB_SYNC_SIGNED_OUT || !s->has_vk || !s->token)
        return;
    if (s->busy) {
        s->kick_pending = TRUE;
        return;
    }
    s->busy = TRUE;
    s->round_start = g_get_monotonic_time();
    g_clear_object(&s->round_cancel);
    s->round_cancel = g_cancellable_new();
    watchdog_disarm(s);
    s->watchdog_src = g_timeout_add_seconds(WATCHDOG_SEC, watchdog_cb, s);
    set_status_txt(s, "同步中…");
    set_state(s, HB_SYNC_SYNCING);
    pull_step(s);
}

static void set_status_txt(HbSyncService *s, const char *txt)
{
    g_free(s->status);
    s->status = g_strdup(txt);
}

/* ============================ WebSocket ============================ */

static void ws_schedule(HbSyncService *s);

static void ws_on_msg(SoupWebsocketConnection *c, gint type, GBytes *msg,
                      gpointer ud)
{
    HbSyncService *s = ud;
    const char *d;
    gsize n;
    (void)c;
    s->ws_last_rx = g_get_monotonic_time();
    if (type != SOUP_WEBSOCKET_DATA_TEXT)
        return;
    d = g_bytes_get_data(msg, &n);
    if (!d) return;
    {
        g_autofree char *txt = g_strndup(d, n);
        if (strstr(txt, "\"revoked\"")) {
            gboolean wipe = strstr(txt, "\"wipe\":true") != NULL;
            sign_out_local(s, wipe, NULL);
            hb_notify_post("err", "此设备已被吊销",
                           wipe ? "账号已退出，本地数据已擦除" : "账号已退出",
                           NULL);
            return;
        }
        if (strstr(txt, "\"changed\""))
            sync_kick(s);
    }
}

static void ws_on_closed(SoupWebsocketConnection *c, gpointer ud)
{
    HbSyncService *s = ud;
    (void)c;
    s->ws_open = FALSE;
    g_clear_object(&s->ws);
    if (s->state != HB_SYNC_SIGNED_OUT)
        ws_schedule(s);
    emit_changed(s);
}

static void ws_on_error(SoupWebsocketConnection *c, GError *err,
                        gpointer ud)
{
    (void)c; (void)err; (void)ud;
    /* closed 信号随后会处理重连 */
}

/* 存活检测：keepalive_interval 让 libsoup 定期发 ping，服务端回 pong 经
 * "pong" 信号刷新 last_rx；任何 message 同样刷新。超过 WS_SILENCE_SEC
 * 无任何帧即判半开（服务端被冻结/静默消失），关闭后按 backoff 重连。
 * 注：服务端入站 ping 由 libsoup 自动 pong，无法观察——故以我方 ping 的
 * 回包作为活性证据。 */
static void ws_on_pong(SoupWebsocketConnection *c, GBytes *data, gpointer ud)
{
    HbSyncService *s = ud;
    (void)c; (void)data;
    s->ws_last_rx = g_get_monotonic_time();
}

static gboolean ws_check_cb(gpointer p)
{
    HbSyncService *s = p;
    if (s->ws && s->ws_open &&
        g_get_monotonic_time() - s->ws_last_rx >
            (gint64)WS_SILENCE_SEC * G_USEC_PER_SEC) {
        ws_close_conn(s);
        ws_schedule(s);
        /* 顺手探一次 HTTP——若整个服务端已死，同步轮会以 I/O 超时
         * 转入 OFFLINE，而不必等下一个轮询周期。 */
        sync_kick(s);
    }
    return G_SOURCE_CONTINUE;
}

static void ws_cb(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbSyncService *s = ud;
    GError *err = NULL;
    SoupWebsocketConnection *c =
        soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
    s->ws_connecting = FALSE;
    if (!c) {
        if (err) g_error_free(err);
        if (s->state != HB_SYNC_SIGNED_OUT)
            ws_schedule(s);
        emit_changed(s);
        return;
    }
    s->ws = c;
    s->ws_open = TRUE;
    s->ws_backoff = 0;
    s->ws_last_rx = g_get_monotonic_time();
    g_signal_connect(c, "message", G_CALLBACK(ws_on_msg), s);
    g_signal_connect(c, "pong", G_CALLBACK(ws_on_pong), s);
    g_signal_connect(c, "closed", G_CALLBACK(ws_on_closed), s);
    g_signal_connect(c, "error", G_CALLBACK(ws_on_error), s);
    soup_websocket_connection_set_keepalive_interval(c, WS_PING_SEC);
    if (!s->ws_check_src)
        s->ws_check_src = g_timeout_add_seconds(WS_CHECK_SEC, ws_check_cb,
                                                s);
    emit_changed(s);
    /* 断线重连后主动对齐：离线期间可能错过 changed，且本地脏
     * 数据待推（poll 只在 ws 断开时触发）。 */
    sync_kick(s);
}

static char *ws_url(HbSyncService *s)
{
    const char *u = s->server_url;
    if (g_str_has_prefix(u, "https://"))
        return g_strdup_printf("wss://%s/v1/ws", u + 8);
    if (g_str_has_prefix(u, "http://"))
        return g_strdup_printf("ws://%s/v1/ws", u + 7);
    return NULL;
}

static void ws_connect(HbSyncService *s)
{
    g_autofree char *url = NULL;
    SoupMessage *m;
    if (s->ws_connecting || s->ws_open || !s->token ||
        s->state == HB_SYNC_SIGNED_OUT)
        return;
    url = ws_url(s);
    if (!url) return;
    m = soup_message_new(SOUP_METHOD_GET, url);
    {
        g_autofree char *a = g_strdup_printf("Bearer %s", s->token);
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", a);
    }
    s->ws_connecting = TRUE;
    soup_session_websocket_connect_async(s->sess, m, NULL, NULL,
                                         G_PRIORITY_DEFAULT, NULL,
                                         ws_cb, s);
    g_object_unref(m);
}

static gboolean ws_retry_cb(gpointer p)
{
    HbSyncService *s = p;
    s->ws_retry_src = 0;
    ws_connect(s);
    return G_SOURCE_REMOVE;
}

static void ws_schedule(HbSyncService *s)
{
    int d;
    if (s->ws_retry_src || s->state == HB_SYNC_SIGNED_OUT)
        return;
    d = BACKOFF[MIN(s->ws_backoff, (int)G_N_ELEMENTS(BACKOFF) - 1)];
    if (s->ws_backoff < (int)G_N_ELEMENTS(BACKOFF) - 1)
        s->ws_backoff++;
    s->ws_retry_src = g_timeout_add_seconds(d, ws_retry_cb, s);
}

static void ws_close_conn(HbSyncService *s)
{
    if (s->ws_retry_src) {
        g_source_remove(s->ws_retry_src);
        s->ws_retry_src = 0;
    }
    if (s->ws_check_src) {
        g_source_remove(s->ws_check_src);
        s->ws_check_src = 0;
    }
    s->ws_connecting = FALSE;
    s->ws_open = FALSE;
    if (s->ws) {
        g_signal_handlers_disconnect_by_data(s->ws, s);
        soup_websocket_connection_close(s->ws, 1000, NULL);
        g_clear_object(&s->ws);
    }
}

/* ============================ 触发器 ============================ */

static gboolean debounce_cb(gpointer p)
{
    HbSyncService *s = p;
    s->debounce_src = 0;
    sync_kick(s);
    return G_SOURCE_REMOVE;
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbSyncService *s = ud;
    (void)st; (void)type; (void)id;
    if (s->state == HB_SYNC_SIGNED_OUT)
        return;
    /* 拉取应用的写入也触发 changed，但 pulled 对象已标干净，
     * build_push 无脏数据即空跑 —— 不会循环。 */
    if (!s->debounce_src)
        s->debounce_src = g_timeout_add(PUSH_DEBOUNCE_MS, debounce_cb, s);
}

static gboolean poll_cb(gpointer p)
{
    HbSyncService *s = p;
    /* WS 断开时定期重试；WS 在但 OFFLINE（如看门狗/超时）同样要重试，
     * 否则离线状态永远无法自愈。 */
    if (!s->busy && s->state != HB_SYNC_SIGNED_OUT &&
        (!s->ws_open || s->state == HB_SYNC_OFFLINE))
        sync_kick(s);
    return G_SOURCE_CONTINUE;
}

static void on_net_changed(GNetworkMonitor *mon, gboolean available,
                           gpointer ud)
{
    HbSyncService *s = ud;
    (void)mon;
    if (!available || s->state == HB_SYNC_SIGNED_OUT)
        return;
    s->ws_backoff = 0;
    ws_connect(s);
    sync_kick(s);
}

static void start_triggers(HbSyncService *s)
{
    if (!s->poll_src)
        s->poll_src = g_timeout_add_seconds(POLL_SEC, poll_cb, s);
}

static void stop_triggers(HbSyncService *s)
{
    if (s->poll_src) { g_source_remove(s->poll_src); s->poll_src = 0; }
    if (s->debounce_src) { g_source_remove(s->debounce_src); s->debounce_src = 0; }
}

/* ============================ 登出 / 吊销 ============================ */

static void sign_out_local(HbSyncService *s, gboolean wipe, const char *why)
{
    ws_close_conn(s);
    stop_triggers(s);
    watchdog_disarm(s);
    if (s->round_cancel)
        g_cancellable_cancel(s->round_cancel);
    s->busy = FALSE;
    s->kick_pending = FALSE;
    s->merge_pending = FALSE;
    if (wipe)
        hb_sync_wipe(s->sync);
    else
        hb_sync_logout_local(s->sync);
    wfree(&s->token);
    g_free(s->server_url);
    s->server_url = NULL;
    creds_wipe(s);
    if (s->vk) {
        sodium_memzero(s->vk, 32);
        sodium_free(s->vk);
        s->vk = NULL;
    }
    s->has_vk = FALSE;
    s->gen++;
    s->totp_enabled = FALSE;
    hb_sync_clear_vk(s->sync);
    g_free(s->last_error);
    s->last_error = g_strdup(why);
    set_state(s, HB_SYNC_SIGNED_OUT);
}

void hb_sync_service_logout(HbSyncService *s, gboolean wipe)
{
    /* 尽力通知服务端吊销设备（响应无需处理——本地无论如何登出） */
    if (s->token && s->server_url)
        http(s, s->server_url, SOUP_METHOD_POST, "/v1/logout", "{}", FALSE, NULL,
             logout_ignore_cb, NULL);
    sign_out_local(s, wipe, NULL);
}

static void logout_ignore_cb(HbSyncService *s, guint st, const char *b,
                             JsonObject *o, gpointer u)
{
    (void)s; (void)st; (void)b; (void)o; (void)u;
}

void hb_sync_service_confirm_merge(HbSyncService *s, gboolean merge)
{
    s->merge_pending = FALSE;
    if (!merge)
        hb_sync_wipe(s->sync);
    start_triggers(s);
    ws_connect(s);
    sync_kick(s);
}

/* ============================ 启动 / 恢复 ============================ */

static void try_resume(HbSyncService *s)
{
    char *aid = NULL, *em = NULL, *srv = NULL, *dev = NULL;
    if (hb_sync_get_account(s->sync, &aid, &em, &srv, &dev) != HB_OK ||
        !aid || !srv) {
        g_free(aid); g_free(em); g_free(srv); g_free(dev);
        return;
    }
    if (creds_load(s, ensure_vk(s), &s->token)) {
        s->has_vk = TRUE;
        s->server_url = srv;
        srv = NULL;
        hb_sync_set_vk(s->sync, s->vk);
        set_state(s, HB_SYNC_IDLE);
        start_triggers(s);
        ws_connect(s);
        sync_kick(s);
    }
    g_free(aid); g_free(em); g_free(srv); g_free(dev);
}

/* ============================ API ============================ */

HbSyncService *hb_sync_service_default(void)
{
    if (!g_svc) {
        HbSyncService *s = g_object_new(HB_TYPE_SYNC_SERVICE, NULL);
        s->store = hb_store_default();
        s->sync = hb_sync_new(hb_store_vault(s->store));
        s->sess = soup_session_new_with_options("timeout", 30, NULL);
        s->pool = g_thread_pool_new(work_runner, NULL, HASH_THREADS, FALSE,
                                    NULL);
        s->mon = g_network_monitor_get_default();
        s->state = HB_SYNC_SIGNED_OUT;
        g_signal_connect(s->store, "changed",
                         G_CALLBACK(on_store_changed), s);
        g_signal_connect(s->mon, "network-changed",
                         G_CALLBACK(on_net_changed), s);
        try_resume(s);
        g_svc = s;
    }
    return g_svc;
}

HbSyncState hb_sync_service_state(HbSyncService *s) { return s->state; }

const char *hb_sync_service_status(HbSyncService *s)
{
    if (s->status) return s->status;
    switch (s->state) {
    case HB_SYNC_SIGNED_OUT: return "未登录";
    case HB_SYNC_SYNCING:    return "同步中…";
    case HB_SYNC_OFFLINE:    return "离线 · 重试中";
    case HB_SYNC_ERROR:      return s->last_error ? s->last_error : "错误";
    default:                 return "已同步";
    }
}

const char *hb_sync_service_email(HbSyncService *s)
{
    char *em = NULL, *a = NULL, *srv = NULL, *d = NULL;
    const char *ret = NULL;
    static char buf[256];
    hb_sync_get_account(s->sync, &a, &em, &srv, &d);
    if (em) {
        g_strlcpy(buf, em, sizeof buf);
        ret = buf;
    }
    g_free(a); g_free(em); g_free(srv); g_free(d);
    return ret;
}

const char *hb_sync_service_server(HbSyncService *s)
{
    return s->server_url;
}

const char *hb_sync_service_last_error(HbSyncService *s)
{
    return s->last_error;
}

gint64 hb_sync_service_last_sync(HbSyncService *s)
{
    return s->last_sync;
}

gsize hb_sync_service_conflicts(HbSyncService *s)
{
    return hb_sync_conflict_count(s->sync);
}

gboolean hb_sync_service_totp_enabled(HbSyncService *s)
{
    return s->totp_enabled;
}

gboolean hb_sync_service_busy(HbSyncService *s)
{
    return s->op_count > 0 || s->busy;
}

hb_sync *hb_sync_service_sync(HbSyncService *s) { return s->sync; }

void hb_sync_service_sync_now(HbSyncService *s)
{
    sync_kick(s);
}

void hb_sync_service_set_scope(HbSyncService *s, const char *scope,
                               gboolean on)
{
    if (hb_sync_scope_set(s->sync, scope, on) == HB_OK)
        sync_kick(s);
}

char *hb_sync_check_server(const char *url)
{
    g_autofree char *t = g_strdup(url ? url : "");
    g_autoptr(GError) err = NULL;
    GUri *u;
    const char *scheme, *host;
    g_strstrip(t);
    if (!*t) return NULL;
    u = g_uri_parse(t, G_URI_FLAGS_NONE, &err);
    if (!u) return NULL;
    scheme = g_uri_get_scheme(u);
    host = g_uri_get_host(u);
    if (!scheme || !host || !*host) { g_uri_unref(u); return NULL; }
    if (!strcmp(scheme, "http")) {
        g_autoptr(GInetAddress) ia = g_inet_address_new_from_string(host);
        gboolean loop = ia ? g_inet_address_get_is_loopback(ia)
                         : !strcmp(host, "localhost");
        if (!loop) { g_uri_unref(u); return NULL; }
    } else if (strcmp(scheme, "https")) {
        g_uri_unref(u);
        return NULL;
    }
    {
        const char *path = g_uri_get_path(u);
        char *out = path && *path && strcmp(path, "/")
                        ? g_strdup(t)
                        : g_strdup_printf("%s://%s:%d", scheme, host,
                                          g_uri_get_port(u));
        g_uri_unref(u);
        return out;
    }
}

static void hb_sync_service_class_init(HbSyncServiceClass *klass)
{
    sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                     G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                     NULL, G_TYPE_NONE, 0);
}

static void hb_sync_service_init(HbSyncService *s)
{
    s->state = HB_SYNC_SIGNED_OUT;
}
