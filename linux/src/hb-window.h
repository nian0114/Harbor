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
/* dot: "ok" | "err" | "share" | NULL */
void hb_window_set_tab_dot(HbWindow *win, const char *id, const char *dot);
void hb_window_set_tab_title(HbWindow *win, const char *id, const char *title);
void hb_window_close_tab(HbWindow *win, const char *id);
void hb_window_switch_tab(HbWindow *win, const char *id);

/* Vault 页指针：标题栏侧栏开关要翻转它的侧栏可见性 */
void hb_window_set_vault_page(HbWindow *win, GtkWidget *vault_page);

/* 打开一个 Local 终端页（标题 "Local"、terminal 图标、is_term）；
 * 返回 HbTermPage widget（供 dev --open-local=CMD 注入输入）。 */
void       hb_window_open_local(HbWindow *win, GtkWidget **page_out);

/* signals: "tab-closed"(gchar *id) */

G_END_DECLS

#endif
