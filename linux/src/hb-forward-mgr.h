#ifndef HB_FORWARD_MGR_H
#define HB_FORWARD_MGR_H

#include <gtk/gtk.h>
#include <stdint.h>
#include "hb-window.h"

G_BEGIN_DECLS

#define HB_TYPE_FORWARD_MGR (hb_forward_mgr_get_type())
G_DECLARE_FINAL_TYPE(HbForwardMgr, hb_forward_mgr, HB, FORWARD_MGR, GObject)

typedef enum {
    HB_FWD_STOPPED,       /* 已停止 */
    HB_FWD_CONNECTING,    /* 连接中（含握手/认证/远程转发请求） */
    HB_FWD_LISTENING,     /* 监听中 */
    HB_FWD_RECONNECTING,  /* 断开，退避重连中 */
    HB_FWD_FAILED,        /* 失败（不可重试） */
} hb_fwd_state;

/* 单条规则的运行时状态快照（hb_forward_mgr_status 填充；message 借用到
 * 下一次 changed）。 */
typedef struct {
    hb_fwd_state state;
    int          bound_port;        /* LISTENING：实际监听端口（remote=服务端端口） */
    uint64_t     bytes_up, bytes_down;
    uint32_t     active, total;
    const char  *message;           /* 最近 WARN / 失败原因，NULL=无 */
    int          retry_in_s;        /* RECONNECTING：距下次重试秒数 */
} hb_fwd_status;

HbForwardMgr *hb_forward_mgr_default(void);

/* HbWindow 构造时调用一次：window 作为连接期对话框（TOFU/口令/密钥
 * 确认）的父级，必须已根植 —— 故不可为 NULL 时启动规则。 */
void hb_forward_mgr_set_window(HbForwardMgr *m, HbWindow *win);

void hb_forward_mgr_start(HbForwardMgr *m, const char *rule_id);
void hb_forward_mgr_stop(HbForwardMgr *m, const char *rule_id);
void hb_forward_mgr_start_all(HbForwardMgr *m);   /* enabled=true 的全部 */
void hb_forward_mgr_stop_all(HbForwardMgr *m);
/* 运行中（含退避重连）的规则数 —— tray "在线会话" 计数用 */
guint hb_forward_mgr_running_count(HbForwardMgr *m);

/* 当前状态；FALSE = 从未启动（按 STOPPED 处理）。 */
gboolean hb_forward_mgr_status(HbForwardMgr *m, const char *rule_id,
                               hb_fwd_status *out);

/* hb-conn.c 的 connected 钩子：SHELL/SFTP 连上 host X 后调用，拉起
 * via==X && autostart 的所有停止规则（FORWARD 连接不触发）。 */
void hb_forward_mgr_autostart(const char *host_id);

/* signals: "changed"(gchar *rule_id) — id 为 NULL 表示多条/批量 */

G_END_DECLS

#endif
