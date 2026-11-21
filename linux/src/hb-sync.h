#ifndef HB_SYNC_SERVICE_H
#define HB_SYNC_SERVICE_H

#include <glib-object.h>
#include <harbor/sync.h>

G_BEGIN_DECLS

#define HB_TYPE_SYNC_SERVICE (hb_sync_service_get_type())
G_DECLARE_FINAL_TYPE(HbSyncService, hb_sync_service, HB, SYNC_SERVICE, GObject)

typedef enum {
    HB_SYNC_SIGNED_OUT,   /* 未登录 */
    HB_SYNC_SYNCING,      /* 正在拉/推 */
    HB_SYNC_IDLE,         /* 已登录、已同步 */
    HB_SYNC_OFFLINE,      /* 网络/服务端错误，重试中 */
    HB_SYNC_ERROR,        /* 需用户处理的错误（如凭证失效） */
} HbSyncState;

/* 应用级单例：绑定 hb_store_default() 的 vault。首次调用即创建，
 * 自动尝试用已存凭证恢复会话。仅 UI 线程使用。 */
HbSyncService *hb_sync_service_default(void);

HbSyncState  hb_sync_service_state(HbSyncService *s);
const char  *hb_sync_service_status(HbSyncService *s);    /* 状态行 */
const char  *hb_sync_service_email(HbSyncService *s);     /* 未登录 NULL */
const char  *hb_sync_service_server(HbSyncService *s);
const char  *hb_sync_service_last_error(HbSyncService *s);
gint64       hb_sync_service_last_sync(HbSyncService *s); /* unix 秒，0=从未 */
gsize        hb_sync_service_conflicts(HbSyncService *s);
gboolean     hb_sync_service_totp_enabled(HbSyncService *s);
gboolean     hb_sync_service_busy(HbSyncService *s);      /* 账号操作进行中 */

/* core 引擎（borrowed，仅主线程）：scope/conflicts 等直查 */
hb_sync     *hb_sync_service_sync(HbSyncService *s);

/* signals: "changed" */

/* ---- 操作结果 ---- */
typedef struct {
    gboolean    ok;
    const char *code;         /* invalid_credentials|totp_required|rate_limited|
                               * network|server_error|email_taken */
    const char *message;      /* 可直接展示 */
    int         retry_after;  /* rate_limited 秒 */
    char       *words;        /* register/rotate：24 词（调用方 g_free） */
    gboolean    need_merge;   /* login/recover：本地有范围内数据，询问合并 */
    char       *totp_secret;  /* totp/setup */
    char       *totp_uri;
    GPtrArray  *devices;      /* devices() 返回 HbSyncDevice* */
} HbSyncOpResult;

typedef struct {
    char    *id, *name, *platform;
    gint64   created, last_seen;
    gboolean current;
} HbSyncDevice;

typedef void (*HbSyncOpCb)(HbSyncService *s, const HbSyncOpResult *r,
                           gpointer ud);

/* server：仅接受 https:// 或回环 http://（§6）；返回规范化后的 URL
 * （无尾随 /），不合法返回 NULL（g_free） */
char *hb_sync_check_server(const char *url);

/* 账号操作：全部异步；password 调用方先做 NFKC 归一由服务内部完成。 */
void hb_sync_service_login(HbSyncService *s, const char *server,
                           const char *email, const char *password,
                           const char *totp, HbSyncOpCb cb, gpointer ud);
void hb_sync_service_register(HbSyncService *s, const char *server,
                              const char *email, const char *password,
                              HbSyncOpCb cb, gpointer ud);
void hb_sync_service_recover(HbSyncService *s, const char *server,
                             const char *email, const char *words,
                             const char *new_password,
                             HbSyncOpCb cb, gpointer ud);
/* 登录/恢复返回 need_merge 后调用：merge=TRUE 合并本机数据，FALSE 丢弃 */
void hb_sync_service_confirm_merge(HbSyncService *s, gboolean merge);

void hb_sync_service_logout(HbSyncService *s, gboolean wipe);
void hb_sync_service_rotate_recovery(HbSyncService *s, const char *password,
                                     HbSyncOpCb cb, gpointer ud);
void hb_sync_service_change_password(HbSyncService *s, const char *old_pw,
                                     const char *new_pw,
                                     HbSyncOpCb cb, gpointer ud);
void hb_sync_service_totp_setup(HbSyncService *s, HbSyncOpCb cb,
                                gpointer ud);
void hb_sync_service_totp_enable(HbSyncService *s, const char *code,
                                 HbSyncOpCb cb, gpointer ud);
void hb_sync_service_totp_disable(HbSyncService *s, const char *code,
                                  HbSyncOpCb cb, gpointer ud);
void hb_sync_service_devices(HbSyncService *s, HbSyncOpCb cb,
                             gpointer ud);
void hb_sync_service_revoke(HbSyncService *s, const char *device_id,
                            gboolean wipe, HbSyncOpCb cb, gpointer ud);

/* 立即同步一轮 + 调整 scope（并触发同步） */
void hb_sync_service_sync_now(HbSyncService *s);
void hb_sync_service_set_scope(HbSyncService *s, const char *scope,
                               gboolean on);

G_END_DECLS

#endif
