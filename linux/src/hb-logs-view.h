#ifndef HB_LOGS_VIEW_H
#define HB_LOGS_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_LOGS_VIEW (hb_logs_view_get_type())
G_DECLARE_FINAL_TYPE(HbLogsView, hb_logs_view,
                     HB, LOGS_VIEW, GtkBox)

GtkWidget *hb_logs_view_new(HbStore *store);

G_END_DECLS

#endif
