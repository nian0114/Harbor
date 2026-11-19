#ifndef HB_TERMINAL_H
#define HB_TERMINAL_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include "hb-term-theme.h"

G_BEGIN_DECLS

#define HB_TYPE_TERMINAL (hb_terminal_get_type())
G_DECLARE_FINAL_TYPE(HbTerminal, hb_terminal, HB, TERMINAL, GtkWidget)

GtkWidget *hb_terminal_new(void);

/* session 归属调用方；换 session 前先 hb_terminal_set_session(t, NULL) 无妨 */
void        hb_terminal_set_session(HbTerminal *t, hb_session *s);
hb_session *hb_terminal_get_session(HbTerminal *t);

void hb_terminal_set_theme(HbTerminal *t, const HbTermTheme *theme);
void hb_terminal_set_font(HbTerminal *t, const char *family,
                          double px, double line_height);

void hb_terminal_set_osc52(HbTerminal *t, gboolean allowed);
void hb_terminal_set_cursor_blink(HbTerminal *t, gboolean enabled);

void hb_terminal_copy_clipboard(HbTerminal *t);
void hb_terminal_paste_clipboard(HbTerminal *t);
void hb_terminal_select_all(HbTerminal *t);
void hb_terminal_clear_scrollback(HbTerminal *t);
void hb_terminal_scroll_to_bottom(HbTerminal *t);
/* 供页面 overlay 的 GtkScrollbar 绑定（value = scroll_off） */
GtkAdjustment *hb_terminal_scroll_adjustment(HbTerminal *t);

/* signals:
 *   "title-changed" (gchar*)
 *   "exited"        (gint code)
 *   "bell"
 *   "osc133"        (gchar mark, gint code)
 *   "notify-event"        (gchar* title, gchar* body)
 */

G_END_DECLS

#endif
