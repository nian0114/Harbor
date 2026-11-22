#ifndef HB_WINDOW_H
#define HB_WINDOW_H

#include <adwaita.h>

G_BEGIN_DECLS

#define HB_TYPE_WINDOW (hb_window_get_type())
G_DECLARE_FINAL_TYPE(HbWindow, hb_window, HB, WINDOW, AdwApplicationWindow)

HbWindow *hb_window_new(GtkApplication *app);

/* id 唯一；page 由窗口接管所有权；icon 为 hb-icon 名；is_term → 绿色终端样式 */
void hb_window_add_tab(HbWindow *win, const char *id, const char *title,
                       const char *icon, GtkWidget *page,
                       gboolean closable, gboolean is_term);
/* dot: "ok" | "err" | "share" | "pending" | NULL */
void hb_window_set_tab_dot(HbWindow *win, const char *id, const char *dot);
/* 监控角标：与普通 dot 相同绘制，但额外标记为 watch 角标 ——
 * 切到该 tab 时自动清除。返回 token（0=tab 不存在），供
 * clear_watch_dot 做"只清掉自己这颗"的时序防护。 */
guint hb_window_set_watch_dot(HbWindow *win, const char *id,
                              const char *dot);
void hb_window_clear_watch_dot(HbWindow *win, const char *id, guint token);
/* 当前活动 tab 的 id / page（借用指针；无活动 tab → NULL） */
const char *hb_window_active_tab_id(HbWindow *win);
/* tray 聚合：本窗口在线会话数 / 断开全部会话（tab 保留） */
int  hb_window_online_count(HbWindow *win);
void hb_window_disconnect_all(HbWindow *win);
GtkWidget *hb_window_active_page(HbWindow *win);
/* 按 id 取 tab 页面（借用；无 → NULL） */
GtkWidget *hb_window_tab_page(HbWindow *win, const char *id);
void hb_window_set_tab_title(HbWindow *win, const char *id, const char *title);
void hb_window_set_tab_icon(HbWindow *win, const char *id, const char *icon);

/* toast：经窗口顶部的 AdwToastOverlay 弹出 */
void hb_window_toast(HbWindow *win, const char *text);
void hb_window_toast_undo(HbWindow *win, const char *text,
                          GCallback undo_cb, gpointer ud);
/* toast 带自定义按钮（撤销之外的动作，如"查看/编辑"） */
void hb_window_toast_action(HbWindow *win, const char *text,
                            const char *btn_label, int timeout,
                            GCallback cb, gpointer ud);
/* 同 toast_action，但 toast 消失（超时/点按钮/手动关）时用
 * destroy(ud) 释放用户数据。 */
void hb_window_toast_action2(HbWindow *win, const char *text,
                             const char *btn_label, int timeout,
                             GCallback cb, GDestroyNotify destroy,
                             gpointer ud);
/* tab ↔ vault host 关联：删除主机时按 host_id 关页（不拼 tab id） */
void hb_window_set_tab_host(HbWindow *win, const char *id,
                            const char *host_id);
void hb_window_close_host_tabs(HbWindow *win, const char *host_id);

gboolean hb_window_has_tab(HbWindow *win, const char *id);
void hb_window_close_tab(HbWindow *win, const char *id);
void hb_window_switch_tab(HbWindow *win, const char *id);

/* 按序号/相对位置切 tab（快捷键用）；idx 0 = Vault。越界 no-op。 */
void hb_window_switch_tab_n(HbWindow *win, guint idx);
void hb_window_switch_tab_rel(HbWindow *win, int dir);

/* Vault 页指针：标题栏侧栏开关要翻转它的侧栏可见性 */
void hb_window_set_vault_page(HbWindow *win, GtkWidget *vault_page);
GtkWidget *hb_window_vault_page(HbWindow *win);   /* borrowed，可能为 NULL */

/* 全窗口浮层（palette 等）：调用方 unparent/remove 自己来关闭 */
void hb_window_overlay_add(HbWindow *win, GtkWidget *w);

/* 打开一个 Local 终端页（标题 "Local"、terminal 图标、is_term）；
 * 返回 HbTermPage widget（供 dev --open-local=CMD 注入输入）。 */
void       hb_window_open_local(HbWindow *win, GtkWidget **page_out);

/* ------------------------- Workspace 分屏 ------------------------- */

/* 当前 tab 是终端页 → 包成 workspace 并分屏（H=右，V=下）；
 * 已是 workspace → 焦点 pane clone 后分屏。 */
void       hb_window_split_active(HbWindow *win, GtkOrientation o);
/* Ctrl+Shift+W：workspace 且 >1 pane → 关焦点 pane（剩 1 个时自动
 * 塌回普通 tab）；否则关整个 tab。 */
void       hb_window_close_pane_or_tab(HbWindow *win);
/* ws 内部关 pane 后回调：剩 1 → 塌回普通 tab；0 → 关 tab */
void       hb_window_workspace_panes_changed(HbWindow *win,
                                             GtkWidget *ws);
/* 活动 tab 的 session 页：workspace → 焦点 pane；否则原样。供
 * palette/snippet/feed/查找 解析"当前终端"。 */
GtkWidget *hb_window_active_session_page(HbWindow *win);
/* Alt+方向：workspace 内几何移动焦点；非 ws no-op */
void       hb_window_move_pane_focus(HbWindow *win, int dir);
/* Ctrl+Shift+M：ws 的焦点模式切换；非 ws no-op */
void       hb_window_toggle_focus_mode(HbWindow *win);
/* 从目标选择对话框创建：pages 是一列 HbSessionPage 页（borrowed，
 * ws 接管），并排一行。 */
void       hb_window_open_workspace(HbWindow *win, GList *pages);

/* signals: "tab-closed"(gchar *id) */

G_END_DECLS

#endif
