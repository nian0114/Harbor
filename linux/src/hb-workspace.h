#ifndef HB_WORKSPACE_H
#define HB_WORKSPACE_H

#include <gtk/gtk.h>
#include "hb-store.h"
#include "hb-terminal.h"

G_BEGIN_DECLS

/* Workspace：一棵 GtkPaned 树，叶子是 HbSessionPage（compact）。
 * 顶部工具栏（Broadcast/助手栏/Focus/搜索），每个 pane 有 pane-head，
 * 底部一条 footer 显示焦点 pane 的状态条。 */
#define HB_TYPE_WORKSPACE (hb_workspace_get_type())
G_DECLARE_FINAL_TYPE(HbWorkspace, hb_workspace, HB, WORKSPACE, GtkBox)

GtkWidget *hb_workspace_new(HbStore *st);

/* add_tab 之后调用：转发 attach 到所有 pane（watch 路由/tab 归属） */
void   hb_workspace_attach_tab(HbWorkspace *w, GtkWidget *win,
                               const char *tab_id);

/* 把一个 session page 作为首个 pane（不 clone） */
void   hb_workspace_seed(HbWorkspace *w, GtkWidget *page);

/* 焦点 pane clone 一个挂到其右/下；>=6 pane → FALSE（动作置灰用） */
gboolean hb_workspace_split_focused(HbWorkspace *w, GtkOrientation o);
gboolean hb_workspace_can_split(HbWorkspace *w);

/* 直接把一个已建好的 session page 挂到最后一个 leaf 的 o 侧，
 * paned 初始比例 frac（批量建"一行 N 个"用） */
void   hb_workspace_add_pane(HbWorkspace *w, GtkWidget *page,
                             GtkOrientation o, double frac);

/* 关闭焦点 pane。返回 TRUE = workspace 只剩 1 pane，
 * 调用方应 hb_workspace_take_last_page + 换 tab 页并销毁 ws */
gboolean hb_workspace_close_focused(HbWorkspace *w);
/* 关闭指定 pane（pane-head × 用；host_id 删除批调用走 close_host） */
gboolean hb_workspace_close_page(HbWorkspace *w, GtkWidget *page);
/* 删除某 host 的所有 pane；返回 TRUE 若 ws 已空/只剩 1 pane（调用方处理收尾） */
gboolean hb_workspace_close_host(HbWorkspace *w, const char *host_id);
/* 取出仅剩的那个 pane 页（已 set_compact(FALSE)、已从 ws 摘除，
 * 返回引用转移 full）；ws 随后可销毁 */
GtkWidget *hb_workspace_take_last_page(HbWorkspace *w);

guint      hb_workspace_pane_count(HbWorkspace *w);
/* 焦点 pane 的页 widget（HbSessionPage 实现），供 active_page 解析 */
GtkWidget *hb_workspace_focused_page(HbWorkspace *w);
/* session page → 焦点 pane 页；非 ws 原样返回（palette/feed/shortcut 解析用） */
GtkWidget *hb_workspace_resolve_page(GtkWidget *page);

void     hb_workspace_toggle_focus_mode(HbWorkspace *w);
gboolean hb_workspace_is_focus_mode(HbWorkspace *w);
gboolean hb_workspace_is_broadcast(HbWorkspace *w);
void     hb_workspace_move_focus(HbWorkspace *w, int dir); /* 0左 1右 2上 3下 */
void     hb_workspace_open_find(HbWorkspace *w);

/* 目标选择对话框：2-4 个目标（hosts + Local）→ 新建 workspace tab */
void     hb_workspace_pick_targets(GtkWidget *win, HbStore *st);

G_END_DECLS

#endif
