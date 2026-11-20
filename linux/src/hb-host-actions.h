/* hb-host-actions.h — 主机 CRUD 共享动作（抽屉/右键菜单/行按钮/SSH 页复用） */
#pragma once

#include <gtk/gtk.h>
#include "hb-store.h"

struct HbWindow;
#include "hb-window.h"
/* 按 proto 连接已加载对象（application 的 quick-connect 路径复用） */
void hb_host_connect_obj(HbWindow *win, HbStore *st, hb_obj *o);

/* any_widget：窗口内任意 widget（经 gtk_widget_get_root 找 HbWindow） */
void hb_host_action_connect(GtkWidget *any_widget, HbStore *st,
                            const char *id);
void hb_host_action_edit(GtkWidget *any_widget, HbStore *st,
                         const char *id);
void hb_host_action_duplicate(GtkWidget *any_widget, HbStore *st,
                              const char *id);
void hb_host_action_delete(GtkWidget *any_widget, HbStore *st,
                           const char *id);
