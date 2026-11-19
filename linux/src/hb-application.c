#include "hb-application.h"
#include "hb-window.h"
#include "hb-store.h"
#include "hb-vault-page.h"
#include "hb-hosts-view.h"
#include "hb-term-page.h"
#include "hb-term-theme.h"
#include "hb-notify.h"
#include "hb-icons.h"

struct _HbApplication {
    AdwApplication parent;
};

G_DEFINE_TYPE(HbApplication, hb_application, ADW_TYPE_APPLICATION)

/* dev 选项（main.c 用 --xxx 传入） */
char *hb_opt_screenshot;
int   hb_opt_screenshot_delay = 800;
char *hb_opt_section;
char *hb_opt_filter;
char *hb_opt_host_view;
char *hb_opt_theme;
char *hb_opt_open_local;    /* "" = 只开页；非空 = 启动后注入 CMD\n */

static GtkWidget *sftp_placeholder(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ic = hb_icon_new("folder", 24);
    GtkWidget *t = gtk_label_new("SFTP 双栏文件管理");
    GtkWidget *s = gtk_label_new("本地与远程文件的双栏浏览，将在 SFTP 单元实现");

    gtk_widget_add_css_class(box, "hb-empty");
    gtk_widget_add_css_class(box, "hb-content");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(ico, "hb-e-ico");
    gtk_widget_set_size_request(ico, 58, 58);
    gtk_widget_set_halign(ico, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(ic, TRUE);
    gtk_box_append(GTK_BOX(ico), ic);
    gtk_widget_add_css_class(t, "hb-e-title");
    gtk_widget_add_css_class(s, "hb-e-sub");
    gtk_box_append(GTK_BOX(box), ico);
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), s);
    return box;
}

/* ------------------------------ 截图 ------------------------------ */

static gboolean take_shot(gpointer ud)
{
    GtkWidget *win = ud;
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(win));
    GdkPaintable *p = gtk_widget_paintable_new(win);
    GskRenderer *r = gsk_renderer_new_for_surface(surf);
    GtkSnapshot *snap = gtk_snapshot_new();
    GskRenderNode *node;
    GdkTexture *tex;
    graphene_rect_t bounds;
    int w = gtk_widget_get_width(win), h = gtk_widget_get_height(win);
    gboolean ok;

    graphene_rect_init(&bounds, 0, 0, (float)w, (float)h);
    gdk_paintable_snapshot(p, snap, w, h);
    node = gtk_snapshot_free_to_node(snap);
    tex = node ? gsk_renderer_render_texture(r, node, &bounds) : NULL;
    ok = tex && gdk_texture_save_to_png(tex, hb_opt_screenshot);
    g_printerr("harbor: screenshot %s -> %s\n",
               ok ? "saved" : "FAILED", hb_opt_screenshot);
    g_clear_object(&tex);
    gsk_render_node_unref(node);
    gsk_renderer_unrealize(r);
    g_clear_object(&r);
    g_clear_object(&p);
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

/* ------------------------------ 生命周期 ------------------------------ */

static void hb_application_startup(GApplication *app)
{
    GtkCssProvider *css;

    G_APPLICATION_CLASS(hb_application_parent_class)->startup(app);

    hb_store_default();          /* vault 打开 + 首次播种 */
    hb_notify_default();

    css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/dev/harbor/Harbor/style.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    adw_style_manager_set_color_scheme(
        adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_LIGHT);
    hb_term_theme_install_css();
}

/* Hosts 页 connect(id)：local → 打开 Local 终端页；其余协议留待 SSH 单元 */
static void on_host_connect(GtkWidget *hv, const char *hid, gpointer ud)
{
    HbWindow *win = ud;
    hb_obj *o = NULL;
    const char *proto;
    (void)hv;
    if (hb_vault_get(hb_store_vault(hb_store_default()), hid, &o) != HB_OK ||
        !o)
        return;
    proto = hb_obj_get_str(o, "proto");
    if (!proto) proto = "";
    if (!strcmp(proto, "local"))
        hb_window_open_local(win, NULL);
    hb_obj_free(o);
}

static void hb_application_activate(GApplication *app)
{
    HbWindow *win;
    GtkWidget *vp;
    GtkWindow *cur = gtk_application_get_active_window(
        GTK_APPLICATION(app));

    if (cur) {
        gtk_window_present(cur);
        return;
    }

    win = hb_window_new(GTK_APPLICATION(app));

    vp = hb_vault_page_new();
    hb_window_add_tab(win, "vault", "Vault", "server", vp, FALSE, FALSE);
    hb_window_set_vault_page(win, vp);
    hb_window_add_tab(win, "sftp", "SFTP", "folder",
                      sftp_placeholder(), FALSE, FALSE);
    hb_window_switch_tab(win, "vault");

    /* dev 选项 */
    if (hb_opt_section)
        hb_vault_page_set_section(HB_VAULT_PAGE(vp), hb_opt_section);
    if (hb_opt_filter)
        hb_hosts_view_set_search(
            HB_HOSTS_VIEW(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))),
            hb_opt_filter);
    if (hb_opt_host_view)
        hb_hosts_view_set_list_mode(
            HB_HOSTS_VIEW(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))),
            !strcmp(hb_opt_host_view, "list"));
    g_signal_connect(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp)),
                     "connect", G_CALLBACK(on_host_connect), win);
    if (hb_opt_open_local) {
        GtkWidget *page = NULL;
        hb_window_open_local(win, &page);
        if (page && *hb_opt_open_local) {
            char *cmd = g_strconcat(hb_opt_open_local, "\n", NULL);
            hb_term_page_feed_input(HB_TERM_PAGE(page), cmd);
            g_free(cmd);
        }
    }

    gtk_window_present(GTK_WINDOW(win));

    if (hb_opt_screenshot)
        g_timeout_add(hb_opt_screenshot_delay, take_shot, win);
}

static void hb_application_class_init(HbApplicationClass *klass)
{
    GApplicationClass *ac = G_APPLICATION_CLASS(klass);
    ac->startup = hb_application_startup;
    ac->activate = hb_application_activate;
}

static void hb_application_init(HbApplication *app)
{
    (void)app;
}

HbApplication *hb_application_new(void)
{
    return g_object_new(HB_TYPE_APPLICATION,
        "application-id", "dev.harbor.Harbor",
        "flags", G_APPLICATION_DEFAULT_FLAGS,
        NULL);
}
