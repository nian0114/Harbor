#ifndef HB_DIALOGS_H
#define HB_DIALOGS_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

/* 原型风格模态对话框（白色 14px 圆角、header 标题+✕、body、footer）。
 * 基于 libadwaita 1.5 的 AdwDialog；回调均为异步（用户点击后调用一次，
 * 由对话框托管释放，可在 cb 里安全销毁任何东西）。
 *
 * 全部按 (accepted, secret) 风格回调：取消/关闭 → accepted=FALSE。
 */

typedef void (*hb_dialog_bool_cb)(gboolean accepted, gpointer ud);
typedef void (*hb_dialog_secret_cb)(const char *secret /*NULL=取消*/,
                                    gboolean remember, gpointer ud);

/* TOFU 首次连接：展示指纹+randomart，取消 / 信任并继续。
 * 返回值是 present 出去的对话框 —— 页面可在换会话/销毁前
 * adw_dialog_force_close 它，避免陈旧 cb 应答错会话。 */
AdwDialog *hb_dialog_tofu(GtkWindow *parent,
                         const char *host_port,
                         const char *keytype,
                         const char *fingerprint,
                         const char *randomart,
                         hb_dialog_bool_cb cb, gpointer ud);

/* 主机密钥已变更：红色危险对话框；继续前必须勾选"我了解风险"。 */
AdwDialog *hb_dialog_hostkey_changed(GtkWindow *parent,
                                     const char *host_port,
                                     const char *keytype,
                                     const char *old_fp,
                                     const char *new_fp,
                                     hb_dialog_bool_cb cb, gpointer ud);

/* 密码 / 私钥 passphrase 输入；show_remember=TRUE 时带"记住"勾选框。
 * cb 收到 secret（取消为 NULL）；密码在传给 cb 后立刻从 entry 抹掉。 */
AdwDialog *hb_dialog_password(GtkWindow *parent,
                              const char *prompt,
                              gboolean is_passphrase,
                              gboolean show_remember,
                              hb_dialog_secret_cb cb, gpointer ud);

/* toast（委托给所在窗口的 AdwToastOverlay） */
void hb_toast(GtkWidget *any_widget, const char *text);
typedef void (*hb_toast_undo_cb)(gpointer ud);
void hb_toast_undo(GtkWidget *any_widget, const char *text,
                   hb_toast_undo_cb undo_cb, gpointer ud);

G_END_DECLS

#endif
