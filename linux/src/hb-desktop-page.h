#ifndef HB_DESKTOP_PAGE_H
#define HB_DESKTOP_PAGE_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_DESKTOP_PAGE (hb_desktop_page_get_type())
G_DECLARE_FINAL_TYPE(HbDesktopPage, hb_desktop_page,
                     HB, DESKTOP_PAGE, GtkBox)

/* FR-07 VNC/RDP 远程桌面页：可选 SSH 隧道（tab 私有，不落 vault
 * 转发规则）+ rd core 会话 + 画布 + 断线重连。
 * signals: "edit-host"(gchar *host_id) —— 错误卡的「编辑主机」。 */
GtkWidget *hb_desktop_page_new(HbStore *st, hb_obj *host);
void       hb_desktop_page_attach_tab(HbDesktopPage *p, GtkWidget *win,
                                      const char *tab_id);

/* tray "在线会话" 计数用；tray "全部断开"：detach 当前 rd/tun
 * 会话并回 connect 视图（tab 保留）。 */
gboolean   hb_desktop_page_connected(HbDesktopPage *p);
void       hb_desktop_page_disconnect(HbDesktopPage *p);

G_END_DECLS

#endif
