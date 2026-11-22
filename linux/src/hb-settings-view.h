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

/* 数据面板按钮的程序化入口（dev-ui / 测试）。 */
void hb_settings_view_export_backup(HbSettingsView *v);
void hb_settings_view_import_backup(HbSettingsView *v);
void hb_settings_view_bk_dev_out(const char *path);
void hb_settings_view_bk_dev_fill(const char *txt);
void hb_settings_view_bk_dev_press(void);
void hb_settings_view_bk_dev_confirm(gboolean yes);
void hb_settings_view_import_backup_dev(HbSettingsView *v,
                                        const char *path);
gboolean hb_settings_view_sc_dev(HbSettingsView *v, size_t idx,
                                 const char *accel);
