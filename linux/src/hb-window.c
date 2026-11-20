#include "hb-window.h"
#include "hb-shortcuts.h"
#include "hb-icons.h"
#include "hb-notify.h"
#include "hb-vault-page.h"
#include "hb-term-page.h"
#include "hb-ssh-page.h"
#include "hb-session-page.h"
#include "hb-workspace.h"
#include "hb-store.h"

#include <stdio.h>

typedef struct HbTabRec {
    HbWindow  *win;
    char      *id;
    char      *icon;
    char      *host_id;     /* 关联 vault host（NULL=非主机页） */
    GtkWidget *page;        /* stack child */
    GtkWidget *box;         /* .hb-tab container */
    GtkWidget *main;        /* 选中按钮 */
    GtkWidget *title;       /* label inside main btn */
    GtkWidget *dot;
    GtkWidget *close_btn;
    gboolean   closable;
    gboolean   is_term;
    gboolean   watch_dot;   /* dot 是监控角标 → 切到此 tab 时自动清 */
    guint      watch_token; /* clear_watch_dot 的时序令牌 */
    char      *ws_title;    /* 包成 workspace 前的标题/图标（塌回时还原） */
    char      *ws_icon;
} HbTabRec;

struct _HbWindow {
    AdwApplicationWindow parent;
    GtkWidget *tabstrip;     /* hbox inside scroller */
    GtkWidget *stack;
    GtkWidget *toasts;       /* AdwToastOverlay 包住全部内容 */
    GtkWidget *overlay;      /* GtkOverlay：palette 等浮层挂载点 */
    GtkWidget *badge;
    GtkWidget *bell;
    GtkWidget *vault_page;
    GList     *tabs;         /* HbTabRec* */
    HbTabRec  *active;
};

G_DEFINE_TYPE(HbWindow, hb_window, ADW_TYPE_APPLICATION_WINDOW)

enum { SIG_TAB_CLOSED, N_SIGS };
static guint sigs[N_SIGS];

/* ------------------------------- tabs ------------------------------- */

static HbTabRec *find_tab(HbWindow *win, const char *id)
{
    for (GList *l = win->tabs; l; l = l->next) {
        HbTabRec *t = l->data;
        if (!strcmp(t->id, id))
            return t;
    }
    return NULL;
}

static void refresh_active(HbWindow *win)
{
    for (GList *l = win->tabs; l; l = l->next) {
        HbTabRec *t = l->data;
        if (t == win->active) {
            gtk_widget_add_css_class(t->box, "active");
            gtk_stack_set_visible_child_name(GTK_STACK(win->stack), t->id);
            if (t->watch_dot) {          /* 切到带监控角标的 tab → 已读 */
                t->watch_dot = FALSE;
                hb_window_set_tab_dot(win, t->id, NULL);
            }
        } else {
            gtk_widget_remove_css_class(t->box, "active");
        }
    }
}

void hb_window_switch_tab(HbWindow *win, const char *id)
{
    HbTabRec *t = find_tab(win, id);
    if (t) {
        win->active = t;
        refresh_active(win);
    }
}

void hb_window_switch_tab_n(HbWindow *win, guint idx)
{
    GList *l = g_list_nth(win->tabs, idx);
    if (l)
        hb_window_switch_tab(win, ((HbTabRec *)l->data)->id);
}

void hb_window_switch_tab_rel(HbWindow *win, int dir)
{
    guint n = g_list_length(win->tabs);
    guint i = win->active ? g_list_index(win->tabs, win->active) : 0;
    guint ni;
    GList *l;
    if (!n)
        return;
    ni = (guint)(((int)i + dir + (int)n) % (int)n);
    l = g_list_nth(win->tabs, ni);
    if (l)
        hb_window_switch_tab(win, ((HbTabRec *)l->data)->id);
}

static void on_tab_click(GtkButton *b, gpointer ud)
{
    HbTabRec *t = ud;
    (void)b;
    t->win->active = t;
    refresh_active(t->win);
}

static void free_tab_rec(HbTabRec *t)
{
    g_free(t->id);
    g_free(t->icon);
    g_free(t->host_id);
    g_free(t->ws_title);
    g_free(t->ws_icon);
    g_free(t);
}

GtkWidget *hb_window_tab_page(HbWindow *win, const char *id)
{
    HbTabRec *t = find_tab(win, id);
    return t ? t->page : NULL;
}

gboolean hb_window_has_tab(HbWindow *win, const char *id)
{
    return find_tab(win, id) != NULL;
}

void hb_window_close_tab(HbWindow *win, const char *id)
{
    HbTabRec *t = find_tab(win, id);
    GList *link;
    guint idx;
    gboolean was_active;
    if (!t || !t->closable)
        return;
    was_active = (win->active == t);
    link = g_list_find(win->tabs, t);
    idx = g_list_position(win->tabs, link);
    win->tabs = g_list_delete_link(win->tabs, link);
    gtk_stack_remove(GTK_STACK(win->stack), t->page);   /* destroys page */
    gtk_widget_unparent(t->box);
    g_signal_emit(win, sigs[SIG_TAB_CLOSED], 0, t->id);
    free_tab_rec(t);
    if (was_active) {
        GList *adj = g_list_nth(win->tabs, idx ? idx - 1 : 0);
        win->active = adj ? adj->data : NULL;
    }
    refresh_active(win);
}

static void on_close_click(GtkButton *b, gpointer ud)
{
    HbTabRec *t = ud;
    (void)b;
    hb_window_close_tab(t->win, t->id);
}

void hb_window_set_tab_host(HbWindow *win, const char *id,
                            const char *host_id)
{
    HbTabRec *t = find_tab(win, id);
    if (!t) return;
    g_free(t->host_id);
    t->host_id = g_strdup(host_id);
}

void hb_window_close_host_tabs(HbWindow *win, const char *host_id)
{
    GList *ids = NULL, *l;
    if (!host_id) return;
    /* ws close_host 可能塌/关 tab（panes_changed）→ 先快照 id 再逐个处理 */
    for (l = win->tabs; l; l = l->next)
        ids = g_list_append(ids, g_strdup(((HbTabRec *)l->data)->id));
    for (l = ids; l; l = l->next) {
        HbTabRec *t = find_tab(win, l->data);
        if (!t) continue;
        if (t->page && HB_IS_WORKSPACE(t->page)) {
            hb_workspace_close_host(HB_WORKSPACE(t->page), host_id);
            continue;
        }
        if (t->host_id && !strcmp(t->host_id, host_id))
            hb_window_close_tab(win, t->id);
    }
    g_list_free_full(ids, g_free);
}

void hb_window_set_tab_dot(HbWindow *win, const char *id, const char *dot)
{
    HbTabRec *t = find_tab(win, id);
    static const char *cls[] = { "ok", "err", "share", "pending" };
    int i;
    if (!t) return;
    for (i = 0; i < 4; i++)
        gtk_widget_remove_css_class(t->dot, cls[i]);
    gtk_widget_set_visible(t->dot, dot != NULL);
    if (dot) gtk_widget_add_css_class(t->dot, dot);
}

guint hb_window_set_watch_dot(HbWindow *win, const char *id,
                              const char *dot)
{
    static guint seq;
    HbTabRec *t = find_tab(win, id);
    if (!t) return 0;
    hb_window_set_tab_dot(win, id, dot);
    t->watch_dot = dot != NULL;
    t->watch_token = ++seq;
    return t->watch_token;
}

void hb_window_clear_watch_dot(HbWindow *win, const char *id, guint token)
{
    HbTabRec *t = find_tab(win, id);
    if (t && t->watch_dot && t->watch_token == token) {
        t->watch_dot = FALSE;
        hb_window_set_tab_dot(win, id, NULL);
    }
}

const char *hb_window_active_tab_id(HbWindow *win)
{
    return win->active ? win->active->id : NULL;
}

GtkWidget *hb_window_active_page(HbWindow *win)
{
    return win->active ? win->active->page : NULL;
}

void hb_window_set_tab_title(HbWindow *win, const char *id, const char *title)
{
    HbTabRec *t = find_tab(win, id);
    if (t) gtk_label_set_label(GTK_LABEL(t->title), title);
}

void hb_window_set_tab_icon(HbWindow *win, const char *id, const char *icon)
{
    HbTabRec *t = find_tab(win, id);
    GtkWidget *bb, *c;
    if (!t || !icon || (t->icon && !strcmp(t->icon, icon)))
        return;
    g_free(t->icon);
    t->icon = g_strdup(icon);
    bb = gtk_button_get_child(GTK_BUTTON(t->main));
    for (c = gtk_widget_get_first_child(bb); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (HB_IS_ICON(c)) {
            hb_icon_set_name(HB_ICON(c), icon);
            return;
        }
    }
    /* 原本没有图标：插在 dot 之后 */
    gtk_box_insert_child_after(GTK_BOX(bb), hb_icon_new(icon, 15),
                               t->dot);
}

/* ------------------------------- toast ------------------------------- */

void hb_window_toast(HbWindow *win, const char *text)
{
    AdwToast *t;
    if (!win->toasts || !text)
        return;
    t = adw_toast_new(text);
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(win->toasts), t);
}

void hb_window_toast_undo(HbWindow *win, const char *text,
                          GCallback undo_cb, gpointer ud)
{
    hb_window_toast_action(win, text, "撤销", 9, undo_cb, ud);
}

void hb_window_toast_action(HbWindow *win, const char *text,
                            const char *btn_label, int timeout,
                            GCallback cb, gpointer ud)
{
    hb_window_toast_action2(win, text, btn_label, timeout, cb,
                            NULL, ud);
}

void hb_window_toast_action2(HbWindow *win, const char *text,
                             const char *btn_label, int timeout,
                             GCallback cb, GDestroyNotify destroy,
                             gpointer ud)
{
    AdwToast *t;
    if (!win->toasts || !text)
        return;
    t = adw_toast_new(text);
    adw_toast_set_button_label(t, btn_label);
    adw_toast_set_timeout(t, timeout > 0 ? timeout : 9);
    if (cb)
        g_signal_connect_swapped(t, "button-clicked", cb, ud);
    if (destroy)
        g_signal_connect_swapped(t, "dismissed",
                                 G_CALLBACK(destroy), ud);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(win->toasts), t);
}

void hb_window_add_tab(HbWindow *win, const char *id, const char *title,
                       const char *icon, GtkWidget *page,
                       gboolean closable, gboolean is_term)
{
    HbTabRec *t;
    GtkWidget *btn_box;

    if (find_tab(win, id)) {
        g_object_unref(page);      /* 未 parent 的 widget：drop ref 即销毁 */
        return;
    }
    t = g_new0(HbTabRec, 1);
    t->win = win;
    t->id = g_strdup(id);
    t->icon = g_strdup(icon);
    t->page = page;
    t->closable = closable;
    t->is_term = is_term;

    t->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(t->box, "hb-tab");
    if (is_term)
        gtk_widget_add_css_class(t->box, "term");

    t->main = gtk_button_new();
    gtk_widget_add_css_class(t->main, "hb-tab-main");
    gtk_widget_add_css_class(t->main, "flat");
    btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(btn_box, GTK_ALIGN_CENTER);
    t->dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(t->dot, "hb-tab-dot");
    gtk_widget_set_size_request(t->dot, 7, 7);
    gtk_widget_set_valign(t->dot, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(t->dot, FALSE);
    gtk_box_append(GTK_BOX(btn_box), t->dot);
    if (icon && *icon)
        gtk_box_append(GTK_BOX(btn_box), hb_icon_new(icon, 15));
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(sp, 6, 1);
        gtk_box_append(GTK_BOX(btn_box), sp);
    }
    t->title = gtk_label_new(title);
    gtk_widget_set_valign(t->title, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(btn_box), t->title);
    gtk_button_set_child(GTK_BUTTON(t->main), btn_box);
    g_signal_connect(t->main, "clicked", G_CALLBACK(on_tab_click), t);
    gtk_box_append(GTK_BOX(t->box), t->main);

    if (closable) {
        t->close_btn = gtk_button_new();
        gtk_widget_add_css_class(t->close_btn, "hb-tab-x");
        gtk_widget_set_valign(t->close_btn, GTK_ALIGN_CENTER);
        gtk_button_set_child(GTK_BUTTON(t->close_btn),
                             hb_icon_new("x", 11));
        g_signal_connect(t->close_btn, "clicked",
                         G_CALLBACK(on_close_click), t);
        gtk_box_append(GTK_BOX(t->box), t->close_btn);
    }

    gtk_box_append(GTK_BOX(win->tabstrip), t->box);
    gtk_stack_add_named(GTK_STACK(win->stack), page, id);

    /* 新 tab 立即激活，与原型一致 */
    win->active = t;
    win->tabs = g_list_append(win->tabs, t);
    refresh_active(win);
}

/* ---------------------------- 通知面板 ---------------------------- */

static void update_badge(HbWindow *win)
{
    guint n = hb_notify_unread();
    char buf[16];
    gtk_widget_set_visible(win->badge, n > 0);
    if (n > 0) {
        snprintf(buf, sizeof buf, "%u", n > 99 ? 99 : n);
        gtk_label_set_label(GTK_LABEL(win->badge), buf);
    }
}

static GtkWidget *notif_row(HbNotifItem *n)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *glyph = hb_icon_new(
        !strcmp(hb_notif_item_kind(n), "ok")   ? "check" :
        !strcmp(hb_notif_item_kind(n), "err")  ? "x" :
        !strcmp(hb_notif_item_kind(n), "share")? "broadcast" : "info", 16);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(hb_notif_item_title(n));
    GtkWidget *b = gtk_label_new(hb_notif_item_body(n));
    GtkWidget *tm = gtk_label_new(hb_notif_item_time(n));

    gtk_widget_add_css_class(row, "hb-notif-item");
    if (hb_notif_item_unread(n))
        gtk_widget_add_css_class(row, "unread");
    gtk_widget_add_css_class(ico, "hb-n-ico");
    gtk_widget_add_css_class(ico, hb_notif_item_kind(n));
    gtk_widget_set_size_request(ico, 30, 30);
    gtk_widget_set_halign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(glyph, TRUE);
    gtk_box_append(GTK_BOX(ico), glyph);
    gtk_box_append(GTK_BOX(row), ico);

    gtk_widget_add_css_class(t, "hb-n-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(vb), t);
    gtk_widget_add_css_class(b, "hb-n-body");
    gtk_label_set_xalign(GTK_LABEL(b), 0);
    gtk_label_set_wrap(GTK_LABEL(b), TRUE);
    gtk_box_append(GTK_BOX(vb), b);
    gtk_widget_add_css_class(tm, "hb-n-time");
    gtk_label_set_xalign(GTK_LABEL(tm), 0);
    gtk_box_append(GTK_BOX(vb), tm);
    gtk_widget_set_hexpand(vb, TRUE);
    gtk_box_append(GTK_BOX(row), vb);
    return row;
}

static void rebuild_notif_list(GtkWidget *box)
{
    GListModel *m = hb_notify_model();
    guint i, n = g_list_model_get_n_items(m);
    GtkWidget *ch;

    while ((ch = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), ch);
    if (!n) {
        GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *ic = hb_icon_new("bell", 22);
        GtkWidget *t = gtk_label_new("暂无通知");
        GtkWidget *s = gtk_label_new("连接、任务与同步提醒会出现在这里");
        gtk_widget_add_css_class(empty, "hb-empty");
        gtk_widget_add_css_class(ico, "hb-e-ico");
        gtk_widget_add_css_class(t, "hb-e-title");
        gtk_widget_add_css_class(s, "hb-e-sub");
        gtk_widget_set_size_request(ico, 58, 58);
        gtk_widget_set_halign(ico, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(ic, TRUE);
        gtk_box_append(GTK_BOX(ico), ic);
        gtk_box_append(GTK_BOX(empty), ico);
        gtk_box_append(GTK_BOX(empty), t);
        gtk_box_append(GTK_BOX(empty), s);
        gtk_widget_set_margin_top(empty, 40);
        gtk_widget_set_margin_bottom(empty, 40);
        gtk_box_append(GTK_BOX(box), empty);
        return;
    }
    for (i = 0; i < n; i++) {
        HbNotifItem *it = g_list_model_get_item(m, i);
        gtk_box_append(GTK_BOX(box), notif_row(it));
        g_object_unref(it);
    }
}

static void on_notify_changed(HbNotifications *nn, gpointer ud)
{
    HbWindow *win = ud;
    (void)nn;
    update_badge(win);
    /* 面板打开时列表同步刷新 */
    {
        GtkPopover *pop = gtk_menu_button_get_popover(
            GTK_MENU_BUTTON(win->bell));
        GtkWidget *list = pop ? g_object_get_data(G_OBJECT(pop), "list") : NULL;
        if (list)
            rebuild_notif_list(list);
    }
}

static void on_mark_read(GtkButton *b, gpointer ud)
{ (void)b; (void)ud; hb_notify_mark_all_read(); }
static void on_clear_all(GtkButton *b, gpointer ud)
{ (void)b; (void)ud; hb_notify_clear(); }

static GtkWidget *build_bell(HbWindow *win)
{
    GtkWidget *btn = gtk_menu_button_new();
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = gtk_label_new("通知中心");
    GtkWidget *read = gtk_button_new_with_label("全部已读");
    GtkWidget *clear = gtk_button_new_with_label("清空");
    GtkWidget *sc = gtk_scrolled_window_new();
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *ov = gtk_overlay_new();

    gtk_widget_add_css_class(btn, "hb-icobtn");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), pop);
    gtk_widget_add_css_class(panel, "hb-notif-panel");
    gtk_widget_set_size_request(panel, 380, -1);

    gtk_widget_add_css_class(title, "hb-notif-head");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(read, "hb-btn");
    gtk_widget_add_css_class(read, "ghost");
    gtk_widget_add_css_class(read, "sm");
    gtk_widget_add_css_class(clear, "hb-btn");
    gtk_widget_add_css_class(clear, "ghost");
    gtk_widget_add_css_class(clear, "sm");
    g_signal_connect(read, "clicked", G_CALLBACK(on_mark_read), NULL);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_all), NULL);
    gtk_box_append(GTK_BOX(head), title);
    gtk_box_append(GTK_BOX(head), read);
    gtk_box_append(GTK_BOX(head), clear);
    gtk_box_append(GTK_BOX(panel), head);

    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc), 200);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sc), 460);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(sc), TRUE);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), list);
    gtk_box_append(GTK_BOX(panel), sc);
    gtk_popover_set_child(GTK_POPOVER(pop), panel);
    g_object_set_data(G_OBJECT(pop), "list", list);
    rebuild_notif_list(list);

    /* 铃铛 + 角标 */
    gtk_overlay_set_child(GTK_OVERLAY(ov), hb_icon_new("bell", 17));
    win->badge = gtk_label_new("");
    gtk_widget_add_css_class(win->badge, "hb-badge");
    gtk_widget_set_halign(win->badge, GTK_ALIGN_END);
    gtk_widget_set_valign(win->badge, GTK_ALIGN_START);
    gtk_widget_set_margin_top(win->badge, -3);
    gtk_widget_set_margin_end(win->badge, -5);
    gtk_widget_set_visible(win->badge, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), win->badge);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), ov);
    return btn;
}

/* --------------------------- Local 终端 --------------------------- */

void hb_window_open_local(HbWindow *win, GtkWidget **page_out)
{
    static int seq;
    char id[32];
    GtkWidget *page;

    page = hb_term_page_new_local(hb_store_default());
    g_snprintf(id, sizeof id, "term-local-%d", ++seq);
    hb_window_add_tab(win, id, "Local", "terminal", page, TRUE, TRUE);
    hb_term_page_attach_tab(HB_TERM_PAGE(page), GTK_WIDGET(win), id);
    if (page_out)
        *page_out = page;
}

/* ------------------------- Workspace 分屏 ------------------------- */

static HbTabRec *tab_of_page(HbWindow *win, GtkWidget *page)
{
    for (GList *l = win->tabs; l; l = l->next) {
        HbTabRec *t = l->data;
        if (t->page == page) return t;
    }
    return NULL;
}

void hb_window_split_active(HbWindow *win, GtkOrientation o)
{
    HbTabRec *t = win->active;
    GtkWidget *ws, *page;
    if (!t || !t->page) return;
    if (HB_IS_WORKSPACE(t->page)) {
        if (!hb_workspace_split_focused(HB_WORKSPACE(t->page), o))
            hb_window_toast(win, "最多 6 个分屏");
        return;
    }
    if (!HB_IS_SESSION_PAGE(t->page)) return;   /* Vault 等非终端页 */

    page = t->page;
    /* 记原 tab 外观，塌回时还原 */
    g_free(t->ws_title);
    g_free(t->ws_icon);
    t->ws_title = g_strdup(gtk_label_get_text(GTK_LABEL(t->title)));
    t->ws_icon = g_strdup(t->icon);

    ws = hb_workspace_new(hb_store_default());
    g_object_ref(page);
    gtk_stack_remove(GTK_STACK(win->stack), page);  /* ref 保活跨搬移 */
    gtk_stack_add_named(GTK_STACK(win->stack), ws, t->id);
    t->page = ws;
    t->is_term = TRUE;
    hb_workspace_attach_tab(HB_WORKSPACE(ws), GTK_WIDGET(win), t->id);
    hb_workspace_seed(HB_WORKSPACE(ws), page);
    g_object_unref(page);
    hb_workspace_split_focused(HB_WORKSPACE(ws), o); /* 不会失败：<6 */
    hb_window_set_tab_title(win, t->id, "Workspace");
    hb_window_set_tab_icon(win, t->id, "cols");
    refresh_active(win);
}

static void collapse_workspace(HbWindow *win, HbTabRec *t)
{
    GtkWidget *page =
        hb_workspace_take_last_page(HB_WORKSPACE(t->page));
    if (!page) return;
    gtk_stack_remove(GTK_STACK(win->stack), t->page);   /* 销毁 ws 壳 */
    gtk_stack_add_named(GTK_STACK(win->stack), page, t->id);
    g_object_unref(page);
    t->page = page;
    if (t->ws_title) {
        gtk_label_set_label(GTK_LABEL(t->title), t->ws_title);
        g_clear_pointer(&t->ws_title, g_free);
    }
    if (t->ws_icon) {
        g_free(t->icon);
        t->icon = NULL;                 /* 绕开 set_tab_icon 的早退 */
        hb_window_set_tab_icon(win, t->id, t->ws_icon);
        g_clear_pointer(&t->ws_icon, g_free);
    }
    /* pane 在 ws 内不写 tab host → 塌回时补回关联（删除主机用） */
    hb_window_set_tab_host(win, t->id,
        hb_session_page_host_id(HB_SESSION_PAGE(page)));
    refresh_active(win);
}

void hb_window_workspace_panes_changed(HbWindow *win, GtkWidget *ws)
{
    HbTabRec *t = tab_of_page(win, ws);
    guint n;
    if (!t) return;
    n = hb_workspace_pane_count(HB_WORKSPACE(ws));
    if (n == 0)
        hb_window_close_tab(win, t->id);
    else if (n == 1)
        collapse_workspace(win, t);
}

void hb_window_close_pane_or_tab(HbWindow *win)
{
    HbTabRec *t = win->active;
    if (!t) return;
    if (t->page && HB_IS_WORKSPACE(t->page) &&
        hb_workspace_pane_count(HB_WORKSPACE(t->page)) > 1) {
        hb_workspace_close_focused(HB_WORKSPACE(t->page));
        return;
    }
    hb_window_close_tab(win, t->id);
}

GtkWidget *hb_window_active_session_page(HbWindow *win)
{
    return hb_workspace_resolve_page(hb_window_active_page(win));
}

void hb_window_move_pane_focus(HbWindow *win, int dir)
{
    HbTabRec *t = win->active;
    if (t && t->page && HB_IS_WORKSPACE(t->page))
        hb_workspace_move_focus(HB_WORKSPACE(t->page), dir);
}

void hb_window_toggle_focus_mode(HbWindow *win)
{
    HbTabRec *t = win->active;
    if (t && t->page && HB_IS_WORKSPACE(t->page))
        hb_workspace_toggle_focus_mode(HB_WORKSPACE(t->page));
}

void hb_window_open_workspace(HbWindow *win, GList *pages)
{
    static int seq;
    char id[32];
    GtkWidget *ws;
    GList *l;
    int i = 0, n;
    if (!pages) return;
    n = (int)g_list_length(pages);
    ws = hb_workspace_new(hb_store_default());
    g_snprintf(id, sizeof id, "ws-%d", ++seq);
    for (l = pages; l; l = l->next, i++) {
        if (i == 0)
            hb_workspace_seed(HB_WORKSPACE(ws), l->data);
        else
            /* 并排一行：paned 链向右，第 i 个 leaf 占 1/(n-i+1) */
            hb_workspace_add_pane(HB_WORKSPACE(ws), l->data,
                GTK_ORIENTATION_HORIZONTAL, 1.0 / (n - i + 1));
    }
    hb_window_add_tab(win, id, "Workspace", "cols", ws, TRUE, TRUE);
    hb_workspace_attach_tab(HB_WORKSPACE(ws), GTK_WIDGET(win), id);
}

static void on_plus_local(GtkButton *b, gpointer ud)
{
    HbWindow *win = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    hb_window_open_local(win, NULL);
}

static void on_plus_workspace(GtkButton *b, gpointer ud)
{
    HbWindow *win = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    hb_workspace_pick_targets(GTK_WIDGET(win), hb_store_default());
}

static GtkWidget *build_plus(HbWindow *win)
{
    GtkWidget *mb = gtk_menu_button_new();
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_add_css_class(mb, "hb-icobtn");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), hb_icon_new("plus", 16));
    gtk_widget_set_tooltip_text(mb, "新建标签页");

    {
        GtkWidget *kbd = gtk_label_new("Ctrl+Shift+T");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
        GtkWidget *name = gtk_label_new("Local 终端");
        GtkWidget *btn;
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_add_css_class(kbd, "hb-kbd");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), kbd);
        btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "hb-menu-item");
        gtk_button_set_child(GTK_BUTTON(btn), row);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_plus_local), win);
        gtk_box_append(GTK_BOX(vb), btn);
    }
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
        GtkWidget *name = gtk_label_new("Workspace 分屏…");
        GtkWidget *btn;
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(row), name);
        btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "hb-menu-item");
        gtk_button_set_child(GTK_BUTTON(btn), row);
        g_signal_connect(btn, "clicked",
                         G_CALLBACK(on_plus_workspace), win);
        gtk_box_append(GTK_BOX(vb), btn);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), vb);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    return mb;
}

/* ------------------------------ 窗口 ------------------------------ */

static void on_sidebar_toggle(GtkButton *b, gpointer ud)
{
    HbWindow *win = ud;
    (void)b;
    if (win->vault_page)
        hb_vault_page_toggle_sidebar(HB_VAULT_PAGE(win->vault_page));
}

void hb_window_set_vault_page(HbWindow *win, GtkWidget *vault_page)
{
    win->vault_page = vault_page;
}

GtkWidget *hb_window_vault_page(HbWindow *win)
{
    return win->vault_page;
}

void hb_window_overlay_add(HbWindow *win, GtkWidget *w)
{
    gtk_overlay_add_overlay(GTK_OVERLAY(win->overlay), w);
}

static void hb_window_dispose(GObject *o)
{
    HbWindow *win = HB_WINDOW(o);
    g_list_free_full(win->tabs, (GDestroyNotify)free_tab_rec);
    win->tabs = NULL;
    G_OBJECT_CLASS(hb_window_parent_class)->dispose(o);
}

static void hb_window_class_init(HbWindowClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_window_dispose;
    sigs[SIG_TAB_CLOSED] = g_signal_new("tab-closed", HB_TYPE_WINDOW,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void hb_window_init(HbWindow *win)
{
    GtkWidget *titlebar, *handle, *scroller, *side, *plus, *wctl;

    gtk_window_set_default_size(GTK_WINDOW(win), 1400, 870);
    gtk_widget_set_size_request(GTK_WIDGET(win), 960, 600);

    /* ---- titlebar：AdwApplicationWindow 用 AdwToolbarView 承载自定义顶栏 ---- */
    {
        GtkWidget *tv = adw_toolbar_view_new();
        handle = gtk_window_handle_new();
        titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(titlebar, "hb-titlebar");
        gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), titlebar);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), handle);
        win->stack = gtk_stack_new();
        gtk_widget_add_css_class(win->stack, "hb-content");
        gtk_stack_set_transition_type(GTK_STACK(win->stack),
                                      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), win->stack);
        win->overlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(win->overlay), tv);
        win->toasts = adw_toast_overlay_new();
        adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(win->toasts),
                                    win->overlay);
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(win),
                                           win->toasts);
    }

    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_widget_set_hexpand(scroller, TRUE);
    win->tabstrip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(win->tabstrip, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                  win->tabstrip);
    gtk_box_append(GTK_BOX(titlebar), scroller);

    plus = build_plus(win);
    gtk_box_append(GTK_BOX(titlebar), plus);

    side = gtk_button_new();
    gtk_widget_add_css_class(side, "hb-icobtn");
    gtk_button_set_child(GTK_BUTTON(side), hb_icon_new("panel", 16));
    gtk_widget_set_tooltip_text(side, "切换侧栏");
    g_signal_connect(side, "clicked", G_CALLBACK(on_sidebar_toggle), win);
    gtk_box_append(GTK_BOX(titlebar), side);

    win->bell = build_bell(win);
    gtk_box_append(GTK_BOX(titlebar), win->bell);

    wctl = gtk_window_controls_new(GTK_PACK_END);
    gtk_box_append(GTK_BOX(titlebar), wctl);


    hb_shortcuts_install(win);

    g_signal_connect(hb_notify_default(), "changed",
                     G_CALLBACK(on_notify_changed), win);
}

HbWindow *hb_window_new(GtkApplication *app)
{
    return g_object_new(HB_TYPE_WINDOW, "application", app, NULL);
}
