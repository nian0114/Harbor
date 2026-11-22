/* FR-12 共享服务。传输层（REST + relay WS），核心加密/路由全在
 * hb_share_owner / hb_share_peer（core/src/share.c）。 */

#include "hb-share.h"
#include "hb-sync.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <string.h>
#include <glib-unix.h>
#include <glib/gi18n.h>

#define WS_PAYLOAD_MAX (8u * 1024 * 1024)   /* > relay 4 MiB 上限 */
#define WS_PING_SEC 5

struct _HbShareService {
    GObject       parent;
    SoupSession  *sess;
    GHashTable   *owners;    /* share_id → ShareOwnerCtx */
    GPtrArray    *peers;     /* hb_share_peer_ctx* */
    GHashTable   *by_sess;   /* hb_session* → share_id (char*) */
};

typedef struct {
    HbShareService *svc;         /* borrowed */
    char           *share_id;
    char           *title, *host;
    hb_share_owner *eng;
    hb_session     *sess;        /* borrowed */
    SoupWebsocketConnection *ws;
    guint           fd_src, tick_src;
    gboolean        active;      /* POST+WS 完成 */
    gboolean        stopping;
    hb_share_owner_cb cb;
    gpointer        cb_ud;
    char           *tab_id;      /* 页绑定的 tab（卡片点击切换用） */
    GHashTable     *parts;       /* guint id → SvcPart（对话框打开时种子） */
} ShareOwnerCtx;

typedef struct {
    guint    id;
    char    *name;
    char    *device;
    gboolean guest;
} SvcPart;

static void svc_part_free(gpointer p)
{
    SvcPart *sp = p;
    g_free(sp->name); g_free(sp->device); g_free(sp);
}

struct hb_share_peer_ctx {
    HbShareService *svc;
    char           *share_id;
    char           *role;        /* "member"|"guest" */
    hb_share_peer  *eng;
    SoupWebsocketConnection *ws;
    guint           fd_src, tick_src;
    gboolean        ended;
    hb_share_join_cb cb;
    gpointer        cb_ud;
};

G_DEFINE_TYPE(HbShareService, hb_share_service, G_TYPE_OBJECT)

enum {
    SIG_SHARE_CHANGED, SIG_OWNER_EVENT,
    SIG_PEER_EVENT, SIG_PEER_ENDED, SIG_PEER_ERROR, N_SIGS
};
static guint sigs[N_SIGS];
static HbShareService *g_svc;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static HbSyncService *sync_svc(void) { return hb_sync_service_default(); }

static gboolean signed_in(void)
{
    return hb_sync_service_state(sync_svc()) != HB_SYNC_SIGNED_OUT &&
           hb_sync_service_token(sync_svc()) != NULL;
}

/* http(s):// → ws(s):// */
static char *ws_url(const char *server, const char *path_q)
{
    if (g_str_has_prefix(server, "https://"))
        return g_strdup_printf("wss://%s%s", server + 8, path_q);
    if (g_str_has_prefix(server, "http://"))
        return g_strdup_printf("ws://%s%s", server + 7, path_q);
    return g_strdup_printf("wss://%s%s", server, path_q);
}

static char *http_url(const char *server, const char *path)
{
    return g_strdup_printf("%s%s", server, path);
}

typedef void (*hb_http_cb)(guint status, const char *body, gsize body_n,
                           gpointer ud);

typedef struct { hb_http_cb cb; gpointer ud; } HttpCtx;

static void http_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HttpCtx *c = ud;
    GBytes *b = soup_session_send_and_read_finish(
        SOUP_SESSION(src), res, NULL);
    SoupMessage *m = soup_session_get_async_result_message(
        SOUP_SESSION(src), res);
    guint st = m ? soup_message_get_status(m) : 0;
    const char *d = NULL; gsize n = 0;
    if (b) {
        d = g_bytes_get_data(b, &n);
        if (c->cb) c->cb(st, d, n, c->ud);
        g_bytes_unref(b);
    } else if (c->cb) {
        c->cb(st, NULL, 0, c->ud);
    }
    g_free(c);
}

/* 带 Bearer 的 JSON REST；body NULL → 无请求体（GET/DELETE）。 */
static void http(HbShareService *s, const char *method, const char *path,
                 const char *json, gboolean auth, hb_http_cb cb,
                 gpointer ud)
{
    const char *server = hb_sync_service_server(sync_svc());
    SoupMessage *m;
    char *url, *a;
    HttpCtx *c;
    if (!server) {
        if (cb) cb(0, NULL, 0, ud);
        return;
    }
    url = http_url(server, path);
    m = soup_message_new(method, url);
    g_free(url);
    if (auth && hb_sync_service_token(sync_svc())) {
        a = g_strdup_printf("Bearer %s",
                            hb_sync_service_token(sync_svc()));
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", a);
        g_free(a);
    }
    if (json) {
        GBytes *bb = g_bytes_new(json, strlen(json));
        soup_message_set_request_body_from_bytes(m, "application/json",
                                                 bb);
        g_bytes_unref(bb);
    }
    c = g_new0(HttpCtx, 1);
    c->cb = cb; c->ud = ud;
    soup_session_send_and_read_async(s->sess, m, G_PRIORITY_DEFAULT, NULL,
                                     http_done, c);
    g_object_unref(m);
}

static void owner_ctx_free(ShareOwnerCtx *c);
static void peer_ctx_free(hb_share_peer_ctx *c);

/* ------------------------------------------------------------------ */
/* owner frame/event pump                                              */

static void owner_track(ShareOwnerCtx *c, const hb_share_event *ev)
{
    SvcPart *sp;
    switch (ev->type) {
    case HB_SH_EV_JOINED:
    case HB_SH_EV_PAKE_OK:
        sp = g_new0(SvcPart, 1);
        sp->id = ev->id;
        sp->name = g_strdup(ev->name && *ev->name ? ev->name : "guest");
        sp->device = g_strdup(ev->device ? ev->device : "");
        sp->guest = !(ev->device && *ev->device);
        g_hash_table_replace(c->parts, GUINT_TO_POINTER(ev->id), sp);
        break;
    case HB_SH_EV_LEFT:
        g_hash_table_remove(c->parts, GUINT_TO_POINTER(ev->id));
        break;
    default:
        break;
    }
}

static void owner_emit(HbShareService *s, ShareOwnerCtx *c)
{
    hb_share_event ev;
    while (hb_share_owner_next_event(c->eng, &ev)) {
        owner_track(c, &ev);
        /* (share_id, ev_type:int, part_id:uint, name, device, text) */
        g_signal_emit(s, sigs[SIG_OWNER_EVENT], 0, c->share_id,
                      (int)ev.type, ev.id,
                      ev.name ? ev.name : "", ev.device ? ev.device : "",
                      ev.text ? ev.text : "");
        hb_share_event_clear(&ev);
    }
    g_signal_emit(s, sigs[SIG_SHARE_CHANGED], 0, c->share_id);
}

static void owner_drain(ShareOwnerCtx *c)
{
    hb_share_frame f;
    while (hb_share_owner_next_frame(c->eng, &f)) {
        if (c->ws &&
            soup_websocket_connection_get_state(c->ws) ==
                SOUP_WEBSOCKET_STATE_OPEN) {
            if (f.is_text)
                soup_websocket_connection_send_text(c->ws, (const char *)f.data);
            else
                soup_websocket_connection_send_binary(c->ws, f.data,
                                                      f.len);
        }
        hb_share_frame_clear(&f);
    }
}

static gboolean owner_fd_cb(int fd, GIOCondition cond, gpointer ud)
{
    ShareOwnerCtx *c = ud;
    (void)fd;
    if (cond & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;
    owner_drain(c);
    owner_emit(c->svc, c);
    return G_SOURCE_CONTINUE;
}

static gboolean owner_tick_cb(gpointer ud)
{
    ShareOwnerCtx *c = ud;
    int64_t ms;
    c->tick_src = 0;
    hb_share_owner_tick(c->eng);
    owner_drain(c);
    owner_emit(c->svc, c);
    ms = hb_share_owner_next_ms(c->eng);
    if (ms >= 0) {
        c->tick_src = g_timeout_add((guint)MAX(ms, 50), owner_tick_cb, c);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_REMOVE;
}

static void owner_arm_tick(ShareOwnerCtx *c)
{
    int64_t ms;
    if (c->tick_src) {
        g_source_remove(c->tick_src);
        c->tick_src = 0;
    }
    ms = hb_share_owner_next_ms(c->eng);
    if (ms >= 0)
        c->tick_src = g_timeout_add((guint)MAX(ms, 50), owner_tick_cb, c);
}

static void owner_ws_msg(SoupWebsocketConnection *w, gint type, GBytes *msg,
                         gpointer ud)
{
    ShareOwnerCtx *c = ud;
    const char *d; gsize n;
    (void)w;
    d = g_bytes_get_data(msg, &n);
    if (!d) return;
    hb_share_owner_incoming(c->eng, (const uint8_t *)d, n,
                            type == SOUP_WEBSOCKET_DATA_TEXT);
    owner_drain(c);
    owner_emit(c->svc, c);
}

static void owner_ws_closed(SoupWebsocketConnection *w, gpointer ud)
{
    ShareOwnerCtx *c = ud;
    (void)w;
    g_clear_object(&c->ws);
    if (c->active && !c->stopping) {
        /* relay 断开 = share 结束（owner_gone 语义在服务端侧生效） */
        hb_share_service_stop(c->svc, c->share_id);
    }
}

static void owner_ws_err(SoupWebsocketConnection *w, GError *e, gpointer ud)
{ (void)w; (void)e; (void)ud; }

static void owner_ctx_free(ShareOwnerCtx *c)
{
    if (!c) return;
    if (c->fd_src) g_source_remove(c->fd_src);
    if (c->tick_src) g_source_remove(c->tick_src);
    if (c->ws) {
        g_signal_handlers_disconnect_by_data(c->ws, c);
        if (soup_websocket_connection_get_state(c->ws) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(c->ws, 1000, NULL);
        g_object_unref(c->ws);
    }
    g_clear_pointer(&c->eng, hb_share_owner_free);
    g_free(c->share_id); g_free(c->title); g_free(c->host);
    g_free(c->tab_id);
    g_clear_pointer(&c->parts, g_hash_table_destroy);
    g_free(c);
}

/* ------------------------- start sequence -------------------------- */

static void owner_finish_start(ShareOwnerCtx *c);

static void owner_ws_ready(GObject *src, GAsyncResult *res, gpointer ud)
{
    ShareOwnerCtx *c = ud;
    GError *err = NULL;
    SoupWebsocketConnection *w =
        soup_session_websocket_connect_finish(SOUP_SESSION(src), res,
                                              &err);
    if (!w) {
        char *emsg = g_strdup(err ? err->message : "ws connect failed");
        g_clear_error(&err);
        if (c->cb)
            c->cb(c->svc, NULL, emsg, c->cb_ud);
        g_free(emsg);
        /* POST 已建 share：DELETE 清理 */
        {
            char *p = g_strdup_printf("/v1/shares/%s", c->share_id);
            http(c->svc, SOUP_METHOD_DELETE, p, NULL, TRUE,
                 NULL, NULL);
            g_free(p);
        }
        g_hash_table_remove(c->svc->owners, c->share_id);
        return;
    }
    c->ws = w;
    soup_websocket_connection_set_max_incoming_payload_size(w,
        WS_PAYLOAD_MAX);
    soup_websocket_connection_set_keepalive_interval(w, WS_PING_SEC);
    g_signal_connect(w, "message", G_CALLBACK(owner_ws_msg), c);
    g_signal_connect(w, "closed", G_CALLBACK(owner_ws_closed), c);
    g_signal_connect(w, "error", G_CALLBACK(owner_ws_err), c);
    owner_finish_start(c);
}

static void owner_finish_start(ShareOwnerCtx *c)
{
    c->active = TRUE;
    hb_share_owner_attach(c->eng, c->sess);
    c->fd_src = g_unix_fd_add(hb_share_owner_fd(c->eng),
                              G_IO_IN | G_IO_ERR | G_IO_HUP,
                              owner_fd_cb, c);
    owner_arm_tick(c);
    owner_drain(c);   /* hello-ack 队列/初始帧 */
    g_signal_emit(c->svc, sigs[SIG_SHARE_CHANGED], 0, c->share_id);
    if (c->cb)
        c->cb(c->svc, c->share_id, NULL, c->cb_ud);
}

static void owner_post_done(guint st, const char *body, gsize n,
                            gpointer ud)
{
    ShareOwnerCtx *c = ud;
    (void)body; (void)n;
    if (st != 200) {
        if (c->cb)
            c->cb(c->svc, NULL, _("创建共享失败（服务器拒绝）"),
                  c->cb_ud);
        g_hash_table_remove(c->svc->owners, c->share_id);
        return;
    }
    {
        const char *server = hb_sync_service_server(sync_svc());
        const char *token = hb_sync_service_token(sync_svc());
        char *path = g_strdup_printf("/v1/share/%s/ws?role=owner",
                                     c->share_id);
        char *url = ws_url(server, path);
        SoupMessage *m = soup_message_new(SOUP_METHOD_GET, url);
        char *a = g_strdup_printf("Bearer %s", token);
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", a);
        g_free(a); g_free(url); g_free(path);
        soup_session_websocket_connect_async(c->svc->sess, m, NULL, NULL,
                                             G_PRIORITY_DEFAULT, NULL,
                                             owner_ws_ready, c);
        g_object_unref(m);
    }
}

void hb_share_service_start(HbShareService *s, hb_session *sess,
                            const char *title, const char *host,
                            int mode, hb_share_owner_cb cb, gpointer ud)
{
    ShareOwnerCtx *c;
    uint8_t *vk;
    char uuid[37], *meta, *body;
    if (!signed_in()) {
        if (cb) cb(s, NULL, _("未登录：请先在账号页登录"), ud);
        return;
    }
    vk = sodium_malloc(32);
    if (!vk || !hb_sync_service_vk_copy(sync_svc(), vk)) {
        if (vk) { sodium_memzero(vk, 32); sodium_free(vk); }
        if (cb) cb(s, NULL, _("账号密钥不可用"), ud);
        return;
    }
    {
        char *id = g_uuid_string_random();
        memcpy(uuid, id, sizeof uuid - 1);
        uuid[sizeof uuid - 1] = 0;
        g_free(id);
    }
    c = g_new0(ShareOwnerCtx, 1);
    c->svc = s;
    c->parts = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                     NULL, svc_part_free);
    c->share_id = g_strdup(uuid);
    c->title = g_strdup(title);
    c->host = g_strdup(host);
    c->sess = sess;
    c->cb = cb; c->cb_ud = ud;
    c->eng = hb_share_owner_new(uuid, vk, mode);
    sodium_memzero(vk, 32); sodium_free(vk);
    if (!c->eng) {
        owner_ctx_free(c);
        if (cb) cb(s, NULL, _("引擎创建失败"), ud);
        return;
    }
    g_hash_table_insert(s->owners, g_strdup(c->share_id), c);
    g_hash_table_insert(s->by_sess, sess, g_strdup(c->share_id));
    meta = hb_share_owner_meta(c->eng, title, host,
                               hb_sync_service_device_name(sync_svc()));
    body = g_strdup_printf("{\"share_id\":\"%s\",\"meta\":\"%s\"}",
                           c->share_id, meta ? meta : "");
    g_free(meta);
    http(s, SOUP_METHOD_POST, "/v1/shares", body, TRUE,
         owner_post_done, c);
    g_free(body);
}

void hb_share_service_stop(HbShareService *s, const char *share_id)
{
    ShareOwnerCtx *c;
    if (!share_id) return;
    c = g_hash_table_lookup(s->owners, share_id);
    if (!c) return;
    c->stopping = TRUE;
    if (c->active) {
        char *p = g_strdup_printf("/v1/shares/%s", share_id);
        http(s, SOUP_METHOD_DELETE, p, NULL, TRUE, NULL, NULL);
        g_free(p);
    }
    g_hash_table_remove(s->by_sess, c->sess);
    g_hash_table_remove(s->owners, share_id);   /* frees ctx */
    g_signal_emit(s, sigs[SIG_SHARE_CHANGED], 0, share_id);
}

void hb_share_service_stop_session(HbShareService *s, hb_session *sess)
{
    const char *id = g_hash_table_lookup(s->by_sess, sess);
    if (id)
        hb_share_service_stop(s, id);
}

const char *hb_share_service_share_for_session(HbShareService *s,
                                               hb_session *sess)
{
    return g_hash_table_lookup(s->by_sess, sess);
}

gboolean hb_share_service_session_shared(HbShareService *s,
                                         hb_session *sess)
{
    return g_hash_table_lookup(s->by_sess, sess) != NULL;
}

gboolean hb_share_service_starting(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = g_hash_table_lookup(s->owners, id);
    return c && !c->active;
}

static ShareOwnerCtx *own_ctx(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = id ? g_hash_table_lookup(s->owners, id) : NULL;
    return c && c->active ? c : NULL;
}

const char *hb_share_service_link(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    const char *server = hb_sync_service_server(sync_svc());
    static char *cached;   /* 单缓冲，借用语义 */
    if (!c || !server) return NULL;
    g_free(cached);
    cached = hb_share_owner_link(c->eng, server);
    return cached;
}

const char *hb_share_service_code(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    return c ? hb_share_owner_code(c->eng) : NULL;
}

int hb_share_service_mode(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    return c ? hb_share_owner_mode(c->eng) : HB_SHARE_VIEW;
}

int hb_share_service_count(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    return c ? hb_share_owner_count(c->eng) : 0;
}

static void collect_part(gpointer key, gpointer val, gpointer ud)
{
    SvcPart *sp = val;
    GList **out = ud;
    hb_share_part *row = g_new0(hb_share_part, 1);
    (void)key;
    row->id = sp->id;
    row->name = g_strdup(sp->name);
    row->device = g_strdup(sp->device);
    row->guest = sp->guest;
    *out = g_list_prepend(*out, row);
}

void hb_share_part_free(hb_share_part *p)
{
    if (!p) return;
    g_free(p->name); g_free(p->device); g_free(p);
}

/* 引擎无参与者枚举 API → service 随 owner 事件维护的当前表。 */
GList *hb_share_service_participants(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    GList *out = NULL;
    if (c && c->parts)
        g_hash_table_foreach(c->parts, collect_part, &out);
    return out;
}

static void put_meta(HbShareService *s, ShareOwnerCtx *c)
{
    char *meta = hb_share_owner_meta(c->eng, c->title, c->host,
                                     hb_sync_service_device_name(
                                         sync_svc()));
    char *body = g_strdup_printf("{\"meta\":\"%s\"}",
                                 meta ? meta : "");
    char *p = g_strdup_printf("/v1/shares/%s", c->share_id);
    http(s, SOUP_METHOD_PUT, p, body, TRUE, NULL, NULL);
    g_free(body); g_free(p); g_free(meta);
}

void hb_share_service_set_mode(HbShareService *s, const char *id, int mode)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    if (!c) return;
    hb_share_owner_set_mode(c->eng, mode);
    put_meta(s, c);
    owner_drain(c);
    owner_emit(s, c);
    owner_arm_tick(c);
}

void hb_share_service_regen(HbShareService *s, const char *id)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    char *p;
    if (!c) return;
    hb_share_owner_regen_guest(c->eng);
    p = g_strdup_printf("/v1/shares/%s/guest", id);
    http(s, SOUP_METHOD_POST, p, NULL, TRUE, NULL, NULL);
    g_free(p);
    owner_drain(c);
    owner_emit(s, c);
    g_signal_emit(s, sigs[SIG_SHARE_CHANGED], 0, id);
}

void hb_share_service_kick(HbShareService *s, const char *id, uint32_t pid)
{
    ShareOwnerCtx *c = own_ctx(s, id);
    if (!c) return;
    hb_share_owner_kick(c->eng, pid);
    owner_drain(c);
    owner_emit(s, c);
}

/* 页把自己的 tab id 绑给 share（本机共享卡片点击 → 切 tab）。 */
void hb_share_service_bind_tab(HbShareService *s, const char *share_id,
                               const char *tab_id)
{
    ShareOwnerCtx *c = g_hash_table_lookup(s->owners, share_id);
    if (!c) return;
    g_free(c->tab_id);
    c->tab_id = g_strdup(tab_id);
}

/* 本机活跃 shares（含 starting）：GList of HbShareOwnInfo*，调用方
 * g_list_free_full(g_free)。 */
GList *hb_share_service_own_shares(HbShareService *s)
{
    GList *out = NULL;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, s->owners);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        ShareOwnerCtx *c = v;
        HbShareOwnInfo *i = g_new0(HbShareOwnInfo, 1);
        i->share_id = g_strdup(c->share_id);
        i->title = g_strdup(c->title);
        i->host = g_strdup(c->host);
        i->tab_id = g_strdup(c->tab_id);
        i->mode = hb_share_owner_mode(c->eng);
        out = g_list_append(out, i);
    }
    return out;
}

void hb_share_own_info_free(gpointer p)
{
    HbShareOwnInfo *i = p;
    g_free(i->share_id); g_free(i->title);
    g_free(i->host); g_free(i->tab_id);
    g_free(i);
}

/* ------------------------------------------------------------------ */
/* peer list                                                           */

void hb_share_item_free(HbShareItem *it)
{
    g_free(it->share_id); g_free(it->meta_b64);
    g_free(it->owner_device); g_free(it);
}

typedef struct {
    HbShareService *s; hb_share_list_cb cb; gpointer ud;
} ListCtx;

static void list_done(guint st, const char *body, gsize n, gpointer ud)
{
    ListCtx *lc = ud;
    GList *items = NULL;
    g_autoptr(JsonParser) jp = NULL;
    JsonObject *o; JsonArray *arr; guint i, na;
    if (st != 200 || !body) {
        lc->cb(lc->s, NULL, _("获取共享列表失败"), lc->ud);
        g_free(lc); return;
    }
    jp = json_parser_new();
    json_parser_load_from_data(jp, body, n, NULL);
    o = json_node_get_object(json_parser_get_root(jp));
    arr = json_object_get_array_member(o, "shares");
    na = arr ? json_array_get_length(arr) : 0;
    for (i = 0; i < na; i++) {
        JsonObject *e = json_array_get_object_element(arr, i);
        HbShareItem *it = g_new0(HbShareItem, 1);
        const char *m;
        it->share_id = g_strdup(
            json_object_get_string_member(e, "share_id"));
        m = json_object_has_member(e, "meta")
            ? json_object_get_string_member(e, "meta") : NULL;
        it->meta_b64 = g_strdup(m);
        it->owner_device = g_strdup(
            json_object_has_member(e, "owner_device")
                ? json_object_get_string_member(e, "owner_device")
                : "");
        it->created = json_object_has_member(e, "created")
            ? json_object_get_int_member(e, "created") : 0;
        items = g_list_append(items, it);
    }
    lc->cb(lc->s, items, NULL, lc->ud);
    g_list_free_full(items, (GDestroyNotify)hb_share_item_free);
    g_free(lc);
}

void hb_share_service_list(HbShareService *s, hb_share_list_cb cb,
                           gpointer ud)
{
    ListCtx *lc;
    if (!signed_in()) {
        cb(s, NULL, _("未登录"), ud);
        return;
    }
    lc = g_new0(ListCtx, 1);
    lc->s = s; lc->cb = cb; lc->ud = ud;
    http(s, SOUP_METHOD_GET, "/v1/shares", NULL, TRUE, list_done, lc);
}

/* ------------------------------------------------------------------ */
/* peer join                                                           */

static void peer_emit(hb_share_peer_ctx *c)
{
    hb_share_event ev;
    while (hb_share_peer_next_event(c->eng, &ev)) {
        g_signal_emit(c->svc, sigs[SIG_PEER_EVENT], 0, c, &ev);
        hb_share_event_clear(&ev);
        /* 引擎结束（pake 失败/被踢/owner 停止）：立即收 ws。
         * 否则 guest 占用 relay 的 pending-PAKE 槽位直到页面销毁，
         * 下一次重试会被 4009 拒绝。 */
        if (ev.type == HB_SH_EV_ENDED)
            hb_share_peer_ctx_close(c);
    }
}

static void peer_drain(hb_share_peer_ctx *c)
{
    hb_share_frame f;
    while (hb_share_peer_next_frame(c->eng, &f)) {
        if (c->ws &&
            soup_websocket_connection_get_state(c->ws) ==
                SOUP_WEBSOCKET_STATE_OPEN) {
            if (f.is_text)
                soup_websocket_connection_send_text(c->ws, (const char *)f.data);
            else
                soup_websocket_connection_send_binary(c->ws, f.data,
                                                      f.len);
        }
        hb_share_frame_clear(&f);
    }
}

static gboolean peer_fd_cb(int fd, GIOCondition cond, gpointer ud)
{
    hb_share_peer_ctx *c = ud;
    (void)fd;
    if (cond & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;
    peer_drain(c);
    peer_emit(c);
    return G_SOURCE_CONTINUE;
}

static gboolean peer_tick_cb(gpointer ud)
{
    hb_share_peer_ctx *c = ud;
    int64_t ms;
    c->tick_src = 0;
    hb_share_peer_tick(c->eng);
    peer_drain(c);
    peer_emit(c);
    ms = hb_share_peer_next_ms(c->eng);
    if (ms >= 0)
        c->tick_src = g_timeout_add((guint)MAX(ms, 50), peer_tick_cb, c);
    return G_SOURCE_REMOVE;
}

static void peer_arm_tick(hb_share_peer_ctx *c)
{
    int64_t ms;
    if (c->tick_src) { g_source_remove(c->tick_src); c->tick_src = 0; }
    ms = hb_share_peer_next_ms(c->eng);
    if (ms >= 0)
        c->tick_src = g_timeout_add((guint)MAX(ms, 50), peer_tick_cb, c);
}

static void peer_ws_msg(SoupWebsocketConnection *w, gint type, GBytes *msg,
                        gpointer ud)
{
    hb_share_peer_ctx *c = ud;
    const char *d; gsize n;
    (void)w;
    d = g_bytes_get_data(msg, &n);
    if (!d) return;
    /* relay 文本控制（hello/locked/ended）也交给引擎 */
    hb_share_peer_incoming(c->eng, (const uint8_t *)d, n,
                           type == SOUP_WEBSOCKET_DATA_TEXT);
    peer_drain(c);
    peer_emit(c);
    peer_arm_tick(c);
}

static void peer_ws_closed(SoupWebsocketConnection *w, gpointer ud)
{
    hb_share_peer_ctx *c = ud;
    (void)w;
    g_clear_object(&c->ws);
    if (!c->ended) {
        c->ended = TRUE;
        g_signal_emit(c->svc, sigs[SIG_PEER_ENDED], 0, c, "owner_gone");
    }
}

static void peer_ws_err(SoupWebsocketConnection *w, GError *e, gpointer ud)
{ (void)w; (void)e; (void)ud; }

static void peer_ctx_free(hb_share_peer_ctx *c)
{
    if (!c) return;
    if (c->fd_src) g_source_remove(c->fd_src);
    if (c->tick_src) g_source_remove(c->tick_src);
    if (c->ws) {
        g_signal_handlers_disconnect_by_data(c->ws, c);
        if (soup_websocket_connection_get_state(c->ws) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(c->ws, 1000, NULL);
        g_object_unref(c->ws);
    }
    g_clear_pointer(&c->eng, hb_share_peer_free);
    g_free(c->share_id); g_free(c->role);
    g_free(c);
}

static void peer_ws_ready(GObject *src, GAsyncResult *res, gpointer ud)
{
    hb_share_peer_ctx *c = ud;
    GError *err = NULL;
    SoupWebsocketConnection *w =
        soup_session_websocket_connect_finish(SOUP_SESSION(src), res,
                                              &err);
    if (!w) {
        char *emsg = g_strdup(err ? err->message : "ws connect failed");
        g_clear_error(&err);
        if (c->cb)
            c->cb(c->svc, NULL, emsg, c->cb_ud);
        g_free(emsg);
        g_ptr_array_remove(c->svc->peers, c);
        peer_ctx_free(c);
        return;
    }
    c->ws = w;
    soup_websocket_connection_set_max_incoming_payload_size(w,
        WS_PAYLOAD_MAX);
    soup_websocket_connection_set_keepalive_interval(w, WS_PING_SEC);
    g_signal_connect(w, "message", G_CALLBACK(peer_ws_msg), c);
    g_signal_connect(w, "closed", G_CALLBACK(peer_ws_closed), c);
    g_signal_connect(w, "error", G_CALLBACK(peer_ws_err), c);
    c->fd_src = g_unix_fd_add(hb_share_peer_fd(c->eng),
                              G_IO_IN | G_IO_ERR | G_IO_HUP,
                              peer_fd_cb, c);
    peer_arm_tick(c);
    peer_drain(c);      /* JOIN / PAKE1 排队帧立即发出 */
    peer_emit(c);
    if (c->cb)
        c->cb(c->svc, c, NULL, c->cb_ud);
    c->cb = NULL;
}

static void peer_connect(hb_share_peer_ctx *c, const char *server)
{
    char *path = g_strdup_printf("/v1/share/%s/ws?role=%s",
                                 c->share_id, c->role);
    char *url = ws_url(server, path);
    SoupMessage *m = soup_message_new(SOUP_METHOD_GET, url);
    const char *token = hb_sync_service_token(sync_svc());
    g_free(url); g_free(path);
    if (!strcmp(c->role, "member") && token) {
        char *a = g_strdup_printf("Bearer %s", token);
        soup_message_headers_append(soup_message_get_request_headers(m),
                                    "Authorization", a);
        g_free(a);
    }
    soup_session_websocket_connect_async(c->svc->sess, m, NULL, NULL,
                                         G_PRIORITY_DEFAULT, NULL,
                                         peer_ws_ready, c);
    g_object_unref(m);
}

void hb_share_service_join_member(HbShareService *s, const char *share_id,
                                  const char *meta_b64,
                                  hb_share_join_cb cb, gpointer ud)
{
    hb_share_peer_ctx *c;
    hb_err e;
    uint8_t *vk;
    if (!signed_in() || !share_id || !meta_b64) {
        if (cb) cb(s, NULL, _("未登录或参数缺失"), ud);
        return;
    }
    vk = sodium_malloc(32);
    if (!vk || !hb_sync_service_vk_copy(sync_svc(), vk)) {
        if (vk) { sodium_memzero(vk, 32); sodium_free(vk); }
        if (cb) cb(s, NULL, _("账号密钥不可用"), ud);
        return;
    }
    c = g_new0(hb_share_peer_ctx, 1);
    c->svc = s;
    c->share_id = g_strdup(share_id);
    c->role = g_strdup("member");
    c->cb = cb; c->cb_ud = ud;
    c->eng = hb_share_peer_new_member(
        share_id, vk, meta_b64,
        hb_sync_service_device_id(sync_svc()),
        hb_sync_service_device_name(sync_svc()), &e);
    sodium_memzero(vk, 32); sodium_free(vk);
    if (!c->eng) {
        peer_ctx_free(c);
        if (cb) cb(s, NULL, _("加入失败：无法解密共享信息"), ud);
        return;
    }
    g_ptr_array_add(s->peers, c);
    peer_connect(c, hb_sync_service_server(sync_svc()));
}

void hb_share_service_join_guest(HbShareService *s, const char *link,
                                 const char *code,
                                 hb_share_join_cb cb, gpointer ud)
{
    hb_share_peer_ctx *c;
    hb_err e;
    const char *server;
    if (!link || !code) {
        if (cb) cb(s, NULL, _("需要链接和配对码"), ud);
        return;
    }
    c = g_new0(hb_share_peer_ctx, 1);
    c->svc = s;
    c->role = g_strdup("guest");
    c->cb = cb; c->cb_ud = ud;
    c->eng = hb_share_peer_new_guest(link, code, &e);
    if (!c->eng) {
        peer_ctx_free(c);
        if (cb) cb(s, NULL, _("链接或配对码无效"), ud);
        return;
    }
    c->share_id = g_strdup(hb_share_peer_share_id(c->eng));
    server = hb_share_peer_server(c->eng);
    if (!server)
        server = hb_sync_service_server(sync_svc());
    g_ptr_array_add(s->peers, c);
    peer_connect(c, server);
}

hb_share_peer *hb_share_peer_ctx_engine(hb_share_peer_ctx *c)
{ return c->eng; }
hb_vt *hb_share_peer_ctx_vt(hb_share_peer_ctx *c)
{ return c->eng ? hb_share_peer_vt(c->eng) : NULL; }
int hb_share_peer_ctx_perm(hb_share_peer_ctx *c)
{ return c->eng ? hb_share_peer_perm(c->eng) : HB_SHARE_VIEW; }
const char *hb_share_peer_ctx_title(hb_share_peer_ctx *c)
{ return c->eng ? hb_share_peer_title(c->eng) : NULL; }
const char *hb_share_peer_ctx_id(hb_share_peer_ctx *c)
{ return c->share_id; }

void hb_share_peer_ctx_close(hb_share_peer_ctx *c)
{
    if (!c || c->ended) return;
    c->ended = TRUE;
    if (c->ws) {
        g_signal_handlers_disconnect_by_data(c->ws, c);
        if (soup_websocket_connection_get_state(c->ws) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(c->ws, 1000, NULL);
        g_clear_object(&c->ws);
    }
}

/* 供 viewer 页在 tab 关闭时归还 ctx。 */
void hb_share_service_release_peer(HbShareService *s,
                                   hb_share_peer_ctx *c)
{
    if (!c) return;
    hb_share_peer_ctx_close(c);
    if (g_ptr_array_remove(s->peers, c))
        peer_ctx_free(c);
}

/* ------------------------------------------------------------------ */

static void hb_share_service_finalize(GObject *o)
{
    HbShareService *s = HB_SHARE_SERVICE(o);
    while (s->peers->len) {
        hb_share_peer_ctx *pc = g_ptr_array_index(s->peers, 0);
        g_ptr_array_remove_index(s->peers, 0);
        peer_ctx_free(pc);
    }
    g_ptr_array_free(s->peers, TRUE);
    g_hash_table_destroy(s->owners);
    g_hash_table_destroy(s->by_sess);
    g_clear_object(&s->sess);
    G_OBJECT_CLASS(hb_share_service_parent_class)->finalize(o);
}

static void hb_share_service_class_init(HbShareServiceClass *k)
{
    GObjectClass *goc = G_OBJECT_CLASS(k);
    goc->finalize = hb_share_service_finalize;
    sigs[SIG_SHARE_CHANGED] = g_signal_new("share-changed",
        G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    sigs[SIG_OWNER_EVENT] = g_signal_new("owner-event",
        G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 6,
        G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    sigs[SIG_PEER_EVENT] = g_signal_new("peer-event",
        G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
    sigs[SIG_PEER_ENDED] = g_signal_new("peer-ended",
        G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_STRING);
    sigs[SIG_PEER_ERROR] = g_signal_new("peer-error",
        G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_STRING);
}

static void hb_share_service_init(HbShareService *s)
{
    s->sess = soup_session_new();
    s->owners = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free,
                                      (GDestroyNotify)owner_ctx_free);
    s->by_sess = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, g_free);
    s->peers = g_ptr_array_new();
}

HbShareService *hb_share_service_default(void)
{
    if (!g_svc)
        g_svc = g_object_new(HB_TYPE_SHARE_SERVICE, NULL);
    return g_svc;
}
