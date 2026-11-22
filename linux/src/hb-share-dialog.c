/* FR-12 owner 共享对话框（prototype openShareModal）。 */

#include "hb-share-dialog.h"
#include "hb-share.h"
#include "hb-sync.h"
#include "hb-icons.h"
#include "hb-vault-page.h"
#include "hb-window.h"

#include <string.h>

struct _HbShareDialog {
    AdwDialog    parent;

    hb_session  *sess;          /* borrowed */
    char        *title, *host;
    char        *share_id;      /* owned；active 或 starting */
    gboolean     starting;

    GtkWidget   *sw;            /* 开启云端共享 */
    GtkWidget   *mode_dd;       /* 参与权限 */
    GtkWidget   *body;          /* 共享信息区（激活后可见） */
    GtkWidget   *link_row;
    GtkWidget   *code_lbl;
    GtkWidget   *plist;         /* 参与者列表 */
    GtkWidget   *empty_lbl;
    GtkWidget   *stop_btn;
    GtkWidget   *signedout;     /* 未登录提示 */
    gulong       sig_changed, sig_event;
    gboolean     updating;
};

#define HB_TYPE_SHARE_DIALOG (hb_share_dialog_get_type())
G_DECLARE_FINAL_TYPE(HbShareDialog, hb_share_dialog, HB, SHARE_DIALOG,
                     AdwDialog)
G_DEFINE_TYPE(HbShareDialog, hb_share_dialog, ADW_TYPE_DIALOG)

static void refresh(HbShareDialog *d);
static void refresh_plist(HbShareDialog *d);

static void on_kick_btn(GtkButton *b, HbShareDialog *d)
{
    if (!d->share_id) return;
    hb_share_service_kick(hb_share_service_default(), d->share_id,
        (uint32_t)GPOINTER_TO_UINT(
            g_object_get_data(G_OBJECT(b), "pid")));
}

static void on_goto_account(GtkButton *b, HbShareDialog *d)
{
    GtkRoot *r;
    GtkWidget *vp;
    (void)b;
    r = gtk_widget_get_root(GTK_WIDGET(d));
    if (HB_IS_WINDOW(r) &&
        (vp = hb_window_vault_page(HB_WINDOW(r))))
        hb_vault_page_set_section(HB_VAULT_PAGE(vp), "account");
    adw_dialog_close(ADW_DIALOG(d));
}

/* --------------------------- 参与者列表 --------------------------- */
/* owner 引擎事件驱动：JOINED/LEFT → refresh_plist 重建。
 * 引擎没有"列举参与者"API，所以对话自己维护 id→{name,perm} 表。 */
typedef struct {
    uint32_t id;
    char    *name;
    char    *device;
    int      perm;              /* HB_SHARE_*；guest pake_ok 后按 mode */
    gboolean guest;
} PartRow;

static void free_prow(gpointer p)
{
    PartRow *r = p;
    g_free(r->name); g_free(r->device); g_free(r);
}

static void refresh_plist(HbShareDialog *d)
{
    GtkWidget *ch;
    while ((ch = gtk_widget_get_first_child(d->plist)))
        gtk_box_remove(GTK_BOX(d->plist), ch);
    {
        /* 从 g_object_data 的 GList 重建 */
        GList *l = g_object_get_data(G_OBJECT(d), "parts");
        guint n = 0;
        for (; l; l = l->next) {
            PartRow *r = l->data;
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *nm, *pill, *rm;
            char *label;
            n++;
            label = g_strdup_printf("%s%s%s", r->name ? r->name : "",
                r->device && *r->device ? " · " : "",
                r->device ? r->device : "");
            nm = gtk_label_new(label);
            g_free(label);
            gtk_label_set_xalign(GTK_LABEL(nm), 0);
            gtk_widget_set_hexpand(nm, TRUE);
            gtk_box_append(GTK_BOX(row), nm);
            pill = gtk_label_new(r->perm == HB_SHARE_COLLAB
                                 ? "可输入" : "只读");
            gtk_widget_add_css_class(pill, "hb-share-pill");
            if (r->perm == HB_SHARE_COLLAB)
                gtk_widget_add_css_class(pill, "collab");
            gtk_box_append(GTK_BOX(row), pill);
            rm = gtk_button_new_with_label("移除");
            gtk_widget_add_css_class(rm, "tbtn");
            g_object_set_data(G_OBJECT(rm), "pid",
                              GUINT_TO_POINTER(r->id));
            g_signal_connect(rm, "clicked",
                             G_CALLBACK(on_kick_btn), d);
            gtk_box_append(GTK_BOX(row), rm);
            gtk_box_append(GTK_BOX(d->plist), row);
        }
        gtk_widget_set_visible(d->empty_lbl, n == 0);
    }
}

static void add_part(HbShareDialog *d, uint32_t id, const char *name,
                     const char *device, int perm, gboolean guest)
{
    GList *l = g_object_get_data(G_OBJECT(d), "parts");
    GList *it;
    for (it = l; it; it = it->next) {
        PartRow *r = it->data;
        if (r->id == id) {
            g_free(r->name); r->name = g_strdup(name);
            g_free(r->device); r->device = g_strdup(device);
            r->perm = perm; r->guest = guest;
            refresh_plist(d);
            return;
        }
    }
    {
        PartRow *r = g_new0(PartRow, 1);
        r->id = id;
        r->name = g_strdup(name);
        r->device = g_strdup(device);
        r->perm = perm; r->guest = guest;
        l = g_list_append(l, r);
        g_object_set_data(G_OBJECT(d), "parts", l);
    }
    refresh_plist(d);
}

static void del_part(HbShareDialog *d, uint32_t id)
{
    GList *l = g_object_get_data(G_OBJECT(d), "parts");
    GList *it;
    for (it = l; it; it = it->next) {
        PartRow *r = it->data;
        if (r->id == id) {
            l = g_list_delete_link(l, it);
            free_prow(r);
            break;
        }
    }
    g_object_set_data(G_OBJECT(d), "parts", l);
    refresh_plist(d);
}

/* --------------------------- service 信号 --------------------------- */

static void on_owner_event(HbShareService *s, const char *sid,
                           int type, guint pid,
                           const char *name, const char *device,
                           const char *text, HbShareDialog *d)
{
    HbShareService *sv = hb_share_service_default();
    (void)s;
    if (!d->share_id || !sid || strcmp(sid, d->share_id))
        return;
    switch (type) {
    case HB_SH_EV_JOINED:
        add_part(d, pid, name && *name ? name : "guest",
                 device, hb_share_service_mode(sv, sid),
                 !(device && *device));
        break;
    case HB_SH_EV_LEFT:
        del_part(d, pid);
        break;
    case HB_SH_EV_PAKE_OK:
        add_part(d, pid, name && *name ? name : "guest", "",
                 hb_share_service_mode(sv, sid), TRUE);
        break;
    case HB_SH_EV_PAKE_FAIL:
        /* 引擎已计数/踢出；UI 只刷新列表 */
        break;
    default:
        (void)text;
        break;
    }
}

static void on_share_changed(HbShareService *s, const char *sid,
                             HbShareDialog *d)
{
    (void)s;
    if (!d->share_id || !sid || !strcmp(sid, d->share_id))
        refresh(d);
}

/* --------------------------- 回调 --------------------------- */

static void started_cb(HbShareService *s, const char *sid,
                       const char *err, gpointer ud)
{
    HbShareDialog *d = ud;
    (void)s;
    d->starting = FALSE;
    if (err) {
        gtk_switch_set_active(GTK_SWITCH(d->sw), FALSE);
        g_clear_pointer(&d->share_id, g_free);
        refresh(d);
        /* TODO: toast；对话框里 inline error 即可 */
        return;
    }
    g_free(d->share_id);
    d->share_id = g_strdup(sid);
    refresh(d);
}

static void on_switch(GtkSwitch *sw, GParamSpec *ps, HbShareDialog *d)
{
    gboolean on;
    (void)ps;
    if (d->updating) return;
    on = gtk_switch_get_active(sw);
    if (on && !d->share_id && !d->starting) {
        d->starting = TRUE;
        hb_share_service_start(hb_share_service_default(),
                               d->sess, d->title, d->host,
                               gtk_drop_down_get_selected(
                                   GTK_DROP_DOWN(d->mode_dd)) == 1
                                   ? HB_SHARE_COLLAB : HB_SHARE_VIEW,
                               started_cb, d);
    } else if (!on && d->share_id) {
        hb_share_service_stop(hb_share_service_default(),
                              d->share_id);
        g_clear_pointer(&d->share_id, g_free);
        refresh(d);
    }
}

static void on_mode(GtkDropDown *dd, GParamSpec *ps, HbShareDialog *d)
{
    int mode;
    (void)ps;
    if (d->updating || !d->share_id) return;
    mode = gtk_drop_down_get_selected(dd) == 1
           ? HB_SHARE_COLLAB : HB_SHARE_VIEW;
    hb_share_service_set_mode(hb_share_service_default(),
                              d->share_id, mode);
    {
        /* 既有参与者 pill 跟随 mode */
        GList *l = g_object_get_data(G_OBJECT(d), "parts");
        for (; l; l = l->next) {
            PartRow *r = l->data;
            r->perm = mode;
        }
        refresh_plist(d);
    }
}

static void on_copy(GtkButton *b, HbShareDialog *d)
{
    const char *link;
    (void)b;
    link = hb_share_service_link(hb_share_service_default(),
                                 d->share_id);
    if (link) {
        GdkClipboard *cb = gdk_display_get_clipboard(
            gtk_widget_get_display(GTK_WIDGET(d)));
        gdk_clipboard_set_text(cb, link);
    }
}

static void on_regen(GtkButton *b, HbShareDialog *d)
{
    (void)b;
    if (d->share_id)
        hb_share_service_regen(hb_share_service_default(),
                               d->share_id);
    refresh(d);
}

static void on_stop(GtkButton *b, HbShareDialog *d)
{
    (void)b;
    if (d->share_id)
        hb_share_service_stop(hb_share_service_default(),
                              d->share_id);
    adw_dialog_close(ADW_DIALOG(d));
}

/* --------------------------- refresh --------------------------- */

static void refresh(HbShareDialog *d)
{
    HbShareService *s = hb_share_service_default();
    gboolean in;
    const char *link, *code;
    char fmt[8];

    in = hb_sync_service_state(hb_sync_service_default())
         != HB_SYNC_SIGNED_OUT;
    gtk_widget_set_visible(d->signedout, !in);
    gtk_widget_set_sensitive(d->sw, in && !d->starting);

    gtk_widget_set_visible(d->body, d->share_id != NULL);
    if (!d->share_id) return;

    /* 参与者表以 service 维护的当前快照为准（打开对话前加入的
     * 成员/访客也可见）；后续 JOINED/LEFT 事件继续增量更新。 */
    {
        GList *l = g_object_get_data(G_OBJECT(d), "parts");
        GList *snap = hb_share_service_participants(s, d->share_id);
        GList *it;
        g_list_free_full(l, free_prow);
        l = NULL;
        for (it = snap; it; it = it->next) {
            hb_share_part *sp = it->data;
            PartRow *r = g_new0(PartRow, 1);
            r->id = sp->id;
            r->name = g_strdup(sp->name);
            r->device = g_strdup(sp->device);
            r->perm = hb_share_service_mode(s, d->share_id);
            r->guest = sp->guest;
            l = g_list_append(l, r);
        }
        g_list_free_full(snap, (GDestroyNotify)hb_share_part_free);
        g_object_set_data(G_OBJECT(d), "parts", l);
        refresh_plist(d);
    }

    link = hb_share_service_link(s, d->share_id);
    code = hb_share_service_code(s, d->share_id);
    d->updating = TRUE;
    if (link)
        gtk_label_set_label(GTK_LABEL(d->link_row), link);
    if (code && strlen(code) >= 3) {
        snprintf(fmt, sizeof fmt, "%.3s %.3s", code, code + 3);
        gtk_label_set_label(GTK_LABEL(d->code_lbl), fmt);
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->mode_dd),
        hb_share_service_mode(s, d->share_id) == HB_SHARE_COLLAB
            ? 1 : 0);
    gtk_switch_set_active(GTK_SWITCH(d->sw), TRUE);
    d->updating = FALSE;
}

/* --------------------------- 构造 --------------------------- */

static GtkWidget *row_lbl(const char *txt, const char *css)
{
    GtkWidget *l = gtk_label_new(txt);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css) gtk_widget_add_css_class(l, css);
    return l;
}

static void hb_share_dialog_init(HbShareDialog *d)
{
    GtkWidget *tb, *root, *hd, *swrow, *mrow, *lrow, *crow, *hint,
              *stopwrap, *join;
    const char *modes[] = { "仅观看", "可协作输入", NULL };

    tb = adw_toolbar_view_new();
    hd = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hd),
        adw_window_title_new("共享会话", NULL));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), hd);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_size_request(root, 420, -1);

    /* 未登录提示 */
    d->signedout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(d->signedout),
        row_lbl("共享需要登录 Harbor 账号", "hb-share-hint-title"));
    join = gtk_button_new_with_label("前往账号登录");
    gtk_widget_set_halign(join, GTK_ALIGN_CENTER);
    g_signal_connect(join, "clicked",
                     G_CALLBACK(on_goto_account), d);
    gtk_box_append(GTK_BOX(d->signedout), join);
    gtk_box_append(GTK_BOX(root), d->signedout);

    /* 开启云端共享 */
    swrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(swrow),
        row_lbl("开启云端共享", "hb-share-row-title"));
    d->sw = gtk_switch_new();
    gtk_widget_set_hexpand(d->sw, TRUE);
    gtk_widget_set_halign(d->sw, GTK_ALIGN_END);
    gtk_widget_set_valign(d->sw, GTK_ALIGN_CENTER);
    g_signal_connect(d->sw, "notify::active",
                     G_CALLBACK(on_switch), d);
    gtk_box_append(GTK_BOX(swrow), d->sw);
    gtk_box_append(GTK_BOX(root), swrow);

    /* 参与权限 */
    mrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(mrow),
        row_lbl("参与权限", "hb-share-row-title"));
    d->mode_dd = gtk_drop_down_new_from_strings(modes);
    gtk_widget_set_hexpand(d->mode_dd, TRUE);
    gtk_widget_set_halign(d->mode_dd, GTK_ALIGN_END);
    g_signal_connect(d->mode_dd, "notify::selected",
                     G_CALLBACK(on_mode), d);
    gtk_box_append(GTK_BOX(mrow), d->mode_dd);
    gtk_box_append(GTK_BOX(root), mrow);

    /* 共享信息区 */
    d->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_visible(d->body, FALSE);

    lrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(lrow),
        row_lbl("共享链接", "hb-share-row-title"));
    d->link_row = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(d->link_row), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(d->link_row),
                            PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(d->link_row, TRUE);
    gtk_widget_add_css_class(d->link_row, "hb-share-code");
    gtk_box_append(GTK_BOX(lrow), d->link_row);
    {
        GtkWidget *cp = gtk_button_new();
        gtk_widget_add_css_class(cp, "tbtn");
        gtk_button_set_child(GTK_BUTTON(cp), hb_icon_new("copy", 13));
        gtk_widget_set_tooltip_text(cp, "复制");
        g_signal_connect(cp, "clicked", G_CALLBACK(on_copy), d);
        gtk_box_append(GTK_BOX(lrow), cp);
    }
    gtk_box_append(GTK_BOX(d->body), lrow);

    crow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(crow),
        row_lbl("配对码", "hb-share-row-title"));
    d->code_lbl = gtk_label_new("");
    gtk_widget_add_css_class(d->code_lbl, "hb-share-code");
    gtk_box_append(GTK_BOX(crow), d->code_lbl);
    gtk_box_append(GTK_BOX(crow),
        row_lbl("30 分钟内有效", "hb-term-muted"));
    {
        GtkWidget *rg = gtk_button_new_with_label("重新生成");
        gtk_widget_add_css_class(rg, "tbtn");
        gtk_widget_set_hexpand(rg, TRUE);
        gtk_widget_set_halign(rg, GTK_ALIGN_END);
        g_signal_connect(rg, "clicked", G_CALLBACK(on_regen), d);
        gtk_box_append(GTK_BOX(crow), rg);
    }
    gtk_box_append(GTK_BOX(d->body), crow);

    gtk_box_append(GTK_BOX(d->body),
        row_lbl("参与者", "hb-share-row-title"));
    d->empty_lbl = row_lbl("还没有人加入", "hb-term-muted");
    gtk_box_append(GTK_BOX(d->body), d->empty_lbl);
    d->plist = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(d->body), d->plist);

    hint = row_lbl("共享期间对方可以看到终端内容；"
                   "可随时停止。端到端加密。", "hb-term-muted");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(d->body), hint);

    d->stop_btn = gtk_button_new_with_label("停止共享");
    gtk_widget_add_css_class(d->stop_btn, "destructive-action");
    gtk_widget_add_css_class(d->stop_btn, "pill");
    stopwrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(d->stop_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(stopwrap), d->stop_btn);
    g_signal_connect(d->stop_btn, "clicked",
                     G_CALLBACK(on_stop), d);
    gtk_box_append(GTK_BOX(d->body), stopwrap);

    gtk_box_append(GTK_BOX(root), d->body);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), root);
    adw_dialog_set_content_width(ADW_DIALOG(d), 460);
    adw_dialog_set_follows_content_size(ADW_DIALOG(d), TRUE);
    adw_dialog_set_child(ADW_DIALOG(d), tb);

    d->sig_changed = g_signal_connect(
        hb_share_service_default(), "share-changed",
        G_CALLBACK(on_share_changed), d);
    d->sig_event = g_signal_connect(
        hb_share_service_default(), "owner-event",
        G_CALLBACK(on_owner_event), d);
}

static void hb_share_dialog_dispose(GObject *o)
{
    HbShareDialog *d = HB_SHARE_DIALOG(o);
    g_signal_handler_disconnect(hb_share_service_default(),
                                d->sig_changed);
    g_signal_handler_disconnect(hb_share_service_default(),
                                d->sig_event);
    g_list_free_full(g_object_steal_data(G_OBJECT(d), "parts"),
                     free_prow);
    G_OBJECT_CLASS(hb_share_dialog_parent_class)->dispose(o);
}

static void hb_share_dialog_finalize(GObject *o)
{
    HbShareDialog *d = HB_SHARE_DIALOG(o);
    g_free(d->title); g_free(d->host); g_free(d->share_id);
    G_OBJECT_CLASS(hb_share_dialog_parent_class)->finalize(o);
}

static void hb_share_dialog_class_init(HbShareDialogClass *k)
{
    G_OBJECT_CLASS(k)->dispose = hb_share_dialog_dispose;
    G_OBJECT_CLASS(k)->finalize = hb_share_dialog_finalize;
}

/* --------------------------- 入口 --------------------------- */

void hb_share_dialog_present(GtkWidget *parent, hb_session *sess,
                             const char *title, const char *host)
{
    HbShareDialog *d = g_object_new(HB_TYPE_SHARE_DIALOG, NULL);
    HbShareService *s = hb_share_service_default();
    const char *sid;
    d->sess = sess;
    d->title = g_strdup(title);
    d->host = g_strdup(host);
    sid = hb_share_service_share_for_session(s, sess);
    if (sid) {
        d->share_id = g_strdup(sid);
        gtk_switch_set_active(GTK_SWITCH(d->sw), TRUE);
    }
    refresh(d);
    adw_dialog_present(ADW_DIALOG(d), parent);
}
