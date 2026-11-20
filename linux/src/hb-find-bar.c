#include "hb-find-bar.h"
#include "hb-icons.h"

#include <adwaita.h>

struct _HbFindBar {
    GtkBox parent;

    HbTerminal *term;        /* 绑定的终端（弱引用） */
    gulong      find_sig;

    GtkWidget *entry;
    GtkWidget *count;
};

G_DEFINE_TYPE(HbFindBar, hb_find_bar, GTK_TYPE_BOX)

static void update_count(HbFindBar *b)
{
    guint cur = 0, total = 0;
    char s[32];
    if (b->term)
        hb_terminal_find_counts(b->term, &cur, &total);
    g_snprintf(s, sizeof s, "%u/%u", total ? cur : 0, total);
    gtk_label_set_label(GTK_LABEL(b->count), s);
}

static void on_find_changed(HbTerminal *t, gpointer ud)
{
    (void)t;
    update_count(ud);
}

static void on_search_changed(GtkEditable *e, gpointer ud)
{
    HbFindBar *b = ud;
    if (b->term)
        hb_terminal_find_set(b->term,
            gtk_editable_get_text(e), FALSE);
    update_count(b);
}

static void on_step_btn(GtkButton *btn, gpointer ud)
{
    HbFindBar *b = ud;
    int dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "dir"));
    if (b->term)
        hb_terminal_find_step(b->term, dir);
    update_count(b);
}

void hb_find_bar_close(HbFindBar *b)
{
    if (!gtk_widget_get_visible(GTK_WIDGET(b)))
        return;
    gtk_widget_set_visible(GTK_WIDGET(b), FALSE);
    if (b->term) {
        hb_terminal_find_set(b->term, NULL, FALSE);   /* 清高亮 */
        gtk_widget_grab_focus(GTK_WIDGET(b->term));
    }
}

static gboolean on_entry_key(GtkEventControllerKey *kc, guint keyval,
                             guint code, GdkModifierType state, gpointer ud)
{
    HbFindBar *b = ud;
    (void)kc; (void)code;
    if (keyval == GDK_KEY_Escape) {
        hb_find_bar_close(b);                 /* 不向下/会话传播 */
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
        keyval == GDK_KEY_ISO_Enter) {
        if (b->term)
            hb_terminal_find_step(b->term,
                (state & GDK_SHIFT_MASK) ? -1 : 1);
        update_count(b);
        return TRUE;
    }
    return FALSE;
}

static void on_close_btn(GtkButton *btn, gpointer ud)
{
    (void)btn;
    hb_find_bar_close(ud);
}

static void term_weak(gpointer data, GObject *gone)
{
    HbFindBar *b = data;
    (void)gone;
    b->term = NULL;
    b->find_sig = 0;
    update_count(b);
}

void hb_find_bar_bind(HbFindBar *b, HbTerminal *t)
{
    if (b->term) {
        g_signal_handler_disconnect(b->term, b->find_sig);
        g_object_weak_unref(G_OBJECT(b->term), term_weak, b);
    }
    b->term = t;
    b->find_sig = 0;
    if (t) {
        b->find_sig = g_signal_connect(t, "find-changed",
            G_CALLBACK(on_find_changed), b);
        g_object_weak_ref(G_OBJECT(t), term_weak, b);
    }
    update_count(b);
}

void hb_find_bar_open(HbFindBar *b)
{
    gtk_widget_set_visible(GTK_WIDGET(b), TRUE);
    gtk_editable_select_region(GTK_EDITABLE(b->entry), 0, -1);
    gtk_widget_grab_focus(b->entry);
}

gboolean hb_find_bar_is_open(HbFindBar *b)
{
    return gtk_widget_get_visible(GTK_WIDGET(b));
}

static void hb_find_bar_dispose(GObject *o)
{
    HbFindBar *b = HB_FIND_BAR(o);
    hb_find_bar_bind(b, NULL);
    G_OBJECT_CLASS(hb_find_bar_parent_class)->dispose(o);
}

static void hb_find_bar_class_init(HbFindBarClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_find_bar_dispose;
}

static GtkWidget *icon_btn(const char *icon, const char *tip,
                           GCallback cb, HbFindBar *b, int dir)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "icon-btn");
    gtk_widget_set_tooltip_text(btn, tip);
    gtk_button_set_child(GTK_BUTTON(btn), hb_icon_new(icon, 14));
    if (dir)
        g_object_set_data(G_OBJECT(btn), "dir", GINT_TO_POINTER(dir));
    g_signal_connect(btn, "clicked", cb, b);
    return btn;
}

static void hb_find_bar_init(HbFindBar *b)
{
    GtkEventController *kc;
    GtkWidget *clamp, *up, *dn, *x;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(b),
                                   GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing(GTK_BOX(b), 7);
    gtk_widget_add_css_class(GTK_WIDGET(b), "find-bar");
    gtk_widget_set_visible(GTK_WIDGET(b), FALSE);

    gtk_box_append(GTK_BOX(b), hb_icon_new("search", 14));

    b->entry = gtk_entry_new();
    gtk_widget_set_hexpand(b->entry, TRUE);
    g_object_set(b->entry, "placeholder-text",
                 "在 scrollback 中搜索…", NULL);
    g_signal_connect(b->entry, "changed",
                     G_CALLBACK(on_search_changed), b);
    /* capture：entry 内部会先吃 Return/Escape，bubble 等不到 */
    kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_entry_key), b);
    gtk_widget_add_controller(b->entry, kc);
    /* 原型 flex:1 max-width:340px —— GTK 没有 max-width，用 AdwClamp */
    clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 340);
    gtk_widget_set_hexpand(clamp, TRUE);
    adw_clamp_set_child(ADW_CLAMP(clamp), b->entry);
    gtk_box_append(GTK_BOX(b), clamp);

    b->count = gtk_label_new("0/0");
    gtk_widget_add_css_class(b->count, "f-count");
    gtk_box_append(GTK_BOX(b), b->count);

    up = icon_btn("up", "上一个 (Shift+Enter)",
                  G_CALLBACK(on_step_btn), b, -1);
    dn = icon_btn("down", "下一个 (Enter)",
                  G_CALLBACK(on_step_btn), b, 1);
    x  = icon_btn("x", "关闭 (Esc)", G_CALLBACK(on_close_btn), b, 0);
    gtk_box_append(GTK_BOX(b), up);
    gtk_box_append(GTK_BOX(b), dn);
    gtk_box_append(GTK_BOX(b), x);
}

GtkWidget *hb_find_bar_new(void)
{
    return g_object_new(HB_TYPE_FIND_BAR, NULL);
}
