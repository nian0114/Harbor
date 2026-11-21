#ifndef HB_ACCOUNT_VIEW_H
#define HB_ACCOUNT_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_ACCOUNT_VIEW (hb_account_view_get_type())
G_DECLARE_FINAL_TYPE(HbAccountView, hb_account_view, HB, ACCOUNT_VIEW,
                     GtkBox)

GtkWidget *hb_account_view_new(HbStore *store);

#ifndef NDEBUG
/* dev only：弹出恢复码网格（用于 headless 截图）。 */
void       hb_account_view_dev_words(GtkWidget *any, const char *words);
#endif

G_END_DECLS

#endif
