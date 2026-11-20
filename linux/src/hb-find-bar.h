#ifndef HB_FIND_BAR_H
#define HB_FIND_BAR_H

#include <gtk/gtk.h>
#include "hb-terminal.h"

G_BEGIN_DECLS

#define HB_TYPE_FIND_BAR (hb_find_bar_get_type())
G_DECLARE_FINAL_TYPE(HbFindBar, hb_find_bar, HB, FIND_BAR, GtkBox)

/* scrollback 查找条：绑定 HbTerminal；默认隐藏，hb_find_bar_open 显示。
 * workspace 分屏后直接换绑聚焦 pane 即可复用。 */
GtkWidget *hb_find_bar_new(void);

void hb_find_bar_bind(HbFindBar *b, HbTerminal *t);

void     hb_find_bar_open(HbFindBar *b);   /* 显示 + 聚焦输入框（保留已输入） */
void     hb_find_bar_close(HbFindBar *b);  /* 隐藏 + 清命中高亮，焦点回终端 */
gboolean hb_find_bar_is_open(HbFindBar *b);

G_END_DECLS

#endif
