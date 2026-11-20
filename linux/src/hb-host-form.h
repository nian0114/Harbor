/* hb-host-form.h — 主机编辑/新建对话框（SSH/RDP/VNC/Local） */
#pragma once

#include <gtk/gtk.h>
#include "hb-store.h"

/* host_id == NULL → 新建。对话框自带滚动 body + 钉底 footer。 */
void hb_host_form_open(GtkWidget *parent, HbStore *st, const char *host_id);
