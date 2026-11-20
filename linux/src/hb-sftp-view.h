#ifndef HB_SFTP_VIEW_H
#define HB_SFTP_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_SFTP_VIEW (hb_sftp_view_get_type())
G_DECLARE_FINAL_TYPE(HbSftpView, hb_sftp_view, HB, SFTP_VIEW, GtkBox)

/* SFTP 双栏文件管理（FR-05/FR-19）：左/右两个 pane
 * （Select Host → Connecting → Browser）+ 底部传输队列。 */
GtkWidget *hb_sftp_view_new(HbStore *st);

/* add_tab 后关联（tab_id 用于通知路由 / 桌面通知焦点判定） */
void hb_sftp_view_attach_tab(HbSftpView *v, GtkWidget *win,
                             const char *tab_id);

/* 入口点：左栏 = Local home，右栏 = host_id 并开始连接 */
void hb_sftp_view_open_host(HbSftpView *v, const char *host_id);

G_END_DECLS

#endif
