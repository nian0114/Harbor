#ifndef HB_NOTIFY_H
#define HB_NOTIFY_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define HB_TYPE_NOTIF_ITEM (hb_notif_item_get_type())
G_DECLARE_FINAL_TYPE(HbNotifItem, hb_notif_item, HB, NOTIF_ITEM, GObject)

const char *hb_notif_item_kind(HbNotifItem *n);    /* "ok"|"err"|"info"|"share" */
const char *hb_notif_item_title(HbNotifItem *n);
const char *hb_notif_item_body(HbNotifItem *n);
const char *hb_notif_item_tab_id(HbNotifItem *n);
const char *hb_notif_item_time(HbNotifItem *n);    /* "HH:MM" */
gboolean    hb_notif_item_unread(HbNotifItem *n);
void        hb_notif_item_set_read(HbNotifItem *n);

#define HB_TYPE_NOTIFICATIONS (hb_notifications_get_type())
G_DECLARE_FINAL_TYPE(HbNotifications, hb_notifications, HB, NOTIFICATIONS, GObject)

HbNotifications *hb_notify_default(void);
GListModel      *hb_notify_model(void);                       /* newest first */
void  hb_notify_post(const char *kind, const char *title,
                     const char *body, const char *tab_id);
guint hb_notify_unread(void);
void  hb_notify_mark_all_read(void);
void  hb_notify_clear(void);

/* signals: "changed" */

G_END_DECLS

#endif
