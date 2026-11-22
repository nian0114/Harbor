#ifndef HB_SHARE_SERVICE_H
#define HB_SHARE_SERVICE_H

/* FR-12 共享服务：owner（POST/WS role=owner + hb_share_owner 泵）与
 * peer（list/join member|guest + hb_share_peer 泵）的应用单例。
 * 凭据经 HbSyncService 窄访问器取；signed-out 时 owner/peer 均不可用。
 * 永不记录帧、密钥、配对码或 meta 明文。 */

#include <adwaita.h>
#include <harbor/session.h>
#include <harbor/share.h>

G_BEGIN_DECLS

#define HB_TYPE_SHARE_SERVICE (hb_share_service_get_type())
G_DECLARE_FINAL_TYPE(HbShareService, hb_share_service,
                     HB, SHARE_SERVICE, GObject)

HbShareService *hb_share_service_default(void);

/* ---- owner ---- */

typedef void (*hb_share_owner_cb)(HbShareService *s, const char *share_id,
                                  const char *err, gpointer ud);

/* 异步：engine → POST /v1/shares → WS role=owner → attach session。
 * cb(NULL err) 时 share 已活跃；sess 由调用方拥有，停止前必须存活。 */
void hb_share_service_start(HbShareService *s, hb_session *sess,
                            const char *title, const char *host,
                            int mode /* HB_SHARE_* */,
                            hb_share_owner_cb cb, gpointer ud);

/* DELETE + close WS + free engine。幂等。 */
void hb_share_service_stop(HbShareService *s, const char *share_id);
/* session 退出/页关闭时按 sess 反查停止（等价 stop）。 */
void hb_share_service_stop_session(HbShareService *s, hb_session *sess);

/* 查找 sess 对应的活动 share（无 → NULL）。后续调用可能失效，即用即取。 */
const char *hb_share_service_share_for_session(HbShareService *s,
                                               hb_session *sess);

/* mode → PUT meta + engine set_mode */
void hb_share_service_set_mode(HbShareService *s, const char *share_id,
                               int mode);
/* 重新生成配对码/link secret → POST /shares/{id}/guest */
void hb_share_service_regen(HbShareService *s, const char *share_id);
void hb_share_service_kick(HbShareService *s, const char *share_id,
                           uint32_t participant_id);

/* owner 侧当前值（share 活跃时非 NULL；指针借用） */
const char *hb_share_service_link(HbShareService *s, const char *share_id);
const char *hb_share_service_code(HbShareService *s, const char *share_id);
int         hb_share_service_mode(HbShareService *s, const char *share_id);
int         hb_share_service_count(HbShareService *s, const char *share_id);
/* owner 当前参与者快照（GList* of hb_share_part*，释放用
 * g_list_free_full + hb_share_part_free） */
typedef struct {
    guint    id;
    char    *name;
    char    *device;
    gboolean guest;
} hb_share_part;
void        hb_share_part_free(hb_share_part *p);
GList      *hb_share_service_participants(HbShareService *s,
                                          const char *share_id);
gboolean    hb_share_service_starting(HbShareService *s,
                                      const char *share_id);
/* 该 session 是否有 share（含 starting） */
gboolean    hb_share_service_session_shared(HbShareService *s,
                                            hb_session *sess);

/* 本机 share 绑定 tab（视图卡片点击切换用）。 */
void hb_share_service_bind_tab(HbShareService *s, const char *share_id,
                               const char *tab_id);

typedef struct {
    char *share_id, *title, *host, *tab_id;
    int   mode;
} HbShareOwnInfo;
GList *hb_share_service_own_shares(HbShareService *s);
void   hb_share_own_info_free(gpointer p);

/* ---- peer list（GET /v1/shares）---- */

typedef struct {
    char    *share_id;
    char    *meta_b64;     /* 不透明密封 blob，仅引擎解密 */
    char    *owner_device;
    gint64   created;      /* unix ms */
} HbShareItem;
void hb_share_item_free(HbShareItem *it);

typedef void (*hb_share_list_cb)(HbShareService *s, GList *items,
                                 const char *err, gpointer ud);
void hb_share_service_list(HbShareService *s, hb_share_list_cb cb,
                           gpointer ud);

/* ---- peer join ---- */

/* HbSharePeerCtx 在 join 成功后经 "peer-joined" 信号交付；失败经
 * "peer-error"（text=展示消息）。viewer 页据事件驱动 UI。
 * 引擎事件经 "peer-event" 转发（peer, hb_share_event*）。 */
typedef void (*hb_share_join_cb)(HbShareService *s, gpointer peer_ctx,
                                 const char *err, gpointer ud);

/* member：share_id + GET /shares 的 meta_b64（引擎解出 K_out/title）。 */
void hb_share_service_join_member(HbShareService *s, const char *share_id,
                                  const char *meta_b64,
                                  hb_share_join_cb cb, gpointer ud);
/* guest：完整 link + 6 位配对码。 */
void hb_share_service_join_guest(HbShareService *s, const char *link,
                                 const char *code,
                                 hb_share_join_cb cb, gpointer ud);

/* peer ctx（不透明，经 hb_share_peer_ctx_* 访问）；离开/结束时由
 * 调用方 hb_share_peer_ctx_close + ctx_free，或服务自动清理。 */
typedef struct hb_share_peer_ctx hb_share_peer_ctx;

hb_share_peer *hb_share_peer_ctx_engine(hb_share_peer_ctx *c);
hb_vt         *hb_share_peer_ctx_vt(hb_share_peer_ctx *c);
int            hb_share_peer_ctx_perm(hb_share_peer_ctx *c);
const char    *hb_share_peer_ctx_title(hb_share_peer_ctx *c);
const char    *hb_share_peer_ctx_id(hb_share_peer_ctx *c);
void           hb_share_peer_ctx_close(hb_share_peer_ctx *c);  /* WS 关闭，引擎保留 */
/* viewer/tab 关闭时归还：close + free。 */
void           hb_share_service_release_peer(HbShareService *s,
                                             hb_share_peer_ctx *c);

/* signals:
 *  "share-changed" (gchar *share_id)          owner ctx 状态变化（列表/参与者/mode）
 *  "peer-event"    (pointer ctx, pointer ev)  ev 借用 hb_share_event*（勿存）
 *  "peer-ended"    (pointer ctx, gchar *reason)
 *  "peer-error"    (pointer ctx, gchar *msg)
 */

G_END_DECLS

#endif
