/* hb-host-drawer.h — 主机详情抽屉（右侧 400px，scrim+reveal，live 刷新） */
#pragma once

#include <gtk/gtk.h>
#include "hb-store.h"

typedef struct _HbHostDrawer HbHostDrawer;

/* 返回的 widget 作为 GtkOverlay 的 overlay child 加入（覆盖整个 view） */
HbHostDrawer *hb_host_drawer_new(HbStore *st);
GtkWidget    *hb_host_drawer_widget(HbHostDrawer *d);
void          hb_host_drawer_show(HbHostDrawer *d, const char *host_id);
void          hb_host_drawer_close(HbHostDrawer *d);
gboolean      hb_host_drawer_is_open(HbHostDrawer *d);
