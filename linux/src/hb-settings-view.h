#ifndef HB_SETTINGS_VIEW_H
#define HB_SETTINGS_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define HB_TYPE_SETTINGS_VIEW (hb_settings_view_get_type())
G_DECLARE_FINAL_TYPE(HbSettingsView, hb_settings_view,
                     HB, SETTINGS_VIEW, GtkBox)

GtkWidget *hb_settings_view_new(void);

G_END_DECLS

#endif
