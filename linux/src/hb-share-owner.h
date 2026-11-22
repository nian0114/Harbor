#ifndef HB_SHARE_OWNER_H
#define HB_SHARE_OWNER_H

#include <adwaita.h>
#include <harbor/session.h>

G_BEGIN_DECLS

/* owner 侧页胶合：共享按钮文案、tab "share" 角标、加入/离开通知、
 * tab 绑定（卡片点击切换）。sess 为 borrowed；调用方保证先
 * hb_share_service_stop_session 再释放 sess（exit/dispose）。
 * page 销毁自动断开信号。 */
void hb_share_owner_wire(GtkWidget *page, hb_session *sess,
                         GtkWidget *share_btn,
                         GtkWidget *win, const char *tab_id);

G_END_DECLS

#endif
