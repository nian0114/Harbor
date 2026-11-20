#include "hb-window.h"
#include "hb-icons.h"
#include "hb-notify.h"
#include "hb-vault-page.h"
#include "hb-term-page.h"
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
} HbTabRec;

struct _HbWindow {
    AdwApplicationWindow parent;
    GtkWidget *tabstrip;     /* hbox inside scroller */
    GtkWidget *stack;
    GtkWidget *toasts;       /* AdwToastOverlay 包住全部内容 */
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
    g_free(t);
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
    for (l = win->tabs; l; l = l->next) {
        HbTabRec *t = l->data;
        if (t->host_id && !strcmp(t->host_id, host_id))
            ids = g_list_prepend(ids, g_strdup(t->id));
    }
    for (l = ids; l; l = l->next)
        hb_window_close_tab(win, l->data);
    g_list_free_full(ids, g_free);
}

void hb_window_set_tab_dot(HbWindow *win, const char *id, const char *dot)
{
    HbTabRec *t = find_tab(win, id);
    static const char *cls[] = { "ok", "err", "share" };
    int i;
    if (!t) return;
    for (i = 0; i < 3; i++)
        gtk_widget_remove_css_class(t->dot, cls[i]);
    gtk_widget_set_visible(t->dot, dot != NULL);
    if (dot) gtk_widget_add_css_class(t->dot, dot);
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
    if (page_out)
        *page_out = page;
}

static void on_plus_local(GtkButton *b, gpointer ud)
{
    HbWindow *win = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    hb_window_open_local(win, NULL);
}

static GtkWidget *build_plus(HbWindow *win)
{
    GtkWidget *mb = gtk_menu_button_new();
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *item = gtk_button_new_with_label("Local 终端");
    GtkWidget *kbd = gtk_label_new("Ctrl+Shift+T");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    GtkWidget *name = gtk_label_new("Local 终端");

    (void)item;
    gtk_widget_add_css_class(mb, "hb-icobtn");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), hb_icon_new("plus", 16));
    gtk_widget_set_tooltip_text(mb, "新建标签页");

    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_add_css_class(kbd, "hb-kbd");
    gtk_box_append(GTK_BOX(row), name);
    gtk_box_append(GTK_BOX(row), kbd);
    {
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "hb-menu-item");
        gtk_button_set_child(GTK_BUTTON(btn), row);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_plus_local), win);
        gtk_popover_set_child(GTK_POPOVER(pop), btn);
    }
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    return mb;
}

/* ------------------------------ 快捷键 ------------------------------ */

static gboolean on_key(GtkEventControllerKey *kc, guint keyval,
                       guint code, GdkModifierType state, gpointer ud)
{
    HbWindow *win = ud;
    guint n = g_list_length(win->tabs);
    (void)kc; (void)code;
    if (!(state & GDK_CONTROL_MASK))
        return FALSE;
    if ((state & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_T || keyval == GDK_KEY_t)) {
        hb_window_open_local(win, NULL);
        return TRUE;
    }
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        guint i = win->active
            ? g_list_index(win->tabs, win->active) : 0;
        int dir = (state & GDK_SHIFT_MASK) ? -1 : 1;
        guint ni = (guint)(((int)i + dir + (int)n) % (int)(n ? n : 1));
        GList *l = g_list_nth(win->tabs, ni);
        if (l) hb_window_switch_tab(win, ((HbTabRec *)l->data)->id);
        return TRUE;
    }
    if (keyval == GDK_KEY_w || keyval == GDK_KEY_W) {
        if (win->active && win->active->closable)
            hb_window_close_tab(win, win->active->id);
        return TRUE;
    }
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
        GList *l = g_list_nth(win->tabs, keyval - GDK_KEY_1);
        if (l) hb_window_switch_tab(win, ((HbTabRec *)l->data)->id);
        return TRUE;
    }
    return FALSE;
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
    GtkEventController *key;

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
        win->toasts = adw_toast_overlay_new();
        adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(win->toasts), tv);
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


    key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), win);
    gtk_widget_add_controller(GTK_WIDGET(win), key);

    g_signal_connect(hb_notify_default(), "changed",
                     G_CALLBACK(on_notify_changed), win);
}

HbWindow *hb_window_new(GtkApplication *app)
{
    return g_object_new(HB_TYPE_WINDOW, "application", app, NULL);
}
