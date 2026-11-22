#include "hb-term-page.h"
#include "hb-terminal.h"
#include "hb-find-bar.h"
#include "hb-share.h"
#include "hb-share-owner.h"
#include "hb-share-dialog.h"
#include "hb-term-theme.h"
#include "hb-icons.h"
#include "hb-notify.h"
#include "hb-watch-ui.h"
#include "hb-window.h"
#include "hb-session-page.h"

#include <string.h>
#include <unistd.h>

struct _HbTermPage {
    GtkBox parent;

    HbStore     *store;         /* borrowed */
    hb_session  *sess;          /* owned */
    GtkWidget   *term;
    GtkWidget   *overlay;
    GtkWidget   *dead_lbl;
    GtkWidget   *tt_label;
    GtkWidget   *st_conn;
    GtkWidget   *st_sb;
    GtkWidget   *st_watch;
    GtkWidget   *watch_btn;
    GtkWidget   *share_btn;
    GtkWidget   *find_bar;
    GtkWidget   *toolbar;
    GtkWidget   *status_bar;
    gulong       changed_handler;
    char        *shell_base;
    char        *disp_title;    /* pane-head/title()："Local · bash[·远端标题]" */
    int          sb_lines;
    gboolean     dead;
    gboolean     compact;       /* workspace 托管：藏工具栏，状态条外借 */
    char        *theme_id;      /* dev --theme 覆盖或 settings.theme */
    /* FR-13 完成监控（会话级状态，重连/重启后重挂） */
    gboolean     watch_on;
    unsigned     watch_signals;
    /* FR-08 shell history 采集（settings.history_enabled） */
    gboolean     hist_on;
    char        *tab_id;
    HbWindow    *win;
};

static void hb_term_page_iface_init(HbSessionPageInterface *iface);
G_DEFINE_TYPE_WITH_CODE(HbTermPage, hb_term_page, GTK_TYPE_BOX,
    G_IMPLEMENT_INTERFACE(HB_TYPE_SESSION_PAGE, hb_term_page_iface_init))

extern char *hb_opt_theme;      /* main.c dev 选项 */

/* ------------------------------ 会话 ------------------------------ */

static hb_session *spawn_local(HbTermPage *p, hb_vt *reuse)
{
    hb_pty_opts o = {0};
    return hb_session_new_local_vt(&o, reuse, p->sb_lines);
}

static void set_status(HbTermPage *p, gboolean alive)
{
    gtk_label_set_label(GTK_LABEL(p->st_conn),
        alive ? "● 已连接" : "● 会话已结束");
    if (alive) {
        gtk_widget_add_css_class(p->st_conn, "hb-term-ok");
        gtk_widget_remove_css_class(p->st_conn, "hb-term-ended");
    } else {
        gtk_widget_remove_css_class(p->st_conn, "hb-term-ok");
        gtk_widget_add_css_class(p->st_conn, "hb-term-ended");
    }
}

static void on_title(HbTerminal *t, const char *title, gpointer ud)
{
    HbTermPage *p = ud;
    char *full;
    (void)t;
    if (title && *title)
        full = g_strdup_printf("Local · %s · %s", p->shell_base, title);
    else
        full = g_strdup_printf("Local · %s", p->shell_base);
    gtk_label_set_label(GTK_LABEL(p->tt_label), full);
    g_free(p->disp_title);
    p->disp_title = g_strdup(full);
    g_free(full);
    hb_session_page_meta_changed(HB_SESSION_PAGE(p));
}

static void on_exited(HbTerminal *t, int code, gpointer ud)
{
    HbTermPage *p = ud;
    (void)t; (void)code;
    p->dead = TRUE;
    set_status(p, FALSE);
    gtk_widget_set_visible(p->dead_lbl, TRUE);
    if (p->sess)
        hb_share_service_stop_session(hb_share_service_default(),
                                      p->sess);
    hb_session_page_meta_changed(HB_SESSION_PAGE(p));
}

static void on_notify(HbTerminal *t, const char *title, const char *body,
                      gpointer ud)
{
    HbTermPage *p = ud;
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
        if (host_only)
            hb_notify_post("info", "终端通知 · Local",
                           "Local 上的任务有新通知", NULL);
        else
            hb_notify_post("info", title, body ? body : "", NULL);
    }
}

/* ------------------------------ 监控 ------------------------------ */

static void watch_refresh_ui(HbTermPage *p)
{
    char *t;
    if (p->watch_on) {
        t = hb_watch_signal_summary(p->watch_signals);
        gtk_button_set_label(GTK_BUTTON(p->watch_btn), "");
        {
            GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            gtk_box_append(GTK_BOX(bx), hb_icon_new("eye", 13));
            gtk_box_append(GTK_BOX(bx), gtk_label_new("监控 开"));
            gtk_button_set_child(GTK_BUTTON(p->watch_btn), bx);
        }
        gtk_widget_add_css_class(p->watch_btn, "on");
    } else {
        GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_append(GTK_BOX(bx), hb_icon_new("eye", 13));
        gtk_box_append(GTK_BOX(bx), gtk_label_new("监控 关"));
        gtk_button_set_child(GTK_BUTTON(p->watch_btn), bx);
        gtk_widget_remove_css_class(p->watch_btn, "on");
        t = NULL;
    }
    gtk_label_set_label(GTK_LABEL(p->st_watch),
                        t ? t : "监控: 关");
    g_free(t);
}

static void watch_apply_session(HbTermPage *p)
{
    hb_watch_apply(p->sess, p->store, NULL,
                   p->watch_on, p->watch_signals);
}

static void watch_set(gpointer ud, gboolean on, unsigned signals)
{
    HbTermPage *p = ud;
    p->watch_on = on;
    p->watch_signals = signals;
    watch_refresh_ui(p);
    watch_apply_session(p);
}

static hb_session *watch_get_sess(gpointer ud)
{
    HbTermPage *p = ud;
    return p->sess;
}

static void on_find_btn(GtkButton *b, gpointer ud)
{
    HbTermPage *p = ud;
    (void)b;
    hb_term_page_find_open(p);
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

static void on_share_btn(GtkButton *b, gpointer ud)
{
    HbTermPage *p = ud;
    (void)b;
    if (!p->sess) return;
    hb_share_dialog_present(GTK_WIDGET(p), p->sess,
                            p->disp_title ? p->disp_title : "Local",
                            "localhost");
}

static void on_watch_btn(GtkButton *b, gpointer ud)
{
    HbTermPage *p = ud;
    HbWatchUiOps ops = {0};
    (void)b;
    ops.page = GTK_WIDGET(p);
    ops.store = p->store;
    ops.host_id = NULL;
    ops.is_ssh = FALSE;
    ops.on = p->watch_on;
    ops.signals = p->watch_signals;
    ops.apply = watch_set;
    ops.apply_ud = p;
    ops.session = watch_get_sess;
    ops.session_ud = p;
    hb_watch_ui_open(&ops);
}

static void on_session_ev(HbTerminal *t, const hb_sess_event *ev,
                          gpointer ud)
{
    HbTermPage *p = ud;
    (void)t;
    if (ev->type == HB_SESS_EV_WATCH && ev->watch && p->tab_id)
        hb_watch_route(GTK_WIDGET(p), p->tab_id, "Local", ev->watch);
    else if (ev->type == HB_SESS_EV_COMMAND && ev->text && *ev->text)
        /* FR-08 shell history：host_id NULL = 本地（密码已被会话过滤） */
        hb_vault_history_add(hb_store_vault(p->store), NULL, ev->text);
}

/* 重启本地会话（保留 scrollback：旧会话已死 → 偷 vt 复用） */
static gboolean restart_local(HbTermPage *p)
{
    hb_session *ns;
    hb_vt *reuse = NULL;
    if (p->sess && !hb_session_alive(p->sess))
        reuse = hb_session_steal_vt(p->sess);
    ns = spawn_local(p, reuse);
    if (!ns) {
        if (reuse) hb_vt_free(reuse);
        return FALSE;
    }
    hb_session_set_history(ns, p->hist_on);
    if (p->sess) {
        hb_share_service_stop_session(hb_share_service_default(),
                                      p->sess);
        hb_session_close(p->sess);
        hb_session_free(p->sess);
    }
    p->sess = ns;
    if (p->share_btn && p->win && p->tab_id)
        hb_share_owner_wire(GTK_WIDGET(p), p->sess, p->share_btn,
                            GTK_WIDGET(p->win), p->tab_id);
    p->dead = FALSE;
    hb_terminal_set_session(HB_TERMINAL(p->term), p->sess);
    watch_apply_session(p);       /* 监控状态随重启重挂 */
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    set_status(p, TRUE);
    hb_session_page_meta_changed(HB_SESSION_PAGE(p));
    return TRUE;
}

/* dead 状态下捕获 Enter → 重启本地会话（先于终端自己的 key 处理） */
static gboolean on_dead_key(GtkEventControllerKey *kc, guint keyval,
                            guint code, GdkModifierType state, gpointer ud)
{
    HbTermPage *p = ud;
    (void)kc; (void)code;
    if (!p->dead || keyval != GDK_KEY_Return ||
        (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)))
        return FALSE;
    restart_local(p);
    return TRUE;
}

/* ------------------------------ 设置 ------------------------------ */

static void apply_settings(HbTermPage *p)
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
    if (p->sess)
        hb_session_set_history(p->sess, p->hist_on);
    if (!theme_id || !*theme_id) theme_id = "harbor";
    if (!family || !*family) family = "Monospace";
    if (hb_opt_theme)
        theme_id = hb_opt_theme;

    if (p->theme_id)
        gtk_widget_remove_css_class(GTK_WIDGET(p), p->theme_id);
    g_free(p->theme_id);
    p->theme_id = g_strdup_printf("theme-%s", theme_id);
    gtk_widget_add_css_class(GTK_WIDGET(p), p->theme_id);

    hb_terminal_set_theme(HB_TERMINAL(p->term),
                          hb_term_theme_get(theme_id));
    hb_terminal_set_font(HB_TERMINAL(p->term), family, px, lh);
    hb_terminal_set_cursor_blink(HB_TERMINAL(p->term), blink);
    hb_terminal_set_osc52(HB_TERMINAL(p->term), osc52);

    {
        /* "scrollback 10,000 行"：手工加千位分隔（不依赖 locale） */
        char raw[32], num[40], *txt;
        int i = 0, j = 0, n;
        g_snprintf(raw, sizeof raw, "%d", p->sb_lines);
        n = (int)strlen(raw);
        while (i < n) {
            if (i > 0 && (n - i) % 3 == 0)
                num[j++] = ',';
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
    HbTermPage *p = ud;
    (void)st; (void)id;
    if (!strcmp(type, "settings"))
        apply_settings(p);
}

/* ------------------------------ 构建 ------------------------------ */

static GtkWidget *build_toolbar(HbTermPage *p)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mono = hb_mono_icon_new(">_");
    char *t;

    gtk_widget_add_css_class(bar, "hb-term-toolbar");
    gtk_widget_add_css_class(mono, "hb-term-tt");
    gtk_box_append(GTK_BOX(bar), mono);

    t = g_strdup_printf("Local · %s", p->shell_base);
    p->tt_label = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(p->tt_label, "hb-term-tt");
    gtk_label_set_xalign(GTK_LABEL(p->tt_label), 0);
    gtk_box_append(GTK_BOX(bar), p->tt_label);

    /* spacer */
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(bar), sp);
    }
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

    p->share_btn = gtk_button_new_with_label("共享");
    gtk_widget_add_css_class(p->share_btn, "tbtn");
    gtk_widget_set_tooltip_text(p->share_btn,
                                "共享会话（端到端加密）");
    g_signal_connect(p->share_btn, "clicked",
                     G_CALLBACK(on_share_btn), p);
    gtk_box_append(GTK_BOX(bar), p->share_btn);

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
    return bar;
}

static GtkWidget *build_status(HbTermPage *p)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *term_lbl = gtk_label_new("xterm-256color");
    GtkWidget *enc = gtk_label_new("UTF-8");
    GtkWidget *sp;

    gtk_widget_add_css_class(bar, "hb-term-status");
    p->st_conn = gtk_label_new("● 已连接");
    gtk_widget_add_css_class(p->st_conn, "hb-term-ok");
    gtk_box_append(GTK_BOX(bar), p->st_conn);
    gtk_widget_add_css_class(term_lbl, "hb-term-muted");
    gtk_widget_add_css_class(enc, "hb-term-muted");
    gtk_box_append(GTK_BOX(bar), term_lbl);
    gtk_box_append(GTK_BOX(bar), enc);

    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(bar), sp);

    p->st_watch = gtk_label_new("监控: 关");
    gtk_widget_add_css_class(p->st_watch, "hb-term-muted");
    gtk_box_append(GTK_BOX(bar), p->st_watch);

    p->st_sb = gtk_label_new("");
    gtk_box_append(GTK_BOX(bar), p->st_sb);
    return bar;
}

static void hb_term_page_dispose(GObject *o)
{
    HbTermPage *p = HB_TERM_PAGE(o);
    if (p->sess) {
        hb_share_service_stop_session(hb_share_service_default(),
                                      p->sess);
        hb_terminal_set_session(HB_TERMINAL(p->term), NULL);
        hb_session_close(p->sess);
        hb_session_free(p->sess);
        p->sess = NULL;
    }
    if (p->changed_handler && p->store)
        g_signal_handler_disconnect(p->store, p->changed_handler);
    p->changed_handler = 0;
    G_OBJECT_CLASS(hb_term_page_parent_class)->dispose(o);
}

static void hb_term_page_finalize(GObject *o)
{
    HbTermPage *p = HB_TERM_PAGE(o);
    g_clear_object(&p->status_bar);
    g_free(p->shell_base);
    g_free(p->disp_title);
    g_free(p->theme_id);
    g_free(p->tab_id);
    G_OBJECT_CLASS(hb_term_page_parent_class)->finalize(o);
}

static void hb_term_page_class_init(HbTermPageClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_term_page_dispose;
    oc->finalize = hb_term_page_finalize;
}

static void hb_term_page_init(HbTermPage *p)
{
    GtkWidget *sb;
    GtkEventController *key;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(p),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(p), "hb-term-view");

    p->shell_base = g_path_get_basename(
        g_getenv("SHELL") ? g_getenv("SHELL") : "/bin/sh");
    p->disp_title = g_strdup_printf("Local · %s", p->shell_base);
    p->sb_lines = 10000;

    p->toolbar = build_toolbar(p);
    gtk_box_append(GTK_BOX(p), p->toolbar);

    p->find_bar = hb_find_bar_new();
    gtk_box_append(GTK_BOX(p), p->find_bar);

    /* overlay：terminal + 右侧 overlay 滚动条 + dead 提示 */
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

    p->dead_lbl = gtk_label_new("[进程已退出 · 按 Enter 重新启动]");
    gtk_widget_add_css_class(p->dead_lbl, "hb-term-dead");
    gtk_widget_set_halign(p->dead_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->dead_lbl, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(p->dead_lbl, 24);
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->dead_lbl);

    gtk_box_append(GTK_BOX(p), p->overlay);
    p->status_bar = build_status(p);
    g_object_ref(p->status_bar);   /* unparent 借给 workspace footer 后仍存活 */
    gtk_box_append(GTK_BOX(p), p->status_bar);

    hb_find_bar_bind(HB_FIND_BAR(p->find_bar), HB_TERMINAL(p->term));

    /* dead Enter 重启：capture 阶段先于终端 key handler */
    key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_dead_key), p);
    gtk_widget_add_controller(p->term, key);
}

GtkWidget *hb_term_page_new_local(HbStore *st)
{
    HbTermPage *p = g_object_new(HB_TYPE_TERM_PAGE, NULL);

    p->store = st;
    apply_settings(p);          /* 先取 sb_lines / theme / font */

    /* 监控默认：Local 默认关（settings.watch_default_local），
     * 信号掩码取引擎默认（EOF|133|RULES|TERMNOTIFY） */
    {
        hb_obj *o = NULL;
        hb_watch_cfg dc;
        hb_watch_cfg_defaults(&dc);
        p->watch_signals = dc.signals;
        if (hb_vault_settings(hb_store_vault(st), &o) == HB_OK && o) {
            p->watch_on = hb_obj_get_bool(o, "watch_default_local",
                                          false);
            if (hb_obj_get_bool(o, "watch_idle", false))
                p->watch_signals |= HB_WATCH_SIG_IDLE;
            hb_obj_free(o);
        }
    }

    p->sess = spawn_local(p, NULL);
    if (p->sess)
        hb_session_set_history(p->sess, p->hist_on);
    hb_terminal_set_session(HB_TERMINAL(p->term), p->sess);
    hb_terminal_set_event_cb(HB_TERMINAL(p->term), on_session_ev, p);
    watch_apply_session(p);
    watch_refresh_ui(p);
    set_status(p, p->sess != NULL);
    if (!p->sess) {
        p->dead = TRUE;
        gtk_widget_set_visible(p->dead_lbl, TRUE);
    }

    g_signal_connect(p->term, "title-changed", G_CALLBACK(on_title), p);
    g_signal_connect(p->term, "exited", G_CALLBACK(on_exited), p);
    g_signal_connect(p->term, "notify-event", G_CALLBACK(on_notify), p);

    p->changed_handler = g_signal_connect(st, "changed",
        G_CALLBACK(on_store_changed), p);
    return GTK_WIDGET(p);
}

/* add_tab 之后调用：tab id 供命中路由定位角标 */
void hb_term_page_find_open(HbTermPage *p)
{
    hb_find_bar_open(HB_FIND_BAR(p->find_bar));
}

void hb_term_page_attach_tab(HbTermPage *p, GtkWidget *win,
                             const char *tab_id)
{
    p->win = HB_WINDOW(win);
    g_free(p->tab_id);
    p->tab_id = g_strdup(tab_id);
    if (p->share_btn)
        hb_share_owner_wire(GTK_WIDGET(p), p->sess, p->share_btn,
                            win, tab_id);
}

void hb_term_page_feed_input(HbTermPage *p, const char *text)
{
    if (p->sess && text)
        hb_session_write(p->sess, text, strlen(text));
}

hb_session *hb_term_page_session(HbTermPage *p)
{
    return p->sess;
}

/* -------------------- HbSessionPage 接口 -------------------- */

static HbTerminal *sp_terminal(HbSessionPage *sp)
{
    return HB_TERMINAL(HB_TERM_PAGE(sp)->term);
}

static const char *sp_title(HbSessionPage *sp)
{
    HbTermPage *p = HB_TERM_PAGE(sp);
    return p->disp_title ? p->disp_title : "Local";
}

static const char *sp_icon(HbSessionPage *sp)
{
    (void)sp;
    return "terminal";
}

/* compact：藏页内工具栏（pane-head 接管），状态条从页里摘除借给
 * workspace footer；恢复时重新 append 回页尾 */
static void sp_set_compact(HbSessionPage *sp, gboolean compact)
{
    HbTermPage *p = HB_TERM_PAGE(sp);
    if (p->compact == compact) return;
    p->compact = compact;
    gtk_widget_set_visible(p->toolbar, !compact);
    if (compact) {
        if (gtk_widget_get_parent(p->status_bar) == GTK_WIDGET(p))
            gtk_box_remove(GTK_BOX(p), p->status_bar);
    } else {
        if (!gtk_widget_get_parent(p->status_bar))
            gtk_box_append(GTK_BOX(p), p->status_bar);
    }
}

static GtkWidget *sp_status_widget(HbSessionPage *sp)
{
    return HB_TERM_PAGE(sp)->status_bar;
}

static void sp_open_find(HbSessionPage *sp)
{
    hb_term_page_find_open(HB_TERM_PAGE(sp));
}

static void sp_open_watch(HbSessionPage *sp)
{
    on_watch_btn(NULL, HB_TERM_PAGE(sp));
}

static gboolean sp_is_dead(HbSessionPage *sp)
{
    return HB_TERM_PAGE(sp)->dead;
}

static void sp_reconnect(HbSessionPage *sp)
{
    restart_local(HB_TERM_PAGE(sp));
}

static GtkWidget *sp_clone(HbSessionPage *sp)
{
    return hb_term_page_new_local(HB_TERM_PAGE(sp)->store);
}

static const char *sp_host_id(HbSessionPage *sp)
{
    (void)sp;
    return NULL;
}

static void sp_attach_tab(HbSessionPage *sp, GtkWidget *win,
                          const char *tab_id)
{
    hb_term_page_attach_tab(HB_TERM_PAGE(sp), win, tab_id);
}

static void hb_term_page_iface_init(HbSessionPageInterface *iface)
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
