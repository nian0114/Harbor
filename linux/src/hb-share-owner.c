/* owner 侧页胶合（hb-share-owner.h）。 */

#include "hb-share-owner.h"
#include "hb-share.h"
#include "hb-window.h"
#include "hb-notify.h"
#include "hb-sync.h"

#include <string.h>

typedef struct {
    hb_session *sess;            /* borrowed */
    GtkWidget  *btn;
    GtkWidget  *win;             /* HbWindow borrowed */
    char       *tab_id;
    gulong      s1, s2;
} ShareWire;

static void wire_refresh(ShareWire *w)
{
    HbShareService *s = hb_share_service_default();
    const char *sid = hb_share_service_share_for_session(s, w->sess);
    gboolean on = sid != NULL;
    gtk_button_set_label(GTK_BUTTON(w->btn),
                         on ? "共享中" : "共享");
    if (on)
        gtk_widget_add_css_class(w->btn, "on");
    else
        gtk_widget_remove_css_class(w->btn, "on");
    if (w->win && w->tab_id)
        hb_window_set_tab_dot(HB_WINDOW(w->win), w->tab_id,
                              on ? "share" : NULL);
    if (on && w->tab_id)
        hb_share_service_bind_tab(s, sid, w->tab_id);
}

static void on_changed(HbShareService *s, const char *sid,
                       ShareWire *w)
{
    const char *my;
    (void)sid;
    my = hb_share_service_share_for_session(s, w->sess);
    /* 全局 changed 都可能影响本 session（start/stop/mode） */
    (void)my;
    wire_refresh(w);
}

static void on_owner_event(HbShareService *s, const char *sid,
                           int type, guint pid,
                           const char *name, const char *device,
                           const char *text, ShareWire *w)
{
    const char *my;
    char *title, *body;
    (void)pid; (void)text;
    my = hb_share_service_share_for_session(s, w->sess);
    if (!my || !sid || strcmp(sid, my)) return;
    switch (type) {
    case HB_SH_EV_JOINED:
    case HB_SH_EV_PAKE_OK:
        title = g_strdup_printf("%s 加入共享",
            device && *device ? device :
            (name && *name ? name : "guest"));
        body = g_strdup_printf("正在%s",
            hb_share_service_mode(s, my) == HB_SHARE_COLLAB
                ? "协作" : "观看");
        hb_notify_post("share", title, body, w->tab_id);
        g_free(title); g_free(body);
        break;
    case HB_SH_EV_LEFT:
        title = g_strdup_printf("%s 离开",
            device && *device ? device :
            (name && *name ? name : "guest"));
        hb_notify_post("share", title, NULL, w->tab_id);
        g_free(title);
        break;
    default:
        break;
    }
}

static void wire_free(gpointer p)
{
    ShareWire *w = p;
    g_signal_handler_disconnect(hb_share_service_default(), w->s1);
    g_signal_handler_disconnect(hb_share_service_default(), w->s2);
    if (w->win && w->tab_id)
        hb_window_set_tab_dot(HB_WINDOW(w->win), w->tab_id, NULL);
    g_free(w->tab_id);
    g_free(w);
}

static void on_page_destroy(GtkWidget *page, gpointer ud)
{ (void)page; wire_free(ud); }

void hb_share_owner_wire(GtkWidget *page, hb_session *sess,
                         GtkWidget *share_btn,
                         GtkWidget *win, const char *tab_id)
{
    ShareWire *w;
    if (!page || !sess || !share_btn) return;
    /* 旧 wire（sess 变更时）清掉 */
    {
        gpointer old = g_object_get_data(G_OBJECT(page),
                                         "hb-share-wire");
        if (old) {
            g_object_set_data(G_OBJECT(page), "hb-share-wire", NULL);
            wire_free(old);
        }
    }
    w = g_new0(ShareWire, 1);
    w->sess = sess;
    w->btn = share_btn;
    w->win = win;
    w->tab_id = g_strdup(tab_id);
    w->s1 = g_signal_connect(hb_share_service_default(),
        "share-changed", G_CALLBACK(on_changed), w);
    w->s2 = g_signal_connect(hb_share_service_default(),
        "owner-event", G_CALLBACK(on_owner_event), w);
    g_object_set_data(G_OBJECT(page), "hb-share-wire", w);
    g_signal_connect(page, "destroy",
                     G_CALLBACK(on_page_destroy), w);
    wire_refresh(w);
}
