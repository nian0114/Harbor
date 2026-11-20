#include "hb-ssh-page.h"
#include "hb-conn.h"
#include "hb-terminal.h"
#include "hb-find-bar.h"
#include "hb-term-theme.h"
#include "hb-icons.h"
#include "hb-notify.h"
#include "hb-window.h"
#include "hb-watch-ui.h"
#include "hb-session-page.h"
#include "hb-snippet-send.h"

#include <harbor/ssh.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ==================================================================
 * SSH 连接页：连接进度（节点+日志）→ 原地切换终端 → 断线自动重连。
 *
 * 事件流（来自 HbTerminal 的 ev_cb，见 hb_terminal_set_event_cb）：
 *   STAGE      连接阶段日志行 → 进度节点 + 日志列表
 *   HOSTKEY    TOFU / 密钥变更对话框 → hostkey_reply
 *   PROMPT     密码/私钥口令对话框 → prompt_reply
 *   CONNECTED  切到终端视图
 *   OSINFO     探测到的远端 OS → 更新 host.os + 头像
 *   ERROR      连接期 → 错误卡；已连接 → 断线条 + 指数退避重连
 *   EXIT       干净退出 → dead 状态 + 重连按钮
 * ================================================================== */

struct _HbSshPage {
    GtkBox parent;

    HbStore    *store;

    /* 所在 tab（app 在 add_tab 后 attach，用于改标题/图标/dot） */
    HbWindow   *win;
    char       *tab_id;

    /* 主机信息（深拷贝的显示字段；完整对象每次连接重读） */
    char       *host_id;
    char       *hname;
    char       *haddr;
    char       *huser;
    int         hport;
    char       *os_key;         /* tab 头像键 */
    char       *disp_title;     /* pane-head："<hname> · ssh, <huser>" */
    gboolean    compact;        /* workspace 托管：藏工具栏，状态条外借 */

    /* 会话：连接流程（认证/主机密钥/日志/凭据）由 HbConnCtl 全权处理 */
    HbConnCtl  *ctl;
    gboolean    connecting;     /* 当前处于连接流程（含重连尝试） */
    gboolean    dead;           /* 干净 EXIT 后的终态 */

    /* ---- connect 视图 ---- */
    GtkWidget  *stack;
    GtkWidget  *node[3];        /* link / shield / terminal */
    GtkWidget  *bar[2];
    GtkWidget  *log_box;
    GtkWidget  *log_scroll;
    GtkWidget  *hide_btn;
    gboolean    hide_log;
    int         cur_stage;
    GtkWidget  *err_card;       /* 连接期错误卡片（NULL=无） */
    GtkWidget  *conn_foot;      /* connect 视图底部按钮行（错误卡显示时隐藏） */

    /* ---- 终端视图 ---- */
    GtkWidget  *term_view;
    GtkWidget  *term;
    GtkWidget  *overlay;
    GtkWidget  *toolbar;
    GtkWidget  *status_bar;
    GtkWidget  *tt_label;
    GtkWidget  *tt_icon;
    GtkWidget  *reconn_btn;
    GtkWidget  *watch_btn;
    GtkWidget  *find_bar;
    GtkWidget  *st_conn;
    GtkWidget  *st_sb;
    GtkWidget  *st_watch;
    GtkWidget  *dead_lbl;
    GtkWidget  *strip;          /* 断线重连条 */
    GtkWidget  *strip_label;
    GtkWidget  *buf_hint;       /* "输入将在重连后发送" */

    /* FR-13 完成监控（会话级状态，重连后重挂到新 session） */
    gboolean    watch_on;
    unsigned    watch_signals;

    /* FR-08 shell history 采集（settings.history_enabled） */
    gboolean    hist_on;

    /* 自动重连 */
    guint       backoff_src;
    int         backoff_k;      /* 第 k 次重连（0=未在退避） */
    int         countdown;      /* 距下次重连的秒数 */
    GString    *inbuf;          /* 断线期本地缓冲（≤4KiB） */

    /* 重连：从死会话偷出的 vt，供下一次 start_connect 复用 */
    hb_vt      *reuse_vt;
    gboolean    was_reconnect;
    gboolean    reconnected_feed;   /* 连上后注入"重新连接成功"提示 */

    GtkWidget  *connect_box;
    gulong      changed_handler;

    char       *theme_id;
    int         sb_lines;
};

static void hb_ssh_page_iface_init(HbSessionPageInterface *iface);
G_DEFINE_TYPE_WITH_CODE(HbSshPage, hb_ssh_page, GTK_TYPE_BOX,
    G_IMPLEMENT_INTERFACE(HB_TYPE_SESSION_PAGE, hb_ssh_page_iface_init))

enum { SIG_EDIT_HOST, N_SIGS };
static guint sigs[N_SIGS];

static void on_split_pick(GtkButton *b, gpointer ud);

extern char *hb_opt_theme;      /* main.c dev 选项 */

static void start_connect(HbSshPage *p, gboolean is_reconnect);
static void free_session(HbSshPage *p);

/* ------------------------- 工具 ------------------------- */

static HbWindow *page_win(HbSshPage *p)
{
    if (p->win) return p->win;
    GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(p));
    return HB_IS_WINDOW(r) ? HB_WINDOW(r) : NULL;
}

static void tab_title(HbSshPage *p, const char *t)
{
    HbWindow *w = page_win(p);
    /* workspace pane：tab 标题是 "Workspace"，不改 */
    if (w && p->tab_id && !p->compact)
        hb_window_set_tab_title(w, p->tab_id, t);
}
static void tab_icon(HbSshPage *p, const char *ic)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id && !p->compact)
        hb_window_set_tab_icon(w, p->tab_id, ic);
    hb_session_page_meta_changed(HB_SESSION_PAGE(p));
}
static void tab_dot(HbSshPage *p, const char *dot)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id) hb_window_set_tab_dot(w, p->tab_id, dot);
}
static void tab_close(HbSshPage *p)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id) hb_window_close_tab(w, p->tab_id);
}

static const char *os_avatar(const char *os)
{
    /* host.os → 头像/tab 图标键 */
    if (!os || !*os || !strcmp(os, "auto")) return "asterisk";
    return hb_os_avatar_key(os);
}

/* ------------------------- 终端状态栏 ------------------------- */

static void set_status(HbSshPage *p, const char *txt, const char *cls)
{
    gtk_label_set_label(GTK_LABEL(p->st_conn), txt);
    gtk_widget_remove_css_class(p->st_conn, "hb-term-ok");
    gtk_widget_remove_css_class(p->st_conn, "hb-term-ended");
    gtk_widget_remove_css_class(p->st_conn, "hb-term-warn");
    if (cls) gtk_widget_add_css_class(p->st_conn, cls);
    hb_session_page_meta_changed(HB_SESSION_PAGE(p));
}

/* ------------------------- 连接进度节点 ------------------------- */

/* stage 0-1 → node0, 2 → node1, 3-4 → node2 */
static int node_of(int stage)
{
    if (stage <= 1) return 0;
    if (stage == 2) return 1;
    return 2;
}

static void set_node(GtkWidget *n, const char *state)
{
    gtk_widget_remove_css_class(n, "pending");
    gtk_widget_remove_css_class(n, "active");
    gtk_widget_remove_css_class(n, "spin");
    gtk_widget_remove_css_class(n, "done");
    if (state) gtk_widget_add_css_class(n, state);
}

static void update_nodes(HbSshPage *p)
{
    int cur = node_of(p->cur_stage), i;
    for (i = 0; i < 3; i++) {
        if (i < cur)       set_node(p->node[i], "done");
        else if (i == cur) set_node(p->node[i], "active spin");
        else               set_node(p->node[i], "pending");
    }
    for (i = 0; i < 2; i++) {
        gtk_widget_remove_css_class(p->bar[i], "pending");
        gtk_widget_remove_css_class(p->bar[i], "done");
        gtk_widget_add_css_class(p->bar[i],
                               i < cur ? "done" : "pending");
    }
}

static void add_log_line(HbSshPage *p, const char *text, gboolean err)
{
    GtkWidget *row, *l;
    GtkWidget *v;
    if (!p->log_box) return;
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "connect-log-line");
    gtk_box_append(GTK_BOX(row),
        hb_icon_new(err ? "x" : "user", 13));
    l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    if (err) gtk_widget_add_css_class(l, "hb-red");
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(p->log_box), row);
    /* 滚到底 */
    v = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(p->log_scroll));
    if (v) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(p->log_scroll));
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
    }
}

/* ------------------------- OSINFO ------------------------- */

static void on_osinfo(HbConnCtl *c, const char *id, const char *pretty,
                      gpointer ud)
{
    HbSshPage *p = ud;
    const char *norm;
    hb_obj *h = NULL;
    (void)c; (void)pretty;
    if (!id || !*id) return;
    if (!strcmp(id, "ubuntu"))       norm = "ubuntu";
    else if (!strcmp(id, "debian"))  norm = "debian";
    else if (!strcmp(id, "fedora"))  norm = "fedora";
    else                             norm = "generic";
    /* 仅在 auto/未设置时保存 */
    if (p->host_id &&
        hb_vault_get(hb_store_vault(p->store), p->host_id, &h) == HB_OK && h) {
        const char *os = hb_obj_get_str(h, "os");
        if (!os || !*os || !strcmp(os, "auto")) {
            hb_obj_set_str(h, "os", norm);
            hb_vault_put(hb_store_vault(p->store), h);
        }
        hb_obj_free(h);
    }
    g_free(p->os_key);
    p->os_key = g_strdup(norm);
    tab_icon(p, os_avatar(norm));
    if (p->tt_icon)
        hb_icon_set_name(HB_ICON(p->tt_icon), os_avatar(norm));
}

/* ------------------------- 断线重连 ------------------------- */

static const int backoff_tab[] = { 1, 2, 4, 8, 16, 30 };
static void reconnect_now(HbSshPage *p);

static void stop_backoff(HbSshPage *p)
{
    if (p->backoff_src) {
        g_source_remove(p->backoff_src);
        p->backoff_src = 0;
    }
    gtk_widget_set_visible(p->strip, FALSE);
    gtk_widget_set_visible(p->buf_hint, FALSE);
}

static void strip_set(HbSshPage *p, const char *txt)
{
    gtk_label_set_label(GTK_LABEL(p->strip_label), txt);
}

static gboolean backoff_tick(gpointer ud)
{
    HbSshPage *p = ud;
    char *t;
    if (--p->countdown > 0) {
        t = g_strdup_printf("连接已断开 · %d 秒后重连（第 %d 次）",
                            p->countdown, p->backoff_k);
        strip_set(p, t);
        g_free(t);
        return G_SOURCE_CONTINUE;
    }
    p->backoff_src = 0;
    reconnect_now(p);
    return G_SOURCE_REMOVE;
}

static void start_backoff(HbSshPage *p)
{
    int idx = p->backoff_k;     /* 第 k 次（即将进行的是 k+1） */
    int d = idx < 6 ? backoff_tab[idx] : 30;
    char *t;
    p->backoff_k++;
    p->countdown = d;
    t = g_strdup_printf("连接已断开 · %d 秒后重连（第 %d 次）", d, p->backoff_k);
    strip_set(p, t);
    g_free(t);
    gtk_widget_set_visible(p->strip, TRUE);
    set_status(p, "● 连接已断开", "hb-term-warn");
    tab_dot(p, "err");
    if (p->backoff_src) g_source_remove(p->backoff_src);
    p->backoff_src = g_timeout_add_seconds(1, backoff_tick, p);
}

static void on_reconn_now(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    if (p->backoff_src) { g_source_remove(p->backoff_src); p->backoff_src = 0; }
    reconnect_now(p);
}
static void on_reconn_cancel(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    stop_backoff(p);
    p->backoff_k = 0;
    p->dead = TRUE;
    set_status(p, "● 会话已结束", "hb-term-ended");
    gtk_widget_set_visible(p->dead_lbl, TRUE);
    gtk_widget_set_visible(p->reconn_btn, TRUE);
    g_string_set_size(p->inbuf, 0);
}

/* 会话已死（断线/退出）时的用户输入 → 本地缓冲（≤4KiB），
 * 重连成功后 flush。dead-input 信号只在会话死时才发出，所以无条件缓冲。 */
static void on_dead_input(HbTerminal *t, const char *utf8, gpointer ud)
{
    HbSshPage *p = ud;
    (void)t;
    if (p->inbuf->len + strlen(utf8) > 4096) return;   /* 满则丢 */
    g_string_append(p->inbuf, utf8);
    gtk_widget_set_visible(p->buf_hint, TRUE);
}

/* ------------------------- 会话生命周期 ------------------------- */

/* 偷出旧 vt、喂入重连提示行、起新会话（reuse_vt），重绑终端。 */
static void reconnect_now(HbSshPage *p)
{
    static const char *mark = "\r\n\e[2m[重新连接…]\e[0m\r\n";
    hb_session *sess = hb_conn_ctl_session(p->ctl);

    if (p->backoff_src) {
        g_source_remove(p->backoff_src);
        p->backoff_src = 0;
    }
    if (sess && !hb_session_alive(sess)) {
        p->reuse_vt = hb_session_steal_vt(sess);
        if (p->reuse_vt)
            hb_vt_feed(p->reuse_vt, mark, strlen(mark));
    }
    /* ctl 在 start 里收掉旧会话；先解绑终端 */
    if (sess)
        hb_terminal_set_session(HB_TERMINAL(p->term), NULL);
    strip_set(p, "正在重连…");
    gtk_widget_set_visible(p->strip, TRUE);
    start_connect(p, TRUE);
}

/* ctl "exited"：干净 EXIT / 断线 EXIT 的收尾（终端视图侧） */
static void on_ctl_exited(HbConnCtl *c, int code, gpointer ud)
{
    HbSshPage *p = ud;
    (void)c; (void)code;
    p->dead = TRUE;
    set_status(p, "● 会话已结束", "hb-term-ended");
    tab_dot(p, NULL);
    gtk_widget_set_visible(p->dead_lbl, TRUE);
    gtk_widget_set_visible(p->reconn_btn, TRUE);
}

/* 干净 EXIT / 断线 EXIT 都会走这里（终端把 EXIT 消费成 "exited" 信号；
 * 合成事件转给 ctl，由它做日志/凭据收尾再经 "exited" 信号回来） */
static void on_exited(HbTerminal *t, int code, gpointer ud)
{
    HbSshPage *p = ud;
    hb_sess_event ev = {0};
    (void)t;
    /* EXIT 在 ERROR(DISCONNECTED) 之后到达；若断线退避已启动则忽略 */
    if (p->backoff_k > 0 || p->backoff_src)
        return;
    ev.type = HB_SESS_EV_EXIT;
    ev.code = code;
    hb_conn_ctl_handle_event(p->ctl, &ev);
}

static void on_title(HbTerminal *t, const char *title, gpointer ud)
{
    HbSshPage *p = ud;
    char *f;
    (void)t;
    if (title && *title)
        f = g_strdup_printf("ssh %s@%s · %s", p->huser, p->hname, title);
    else
        f = g_strdup_printf("ssh %s@%s", p->huser, p->hname);
    gtk_label_set_label(GTK_LABEL(p->tt_label), f);
    g_free(p->disp_title);
    p->disp_title = g_strdup(f);
    g_free(f);
    hb_session_page_meta_changed(HB_SESSION_PAGE(p));
}

static void on_notify(HbTerminal *t, const char *title, const char *body,
                      gpointer ud)
{
    HbSshPage *p = ud;
    (void)t;
    /* 监控开着时 OSC 9/777 由 watch 的 TERMNOTIFY hit 路由，
     * 不在这里重复投递 */
    if (p->watch_on)
        return;
    if (title) {
        /* notify_body=host：正文只带主机名，不回显 OSC 内容 */
        hb_obj *o = NULL;
        gboolean host_only = FALSE;
        if (hb_vault_settings(hb_store_vault(p->store), &o) == HB_OK && o) {
            host_only = !g_strcmp0(hb_obj_get_str(o, "notify_body"),
                                   "host");
            hb_obj_free(o);
        }
        if (host_only) {
            char *tt = g_strdup_printf("终端通知 · %s", p->hname);
            char *bb = g_strdup_printf("%s 上的任务有新通知", p->hname);
            hb_notify_post("info", tt, bb, NULL);
            g_free(tt); g_free(bb);
        } else {
            hb_notify_post("info", title, body ? body : "", NULL);
        }
    }
}

/* ------------------------------ 监控 ------------------------------ */

static void watch_refresh_ui(HbSshPage *p)
{
    char *t;
    GtkWidget *bx;

    if (!p->watch_btn)
        return;
    bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(bx), hb_icon_new("eye", 13));
    gtk_box_append(GTK_BOX(bx),
        gtk_label_new(p->watch_on ? "监控 开" : "监控 关"));
    gtk_button_set_child(GTK_BUTTON(p->watch_btn), bx);
    if (p->watch_on) {
        gtk_widget_add_css_class(p->watch_btn, "on");
        t = hb_watch_signal_summary(p->watch_signals);
    } else {
        gtk_widget_remove_css_class(p->watch_btn, "on");
        t = NULL;
    }
    if (p->st_watch)
        gtk_label_set_label(GTK_LABEL(p->st_watch),
                            t ? t : "监控: 关");
    g_free(t);
}

static void watch_apply_session(HbSshPage *p)
{
    hb_session *sess = hb_conn_ctl_session(p->ctl);
    if (sess)
        hb_watch_apply(sess, p->store, p->host_id,
                       p->watch_on, p->watch_signals);
}

static void watch_set(gpointer ud, gboolean on, unsigned signals)
{
    HbSshPage *p = ud;
    p->watch_on = on;
    p->watch_signals = signals;
    watch_refresh_ui(p);
    watch_apply_session(p);
}

static hb_session *watch_get_sess(gpointer ud)
{
    HbSshPage *p = ud;
    return hb_conn_ctl_session(p->ctl);
}

static void on_find_btn(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    hb_ssh_page_find_open(p);
}

static void on_watch_btn(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    HbWatchUiOps ops = {0};
    (void)b;
    ops.page = GTK_WIDGET(p);
    ops.store = p->store;
    ops.host_id = p->host_id;
    ops.is_ssh = TRUE;
    ops.on = p->watch_on;
    ops.signals = p->watch_signals;
    ops.apply = watch_set;
    ops.apply_ud = p;
    ops.session = watch_get_sess;
    ops.session_ud = p;
    hb_watch_ui_open(&ops);
}

/* ------------------------- 事件分发（ev_cb） ------------------------- */

static void on_session_ev(HbTerminal *t, const hb_sess_event *ev, gpointer ud);

/* CONNECTED：切终端 + startup snippet + 重连提示（日志/凭据/
 * last_connected 已由 ctl 处理完） */
static void on_ctl_connected(HbConnCtl *c, gpointer ud)
{
    HbSshPage *p = ud;
    hb_session *sess = hb_conn_ctl_session(c);
    hb_obj *h = NULL;
    char *t;

    p->connecting = FALSE;
    p->backoff_k = 0;
    stop_backoff(p);
    p->dead = FALSE;

    /* startup_snippet：共享发送流（变量 / run） */
    if (p->host_id &&
        hb_vault_get(hb_store_vault(p->store), p->host_id, &h) == HB_OK && h) {
        const char *snid = hb_obj_get_str(h, "startup_snippet");
        if (snid && *snid)
            hb_snippet_send_to_session(GTK_WIDGET(p), sess,
                                       p->store, snid);
        hb_obj_free(h);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "term");
    set_status(p, "● 已连接", "hb-term-ok");
    tab_dot(p, "ok");
    gtk_widget_set_visible(p->reconn_btn, FALSE);
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_widget_set_visible(p->buf_hint, FALSE);

    /* 工具栏标题 */
    t = g_strdup_printf("ssh %s@%s", p->huser, p->hname);
    gtk_label_set_label(GTK_LABEL(p->tt_label), t);
    g_free(t);
    tab_title(p, p->hname);
    tab_icon(p, os_avatar(p->os_key));

    /* 重连成功 → 注入提示行 + flush 缓冲输入 */
    if (p->reconnected_feed) {
        hb_vt *v = hb_session_vt(sess);
        static const char *l1 = "\e[2m[重新连接成功]\e[0m\r\n";
        static const char *l2 =
            "\e[33m提示: 远端会话已丢失，建议在 tmux 内工作以便 attach 恢复\e[0m\r\n";
        if (v) {
            hb_vt_lock(v);
            hb_vt_feed(v, l1, strlen(l1));
            hb_vt_feed(v, l2, strlen(l2));
            hb_vt_unlock(v);
        }
        p->reconnected_feed = FALSE;
    }
    if (p->inbuf->len) {
        hb_session_write(sess, p->inbuf->str, p->inbuf->len);
        g_string_set_size(p->inbuf, 0);
    }
    gtk_widget_grab_focus(p->term);
}

static void on_errcard_close(GtkButton *b, gpointer ud)
{ (void)b; tab_close((HbSshPage *)ud); }
static void on_errcard_retry(GtkButton *b, gpointer ud)
{ (void)b; start_connect((HbSshPage *)ud, FALSE); }
static void on_errcard_edit(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    g_signal_emit(p, sigs[SIG_EDIT_HOST], 0, p->host_id);
}

/* 连接期错误 → 错误卡片（插在日志区下方） */
static void show_error_card(HbSshPage *p, const char *msg, const char *detail,
                            int code)
{
    GtkWidget *card, *title, *exp, *foot, *dl;
    char *markup;

    if (p->err_card) {
        gtk_widget_unparent(p->err_card);
        p->err_card = NULL;
    }
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card, "connect-errcard");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 430, -1);

    markup = g_markup_printf_escaped("<b>%s</b>",
                                     msg ? msg : "连接失败");
    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(card), title);

    if (code == HB_SSH_ERR_HOSTKEY_CHANGED) {
        GtkWidget *w = gtk_label_new(
            "为安全起见已阻断连接 —— 主机密钥与本地记录不符。");
        gtk_widget_add_css_class(w, "hb-red");
        gtk_label_set_wrap(GTK_LABEL(w), TRUE);
        gtk_label_set_xalign(GTK_LABEL(w), 0);
        gtk_box_append(GTK_BOX(card), w);
    }
    if (detail && *detail) {
        exp = gtk_expander_new("技术详情");
        dl = gtk_label_new(detail);
        gtk_label_set_wrap(GTK_LABEL(dl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(dl), 0);
        gtk_expander_set_child(GTK_EXPANDER(exp), dl);
        gtk_widget_add_css_class(exp, "hb-dim");
        gtk_box_append(GTK_BOX(card), exp);
    }

    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    {
        GtkWidget *c = gtk_button_new_with_label("Close");
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect(c, "clicked", G_CALLBACK(on_errcard_close), p);
        gtk_box_append(GTK_BOX(foot), c);
    }
    if (p->host_id) {
        GtkWidget *e = gtk_button_new_with_label("编辑主机");
        gtk_widget_add_css_class(e, "hb-btn");
        g_signal_connect(e, "clicked", G_CALLBACK(on_errcard_edit), p);
        gtk_box_append(GTK_BOX(foot), e);
    }
    {
        GtkWidget *r = gtk_button_new_with_label("重试");
        gtk_widget_add_css_class(r, "hb-btn");
        gtk_widget_add_css_class(r, "primary");
        g_signal_connect(r, "clicked", G_CALLBACK(on_errcard_retry), p);
        gtk_box_append(GTK_BOX(foot), r);
    }
    gtk_box_append(GTK_BOX(card), foot);

    /* 插到日志区后（connect 视图内 footer 之前） */
    gtk_box_insert_child_after(GTK_BOX(p->connect_box),
                               card, p->log_scroll);
    p->err_card = card;
    gtk_widget_set_visible(p->hide_btn, FALSE);
    if (p->conn_foot)
        gtk_widget_set_visible(p->conn_foot, FALSE);
}

static void on_session_ev(HbTerminal *t, const hb_sess_event *ev, gpointer ud)
{
    HbSshPage *p = ud;
    (void)t;
    /* 连接流程事件全部先交给 ctl（STAGE/HOSTKEY/PROMPT/CONNECTED/
     * OSINFO/ERROR）；未消费的才是页面自己的。 */
    if (hb_conn_ctl_handle_event(p->ctl, ev))
        return;
    switch (ev->type) {
    case HB_SESS_EV_WATCH:
        if (ev->watch && p->tab_id)
            hb_watch_route(GTK_WIDGET(p), p->tab_id, p->hname,
                           ev->watch);
        break;

    case HB_SESS_EV_COMMAND:
        /* FR-08 shell history：命令事件入库（密码等已被会话过滤） */
        if (ev->text && *ev->text)
            hb_vault_history_add(hb_store_vault(p->store),
                                 p->host_id, ev->text);
        break;

    default:
        break;
    }
}

/* ------------------------- ctl 信号 → 视图 ------------------------- */

/* "stage"：日志行 + 进度节点（mark='e' → 红色错误行，不推进节点） */
static void on_ctl_stage(HbConnCtl *c, int idx, const char *text,
                         gchar mark, gpointer ud)
{
    HbSshPage *p = ud;
    (void)c;
    add_log_line(p, text ? text : "", mark == 'e');
    if (idx > p->cur_stage) {
        p->cur_stage = idx;
        update_nodes(p);
    }
}

/* "session"：会话已创建 —— 绑终端 + 挂监控/历史 */
static void on_ctl_session(HbConnCtl *c, gpointer sess_p, gpointer ud)
{
    HbSshPage *p = ud;
    hb_session *sess = sess_p;
    (void)c;
    hb_terminal_set_session(HB_TERMINAL(p->term), sess);
    hb_session_set_history(sess, p->hist_on);
    watch_apply_session(p);       /* 监控状态随（重）连重挂 */
    set_status(p, "● 连接中…", "hb-term-warn");
}

/* "failed"：连接期失败 / 需用户介入的错误 → 错误卡
 *（code=0 的三种：拒用密钥、跳板缺失、会话创建失败） */
static void on_ctl_failed(HbConnCtl *c, int code, const char *title,
                          const char *details, gpointer ud)
{
    HbSshPage *p = ud;
    (void)c;
    p->connecting = FALSE;
    stop_backoff(p);
    p->backoff_k = 0;
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");
    show_error_card(p, title, details, code);
}

/* "disconnected"：已连接后掉线（可重连类）→ 指数退避 */
static void on_ctl_disconn(HbConnCtl *c, gpointer ud)
{
    HbSshPage *p = ud;
    (void)c;
    start_backoff(p);
}

/* ------------------------- start_connect ------------------------- */

static void free_session(HbSshPage *p)
{
    /* 先解绑终端，再让 ctl 收尾：强关挂起的模态（其 cb 同步按
     * "拒绝/取消"路径应答）、reply 阻塞中的应答、杀会话。 */
    hb_terminal_set_session(HB_TERMINAL(p->term), NULL);
    hb_conn_ctl_cancel(p->ctl);
}

static void start_connect(HbSshPage *p, gboolean is_reconnect)
{
    hb_vt *vt;
    if (p->err_card) {        /* 上次尝试的错误卡（含重连路径） */
        gtk_widget_unparent(p->err_card);
        p->err_card = NULL;
    }
    p->connecting = TRUE;
    p->was_reconnect = is_reconnect;
    p->reconnected_feed = is_reconnect;
    if (!is_reconnect) {
        p->cur_stage = -1;
        update_nodes(p);
        gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");
        /* 清日志 */
        {
            GtkWidget *ch;
            while ((ch = gtk_widget_get_first_child(p->log_box)))
                gtk_widget_unparent(ch);
        }
        gtk_widget_set_visible(p->hide_btn, TRUE);
        if (p->conn_foot)
            gtk_widget_set_visible(p->conn_foot, TRUE);
    }
    tab_dot(p, NULL);

    /* ctl：强关挂起对话框 → 杀旧会话 → key-confirm → opts → new_ssh。
     * 重连 vt 交出所有权（ctl 持有至成功消费或拒用后保留）。 */
    vt = p->reuse_vt;
    p->reuse_vt = NULL;
    hb_conn_ctl_start(p->ctl, p->host_id, vt, 24, 80);
}

/* ------------------------- 设置/主题 ------------------------- */

static void apply_settings(HbSshPage *p)
{
    hb_obj *o = NULL;
    const char *theme_id = "harbor";
    const char *family = "Monospace";
    double px = 13, lh = 1.5;
    gboolean blink = TRUE, osc52 = FALSE;

    if (hb_vault_settings(hb_store_vault(p->store), &o) == HB_OK && o) {
        theme_id = hb_obj_get_str(o, "theme");
        family = hb_obj_get_str(o, "font_family");
        px = (double)hb_obj_get_int(o, "font_size", 13);
        lh = hb_obj_get_double(o, "line_height", 1.5);
        blink = hb_obj_get_bool(o, "cursor_blink", true);
        osc52 = hb_obj_get_bool(o, "osc52", false);
        p->sb_lines = (int)hb_obj_get_int(o, "scrollback", 10000);
        p->hist_on = hb_obj_get_bool(o, "history_enabled", true);
    }
    {
        hb_session *sess = p->ctl ? hb_conn_ctl_session(p->ctl) : NULL;
        if (sess)
            hb_session_set_history(sess, p->hist_on);
    }
    if (!theme_id || !*theme_id) theme_id = "harbor";
    if (!family || !*family) family = "Monospace";
    if (hb_opt_theme) theme_id = hb_opt_theme;

    /* 主题类加在 term_view 上（.hb-term-view.theme-* 选择器），
     * 连接进度页保持浅色不受主题影响 */
    if (p->theme_id)
        gtk_widget_remove_css_class(p->term_view, p->theme_id);
    g_free(p->theme_id);
    p->theme_id = g_strdup_printf("theme-%s", theme_id);
    gtk_widget_add_css_class(p->term_view, p->theme_id);

    hb_terminal_set_theme(HB_TERMINAL(p->term), hb_term_theme_get(theme_id));
    hb_terminal_set_font(HB_TERMINAL(p->term), family, px, lh);
    hb_terminal_set_cursor_blink(HB_TERMINAL(p->term), blink);
    hb_terminal_set_osc52(HB_TERMINAL(p->term), osc52);

    {
        char raw[32], num[40], *txt; int i = 0, j = 0, n;
        g_snprintf(raw, sizeof raw, "%d", p->sb_lines);
        n = (int)strlen(raw);
        while (i < n) {
            if (i > 0 && (n - i) % 3 == 0) num[j++] = ',';
            num[j++] = raw[i++];
        }
        num[j] = 0;
        txt = g_strdup_printf("scrollback %s 行", num);
        gtk_label_set_label(GTK_LABEL(p->st_sb), txt);
        g_free(txt);
    }
    if (o) hb_obj_free(o);
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbSshPage *p = ud;
    (void)st; (void)id;
    if (!strcmp(type, "settings")) apply_settings(p);
    else if (!strcmp(type, "known") && !p->connecting) {
        /* known_hosts 被外部改了（如同步）—— 下次连接自动生效，无需动作 */
    }
}

/* ------------------------- 视图构建 ------------------------- */

static void on_hide_log(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    p->hide_log = !p->hide_log;
    gtk_button_set_label(b, p->hide_log ? "Show logs" : "Hide logs");
    gtk_widget_set_visible(p->log_scroll, !p->hide_log);
}

static void on_start_over(GtkButton *b, gpointer ud)
{ (void)b; start_connect((HbSshPage *)ud, FALSE); }
static void on_close_tab(GtkButton *b, gpointer ud)
{ (void)b; tab_close((HbSshPage *)ud); }
static void on_reconnect_btn(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_widget_set_visible(p->reconn_btn, FALSE);
    p->dead = FALSE;
    /* 终端视图内原地重连（不跳回连接页） */
    reconnect_now(p);
}

static GtkWidget *build_connect_view(HbSshPage *p)
{
    GtkWidget *wrap, *head, *av, *txtcol, *nm, *sub, *track, *foot;
    int i;
    static const char *node_icons[] = { "link", "shield", "terminal" };
    char *t;

    wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrap, "connect-wrap");
    gtk_widget_set_vexpand(wrap, TRUE);

    /* head：avatar + name + "SSH host:port" + hide-logs */
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(head, "connect-head");
    av = hb_os_avatar_new(p->os_key ? p->os_key : "generic", 38);
    gtk_box_append(GTK_BOX(head), av);
    txtcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    nm = gtk_label_new(p->hname);
    gtk_widget_add_css_class(nm, "connect-name");
    gtk_label_set_xalign(GTK_LABEL(nm), 0);
    gtk_box_append(GTK_BOX(txtcol), nm);
    t = g_strdup_printf("SSH %s:%d", p->haddr, p->hport);
    sub = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(sub, "connect-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_append(GTK_BOX(txtcol), sub);
    gtk_box_append(GTK_BOX(head), txtcol);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(head), sp);
    }
    p->hide_btn = gtk_button_new_with_label("Hide logs");
    gtk_widget_add_css_class(p->hide_btn, "hb-btn");
    gtk_widget_add_css_class(p->hide_btn, "sm");
    gtk_widget_set_valign(p->hide_btn, GTK_ALIGN_CENTER);
    g_signal_connect(p->hide_btn, "clicked", G_CALLBACK(on_hide_log), p);
    gtk_box_append(GTK_BOX(head), p->hide_btn);
    gtk_box_append(GTK_BOX(wrap), head);

    /* progress track：3 节点 + 2 连线 */
    track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(track, "progress-track");
    gtk_widget_set_halign(track, GTK_ALIGN_CENTER);
    for (i = 0; i < 3; i++) {
        p->node[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(p->node[i], "node");
        gtk_widget_add_css_class(p->node[i], "pending");
        gtk_box_append(GTK_BOX(p->node[i]),
                       hb_icon_new(node_icons[i], 12));
        gtk_widget_set_size_request(p->node[i], 22, 22);
        gtk_box_append(GTK_BOX(track), p->node[i]);
        if (i < 2) {
            p->bar[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(p->bar[i], "bar");
            gtk_widget_add_css_class(p->bar[i], "pending");
            gtk_widget_set_size_request(p->bar[i], -1, 3);
            gtk_widget_set_hexpand(p->bar[i], TRUE);
            gtk_widget_set_valign(p->bar[i], GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(track), p->bar[i]);
        }
    }
    gtk_widget_set_size_request(track, 430, -1);
    gtk_box_append(GTK_BOX(wrap), track);

    /* 日志区 */
    p->log_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(p->log_scroll, "connect-log");
    gtk_widget_set_size_request(p->log_scroll, 430, 220);
    gtk_widget_set_halign(p->log_scroll, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->log_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p->log_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(p->log_box, 16);
    gtk_widget_set_margin_end(p->log_box, 16);
    gtk_widget_set_margin_top(p->log_box, 14);
    gtk_widget_set_margin_bottom(p->log_box, 14);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p->log_scroll),
                                  p->log_box);
    gtk_box_append(GTK_BOX(wrap), p->log_scroll);

    /* footer（错误卡显示时隐藏 —— 卡片自带 Close/重试） */
    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    p->conn_foot = foot;
    gtk_widget_set_halign(foot, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(foot, 18);
    {
        GtkWidget *c = gtk_button_new_with_label("Close");
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect(c, "clicked", G_CALLBACK(on_close_tab), p);
        gtk_box_append(GTK_BOX(foot), c);
    }
    {
        GtkWidget *r = gtk_button_new_with_label("Start over");
        gtk_widget_add_css_class(r, "hb-btn");
        gtk_widget_add_css_class(r, "primary");
        g_signal_connect(r, "clicked", G_CALLBACK(on_start_over), p);
        gtk_box_append(GTK_BOX(foot), r);
    }
    gtk_box_append(GTK_BOX(wrap), foot);
    return wrap;
}

static GtkWidget *build_term_view(HbSshPage *p)
{
    GtkWidget *box, *bar, *mono, *sb, *strip;
    char *t;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "hb-term-view");

    /* 工具栏 */
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "hb-term-toolbar");
    p->tt_icon = hb_icon_new(os_avatar(p->os_key), 15);
    gtk_widget_add_css_class(p->tt_icon, "hb-term-tt");
    gtk_box_append(GTK_BOX(bar), p->tt_icon);
    t = g_strdup_printf("ssh %s@%s", p->huser, p->hname);
    p->tt_label = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(p->tt_label, "hb-term-tt");
    gtk_label_set_xalign(GTK_LABEL(p->tt_label), 0);
    gtk_box_append(GTK_BOX(bar), p->tt_label);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(bar), sp);
    }
    mono = hb_mono_icon_new(">_");
    gtk_widget_set_visible(mono, FALSE);
    gtk_box_append(GTK_BOX(bar), mono);
    {
        GtkWidget *fb = gtk_button_new();
        gtk_widget_add_css_class(fb, "tbtn");
        gtk_widget_set_tooltip_text(fb, "在 scrollback 中搜索 (Ctrl+Shift+F)");
        gtk_button_set_child(GTK_BUTTON(fb), hb_icon_new("search", 13));
        g_signal_connect(fb, "clicked",
                         G_CALLBACK(on_find_btn), p);
        gtk_box_append(GTK_BOX(bar), fb);
    }
    p->watch_btn = gtk_button_new();
    gtk_widget_add_css_class(p->watch_btn, "tbtn");
    g_signal_connect(p->watch_btn, "clicked",
                     G_CALLBACK(on_watch_btn), p);
    gtk_box_append(GTK_BOX(bar), p->watch_btn);
    p->reconn_btn = gtk_button_new();
    gtk_widget_add_css_class(p->reconn_btn, "tbtn");
    {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_append(GTK_BOX(r), hb_icon_new("refresh", 13));
        gtk_box_append(GTK_BOX(r), gtk_label_new("重连"));
        gtk_button_set_child(GTK_BUTTON(p->reconn_btn), r);
    }
    g_signal_connect(p->reconn_btn, "clicked",
                     G_CALLBACK(on_reconnect_btn), p);
    gtk_widget_set_visible(p->reconn_btn, FALSE);
    gtk_box_append(GTK_BOX(bar), p->reconn_btn);

    /* 分屏（workspace） */
    {
        GtkWidget *mb = gtk_menu_button_new();
        GtkWidget *pop = gtk_popover_new();
        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *b;
        gtk_widget_add_css_class(mb, "tbtn");
        gtk_widget_set_tooltip_text(mb, "分屏");
        {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            gtk_box_append(GTK_BOX(r), hb_icon_new("cols", 13));
            gtk_box_append(GTK_BOX(r), gtk_label_new("分屏"));
            gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), r);
        }
        b = gtk_button_new_with_label("向右分屏  Ctrl+Shift+D");
        gtk_widget_add_css_class(b, "hb-pop-btn");
        g_signal_connect(b, "clicked", G_CALLBACK(on_split_pick),
                         GINT_TO_POINTER(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(vb), b);
        b = gtk_button_new_with_label("向下分屏  Ctrl+Shift+E");
        gtk_widget_add_css_class(b, "hb-pop-btn");
        g_signal_connect(b, "clicked", G_CALLBACK(on_split_pick),
                         GINT_TO_POINTER(GTK_ORIENTATION_VERTICAL));
        gtk_box_append(GTK_BOX(vb), b);
        gtk_popover_set_child(GTK_POPOVER(pop), vb);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
        gtk_box_append(GTK_BOX(bar), mb);
    }
    p->toolbar = bar;
    gtk_box_append(GTK_BOX(box), bar);

    /* scrollback 查找条（FR-20） */
    p->find_bar = hb_find_bar_new();
    gtk_box_append(GTK_BOX(box), p->find_bar);

    /* 断线重连条 */
    strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(strip, "hb-disc-strip");
    p->strip_label = gtk_label_new("连接已断开");
    gtk_label_set_xalign(GTK_LABEL(p->strip_label), 0);
    gtk_widget_set_hexpand(p->strip_label, TRUE);
    gtk_box_append(GTK_BOX(strip), p->strip_label);
    {
        GtkWidget *now = gtk_button_new_with_label("立即重连");
        gtk_widget_add_css_class(now, "hb-btn");
        gtk_widget_add_css_class(now, "sm");
        g_signal_connect(now, "clicked", G_CALLBACK(on_reconn_now), p);
        gtk_box_append(GTK_BOX(strip), now);
        GtkWidget *cancel = gtk_button_new_with_label("取消");
        gtk_widget_add_css_class(cancel, "hb-btn");
        gtk_widget_add_css_class(cancel, "sm");
        g_signal_connect(cancel, "clicked", G_CALLBACK(on_reconn_cancel), p);
        gtk_box_append(GTK_BOX(strip), cancel);
    }
    gtk_widget_set_visible(strip, FALSE);
    p->strip = strip;
    gtk_box_append(GTK_BOX(box), strip);

    /* overlay：terminal + scrollbar + dead 提示 */
    p->overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(p->overlay, TRUE);
    p->term = hb_terminal_new();
    gtk_widget_set_vexpand(p->term, TRUE);
    gtk_widget_set_hexpand(p->term, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(p->overlay), p->term);
    sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
        hb_terminal_scroll_adjustment(HB_TERMINAL(p->term)));
    gtk_widget_add_css_class(sb, "hb-term-sb");
    gtk_widget_set_halign(sb, GTK_ALIGN_END);
    gtk_widget_set_valign(sb, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), sb);
    p->dead_lbl = gtk_label_new("[连接已关闭]");
    gtk_widget_add_css_class(p->dead_lbl, "hb-term-dead");
    gtk_widget_set_halign(p->dead_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->dead_lbl, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(p->dead_lbl, 24);
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->dead_lbl);
    gtk_box_append(GTK_BOX(box), p->overlay);

    hb_find_bar_bind(HB_FIND_BAR(p->find_bar), HB_TERMINAL(p->term));

    /* 缓冲输入提示 */
    p->buf_hint = gtk_label_new("输入将在重连后发送");
    gtk_widget_add_css_class(p->buf_hint, "hb-disc-strip");
    gtk_label_set_xalign(GTK_LABEL(p->buf_hint), 0);
    gtk_widget_set_visible(p->buf_hint, FALSE);
    gtk_box_append(GTK_BOX(box), p->buf_hint);

    /* 状态栏 */
    {
        GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_add_css_class(st, "hb-term-status");
        p->st_conn = gtk_label_new("● 连接中…");
        gtk_widget_add_css_class(p->st_conn, "hb-term-warn");
        gtk_box_append(GTK_BOX(st), p->st_conn);
        GtkWidget *tt = gtk_label_new("xterm-256color");
        gtk_widget_add_css_class(tt, "hb-term-muted");
        gtk_box_append(GTK_BOX(st), tt);
        GtkWidget *enc = gtk_label_new("UTF-8");
        gtk_widget_add_css_class(enc, "hb-term-muted");
        gtk_box_append(GTK_BOX(st), enc);
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(st), sp);
        p->st_watch = gtk_label_new("监控: 关");
        gtk_widget_add_css_class(p->st_watch, "hb-term-muted");
        gtk_box_append(GTK_BOX(st), p->st_watch);
        p->st_sb = gtk_label_new("");
        gtk_box_append(GTK_BOX(st), p->st_sb);
        p->status_bar = st;
        g_object_ref(st);   /* unparent 借给 workspace footer 后仍存活 */
        gtk_box_append(GTK_BOX(box), st);
    }
    return box;
}

/* ------------------------- 生命周期 ------------------------- */

static void hb_ssh_page_dispose(GObject *o)
{
    HbSshPage *p = HB_SSH_PAGE(o);
    if (p->backoff_src) {
        g_source_remove(p->backoff_src);
        p->backoff_src = 0;
    }
    free_session(p);              /* 含 ctl 对话框强关 + 会话收尾 */
    g_clear_object(&p->ctl);      /* ctl dispose：日志收尾 + 字段释放 */
    if (p->reuse_vt) {
        hb_vt_free(p->reuse_vt);
        p->reuse_vt = NULL;
    }
    if (p->changed_handler && p->store)
        g_signal_handler_disconnect(p->store, p->changed_handler);
    p->changed_handler = 0;
    G_OBJECT_CLASS(hb_ssh_page_parent_class)->dispose(o);
}

static void hb_ssh_page_finalize(GObject *o)
{
    HbSshPage *p = HB_SSH_PAGE(o);
    g_free(p->tab_id);
    g_free(p->host_id);
    g_free(p->hname);
    g_free(p->haddr);
    g_free(p->huser);
    g_free(p->os_key);
    g_free(p->disp_title);
    g_clear_object(&p->status_bar);
    g_free(p->theme_id);
    if (p->inbuf) g_string_free(p->inbuf, TRUE);
    G_OBJECT_CLASS(hb_ssh_page_parent_class)->finalize(o);
}

static void hb_ssh_page_class_init(HbSshPageClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_ssh_page_dispose;
    oc->finalize = hb_ssh_page_finalize;
    sigs[SIG_EDIT_HOST] = g_signal_new("edit-host", HB_TYPE_SSH_PAGE,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void hb_ssh_page_init(HbSshPage *p)
{
    p->sb_lines = 10000;
    p->inbuf = g_string_new(NULL);
    p->cur_stage = -1;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(p),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(p), "hb-ssh-page");

    p->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(p->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(p->stack, TRUE);
    gtk_box_append(GTK_BOX(p), p->stack);
}

GtkWidget *hb_ssh_page_new(HbStore *st, const hb_obj *host)
{
    HbSshPage *p = g_object_new(HB_TYPE_SSH_PAGE, NULL);
    const char *os;

    p->store = st;
    p->host_id = g_strdup(hb_obj_id(host));
    p->hname = g_strdup(hb_obj_get_str(host, "name") ?: "host");
    p->haddr = g_strdup(hb_obj_get_str(host, "address") ?: "");
    p->huser = g_strdup(hb_obj_get_str(host, "user") ?: g_get_user_name());
    p->hport = (int)hb_obj_get_int(host, "port", 22);
    if (p->hport <= 0) p->hport = 22;
    os = hb_obj_get_str(host, "os");
    p->os_key = g_strdup(os && *os && strcmp(os,"auto") ? os : "generic");
    p->disp_title = g_strdup_printf("%s · ssh, %s",
                                    p->hname ? p->hname : "host",
                                    p->huser ? p->huser : "?");

    /* 终端先建（连接视图也用它消费事件） */
    p->term_view = build_term_view(p);
    p->connect_box = build_connect_view(p);
    gtk_stack_add_named(GTK_STACK(p->stack), p->connect_box, "connect");
    gtk_stack_add_named(GTK_STACK(p->stack), p->term_view, "term");
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");

    apply_settings(p);

    /* 监控默认：SSH 默认开（settings.watch_default_ssh），
     * 信号掩码取引擎默认（EOF|133|RULES|TERMNOTIFY） */
    {
        hb_obj *o = NULL;
        hb_watch_cfg dc;
        hb_watch_cfg_defaults(&dc);
        p->watch_signals = dc.signals;
        if (hb_vault_settings(hb_store_vault(st), &o) == HB_OK && o) {
            p->watch_on = hb_obj_get_bool(o, "watch_default_ssh", true);
            if (hb_obj_get_bool(o, "watch_idle", false))
                p->watch_signals |= HB_WATCH_SIG_IDLE;
            hb_obj_free(o);
        }
    }
    watch_refresh_ui(p);

    /* 连接流程控制器：对话框 parent 用本页 widget（弱引用） */
    p->ctl = hb_conn_ctl_new(st, GTK_WIDGET(p), HB_CONN_SHELL);
    g_signal_connect(p->ctl, "stage", G_CALLBACK(on_ctl_stage), p);
    g_signal_connect(p->ctl, "session", G_CALLBACK(on_ctl_session), p);
    g_signal_connect(p->ctl, "connected",
                     G_CALLBACK(on_ctl_connected), p);
    g_signal_connect(p->ctl, "failed", G_CALLBACK(on_ctl_failed), p);
    g_signal_connect(p->ctl, "disconnected",
                     G_CALLBACK(on_ctl_disconn), p);
    g_signal_connect(p->ctl, "exited", G_CALLBACK(on_ctl_exited), p);
    g_signal_connect(p->ctl, "osinfo", G_CALLBACK(on_osinfo), p);

    hb_terminal_set_event_cb(HB_TERMINAL(p->term), on_session_ev, p);
    g_signal_connect(p->term, "title-changed",
                     G_CALLBACK(on_title), p);
    g_signal_connect(p->term, "exited", G_CALLBACK(on_exited), p);
    g_signal_connect(p->term, "dead-input", G_CALLBACK(on_dead_input), p);
    g_signal_connect(p->term, "notify-event", G_CALLBACK(on_notify), p);

    p->changed_handler = g_signal_connect(st, "changed",
        G_CALLBACK(on_store_changed), p);

    update_nodes(p);
    start_connect(p, FALSE);
    return GTK_WIDGET(p);
}

/* tab 关联（add_tab 之后调用；OSINFO/标题更新要用） */
void hb_ssh_page_attach_tab(HbSshPage *p, GtkWidget *win, const char *tab_id)
{
    p->win = HB_WINDOW(win);
    g_free(p->tab_id);
    p->tab_id = g_strdup(tab_id);
    /* workspace pane 不占整个 tab 的 host 关联（多 pane 多 host） */
    if (!p->compact)
        hb_window_set_tab_host(p->win, tab_id, p->host_id);
}

/* 供 dev 调试：像用户输入一样把文本写进会话 */
void hb_ssh_page_feed_input(HbSshPage *p, const char *text)
{
    hb_session *sess = hb_conn_ctl_session(p->ctl);
    if (sess && text)
        hb_session_write(sess, text, strlen(text));
}

hb_session *hb_ssh_page_session(HbSshPage *p)
{
    return hb_conn_ctl_session(p->ctl);
}

void hb_ssh_page_find_open(HbSshPage *p)
{
    /* 连接页/错误卡时没有终端 —— 仅 term 视图可用 */
    if (p->find_bar)
        hb_find_bar_open(HB_FIND_BAR(p->find_bar));
}

/* 工具栏"分屏"菜单：把当前 tab 包装成 workspace 并分屏 */
static void on_split_pick(GtkButton *b, gpointer ud)
{
    GtkOrientation o = GPOINTER_TO_INT(ud);
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b),
                                             GTK_TYPE_POPOVER);
    GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(b));
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    if (HB_IS_WINDOW(r))
        hb_window_split_active(HB_WINDOW(r), o);
}

/* -------------------- HbSessionPage 接口 -------------------- */

static HbTerminal *sp_terminal(HbSessionPage *sp)
{
    return HB_TERMINAL(HB_SSH_PAGE(sp)->term);
}

static const char *sp_title(HbSessionPage *sp)
{
    HbSshPage *p = HB_SSH_PAGE(sp);
    return p->disp_title ? p->disp_title : (p->hname ? p->hname : "ssh");
}

static const char *sp_icon(HbSessionPage *sp)
{
    return os_avatar(HB_SSH_PAGE(sp)->os_key);
}

/* compact：藏页内工具栏（pane-head 接管），状态条从 term_view 摘除
 * 借给 workspace footer；恢复时重新 append 回 term_view 尾部 */
static void sp_set_compact(HbSessionPage *sp, gboolean compact)
{
    HbSshPage *p = HB_SSH_PAGE(sp);
    if (p->compact == compact) return;
    p->compact = compact;
    gtk_widget_set_visible(p->toolbar, !compact);
    if (compact) {
        if (gtk_widget_get_parent(p->status_bar) == p->term_view)
            gtk_box_remove(GTK_BOX(p->term_view), p->status_bar);
    } else {
        if (!gtk_widget_get_parent(p->status_bar))
            gtk_box_append(GTK_BOX(p->term_view), p->status_bar);
    }
}

static GtkWidget *sp_status_widget(HbSessionPage *sp)
{
    return HB_SSH_PAGE(sp)->status_bar;
}

static void sp_open_find(HbSessionPage *sp)
{
    hb_ssh_page_find_open(HB_SSH_PAGE(sp));
}

static void sp_open_watch(HbSessionPage *sp)
{
    on_watch_btn(NULL, HB_SSH_PAGE(sp));
}

static gboolean sp_is_dead(HbSessionPage *sp)
{
    return HB_SSH_PAGE(sp)->dead;
}

static void sp_reconnect(HbSessionPage *sp)
{
    HbSshPage *p = HB_SSH_PAGE(sp);
    if (p->backoff_src) { g_source_remove(p->backoff_src); p->backoff_src = 0; }
    reconnect_now(p);
}

static GtkWidget *sp_clone(HbSessionPage *sp)
{
    HbSshPage *p = HB_SSH_PAGE(sp);
    hb_obj *h = NULL;
    GtkWidget *w = NULL;
    if (!p->host_id ||
        hb_vault_get(hb_store_vault(p->store), p->host_id, &h) != HB_OK || !h)
        return NULL;
    w = hb_ssh_page_new(p->store, h);
    hb_obj_free(h);
    return w;
}

static const char *sp_host_id(HbSessionPage *sp)
{
    return HB_SSH_PAGE(sp)->host_id;
}

static void sp_attach_tab(HbSessionPage *sp, GtkWidget *win,
                          const char *tab_id)
{
    hb_ssh_page_attach_tab(HB_SSH_PAGE(sp), win, tab_id);
}

static void hb_ssh_page_iface_init(HbSessionPageInterface *iface)
{
    iface->terminal       = sp_terminal;
    iface->title          = sp_title;
    iface->icon           = sp_icon;
    iface->set_compact    = sp_set_compact;
    iface->status_widget  = sp_status_widget;
    iface->open_find      = sp_open_find;
    iface->open_watch     = sp_open_watch;
    iface->is_dead        = sp_is_dead;
    iface->reconnect      = sp_reconnect;
    iface->clone          = sp_clone;
    iface->host_id        = sp_host_id;
    iface->attach_tab     = sp_attach_tab;
}
