#ifndef HB_SESSION_PAGE_H
#define HB_SESSION_PAGE_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include "hb-terminal.h"

G_BEGIN_DECLS

/* 可被 Workspace 分屏托管的会话页接口（HbTermPage / HbSshPage 实现）。
 * "meta-changed" 信号：title/icon/status/dead 任一变化时由实现方 emit，
 * workspace 用来刷新 pane-head 与 footer。 */
#define HB_TYPE_SESSION_PAGE (hb_session_page_get_type())
G_DECLARE_INTERFACE(HbSessionPage, hb_session_page, HB, SESSION_PAGE,
                    GtkWidget)

struct _HbSessionPageInterface {
    GTypeInterface parent;

    HbTerminal *(*terminal)(HbSessionPage *p);
    const char *(*title)(HbSessionPage *p);
    const char *(*icon)(HbSessionPage *p);
    void        (*set_compact)(HbSessionPage *p, gboolean compact);
    GtkWidget  *(*status_widget)(HbSessionPage *p);
    void        (*open_find)(HbSessionPage *p);
    void        (*open_watch)(HbSessionPage *p);
    gboolean    (*is_dead)(HbSessionPage *p);
    void        (*reconnect)(HbSessionPage *p);
    GtkWidget  *(*clone)(HbSessionPage *p);
    const char *(*host_id)(HbSessionPage *p);
    void        (*attach_tab)(HbSessionPage *p, GtkWidget *win,
                              const char *tab_id);
};

HbTerminal *hb_session_page_terminal(HbSessionPage *p);
const char *hb_session_page_title(HbSessionPage *p);
const char *hb_session_page_icon(HbSessionPage *p);
/* compact=TRUE：藏起页内工具栏 + 状态条（pane-head / workspace footer 接管） */
void        hb_session_page_set_compact(HbSessionPage *p, gboolean compact);
/* 页的状态条 widget（页拥有所有权；compact 时不在页内，由 workspace
 * 托管进 footer —— 调用方 unparent/reparent，页不再假设其位置） */
GtkWidget  *hb_session_page_status_widget(HbSessionPage *p);
void        hb_session_page_open_find(HbSessionPage *p);
void        hb_session_page_open_watch(HbSessionPage *p);
gboolean    hb_session_page_is_dead(HbSessionPage *p);
void        hb_session_page_reconnect(HbSessionPage *p);
/* 新页：同 host（ssh）/ 新本地 shell；调用方负责 attach_tab */
GtkWidget  *hb_session_page_clone(HbSessionPage *p);
const char *hb_session_page_host_id(HbSessionPage *p);   /* NULL for local */
void        hb_session_page_attach_tab(HbSessionPage *p, GtkWidget *win,
                                       const char *tab_id);

/* 便捷：实现方在 title/icon/status/dead 变化时调用 */
void        hb_session_page_meta_changed(HbSessionPage *p);

G_END_DECLS

#endif
