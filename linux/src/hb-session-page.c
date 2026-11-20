#include "hb-session-page.h"

G_DEFINE_INTERFACE(HbSessionPage, hb_session_page, GTK_TYPE_WIDGET)

static void hb_session_page_default_init(HbSessionPageInterface *iface)
{
    (void)iface;
    g_signal_new("meta-changed", HB_TYPE_SESSION_PAGE,
                 G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);
}

#define GET(p) HB_SESSION_PAGE_GET_IFACE(p)

HbTerminal *hb_session_page_terminal(HbSessionPage *p)
{
    return GET(p)->terminal(p);
}

const char *hb_session_page_title(HbSessionPage *p)
{
    return GET(p)->title(p);
}

const char *hb_session_page_icon(HbSessionPage *p)
{
    return GET(p)->icon(p);
}

void hb_session_page_set_compact(HbSessionPage *p, gboolean compact)
{
    GET(p)->set_compact(p, compact);
}

GtkWidget *hb_session_page_status_widget(HbSessionPage *p)
{
    return GET(p)->status_widget(p);
}

void hb_session_page_open_find(HbSessionPage *p)
{
    GET(p)->open_find(p);
}

void hb_session_page_open_watch(HbSessionPage *p)
{
    GET(p)->open_watch(p);
}

gboolean hb_session_page_is_dead(HbSessionPage *p)
{
    return GET(p)->is_dead(p);
}

void hb_session_page_reconnect(HbSessionPage *p)
{
    GET(p)->reconnect(p);
}

GtkWidget *hb_session_page_clone(HbSessionPage *p)
{
    return GET(p)->clone(p);
}

const char *hb_session_page_host_id(HbSessionPage *p)
{
    return GET(p)->host_id(p);
}

void hb_session_page_attach_tab(HbSessionPage *p, GtkWidget *win,
                                const char *tab_id)
{
    GET(p)->attach_tab(p, win, tab_id);
}

void hb_session_page_meta_changed(HbSessionPage *p)
{
    g_signal_emit_by_name(p, "meta-changed");
}
