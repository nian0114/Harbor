#ifndef HB_WATCH_UI_H
#define HB_WATCH_UI_H

#include <gtk/gtk.h>
#include <harbor/session.h>
#include <harbor/watch.h>

#include "hb-store.h"

G_BEGIN_DECLS

/* ---- 规则文本 <-> hb_watch_rule ----
 * 一行一条："name: <regex>" 为成功规则，"!name: <regex>" 为失败规则；
 * 空行与 # 注释跳过。解析失败返回 NULL 且 err_line 填 1 基行号。
 * 产物的 name/ok_re/err_re 均为深拷贝，hb_watch_rules_free 释放。 */
hb_watch_rule *hb_watch_rules_parse_text(const char *text, size_t *out_n,
                                         int *err_line);
void           hb_watch_rules_free(hb_watch_rule *r, size_t n);

/* settings.watch_rules + host.watch_rules 合并解析（坏行跳过）。
 * host 可为 NULL（Local 会话只有全局规则）。 */
hb_watch_rule *hb_watch_rules_merged(HbStore *st, const hb_obj *host,
                                     size_t *out_n);

/* settings 全局项 + 会话级信号掩码 → 引擎 cfg（rules 指针指向
 * *rules_out，调用方 hb_watch_rules_free；hb_watch_new 深拷贝后可立即释放）。 */
void hb_watch_cfg_build(HbStore *st, const hb_obj *host,
                        unsigned signals, hb_watch_cfg *out,
                        hb_watch_rule **rules_out, size_t *nrules_out);

/* 应用到会话（on=FALSE → hb_session_set_watch(NULL)）。
 * host_id 用于按 host 现取 watch_rules（可为 NULL）。 */
void hb_watch_apply(hb_session *sess, HbStore *st, const char *host_id,
                    gboolean on, unsigned signals);

/* hit 路由：通知中心 + tab 角标 + 系统通知。
 * 受全局设置控制：watch_focus_dnd / watch_dnd / notify_body /
 * watch_sound。page 仅用来定位窗口。 */
void hb_watch_route(GtkWidget *page, const char *tab_id,
                    const char *host_label, const hb_watch_hit *hit);

/* 状态栏 "监控: EOF + OSC133 + AI 预设" 文案（signals 掩码 → 短名列表） */
char *hb_watch_signal_summary(unsigned signals);   /* g_free */

/* ---- 监控设置对话框 ----
 * page 弱引用跟踪；on/signals 的读写与"应用到会话"通过回调委托给页层。 */
typedef void (*hb_watch_apply_cb)(gpointer ud, gboolean on, unsigned signals);
typedef hb_session *(*hb_watch_sess_cb)(gpointer ud);
typedef struct {
    GtkWidget        *page;        /* 对话框 parent + 弱引用 */
    HbStore          *store;
    const char       *host_id;     /* NULL = Local（借用，内部拷贝） */
    gboolean          is_ssh;
    gboolean          on;
    unsigned          signals;
    hb_watch_apply_cb apply;       /* 页层：更新状态 + set_watch + 刷新 UI */
    gpointer          apply_ud;
    hb_watch_sess_cb  session;     /* SSH 安装器写远端用；可 NULL → 禁用 */
    gpointer          session_ud;
} HbWatchUiOps;

void hb_watch_ui_open(const HbWatchUiOps *ops);

G_END_DECLS

#endif
