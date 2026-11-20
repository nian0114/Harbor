#ifndef HB_CONNECT_VIEW_H
#define HB_CONNECT_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define HB_TYPE_CONNECT_VIEW (hb_connect_view_get_type())
G_DECLARE_FINAL_TYPE(HbConnectView, hb_connect_view,
                     HB, CONNECT_VIEW, GtkBox)

/* 纯展示的连接视图：头像头 + 3 节点 stepper + 阶段日志
 * （Hide/Show logs）+ 错误卡（详情折叠 + Close/编辑主机/重试）
 * + 底部 Close / Start over。由 HbConnCtl 信号驱动，终端页与
 * SFTP 面板共用。 */
GtkWidget *hb_connect_view_new(void);

void hb_connect_view_set_head(HbConnectView *v, const char *avatar_key,
                              const char *name, const char *sub);

/* 新一轮连接：节点归零、清空日志、移除错误卡、恢复 footer/hide-btn */
void hb_connect_view_reset(HbConnectView *v);
/* 仅移除错误卡（不碰日志/节点 —— 重连途中再次发起时用） */
void hb_connect_view_clear_error(HbConnectView *v);

/* 阶段日志行；mark='e' → 红色行且不推进节点；idx 单调推进节点 */
void hb_connect_view_stage(HbConnectView *v, int idx,
                           const char *text, char mark);

/* 错误卡；code=HB_SSH_ERR_HOSTKEY_CHANGED 时附红字安全提示 */
void hb_connect_view_error(HbConnectView *v, const char *msg,
                           const char *detail, int code);

/* 错误卡里是否显示 "编辑主机"（默认 FALSE） */
void hb_connect_view_set_edit_btn(HbConnectView *v, gboolean show);

/* signals:
 *   "close"      () — footer/error卡 Close
 *   "start-over" () — footer Start over / error卡 重试
 *   "edit-host"  () — error卡 编辑主机（仅 set_edit_btn(TRUE) 时可见）
 */

G_END_DECLS

#endif
