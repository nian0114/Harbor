#ifndef HB_TERM_PAGE_H
#define HB_TERM_PAGE_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_TERM_PAGE (hb_term_page_get_type())
G_DECLARE_FINAL_TYPE(HbTermPage, hb_term_page, HB, TERM_PAGE, GtkBox)

GtkWidget *hb_term_page_new_local(HbStore *st);

/* 供 --open-local=CMD：像用户输入一样把文本写进会话 */
void hb_term_page_feed_input(HbTermPage *p, const char *text);

/* 页面当前主题 id 是否已应用（dev 用） */
hb_session *hb_term_page_session(HbTermPage *p);

G_END_DECLS

#endif
