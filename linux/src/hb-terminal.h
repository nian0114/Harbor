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

/* -------- scrollback 查找（FR-20 find bar） -------- */

/* needle NULL/"" 清除。返回命中总数。新 needle → cur=0；
 * vt 变化时按绝对行锚定当前项（淘汰出 scrollback 的命中自然消失）。 */
guint hb_terminal_find_set(HbTerminal *t, const char *needle,
                           gboolean case_sensitive);
/* +1 / -1，环绕；返回当前 1-based 下标（无命中 → 0），并滚到该项居中 */
guint hb_terminal_find_step(HbTerminal *t, int dir);
void  hb_terminal_find_counts(HbTerminal *t, guint *cur, guint *total);

/* 终端自身不消费的 session 事件（STAGE/CONNECTED/ERROR/HOSTKEY/PROMPT/
 * OSINFO 等）转发给页层；ev 为借用指针，回调返回后即清理。 */
typedef void (*hb_terminal_event_cb)(HbTerminal *t,
                                     const hb_sess_event *ev, gpointer ud);
void hb_terminal_set_event_cb(HbTerminal *t, hb_terminal_event_cb cb,
                              gpointer ud);

/* signals:
 *   "title-changed" (gchar*)
 *   "exited"        (gint code)
 *   "bell"
 *   "osc133"        (gchar mark, gint code)
 *   "notify-event"        (gchar* title, gchar* body)
 *   "dead-input"          (gchar* utf8)   会话已死后用户输入的文本
 *   "find-changed"        ()              命中数/当前项变化
 */

G_END_DECLS

#endif
