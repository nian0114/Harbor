#ifndef HB_APPLICATION_H
#define HB_APPLICATION_H

#include <adwaita.h>

G_BEGIN_DECLS

#define HB_TYPE_APPLICATION (hb_application_get_type())
G_DECLARE_FINAL_TYPE(HbApplication, hb_application, HB, APPLICATION, AdwApplication)

HbApplication *hb_application_new(void);

/* FR-16 托盘/自启动：settings 页查询 watcher、写 user unit。 */
gboolean hb_application_tray_available(void);

void     hb_app_note_sessions(void);   /* 会话数可能变了 → 刷托盘 */
extern int hb_opt_background;

G_END_DECLS

#endif
