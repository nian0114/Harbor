/* FR-12 共享会话 vault 区（prototype sharedView）：本机共享卡片 +
 * 同账号发现的共享 + 配对码 guest 加入。 */

#include "hb-share-view.h"
#include "hb-share.h"
#include "hb-share-page.h"
#include "hb-sync.h"
#include "hb-window.h"
#include "hb-vault-page.h"
#include "hb-icons.h"

#include <sodium.h>
#include <string.h>
#include <glib/gi18n.h>

struct _HbShareView {
    GtkBox       parent;
    GtkWidget   *local_box;    /* 本机正在共享的会话 */
    GtkWidget   *remote_box;   /* 来自其它设备的共享 */
    GtkWidget   *remote_empty;
    GtkWidget   *signedout;
    GtkWidget   *content;
    gulong       sig_shares, sig_changed, sig_peer;
    gboolean     listed_once;
};

#define HB_TYPE_SHAREVIEW (hb_share_view_get_type())
G_DECLARE_FINAL_TYPE(HbShareView, hb_share_view, HB, SHAREVIEW,
                     GtkBox)
G_DEFINE_TYPE(HbShareView, hb_share_view, GTK_TYPE_BOX)

static void refresh(HbShareView *v);
static void list_remote(HbShareView *v);

/* --------------------------- 卡片 --------------------------- */

static void open_viewer_tab(HbShareView *v, hb_share_peer_ctx *ctx,
                            const char *title)
{
    GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(v));
    if (HB_IS_WINDOW(r))
        hb_share_open_viewer(GTK_WIDGET(r), ctx, title);
}

static void join_cb(HbShareService *s, gpointer ctx, const char *err,
                    gpointer ud)
{
    HbShareView *v = ud;
    (void)s;
    if (err) {
        GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(v));
        if (HB_IS_WINDOW(r))
            hb_window_toast(HB_WINDOW(r), err);
        return;
    }
    open_viewer_tab(v, ctx, hb_share_peer_ctx_title(ctx));
}

static void on_remote_card(GtkButton *b, gpointer ud)
{
    const char *sid = g_object_get_data(G_OBJECT(b), "sid");
    const char *meta = g_object_get_data(G_OBJECT(b), "meta");
    hb_share_service_join_member(hb_share_service_default(),
                                 sid, meta, join_cb, ud);
}

static void on_local_card(GtkButton *b, gpointer ud)
{
    HbShareView *v = ud;
    GtkRoot *r;
    const char *tab = g_object_get_data(G_OBJECT(b), "tab");
    (void)v;
    r = gtk_widget_get_root(GTK_WIDGET(b));
    if (tab && HB_IS_WINDOW(r))
        hb_window_switch_tab(HB_WINDOW(r), tab);
}

static GtkWidget *card(const char *title, const char *sub,
                       const char *pill, GCallback cb, gpointer ud)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    GtkWidget *s;
    gtk_widget_add_css_class(b, "hb-share-card");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_widget_add_css_class(t, "hb-share-card-title");
    gtk_box_append(GTK_BOX(txt), t);
    s = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(s), 0);
    gtk_widget_add_css_class(s, "hb-share-card-sub");
    gtk_box_append(GTK_BOX(txt), s);
    gtk_widget_set_hexpand(txt, TRUE);
    gtk_box_append(GTK_BOX(row), txt);
    if (pill) {
        GtkWidget *pl = gtk_label_new(pill);
        gtk_widget_add_css_class(pl, "hb-share-pill");
        gtk_widget_set_valign(pl, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), pl);
    }
    gtk_button_set_child(GTK_BUTTON(b), row);
    g_signal_connect(b, "clicked", cb, ud);
    return b;
}

/* --------------------------- 填充 --------------------------- */

static void fill_local(HbShareView *v)
{
    GtkWidget *ch;
    GList *l, *it;
    while ((ch = gtk_widget_get_first_child(v->local_box)))
        gtk_box_remove(GTK_BOX(v->local_box), ch);
    l = hb_share_service_own_shares(hb_share_service_default());
    for (it = l; it; it = it->next) {
        HbShareOwnInfo *i = it->data;
        GtkWidget *c;
        char *sub = g_strdup_printf("%s · %s",
            i->host ? i->host : "localhost",
            i->mode == HB_SHARE_COLLAB ? _("可协作输入") : _("仅观看"));
        c = card(i->title ? i->title : _("共享"), sub, _("共享中"),
                 G_CALLBACK(on_local_card), v);
        g_free(sub);
        if (i->tab_id)
            g_object_set_data_full(G_OBJECT(c), "tab",
                                   g_strdup(i->tab_id), g_free);
        gtk_box_append(GTK_BOX(v->local_box), c);
    }
    g_list_free_full(l, hb_share_own_info_free);
    gtk_widget_set_visible(v->local_box, l != NULL || TRUE);
}

static void fill_remote(HbShareView *v, GList *items)
{
    GtkWidget *ch;
    GList *it;
    guint n = 0;
    const char *dev = hb_sync_service_device_name(
        hb_sync_service_default());
    uint8_t vk[32];
    gboolean have_vk = hb_sync_service_vk_copy(
        hb_sync_service_default(), vk);
    while ((ch = gtk_widget_get_first_child(v->remote_box)))
        gtk_box_remove(GTK_BOX(v->remote_box), ch);
    for (it = items; it; it = it->next) {
        HbShareItem *i = it->data;
        GtkWidget *c;
        char *sub, *when;
        hb_share_peer *peek = NULL;
        const char *title = NULL, *host = NULL;
        int mode = HB_SHARE_VIEW;
        if (i->owner_device && dev && !strcmp(i->owner_device, dev))
            continue;               /* 本机的在上方 */
        /* 用引擎解密 meta（title/host/mode/started） */
        if (have_vk && i->meta_b64) {
            peek = hb_share_peer_new_member(
                i->share_id, vk, i->meta_b64, "", "", NULL);
            if (peek) {
                title = hb_share_peer_title(peek);
                host = hb_share_peer_host(peek);
                mode = hb_share_peer_meta_mode(peek);
            }
        }
        when = i->created
            ? g_strdup_printf(ngettext("%ld 分钟前", "%ld 分钟前",
                                  (g_get_real_time() / 1000 - i->created) / 60000),
                              (long)((g_get_real_time() / 1000 -
                                      i->created) / 60000))
            : g_strdup("");
        sub = g_strdup_printf("%s@%s · %s · %s",
                              i->owner_device,
                              (host && *host) ? host : "?",
                              mode == HB_SHARE_COLLAB
                                  ? _("可协作输入") : _("仅观看"),
                              when);
        g_free(when);
        c = card((title && *title) ? title : _("共享会话"), sub, _("加入"),
                 G_CALLBACK(on_remote_card), v);
        g_free(sub);
        g_object_set_data_full(G_OBJECT(c), "sid",
                               g_strdup(i->share_id), g_free);
        g_object_set_data_full(G_OBJECT(c), "meta",
                               g_strdup(i->meta_b64), g_free);
        gtk_box_append(GTK_BOX(v->remote_box), c);
        if (peek) hb_share_peer_free(peek);
        n++;
    }
    sodium_memzero(vk, sizeof vk);
    gtk_widget_set_visible(v->remote_empty, n == 0);
}

static void list_cb(HbShareService *s, GList *items, const char *err,
                    gpointer ud)
{
    HbShareView *v = ud;
    (void)s;
    if (err) return;                /* 离线时静默，保持旧列表 */
    fill_remote(v, items);
}

static void list_remote(HbShareView *v)
{
    hb_share_service_list(hb_share_service_default(), list_cb, v);
}

static void refresh(HbShareView *v)
{
    gboolean in = hb_sync_service_state(hb_sync_service_default())
                  != HB_SYNC_SIGNED_OUT;
    gtk_widget_set_visible(v->signedout, !in);
    gtk_widget_set_visible(v->content, in);
    fill_local(v);
    if (in)
        list_remote(v);
}

/* --------------------------- signals --------------------------- */

static void on_shares(HbSyncService *s, HbShareView *v)
{ (void)s; list_remote(v); }

static void on_changed(HbShareService *s, const char *sid,
                       HbShareView *v)
{ (void)s; (void)sid; refresh(v); }

static void on_map(GtkWidget *w, gpointer ud)
{ (void)ud; refresh(HB_SHAREVIEW(w)); }

static void on_vault_account(GtkButton *b, HbShareView *v)
{
    GtkRoot *r;
    GtkWidget *vp;
    (void)b;
    r = gtk_widget_get_root(GTK_WIDGET(v));
    if (HB_IS_WINDOW(r) &&
        (vp = hb_window_vault_page(HB_WINDOW(r))))
        hb_vault_page_set_section(HB_VAULT_PAGE(vp), "account");
}

/* --------------------------- join 对话框 --------------------------- */

typedef struct {
    AdwDialog *dlg;
    GtkWidget *link, *code;
    HbShareView *v;
} JoinCtx;

static void on_join_go(GtkButton *b, JoinCtx *jc)
{
    const char *link = gtk_editable_get_text(GTK_EDITABLE(jc->link));
    const char *code = gtk_editable_get_text(GTK_EDITABLE(jc->code));
    (void)b;
    if (!link || !*link || !code || strlen(code) < 6) {
        gtk_widget_add_css_class(jc->code, "error");
        return;
    }
    adw_dialog_close(jc->dlg);
    hb_share_service_join_guest(hb_share_service_default(),
                                link, code, join_cb, jc->v);
    g_free(jc);
}

static void on_join_btn(GtkButton *b, HbShareView *v)
{
    AdwDialog *d = adw_dialog_new();
    GtkWidget *tb = adw_toolbar_view_new();
    GtkWidget *hd = adw_header_bar_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    JoinCtx *jc = g_new0(JoinCtx, 1);
    GtkWidget *go;
    (void)b;

    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hd),
        adw_window_title_new(_("通过配对码加入"), NULL));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), hd);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_size_request(root, 380, -1);

    gtk_box_append(GTK_BOX(root),
        gtk_label_new(_("粘贴共享链接：")));
    jc->link = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(jc->link),
                                   "harbor://share/...");
    gtk_box_append(GTK_BOX(root), jc->link);

    gtk_box_append(GTK_BOX(root), gtk_label_new(_("6 位配对码：")));
    jc->code = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(jc->code), "482913");
    gtk_entry_set_max_length(GTK_ENTRY(jc->code), 7);
    gtk_box_append(GTK_BOX(root), jc->code);

    go = gtk_button_new_with_label(_("加入"));
    gtk_widget_add_css_class(go, "suggested-action");
    gtk_widget_add_css_class(go, "pill");
    gtk_widget_set_margin_top(go, 6);
    g_signal_connect(go, "clicked", G_CALLBACK(on_join_go), jc);
    gtk_box_append(GTK_BOX(root), go);

    jc->dlg = d; jc->v = v;
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), root);
    adw_dialog_set_child(d, tb);
    adw_dialog_set_content_width(d, 400);
    adw_dialog_present(d, GTK_WIDGET(v));
}

/* --------------------------- 构造 --------------------------- */

static GtkWidget *sec_title(const char *t)
{
    GtkWidget *l = gtk_label_new(t);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "hb-share-sec");
    return l;
}

static void hb_share_view_init(HbShareView *v)
{
    GtkWidget *scroll, *tb, *join_btn;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(v), "hb-content");

    /* 顶部工具行 */
    tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(tb), sp);
    }
    join_btn = gtk_button_new_with_label(_("通过配对码加入"));
    gtk_widget_add_css_class(join_btn, "suggested-action");
    g_signal_connect(join_btn, "clicked",
                     G_CALLBACK(on_join_btn), v);
    gtk_box_append(GTK_BOX(tb), join_btn);
    gtk_box_append(GTK_BOX(v), tb);

    scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    v->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(v->content, 6);

    gtk_box_append(GTK_BOX(v->content),
                   sec_title(_("本机正在共享的会话")));
    v->local_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(v->content), v->local_box);

    gtk_box_append(GTK_BOX(v->content),
        sec_title(_("来自其它设备的共享（同账号自动发现）")));
    v->remote_empty = gtk_label_new(_("暂无可用共享"));
    gtk_widget_add_css_class(v->remote_empty, "hb-term-muted");
    gtk_label_set_xalign(GTK_LABEL(v->remote_empty), 0);
    gtk_box_append(GTK_BOX(v->content), v->remote_empty);
    v->remote_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(v->content), v->remote_box);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  v->content);
    gtk_box_append(GTK_BOX(v), scroll);

    /* 未登录 */
    v->signedout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_valign(v->signedout, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(v->signedout, FALSE);
    {
        GtkWidget *l = gtk_label_new(
            _("登录 Harbor 账号后可共享会话或加入共享"));
        GtkWidget *b = gtk_button_new_with_label(_("前往账号登录"));
        gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
        g_signal_connect(b, "clicked",
                         G_CALLBACK(on_vault_account), v);
        gtk_box_append(GTK_BOX(v->signedout), l);
        gtk_box_append(GTK_BOX(v->signedout), b);
    }
    gtk_box_append(GTK_BOX(v), v->signedout);

    v->sig_shares = g_signal_connect(hb_sync_service_default(),
        "shares", G_CALLBACK(on_shares), v);
    v->sig_changed = g_signal_connect(hb_share_service_default(),
        "share-changed", G_CALLBACK(on_changed), v);
    g_signal_connect(v, "map", G_CALLBACK(on_map), NULL);
}

static void hb_share_view_dispose(GObject *o)
{
    HbShareView *v = HB_SHAREVIEW(o);
    g_signal_handler_disconnect(hb_sync_service_default(),
                                v->sig_shares);
    g_signal_handler_disconnect(hb_share_service_default(),
                                v->sig_changed);
    G_OBJECT_CLASS(hb_share_view_parent_class)->dispose(o);
}

static void hb_share_view_class_init(HbShareViewClass *k)
{
    G_OBJECT_CLASS(k)->dispose = hb_share_view_dispose;
}

GtkWidget *hb_share_view_new(void)
{
    return g_object_new(HB_TYPE_SHAREVIEW, NULL);
}
