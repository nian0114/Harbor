#include "hb-connect-view.h"
#include "hb-icons.h"

#include <harbor/ssh.h>
#include <string.h>
#include <glib/gi18n.h>

/* 连接视图（从 hb-ssh-page 抽出，纯展示，行为原样）：
 * avatar 头 + 3 节点 stepper + 阶段日志 + 错误卡 + Close/Start over。 */

struct _HbConnectView {
    GtkBox parent;

    GtkWidget *av;
    char      *av_key;
    GtkWidget *nm;
    GtkWidget *sub;

    GtkWidget *node[3];
    GtkWidget *bar[2];
    GtkWidget *log_box;
    GtkWidget *log_scroll;
    GtkWidget *hide_btn;
    gboolean   hide_log;
    int        cur_stage;

    GtkWidget *err_card;
    GtkWidget *foot;
    gboolean   show_edit;
};

G_DEFINE_TYPE(HbConnectView, hb_connect_view, GTK_TYPE_BOX)

enum { SIG_CLOSE, SIG_START_OVER, SIG_EDIT, N_SIGS };
static guint sigs[N_SIGS];

/* ------------------------- 节点 ------------------------- */

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

static void update_nodes(HbConnectView *v)
{
    int cur = node_of(v->cur_stage), i;
    for (i = 0; i < 3; i++) {
        if (i < cur)       set_node(v->node[i], "done");
        else if (i == cur) set_node(v->node[i], "active spin");
        else               set_node(v->node[i], "pending");
    }
    for (i = 0; i < 2; i++) {
        gtk_widget_remove_css_class(v->bar[i], "pending");
        gtk_widget_remove_css_class(v->bar[i], "done");
        gtk_widget_add_css_class(v->bar[i],
                               i < cur ? "done" : "pending");
    }
}

static void add_log_line(HbConnectView *v, const char *text, gboolean err)
{
    GtkWidget *row, *l;
    if (!v->log_box) return;
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
    gtk_box_append(GTK_BOX(v->log_box), row);
    {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(v->log_scroll));
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
    }
}

/* ------------------------- 错误卡 ------------------------- */

static void on_err_close(GtkButton *b, gpointer ud)
{ (void)b; g_signal_emit(ud, sigs[SIG_CLOSE], 0); }
static void on_err_retry(GtkButton *b, gpointer ud)
{ (void)b; g_signal_emit(ud, sigs[SIG_START_OVER], 0); }
static void on_err_edit(GtkButton *b, gpointer ud)
{ (void)b; g_signal_emit(ud, sigs[SIG_EDIT], 0); }

void hb_connect_view_error(HbConnectView *v, const char *msg,
                           const char *detail, int code)
{
    GtkWidget *card, *title, *foot;
    char *markup;

    if (v->err_card) {
        gtk_widget_unparent(v->err_card);
        v->err_card = NULL;
    }
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card, "connect-errcard");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 430, -1);

    markup = g_markup_printf_escaped("<b>%s</b>",
                                     msg ? msg : _("连接失败"));
    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(card), title);

    if (code == HB_SSH_ERR_HOSTKEY_CHANGED) {
        GtkWidget *w = gtk_label_new(
            _("为安全起见已阻断连接 —— 主机密钥与本地记录不符。"));
        gtk_widget_add_css_class(w, "hb-red");
        gtk_label_set_wrap(GTK_LABEL(w), TRUE);
        gtk_label_set_xalign(GTK_LABEL(w), 0);
        gtk_box_append(GTK_BOX(card), w);
    }
    if (detail && *detail) {
        GtkWidget *exp = gtk_expander_new(_("技术详情"));
        GtkWidget *dl = gtk_label_new(detail);
        gtk_label_set_wrap(GTK_LABEL(dl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(dl), 0);
        gtk_expander_set_child(GTK_EXPANDER(exp), dl);
        gtk_widget_add_css_class(exp, "hb-dim");
        gtk_box_append(GTK_BOX(card), exp);
    }

    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    {
        GtkWidget *c = gtk_button_new_with_label(_("Close"));
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect(c, "clicked", G_CALLBACK(on_err_close), v);
        gtk_box_append(GTK_BOX(foot), c);
    }
    if (v->show_edit) {
        GtkWidget *e = gtk_button_new_with_label(_("编辑主机"));
        gtk_widget_add_css_class(e, "hb-btn");
        g_signal_connect(e, "clicked", G_CALLBACK(on_err_edit), v);
        gtk_box_append(GTK_BOX(foot), e);
    }
    {
        GtkWidget *r = gtk_button_new_with_label(_("重试"));
        gtk_widget_add_css_class(r, "hb-btn");
        gtk_widget_add_css_class(r, "primary");
        g_signal_connect(r, "clicked", G_CALLBACK(on_err_retry), v);
        gtk_box_append(GTK_BOX(foot), r);
    }
    gtk_box_append(GTK_BOX(card), foot);

    /* 插到日志区后（footer 之前） */
    gtk_box_insert_child_after(GTK_BOX(v), card, v->log_scroll);
    v->err_card = card;
    gtk_widget_set_visible(v->hide_btn, FALSE);
    gtk_widget_set_visible(v->foot, FALSE);
}

void hb_connect_view_clear_error(HbConnectView *v)
{
    if (v->err_card) {
        gtk_widget_unparent(v->err_card);
        v->err_card = NULL;
    }
}

void hb_connect_view_reset(HbConnectView *v)
{
    GtkWidget *ch;
    hb_connect_view_clear_error(v);
    v->cur_stage = -1;
    update_nodes(v);
    while ((ch = gtk_widget_get_first_child(v->log_box)))
        gtk_widget_unparent(ch);
    gtk_widget_set_visible(v->hide_btn, TRUE);
    gtk_widget_set_visible(v->foot, TRUE);
}

void hb_connect_view_stage(HbConnectView *v, int idx,
                           const char *text, char mark)
{
    add_log_line(v, text ? text : "", mark == 'e');
    if (idx > v->cur_stage) {
        v->cur_stage = idx;
        update_nodes(v);
    }
}

void hb_connect_view_set_edit_btn(HbConnectView *v, gboolean show)
{
    v->show_edit = show;
}

void hb_connect_view_set_head(HbConnectView *v, const char *avatar_key,
                              const char *name, const char *sub)
{
    if (avatar_key && *avatar_key) {
        g_free(v->av_key);
        v->av_key = g_strdup(avatar_key);
        hb_os_avatar_set_key(v->av, avatar_key);
    }
    gtk_label_set_label(GTK_LABEL(v->nm), name ? name : "");
    gtk_label_set_label(GTK_LABEL(v->sub), sub ? sub : "");
}

/* ------------------------- 构建 ------------------------- */

static void on_hide_log(GtkButton *b, gpointer ud)
{
    HbConnectView *v = ud;
    v->hide_log = !v->hide_log;
    gtk_button_set_label(b, v->hide_log ? _("Show logs") : _("Hide logs"));
    gtk_widget_set_visible(v->log_scroll, !v->hide_log);
}

static void on_foot_close(GtkButton *b, gpointer ud)
{ (void)b; g_signal_emit(ud, sigs[SIG_CLOSE], 0); }
static void on_foot_retry(GtkButton *b, gpointer ud)
{ (void)b; g_signal_emit(ud, sigs[SIG_START_OVER], 0); }

static void hb_connect_view_init(HbConnectView *v)
{
    GtkWidget *head, *txtcol, *track, *foot;
    int i;
    static const char *node_icons[] = { "link", "shield", "terminal" };

    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(v), "connect-wrap");
    gtk_widget_set_vexpand(GTK_WIDGET(v), TRUE);
    v->cur_stage = -1;

    /* head：avatar + name + sub + hide-logs */
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(head, "connect-head");
    v->av = hb_os_avatar_new("generic", 38);
    gtk_box_append(GTK_BOX(head), v->av);
    txtcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    v->nm = gtk_label_new("");
    gtk_widget_add_css_class(v->nm, "connect-name");
    gtk_label_set_xalign(GTK_LABEL(v->nm), 0);
    gtk_box_append(GTK_BOX(txtcol), v->nm);
    v->sub = gtk_label_new("");
    gtk_widget_add_css_class(v->sub, "connect-sub");
    gtk_label_set_xalign(GTK_LABEL(v->sub), 0);
    gtk_box_append(GTK_BOX(txtcol), v->sub);
    gtk_box_append(GTK_BOX(head), txtcol);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(head), sp);
    }
    v->hide_btn = gtk_button_new_with_label(_("Hide logs"));
    gtk_widget_add_css_class(v->hide_btn, "hb-btn");
    gtk_widget_add_css_class(v->hide_btn, "sm");
    gtk_widget_set_valign(v->hide_btn, GTK_ALIGN_CENTER);
    g_signal_connect(v->hide_btn, "clicked",
                     G_CALLBACK(on_hide_log), v);
    gtk_box_append(GTK_BOX(head), v->hide_btn);
    gtk_box_append(GTK_BOX(v), head);

    /* progress track */
    track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(track, "progress-track");
    gtk_widget_set_halign(track, GTK_ALIGN_CENTER);
    for (i = 0; i < 3; i++) {
        v->node[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(v->node[i], "node");
        gtk_widget_add_css_class(v->node[i], "pending");
        gtk_box_append(GTK_BOX(v->node[i]),
                       hb_icon_new(node_icons[i], 12));
        gtk_widget_set_size_request(v->node[i], 22, 22);
        gtk_box_append(GTK_BOX(track), v->node[i]);
        if (i < 2) {
            v->bar[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(v->bar[i], "bar");
            gtk_widget_add_css_class(v->bar[i], "pending");
            gtk_widget_set_size_request(v->bar[i], -1, 3);
            gtk_widget_set_hexpand(v->bar[i], TRUE);
            gtk_widget_set_valign(v->bar[i], GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(track), v->bar[i]);
        }
    }
    gtk_widget_set_size_request(track, 430, -1);
    gtk_box_append(GTK_BOX(v), track);

    /* 日志区 */
    v->log_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(v->log_scroll, "connect-log");
    gtk_widget_set_size_request(v->log_scroll, 430, 220);
    gtk_widget_set_halign(v->log_scroll, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(v->log_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    v->log_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(v->log_box, 16);
    gtk_widget_set_margin_end(v->log_box, 16);
    gtk_widget_set_margin_top(v->log_box, 14);
    gtk_widget_set_margin_bottom(v->log_box, 14);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(v->log_scroll),
                                  v->log_box);
    gtk_box_append(GTK_BOX(v), v->log_scroll);

    /* footer（错误卡显示时隐藏） */
    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    v->foot = foot;
    gtk_widget_set_halign(foot, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(foot, 18);
    {
        GtkWidget *c = gtk_button_new_with_label(_("Close"));
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect(c, "clicked", G_CALLBACK(on_foot_close), v);
        gtk_box_append(GTK_BOX(foot), c);
    }
    {
        GtkWidget *r = gtk_button_new_with_label(_("Start over"));
        gtk_widget_add_css_class(r, "hb-btn");
        gtk_widget_add_css_class(r, "primary");
        g_signal_connect(r, "clicked", G_CALLBACK(on_foot_retry), v);
        gtk_box_append(GTK_BOX(foot), r);
    }
    gtk_box_append(GTK_BOX(v), foot);

    update_nodes(v);
}

static void hb_connect_view_finalize(GObject *o)
{
    HbConnectView *v = HB_CONNECT_VIEW(o);
    g_free(v->av_key);
    G_OBJECT_CLASS(hb_connect_view_parent_class)->finalize(o);
}

static void hb_connect_view_class_init(HbConnectViewClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->finalize = hb_connect_view_finalize;
    sigs[SIG_CLOSE] = g_signal_new("close", HB_TYPE_CONNECT_VIEW,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    sigs[SIG_START_OVER] = g_signal_new("start-over",
        HB_TYPE_CONNECT_VIEW, G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        NULL, G_TYPE_NONE, 0);
    sigs[SIG_EDIT] = g_signal_new("edit-host", HB_TYPE_CONNECT_VIEW,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

GtkWidget *hb_connect_view_new(void)
{
    return g_object_new(HB_TYPE_CONNECT_VIEW, NULL);
}
