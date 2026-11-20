#ifndef HB_KNOWN_VIEW_H
#define HB_KNOWN_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_KNOWN_VIEW (hb_known_view_get_type())
G_DECLARE_FINAL_TYPE(HbKnownView, hb_known_view,
                     HB, KNOWN_VIEW, GtkBox)

GtkWidget *hb_known_view_new(HbStore *store);

G_END_DECLS

#endif
