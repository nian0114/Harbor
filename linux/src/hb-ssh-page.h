#ifndef HB_SSH_PAGE_H
#define HB_SSH_PAGE_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_SSH_PAGE (hb_ssh_page_get_type())
G_DECLARE_FINAL_TYPE(HbSshPage, hb_ssh_page, HB, SSH_PAGE, GtkBox)

/* host = vault host 对象（借用；页面自己深拷贝所需字段并保留 id）。
 * 立即开始连接，先展示连接进度页，连通后原地切换为终端。 */
GtkWidget *hb_ssh_page_new(HbStore *st, const hb_obj *host);

/* add_tab 之后调用：关联 tab id（更新标题/图标/dot、关 tab 用） */
void hb_ssh_page_attach_tab(HbSshPage *p, GtkWidget *win, const char *tab_id);

/* 供 dev 调试：像用户输入一样把文本写进会话 */
void hb_ssh_page_feed_input(HbSshPage *p, const char *text);

/* signals: "edit-host"(gchar *host_id) — 目前无人连接（主机 CRUD 单元预留） */

G_END_DECLS

#endif
