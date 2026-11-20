#ifndef HB_SNIPPET_SEND_H
#define HB_SNIPPET_SEND_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include "hb-store.h"

G_BEGIN_DECLS

/* scope 匹配：NULL/""/"global"/"全局" → 全局；"host:<id>"；
 * "group:<name>"（需 group 非空且相等）。 */
gboolean hb_snippet_scope_match(const char *scope, const char *host_id,
                                const char *group);

/* 发送 snippet 到当前目标：活动 session page；活动页是 Broadcast 的
 * workspace → 确认后发到全部分屏。含 {{var}} 变量对话框（记住上次值、
 * 预填默认值）、run 字段追加回车、无终端时 toast「没有活动终端」。
 * Snippets 页 / palette / workspace 助手栏共用此入口。 */
void hb_snippet_send(GtkWidget *win, HbStore *store, const char *snippet_id);

/* 发送到指定 session（startup snippet）：变量对话框照常弹。 */
void hb_snippet_send_to_session(GtkWidget *parent, hb_session *sess,
                                HbStore *store, const char *snippet_id);

G_END_DECLS

#endif
