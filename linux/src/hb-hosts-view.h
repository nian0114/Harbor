#ifndef HB_HOSTS_VIEW_H
#define HB_HOSTS_VIEW_H

#include <gtk/gtk.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_HOSTS_VIEW (hb_hosts_view_get_type())
G_DECLARE_FINAL_TYPE(HbHostsView, hb_hosts_view, HB, HOSTS_VIEW, GtkBox)

GtkWidget *hb_hosts_view_new(HbStore *st);

/* dev/截图用：初始搜索词与 list/grid 视图 */
void hb_hosts_view_set_search(HbHostsView *hv, const char *text);
void hb_hosts_view_set_list_mode(HbHostsView *hv, gboolean list);
void hb_hosts_view_show_drawer(HbHostsView *hv, const char *host_id);
void hb_hosts_view_close_drawer(HbHostsView *hv);

G_END_DECLS

#endif
