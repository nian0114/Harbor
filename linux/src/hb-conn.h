#ifndef HB_CONN_H
#define HB_CONN_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include "hb-store.h"

G_BEGIN_DECLS

#define HB_TYPE_CONN_CTL (hb_conn_ctl_get_type())
G_DECLARE_FINAL_TYPE(HbConnCtl, hb_conn_ctl, HB, CONN_CTL, GObject)

typedef enum { HB_CONN_SHELL, HB_CONN_SFTP } HbConnMode;

/* SSH/SFTP 连接流程控制器：拥有与 hb-ssh-page 完全相同的安全流程
 * （TOFU/密钥变更/口令对话框、使用前确认、known_hosts 写入、凭据
 * 记忆、vault 日志、跳板归属），供终端页与 SFTP 面板复用。
 *
 * dialog_parent：对话框的父 widget（一般传所在页面）；弱引用跟随，
 * 页面销毁后回退无父。调用方拥有返回对象。 */
HbConnCtl  *hb_conn_ctl_new(HbStore *st, GtkWidget *dialog_parent,
                            HbConnMode mode);

/* 开始连接：key-confirm 对话框（如有需要确认的 key）→ 从 vault 组装
 * opts（target + jumps + known lines + secrets）→
 * hb_session_new_ssh / hb_session_new_sftp → vault 日志行。
 * reuse_vt 仅 SHELL 有意义（重连保 scrollback，接管所有权）。 */
void        hb_conn_ctl_start(HbConnCtl *c, const char *host_id,
                              hb_vt *reuse_vt, int rows, int cols);

/* 关闭挂起对话框、对阻塞中的 PROMPT/HOSTKEY 应答 NULL/FALSE、
 * 关闭并释放 session。 */
void        hb_conn_ctl_cancel(HbConnCtl *c);

/* 当前 session（借用；创建前为 NULL） */
hb_session *hb_conn_ctl_session(HbConnCtl *c);

/* 事件入口：owner 把每个 session 事件先喂给它；返回 TRUE = 已消费。
 * 未消费的（WATCH/COMMAND/SFTP…）由 owner 自行处理。 */
gboolean    hb_conn_ctl_handle_event(HbConnCtl *c, const hb_sess_event *ev);

/* signals:
 *   "stage"        (gint stage_idx, const char *text, gchar mark)
 *                  — mark 'e' = 错误日志行（不参与节点推进，idx=-1）
 *   "session"      (gpointer hb_session)  — session 已创建，可绑定/轮询
 *   "connected"    ()
 *   "failed"       (gint hb_ssh_err, const char *title, const char *details)
 *                  — 也用于 "已拒绝使用密钥"、跳板缺失、会话创建失败
 *                  （这几种 code=0）
 *   "disconnected" ()  — 已连接后 ERROR DISCONNECTED（可重连类）
 *   "exited"       (gint code)
 *   "osinfo"       (const char *id, const char *pretty)
 */

G_END_DECLS

#endif
