/* harbor-server share relay：docs/api/share-protocol.md §2（登记 REST）、
 * §4（Relay WS 路由/文本控制/限流/心跳）、§6（PAKE 失败计数与锁定）。
 * 零知识：只按 17 字节帧头路由，从不解析/记录 payload。 */

#include "srv.h"

#include <string.h>

#define SH_META_MAX   (64 << 10)   /* meta 解码上限 */
#define SH_FRAME_MAX  (4 << 20)    /* 单帧上限 */
#define SH_HDR        17           /* type|to|from|seq */
#define SH_PART_MAX   5            /* 参与者上限（owner 不计） */
#define SH_OUTQ_SOFT  (1 << 20)    /* 队列 > 1 MiB → 丢 DELTA */
#define SH_OUTQ_HARD  (4 << 20)    /* 队列 > 4 MiB → 断开 */
#define SH_PING_S     5            /* keepalive ping 间隔（§4） */
#define SH_PONG_MS    10000        /* 无 pong 断开阈值 */
#define SH_TTL_DFL    1800
#define SH_TTL_MAX    86400
#define SH_FAIL_MAX   5            /* PAKE 失败锁定阈值 */

#define T_SNAPSHOT 0x01
#define T_DELTA    0x02
#define T_PAKE1    0x20
#define T_PAKE2    0x21
#define T_PAKE3    0x22
#define T_KEY      0x23

enum { R_OWNER = 1, R_MEMBER, R_GUEST };

/* ============================ 内存结构 ============================ */

typedef struct {
    uint8_t *data;
    size_t   len;
} OutF;

typedef struct SharePart SharePart;
typedef struct ShareRoom ShareRoom;

struct SharePart {
    ShareRoom               *room;
    SoupWebsocketConnection *conn;
    GSocket                 *sock;       /* NULL → 立即转发（TLS 等不可测） */
    GSource                 *wsrc;       /* G_IO_OUT writable source */
    uint32_t                 id;         /* owner=1，其它自 2 递增 */
    int                      role;
    char                    *device;     /* member 设备名；guest "" */
    bool                     pake_ok;    /* guest 已获 KEY 授权 */
    bool                     pake1_seen; /* 发过 PAKE1（兜底计数用） */
    int64_t                  last_pong;
    guint                    liveness;
    GQueue                   outq;
    size_t                   outq_bytes;
    bool                     dead;
};

struct ShareRoom {
    hb_srv     *srv;
    char       *share_id;
    char       *account_id;
    char       *owner_device;
    SharePart  *owner;          /* NULL = owner 未连 */
    GPtrArray  *parts;          /* SharePart*（member/guest，≤5） */
    SharePart  *pending_guest;  /* 未完成 PAKE 的 guest（至多 1） */
    GHashTable *counted;        /* 已计数的 pake id（防重复计） */
    int         pake_fails;
    bool        locked;
    uint32_t    next_id;
    bool        ending;         /* 结束中：跳过断开兜底计数 */
};

/* ============================ 帧导出（测试用，release 无） ============ */

#ifndef NDEBUG
static FILE *g_dump;
static void dump_frame(const uint8_t *d, size_t n)
{
    if (!g_dump) {
        const char *p = g_getenv("HARBOR_SERVER_FRAME_DUMP");
        if (p && *p) g_dump = fopen(p, "ab");
    }
    if (g_dump) {
        fwrite(d, 1, n, g_dump);
        fflush(g_dump);
    }
}
#else
static void dump_frame(const uint8_t *d, size_t n)
{
    (void)d; (void)n;
}
#endif

/* ============================ room ============================ */

static ShareRoom *room_for(hb_srv *srv, const char *id, bool create)
{
    ShareRoom *r = g_hash_table_lookup(srv->sh_rooms, id);
    if (!r && create) {
        r = g_new0(ShareRoom, 1);
        r->srv = srv;
        r->share_id = g_strdup(id);
        r->parts = g_ptr_array_new();
        r->counted = g_hash_table_new(g_direct_hash, g_direct_equal);
        r->next_id = 2;
        g_hash_table_insert(srv->sh_rooms, r->share_id, r);
    }
    return r;
}

static void room_free(ShareRoom *r)
{
    g_hash_table_remove(r->srv->sh_rooms, r->share_id);
    g_free(r->share_id);
    g_free(r->account_id);
    g_free(r->owner_device);
    g_ptr_array_free(r->parts, TRUE);
    g_hash_table_destroy(r->counted);
    g_free(r);
}

static SharePart *room_find(ShareRoom *r, uint32_t id)
{
    guint i;
    if (r->owner && r->owner->id == id)
        return r->owner;
    for (i = 0; i < r->parts->len; i++) {
        SharePart *p = g_ptr_array_index(r->parts, i);
        if (p->id == id)
            return p;
    }
    return NULL;
}

/* ============================ part 生命周期 ============================ */

static gboolean part_writable(GSocket *sock, GIOCondition cond,
                              gpointer ud);

static void qflush(SharePart *p)
{
    while (p->outq.head) {
        OutF *f;
        if (p->sock) {
            GIOCondition c = g_socket_condition_check(
                p->sock, G_IO_OUT | G_IO_ERR | G_IO_HUP);
            if (c & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
                soup_websocket_connection_close(p->conn, 1011, NULL);
                break;
            }
            if (!(c & G_IO_OUT))
                break;
        }
        f = g_queue_pop_head(&p->outq);
        p->outq_bytes -= f->len;
        if (soup_websocket_connection_get_state(p->conn) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_send_binary(p->conn, f->data,
                                                  f->len);
        g_free(f->data);
        g_free(f);
    }
    /* 队列非空且 socket 可观测 → 挂 writable source */
    if (p->outq.length && p->sock && !p->wsrc) {
        p->wsrc = g_socket_create_source(p->sock, G_IO_OUT, NULL);
        g_source_set_callback(p->wsrc,
                              G_SOURCE_FUNC(part_writable), p, NULL);
        g_source_attach(p->wsrc, NULL);
    }
}

static gboolean part_writable(GSocket *sock, GIOCondition cond,
                              gpointer ud)
{
    SharePart *p = ud;
    (void)sock; (void)cond;
    qflush(p);            /* 错误在 condition_check 里一并探测 */
    if (p->dead || !p->outq.length) {
        p->wsrc = NULL;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* 有界发送队列（§4）：>1 MiB 丢 DELTA、>4 MiB 断开；libsoup 无 buffered
 * 量 API，故自管 outq + kernel writable 探测，libsoup 内部滞留 ≤1 帧。 */
static void qsend(SharePart *p, const uint8_t *d, size_t n)
{
    OutF *f;
    if (p->dead)
        return;
    if (p->outq_bytes + n > SH_OUTQ_SOFT && d[0] == T_DELTA)
        return;                              /* 溢出丢 DELTA */
    if (p->outq_bytes + n > SH_OUTQ_HARD) {
        soup_websocket_connection_close(p->conn, 1011, NULL);
        return;
    }
    f = g_new0(OutF, 1);
    f->data = g_memdup2(d, n);
    f->len = n;
    g_queue_push_tail(&p->outq, f);
    p->outq_bytes += n;
    dump_frame(d, n);
    qflush(p);
}

static void part_free(SharePart *p)
{
    OutF *f;
    if (p->liveness)
        g_source_remove(p->liveness);
    if (p->wsrc)
        g_source_destroy(p->wsrc);
    g_signal_handlers_disconnect_by_data(p->conn, p);
    g_object_unref(p->conn);
    if (p->sock)
        g_object_unref(p->sock);
    while ((f = g_queue_pop_head(&p->outq))) {
        g_free(f->data);
        g_free(f);
    }
    g_free(p->device);
    g_free(p);
}

static void room_end(ShareRoom *r, const char *reason);

/* conn closed/error → 摘除 part；owner 断开 → 整个共享结束。 */
static void part_gone(SharePart *p)
{
    ShareRoom *r = p->room;
    if (p->dead)
        return;
    p->dead = true;
    if (r->pending_guest == p)
        r->pending_guest = NULL;
    /* §6 兜底：发过 PAKE1 未 pake_ok 即断开，且所有者未报 pake_fail →
     * 计一次失败（counted 去重，后续所有者的 pake_fail 不再计）。 */
    if (p->role == R_GUEST && p->pake1_seen && !p->pake_ok && !r->ending &&
        !g_hash_table_contains(r->counted, GUINT_TO_POINTER(p->id))) {
        int locked;
        g_hash_table_add(r->counted, GUINT_TO_POINTER(p->id));
        r->pake_fails = hbd_share_pake_fail(r->srv->db, r->share_id,
                                          &locked);
        r->locked = locked != 0;
        hb_log(r->srv, 2, "share %s guest %u pake fallback fail #%d",
               r->share_id, p->id, r->pake_fails);
    }
    if (p->role == R_OWNER) {
        r->owner = NULL;
        part_free(p);
        if (!r->ending)
            room_end(r, "owner_gone");
        return;
    }
    g_ptr_array_remove(r->parts, p);
    if (r->owner && !r->ending) {
        g_autofree char *s =
            g_strdup_printf("{\"t\":\"left\",\"id\":%u}", p->id);
        if (soup_websocket_connection_get_state(r->owner->conn) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_send_text(r->owner->conn, s);
    }
    part_free(p);
}

/* 结束共享：落库、账号 WS 通知、全部参与者 ended(reason) + 关闭。 */
static void room_end(ShareRoom *r, const char *reason)
{
    GPtrArray *all;
    guint i;
    g_autofree char *ended = NULL;
    r->ending = true;
    hbd_share_end(r->srv->db, r->share_id, reason);
    hb_ws_notify_shares(r->srv, r->account_id);
    ended = g_strdup_printf("{\"t\":\"ended\",\"reason\":\"%s\"}", reason);
    all = g_ptr_array_sized_new(r->parts->len + 1);
    if (r->owner) {
        g_ptr_array_add(all, r->owner);
        r->owner = NULL;
    }
    for (i = 0; i < r->parts->len; i++)
        g_ptr_array_add(all, g_ptr_array_index(r->parts, i));
    g_ptr_array_set_size(r->parts, 0);
    r->pending_guest = NULL;
    for (i = 0; i < all->len; i++) {
        SharePart *p = g_ptr_array_index(all, i);
        /* 先断信号，避免 close 同步回调 part_gone 重入 */
        g_signal_handlers_disconnect_by_data(p->conn, p);
        p->dead = true;
        if (soup_websocket_connection_get_state(p->conn) ==
            SOUP_WEBSOCKET_STATE_OPEN) {
            soup_websocket_connection_send_text(p->conn, ended);
            soup_websocket_connection_close(p->conn, 1000, NULL);
        }
    }
    for (i = 0; i < all->len; i++)
        part_free(g_ptr_array_index(all, i));
    g_ptr_array_free(all, TRUE);
    room_free(r);
}

/* REST/外部入口：结束某个进行中共享（无 room 时仅落库 + 通知）。 */
void relay_end_share(hb_srv *srv, const char *share_id, const char *reason)
{
    ShareRoom *r = room_for(srv, share_id, false);
    if (r) {
        room_end(r, reason);
    } else {
        HbShare sh;
        if (hbd_share_get(srv->db, share_id, &sh) == 0) {
            hbd_share_end(srv->db, share_id, reason);
            hb_ws_notify_shares(srv, sh.account_id);
            hbd_share_free(&sh);
        }
    }
}

/* ============================ 路由 ============================ */

/* guest 未 pake_ok：只能收 0x21 PAKE2 与 0x23 KEY（无 0x01–0x05）。 */
static bool can_recv(const SharePart *p, uint8_t type)
{
    if (p->role != R_GUEST || p->pake_ok)
        return true;
    return type == T_PAKE2 || type == T_KEY;
}

static void own_frame(ShareRoom *r, uint8_t *b, size_t n)
{
    uint8_t type = b[0];
    uint32_t to;
    guint i;
    memcpy(&to, b + 1, 4);
    to = GUINT32_FROM_BE(to);
    /* owner 恒为 1：改写 from 防伪造 */
    {
        uint32_t one = GUINT32_TO_BE(1);
        memcpy(b + 5, &one, 4);
    }
    if (to == 0) {
        for (i = 0; i < r->parts->len; i++) {
            SharePart *p = g_ptr_array_index(r->parts, i);
            if (can_recv(p, type))
                qsend(p, b, n);
        }
    } else if (to != 1) {
        SharePart *p = room_find(r, to);
        if (p && can_recv(p, type))
            qsend(p, b, n);
    }
}

static void part_frame(SharePart *p, uint8_t *b, size_t n)
{
    ShareRoom *r = p->room;
    uint8_t type = b[0];
    uint32_t to = GUINT32_TO_BE(1), from = GUINT32_TO_BE(p->id);
    /* 允许类型：member 0x10/0x11/0x12；guest 未授权仅 0x20/0x22，
     * 授权后仅 0x11（0x10 防御性丢弃）。 */
    if (p->role == R_MEMBER) {
        if (type != 0x10 && type != 0x11 && type != 0x12)
            return;
    } else {
        if (p->pake_ok) {
            if (type != 0x11)
                return;
        } else {
            if (type != T_PAKE1 && type != T_PAKE3)
                return;
        }
    }
    if (type == T_PAKE1)
        p->pake1_seen = true;
    memcpy(b + 1, &to, 4);
    memcpy(b + 5, &from, 4);
    if (r->owner)
        qsend(r->owner, b, n);
}

/* ============================ 文本消息 ============================ */

static int64_t j_id(JsonObject *o)
{
    JsonNode *n = json_object_get_member(o, "id");
    return (n && JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_INT64)
               ? json_node_get_int(n)
               : -1;
}

static void count_fail(ShareRoom *r, uint32_t id)
{
    int locked;
    if (g_hash_table_contains(r->counted, GUINT_TO_POINTER(id)))
        return;
    g_hash_table_add(r->counted, GUINT_TO_POINTER(id));
    r->pake_fails = hbd_share_pake_fail(r->srv->db, r->share_id, &locked);
    r->locked = locked != 0;
    hb_log(r->srv, 2, "share %s pake_fail id=%u count=%d locked=%d",
           r->share_id, id, r->pake_fails, r->locked);
}

static void kick_part(SharePart *p)
{
    if (soup_websocket_connection_get_state(p->conn) ==
        SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_send_text(
            p->conn, "{\"t\":\"ended\",\"reason\":\"kicked\"}");
        soup_websocket_connection_close(p->conn, 1000, NULL);
    }
}

/* owner 文本控制：kick / pake_fail / pake_ok。 */
static void owner_text(SharePart *o, const uint8_t *d, size_t n)
{
    ShareRoom *r = o->room;
    g_autoptr(JsonParser) jp = json_parser_new();
    JsonNode *root;
    JsonObject *j;
    const char *t;
    int64_t id;
    SharePart *v;
    if (!json_parser_load_from_data(jp, (const char *)d, (gssize)n, NULL))
        return;
    root = json_parser_get_root(jp);
    j = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root)
                                           : NULL;
    if (!j)
        return;
    {
        JsonNode *tn = json_object_get_member(j, "t");
        t = (tn && JSON_NODE_HOLDS_VALUE(tn) &&
             json_node_get_value_type(tn) == G_TYPE_STRING)
                ? json_node_get_string(tn)
                : NULL;
    }
    id = j_id(j);
    if (!t || id <= 0)
        return;
    if (!strcmp(t, "pake_ok")) {
        g_hash_table_add(r->counted, GUINT_TO_POINTER((guint)id));
        v = room_find(r, (uint32_t)id);
        if (v && v->role == R_GUEST) {
            v->pake_ok = true;
            if (r->pending_guest == v)
                r->pending_guest = NULL;
        }
    } else if (!strcmp(t, "pake_fail")) {
        count_fail(r, (uint32_t)id);
        /* owner 报失败即踢该 guest（§6 step4） */
        v = room_find(r, (uint32_t)id);
        if (v && v->role == R_GUEST)
            kick_part(v);
    } else if (!strcmp(t, "kick")) {
        /* owner 主动踢出：记为已处理，断开不触发兜底计数 */
        g_hash_table_add(r->counted, GUINT_TO_POINTER((guint)id));
        v = room_find(r, (uint32_t)id);
        if (v && v->role != R_OWNER)
            kick_part(v);
    }
}

/* ============================ WS 回调 ============================ */

static void on_sh_msg(SoupWebsocketConnection *conn, int type, GBytes *msg,
                      gpointer u)
{
    SharePart *p = u;
    const uint8_t *d;
    gsize n;
    uint8_t *b;
    (void)conn;
    d = g_bytes_get_data(msg, &n);
    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        if (p->role == R_OWNER)
            owner_text(p, d, n);
        return;
    }
    if (n < SH_HDR || n > SH_FRAME_MAX)
        return;
    b = g_memdup2(d, n);
    if (p->role == R_OWNER)
        own_frame(p->room, b, n);
    else
        part_frame(p, b, n);
    g_free(b);
}

static void on_sh_closed(SoupWebsocketConnection *conn, gpointer u)
{
    (void)conn;
    part_gone(u);
}

static void on_sh_error(SoupWebsocketConnection *conn, GError *err,
                        gpointer u)
{
    (void)conn; (void)err;
    part_gone(u);
}

static void on_sh_pong(SoupWebsocketConnection *conn, GBytes *data,
                       gpointer u)
{
    (void)conn; (void)data;
    ((SharePart *)u)->last_pong = hbu_now_ms();
}

/* 每 1 s 检查：>10 s 无 pong → 断开（owner 失联 ≤ ~10 s 发现）。 */
static gboolean liveness_cb(gpointer u)
{
    SharePart *p = u;
    if (hbu_now_ms() - p->last_pong > SH_PONG_MS) {
        p->liveness = 0;
        /* 死对等方不会回 close 握手：直接断底层 socket，
         * 让 libsoup 立即进入 closed（→ part_gone）。 */
        if (p->sock && !g_socket_is_closed(p->sock))
            g_socket_close(p->sock, NULL);
        else
            soup_websocket_connection_close(p->conn, 1001, NULL);
        part_gone(p);               /* 兜底：不等 closed 信号 */
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* /v1/share/<id>/ws 的 id 提取；非法 → false。 */
static bool share_path_id(const char *path, char out[HB_SRV_ID_MAX + 1])
{
    const char *p, *suf;
    size_t n;
    if (!g_str_has_prefix(path, "/v1/share/"))
        return false;
    p = path + strlen("/v1/share/");
    suf = strstr(p, "/ws");
    if (!suf || suf[3] != 0 || suf == p)
        return false;
    n = (size_t)(suf - p);
    if (n > HB_SRV_ID_MAX)
        return false;
    memcpy(out, p, n);
    out[n] = 0;
    return hbu_valid_id(out);
}

/* 底层 socket（用于 outq writable 探测）；GTlsConnection 沿
 * base-io-stream 下挖；拿不到 → NULL（立即转发模式）。 */
static GSocket *conn_socket(SoupWebsocketConnection *conn)
{
    GIOStream *s = soup_websocket_connection_get_io_stream(conn);
    GIOStream *owned = NULL;
    GSocket *out = NULL;
    while (s) {
        if (G_IS_SOCKET_CONNECTION(s)) {
            out = g_object_ref(
                g_socket_connection_get_socket(G_SOCKET_CONNECTION(s)));
            break;
        }
        {
            GIOStream *base = NULL;
            /* libsoup SoupIOStream → base-iostream；GTlsConnection →
             * base-io-stream。均为实现细节名，探测后逐层下挖。 */
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(s),
                                             "base-iostream"))
                g_object_get(G_OBJECT(s), "base-iostream", &base, NULL);
            else if (g_object_class_find_property(
                         G_OBJECT_GET_CLASS(s), "base-io-stream"))
                g_object_get(G_OBJECT(s), "base-io-stream", &base,
                             NULL);
            if (owned)
                g_object_unref(owned);
            owned = base;
            s = base;
        }
    }
    if (owned)
        g_object_unref(owned);
    return out;
}

static void share_ws_connected(SoupServer *server, SoupServerMessage *msg,
                             const char *path,
                             SoupWebsocketConnection *conn, gpointer u)
{
    hb_srv *srv = u;
    char id[HB_SRV_ID_MAX + 1];
    int role = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(msg),
                                               "hb-sh-role"));
    ShareRoom *r;
    SharePart *p;
    HbShare sh = {0};
    g_autofree char *txt = NULL;
    guint i;
    (void)server;
    if (!share_path_id(path, id) ||
        hbd_share_get(srv->db, id, &sh) != 0 || sh.ended) {
        soup_websocket_connection_close(conn, 1011, NULL);
        goto out;
    }
    r = room_for(srv, id, true);
    r->account_id = r->account_id ? r->account_id : g_strdup(sh.account_id);
    r->owner_device =
        r->owner_device ? r->owner_device : g_strdup(sh.owner_device);
    r->pake_fails = sh.pake_fails;
    r->locked = sh.locked != 0;
    /* 并发上限（owner 不计） */
    if (r->parts->len >= SH_PART_MAX) {
        soup_websocket_connection_close(conn, 4008, NULL);
        goto out;
    }
    if (role == R_OWNER && r->owner) {
        soup_websocket_connection_close(conn, 1008, NULL);
        goto out;
    }
    if (role == R_GUEST) {
        if (r->locked) {
            soup_websocket_connection_send_text(conn, "{\"t\":\"locked\"}");
            soup_websocket_connection_close(conn, 1000, NULL);
            goto out;
        }
        if (sh.guest_expires <= hbu_now_ms()) {
            soup_websocket_connection_send_text(
                conn, "{\"t\":\"ended\",\"reason\":\"expired\"}");
            soup_websocket_connection_close(conn, 1000, NULL);
            goto out;
        }
        if (r->pending_guest) {
            soup_websocket_connection_close(conn, 4009, NULL);
            goto out;
        }
    }
    p = g_new0(SharePart, 1);
    p->room = r;
    p->conn = g_object_ref(conn);
    p->sock = conn_socket(conn);
    p->role = role;
    p->last_pong = hbu_now_ms();
    g_queue_init(&p->outq);
    soup_websocket_connection_set_keepalive_interval(conn, SH_PING_S);
    soup_websocket_connection_set_max_incoming_payload_size(conn,
                                                            SH_FRAME_MAX);
    g_signal_connect(conn, "message", G_CALLBACK(on_sh_msg), p);
    g_signal_connect(conn, "closed", G_CALLBACK(on_sh_closed), p);
    g_signal_connect(conn, "error", G_CALLBACK(on_sh_error), p);
    g_signal_connect(conn, "pong", G_CALLBACK(on_sh_pong), p);
    p->liveness = g_timeout_add(1000, liveness_cb, p);
    if (role == R_OWNER) {
        p->id = 1;
        r->owner = p;
    } else {
        p->id = r->next_id++;
        if (role == R_MEMBER) {
            const char *dev =
                g_object_get_data(G_OBJECT(msg), "hb-dev");
            p->device = hbd_device_name(srv->db, dev);
        } else {
            p->device = g_strdup("");
            r->pending_guest = p;
        }
        g_ptr_array_add(r->parts, p);
    }
    txt = g_strdup_printf("{\"t\":\"hello\",\"id\":%u}", p->id);
    soup_websocket_connection_send_text(conn, txt);
    /* 通知 owner 有新参与者（含 owner 晚到时补发已有的） */
    if (role == R_OWNER) {
        for (i = 0; i < r->parts->len; i++) {
            SharePart *m = g_ptr_array_index(r->parts, i);
            g_autofree char *j = g_strdup_printf(
                "{\"t\":\"joined\",\"id\":%u,\"role\":\"%s\","
                "\"device\":\"%s\"}",
                m->id, m->role == R_MEMBER ? "member" : "guest",
                m->device ? m->device : "");
            soup_websocket_connection_send_text(p->conn, j);
        }
    } else if (r->owner) {
        g_autofree char *j = g_strdup_printf(
            "{\"t\":\"joined\",\"id\":%u,\"role\":\"%s\",\"device\":\"%s\"}",
            p->id, role == R_MEMBER ? "member" : "guest",
            p->device ? p->device : "");
        if (soup_websocket_connection_get_state(r->owner->conn) ==
            SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_send_text(r->owner->conn, j);
    }
    hb_log(srv, 2, "share %s part %u role=%d connected", id, p->id, role);
out:
    hbd_share_free(&sh);
}

/* 升级前校验（early handler）：路径/角色/登记/认证。
 * 失败写出 HTTP 错误（升级中止）；guest 的 locked/expired/pending
 * 与容量检查延迟到 ws_connected（需以 WS 关闭码/文本表达）。 */
static void share_ws_early(SoupServer *server, SoupServerMessage *msg,
                           const char *path, GHashTable *query,
                           gpointer u)
{
    hb_srv *srv = u;
    char id[HB_SRV_ID_MAX + 1];
    const char *role_s;
    int role;
    HbShare sh;
    (void)server;
    if (!share_path_id(path, id))
        return;                     /* 非本协议路径，放行给其它 handler */
    role_s = query ? g_hash_table_lookup(query, "role") : NULL;
    if (!role_s ||
        (strcmp(role_s, "owner") && strcmp(role_s, "member") &&
         strcmp(role_s, "guest"))) {
        hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                  "missing role", NULL);
        return;
    }
    role = !strcmp(role_s, "owner")
               ? R_OWNER
               : !strcmp(role_s, "member") ? R_MEMBER : R_GUEST;
    if (hbd_share_get(srv->db, id, &sh) != 0 || sh.ended) {
        hbu_error(msg, SOUP_STATUS_NOT_FOUND, "share_not_found",
                  "no such share", NULL);
        return;
    }
    if (role != R_GUEST) {
        g_autofree char *acct = NULL, *dev = NULL;
        if (!hb_auth_check(srv, msg, &acct, &dev)) {
            hbd_share_free(&sh);
            return;
        }
        if (strcmp(acct, sh.account_id)) {
            hbu_error(msg, SOUP_STATUS_FORBIDDEN, "forbidden",
                      "not the share account", NULL);
            hbd_share_free(&sh);
            return;
        }
        if (role == R_OWNER) {
            ShareRoom *r;
            if (strcmp(dev, sh.owner_device)) {
                hbu_error(msg, SOUP_STATUS_FORBIDDEN, "forbidden",
                          "not the owner device", NULL);
                hbd_share_free(&sh);
                return;
            }
            r = room_for(srv, id, false);
            if (r && r->owner) {
                hbu_error(msg, SOUP_STATUS_CONFLICT, "owner_connected",
                          "owner already connected", NULL);
                hbd_share_free(&sh);
                return;
            }
        } else if (!strcmp(dev, sh.owner_device)) {
            hbu_error(msg, SOUP_STATUS_FORBIDDEN, "forbidden",
                      "owner device cannot join as member", NULL);
            hbd_share_free(&sh);
            return;
        }
        g_object_set_data_full(G_OBJECT(msg), "hb-acct",
                               g_steal_pointer(&acct), g_free);
        g_object_set_data_full(G_OBJECT(msg), "hb-dev",
                               g_steal_pointer(&dev), g_free);
    }
    g_object_set_data(G_OBJECT(msg), "hb-sh-role", GINT_TO_POINTER(role));
    hbd_share_free(&sh);
}

/* ============================ REST /v1/shares ============================ */

/* owner 设备校验：share 存在、未结束、同账号、owner_device 匹配。 */
static bool share_owner_ok(hb_srv *srv, SoupServerMessage *msg,
                           const char *id, HbShare *out, const char *acct,
                           const char *dev)
{
    if (hbd_share_get(srv->db, id, out) != 0 || out->ended) {
        hbu_error(msg, SOUP_STATUS_NOT_FOUND, "share_not_found",
                  "no such share", NULL);
        return false;
    }
    if (strcmp(out->account_id, acct) || strcmp(out->owner_device, dev)) {
        hbu_error(msg, SOUP_STATUS_FORBIDDEN, "forbidden",
                  "not the owner device", NULL);
        hbd_share_free(out);
        return false;
    }
    return true;
}

static int64_t share_ttl(JsonObject *o, bool *ok)
{
    int64_t ttl;
    if (!json_object_get_member(o, "guest_ttl")) {
        *ok = true;
        return SH_TTL_DFL;
    }
    ttl = hbu_jint(o, "guest_ttl", -1, ok);
    if (!*ok || ttl < 1)
        return -1;
    if (ttl > SH_TTL_MAX)
        ttl = SH_TTL_MAX;
    return ttl;
}

static void h_shares_post(hb_srv *srv, SoupServerMessage *msg,
                          const char *acct, const char *dev)
{
    JsonObject *o = hbu_body_json(msg);
    const char *id = o ? hbu_jstr(o, "share_id") : NULL;
    const char *meta_b64 = o ? hbu_jstr(o, "meta") : NULL;
    uint8_t *meta = NULL;
    size_t meta_n = 0;
    bool ok;
    int64_t ttl;
    HbShare sh;
    int rc;
    if (!id || !hbu_valid_id(id) || !meta_b64)
        goto bad;
    meta = hbu_b64_decode(meta_b64, &meta_n);
    if (!meta || meta_n == 0 || meta_n > SH_META_MAX)
        goto bad;
    ttl = share_ttl(o, &ok);
    if (ttl < 0)
        goto bad;
    memset(&sh, 0, sizeof sh);
    sh.share_id = (char *)id;
    sh.account_id = (char *)acct;
    sh.owner_device = (char *)dev;
    sh.meta = meta;
    sh.meta_n = meta_n;
    sh.created = hbu_now_ms();
    sh.guest_expires = sh.created + ttl * 1000;
    rc = hbd_share_insert(srv->db, &sh);
    if (rc == 2) {
        hbu_error(msg, SOUP_STATUS_CONFLICT, "share_exists",
                  "share_id already registered", NULL);
        goto out;
    }
    if (rc != 0)
        goto bad;
    hb_ws_notify_shares(srv, acct);
    hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
    goto out;
bad:
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed shares request", NULL);
out:
    g_free(meta);
    if (o) json_object_unref(o);
}

static void h_shares_get(hb_srv *srv, SoupServerMessage *msg,
                         const char *acct)
{
    GArray *rows = hbd_share_list(srv->db, acct);
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autofree char *s;
    guint i;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "shares");
    json_builder_begin_array(b);
    for (i = 0; i < rows->len; i++) {
        HbShare *sh = &g_array_index(rows, HbShare, i);
        g_autofree char *b64 = hbu_b64_encode(sh->meta, sh->meta_n);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "share_id");
        json_builder_add_string_value(b, sh->share_id);
        json_builder_set_member_name(b, "owner_device");
        json_builder_add_string_value(b, sh->owner_device);
        json_builder_set_member_name(b, "meta");
        json_builder_add_string_value(b, b64);
        json_builder_set_member_name(b, "created");
        json_builder_add_int_value(b, sh->created);
        json_builder_set_member_name(b, "guest_expires");
        json_builder_add_int_value(b, sh->guest_expires);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    hbd_shares_free(rows);
    {
        JsonGenerator *g = json_generator_new();
        JsonNode *root = json_builder_get_root(b);
        json_generator_set_root(g, root);
        s = json_generator_to_data(g, NULL);
        json_node_free(root);
        g_object_unref(g);
    }
    hbu_respond_json(msg, SOUP_STATUS_OK, s);
}

static void h_share_put(hb_srv *srv, SoupServerMessage *msg,
                        const char *id, const char *acct, const char *dev)
{
    HbShare sh;
    JsonObject *o;
    const char *meta_b64;
    uint8_t *meta;
    size_t meta_n = 0;
    if (!share_owner_ok(srv, msg, id, &sh, acct, dev))
        return;
    hbd_share_free(&sh);
    o = hbu_body_json(msg);
    meta_b64 = o ? hbu_jstr(o, "meta") : NULL;
    meta = meta_b64 ? hbu_b64_decode(meta_b64, &meta_n) : NULL;
    if (!meta || meta_n == 0 || meta_n > SH_META_MAX) {
        g_free(meta);
        hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                  "bad meta", NULL);
        if (o) json_object_unref(o);
        return;
    }
    hbd_share_set_meta(srv->db, id, meta, meta_n);
    g_free(meta);
    if (o) json_object_unref(o);
    hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
}

static void h_share_delete(hb_srv *srv, SoupServerMessage *msg,
                           const char *id, const char *acct,
                           const char *dev)
{
    HbShare sh;
    if (!share_owner_ok(srv, msg, id, &sh, acct, dev))
        return;
    hbd_share_free(&sh);
    relay_end_share(srv, id, "stopped");
    hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
}

static void h_share_guest(hb_srv *srv, SoupServerMessage *msg,
                          const char *id, const char *acct, const char *dev)
{
    HbShare sh;
    JsonObject *o;
    bool ok;
    int64_t ttl;
    ShareRoom *r;
    if (!share_owner_ok(srv, msg, id, &sh, acct, dev))
        return;
    o = hbu_body_json(msg);
    ttl = o ? share_ttl(o, &ok) : SH_TTL_DFL;
    if (ttl < 0) {
        hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                  "bad guest_ttl", NULL);
        if (o) json_object_unref(o);
        hbd_share_free(&sh);
        return;
    }
    {
        int64_t exp = hbu_now_ms() + ttl * 1000;
        hbd_share_guest_reset(srv->db, id, exp);
        sh.guest_expires = exp;
    }
    r = room_for(srv, id, false);
    if (r) {
        r->pake_fails = 0;
        r->locked = false;
        g_hash_table_remove_all(r->counted);
    }
    if (o) json_object_unref(o);
    hbd_share_free(&sh);
    hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
}

void h_shares(hb_srv *srv, SoupServerMessage *msg, const char *path,
              GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    const char *method = soup_server_message_get_method(msg);
    const char *rest;
    char id[HB_SRV_ID_MAX + 1];
    size_t n;
    (void)query;
    if (!hb_auth_check(srv, msg, &acct, &dev))
        return;
    if (!strcmp(path, "/v1/shares")) {
        if (!strcmp(method, SOUP_METHOD_POST))
            h_shares_post(srv, msg, acct, dev);
        else if (!strcmp(method, SOUP_METHOD_GET))
            h_shares_get(srv, msg, acct);
        else
            soup_server_message_set_status(
                msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }
    if (!g_str_has_prefix(path, "/v1/shares/"))
        goto not_found;
    rest = path + strlen("/v1/shares/");
    n = strlen(rest);
    if (n > 6 && !strcmp(rest + n - 6, "/guest")) {
        if (n - 6 > HB_SRV_ID_MAX)
            goto not_found;
        memcpy(id, rest, n - 6);
        id[n - 6] = 0;
        if (!hbu_valid_id(id))
            goto not_found;
        if (!strcmp(method, SOUP_METHOD_POST))
            h_share_guest(srv, msg, id, acct, dev);
        else
            soup_server_message_set_status(
                msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }
    if (strchr(rest, '/') || n > HB_SRV_ID_MAX)
        goto not_found;
    memcpy(id, rest, n + 1);
    if (!hbu_valid_id(id))
        goto not_found;
    if (!strcmp(method, SOUP_METHOD_PUT))
        h_share_put(srv, msg, id, acct, dev);
    else if (!strcmp(method, SOUP_METHOD_DELETE))
        h_share_delete(srv, msg, id, acct, dev);
    else
        soup_server_message_set_status(msg,
                                       SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
    return;
not_found:
    hbu_error(msg, SOUP_STATUS_NOT_FOUND, "share_not_found",
              "no such share", NULL);
}

/* ============================ attach ============================ */

void relay_attach(hb_srv *srv)
{
    srv->sh_rooms = g_hash_table_new(g_str_hash, g_str_equal);
    soup_server_add_early_handler(srv->soup, "/v1/share", share_ws_early,
                                  srv, NULL);
    soup_server_add_websocket_handler(srv->soup, "/v1/share", NULL, NULL,
                                      share_ws_connected, srv, NULL);
}
