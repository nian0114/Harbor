#ifndef HB_KEYCHAIN_VIEW_H
#define HB_KEYCHAIN_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_KEYCHAIN_VIEW (hb_keychain_view_get_type())
G_DECLARE_FINAL_TYPE(HbKeychainView, hb_keychain_view,
                     HB, KEYCHAIN_VIEW, GtkBox)

GtkWidget *hb_keychain_view_new(HbStore *st);

/* "生成新密钥" 对话框：也可由主机表单的密钥下拉打开。
 * done_cb 在对话框关闭后调用一次：new_key_id 为 NULL 表示取消/失败。 */
typedef void (*hb_keychain_gen_cb)(const char *new_key_id, gpointer ud);
void hb_keychain_generate_dialog(GtkWidget *parent, HbStore *st,
                                 hb_keychain_gen_cb done_cb, gpointer ud);

G_END_DECLS

#endif
