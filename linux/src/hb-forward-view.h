#ifndef HB_FORWARD_VIEW_H
#define HB_FORWARD_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_FORWARD_VIEW (hb_forward_view_get_type())
G_DECLARE_FINAL_TYPE(HbForwardView, hb_forward_view, HB, FORWARD_VIEW,
                     GtkBox)

GtkWidget *hb_forward_view_new(HbStore *st);

G_END_DECLS

#endif
