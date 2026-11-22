#ifndef HB_SHARE_DIALOG_H
#define HB_SHARE_DIALOG_H

#include <adwaita.h>
#include <harbor/session.h>

G_BEGIN_DECLS

/* owner 共享对话框；sess 活跃时复用现有 share，否则由开关启动。 */
void hb_share_dialog_present(GtkWidget *parent, hb_session *sess,
                             const char *title, const char *host);

G_END_DECLS

#endif
