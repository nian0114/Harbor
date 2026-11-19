#include "hb-notify.h"

#include <glib.h>

struct _HbNotifItem {
    GObject parent;
    char *kind, *title, *body, *tab_id, *time;
    gboolean unread;
};

G_DEFINE_TYPE(HbNotifItem, hb_notif_item, G_TYPE_OBJECT)

static void hb_notif_item_finalize(GObject *o)
{
    HbNotifItem *n = HB_NOTIF_ITEM(o);
    g_free(n->kind); g_free(n->title); g_free(n->body);
    g_free(n->tab_id); g_free(n->time);
    G_OBJECT_CLASS(hb_notif_item_parent_class)->finalize(o);
}
static void hb_notif_item_class_init(HbNotifItemClass *k)
{ G_OBJECT_CLASS(k)->finalize = hb_notif_item_finalize; }
static void hb_notif_item_init(HbNotifItem *n) { (void)n; }

const char *hb_notif_item_kind(HbNotifItem *n)   { return n->kind; }
const char *hb_notif_item_title(HbNotifItem *n)  { return n->title; }
const char *hb_notif_item_body(HbNotifItem *n)   { return n->body; }
const char *hb_notif_item_tab_id(HbNotifItem *n) { return n->tab_id; }
const char *hb_notif_item_time(HbNotifItem *n)   { return n->time; }
gboolean    hb_notif_item_unread(HbNotifItem *n) { return n->unread; }
void        hb_notif_item_set_read(HbNotifItem *n) { n->unread = FALSE; }

/* ------------------------------------------------ */

struct _HbNotifications {
    GObject parent;
    GListStore *items;      /* HbNotifItem, newest first */
};

G_DEFINE_TYPE(HbNotifications, hb_notifications, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void hb_notifications_dispose(GObject *o)
{
    HbNotifications *self = HB_NOTIFICATIONS(o);
    g_clear_object(&self->items);
    G_OBJECT_CLASS(hb_notifications_parent_class)->dispose(o);
}
static void hb_notifications_class_init(HbNotificationsClass *k)
{
    G_OBJECT_CLASS(k)->dispose = hb_notifications_dispose;
    sigs[SIG_CHANGED] = g_signal_new("changed", HB_TYPE_NOTIFICATIONS,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void hb_notifications_init(HbNotifications *self)
{
    self->items = g_list_store_new(HB_TYPE_NOTIF_ITEM);
}

HbNotifications *hb_notify_default(void)
{
    static HbNotifications *s;
    if (!s) s = g_object_new(HB_TYPE_NOTIFICATIONS, NULL);
    return s;
}

GListModel *hb_notify_model(void)
{
    return G_LIST_MODEL(hb_notify_default()->items);
}

void hb_notify_post(const char *kind, const char *title,
                    const char *body, const char *tab_id)
{
    HbNotifications *self = hb_notify_default();
    HbNotifItem *n = g_object_new(HB_TYPE_NOTIF_ITEM, NULL);
    GDateTime *now = g_date_time_new_now_local();
    n->kind = g_strdup(kind ? kind : "info");
    n->title = g_strdup(title ? title : "");
    n->body = g_strdup(body ? body : "");
    n->tab_id = g_strdup(tab_id);
    n->time = g_date_time_format(now, "%H:%M");
    n->unread = TRUE;
    g_date_time_unref(now);
    g_list_store_insert(self->items, 0, n);
    g_object_unref(n);
    g_signal_emit(self, sigs[SIG_CHANGED], 0);
}

guint hb_notify_unread(void)
{
    HbNotifications *self = hb_notify_default();
    guint i, n = g_list_model_get_n_items(G_LIST_MODEL(self->items)), c = 0;
    for (i = 0; i < n; i++) {
        HbNotifItem *it = g_list_model_get_item(G_LIST_MODEL(self->items), i);
        if (hb_notif_item_unread(it)) c++;
        g_object_unref(it);
    }
    return c;
}

void hb_notify_mark_all_read(void)
{
    HbNotifications *self = hb_notify_default();
    guint i, n = g_list_model_get_n_items(G_LIST_MODEL(self->items));
    for (i = 0; i < n; i++) {
        HbNotifItem *it = g_list_model_get_item(G_LIST_MODEL(self->items), i);
        hb_notif_item_set_read(it);
        g_object_unref(it);
    }
    g_signal_emit(self, sigs[SIG_CHANGED], 0);
}

void hb_notify_clear(void)
{
    HbNotifications *self = hb_notify_default();
    g_list_store_remove_all(self->items);
    g_signal_emit(self, sigs[SIG_CHANGED], 0);
}
