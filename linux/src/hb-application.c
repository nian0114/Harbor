#include "hb-application.h"
#include "hb-window.h"
#include "hb-store.h"
#include "hb-vault-page.h"
#include "hb-hosts-view.h"
#include "hb-term-page.h"
#include "hb-ssh-page.h"
#include "hb-term-theme.h"
#include "hb-notify.h"
#include "hb-icons.h"
#include "hb-dialogs.h"
#include "hb-host-actions.h"

#include <harbor/ssh.h>
#include <string.h>

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
char *hb_opt_connect;       /* --connect=user@host[:port] 启动即快速连接 */

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

static gboolean shot_window(GtkWidget *win, const char *path)
{
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
    ok = tex && gdk_texture_save_to_png(tex, path);
    g_clear_object(&tex);
    if (node)
        gsk_render_node_unref(node);
    gsk_renderer_unrealize(r);
    g_clear_object(&r);
    g_clear_object(&p);
    g_printerr("harbor: screenshot %s -> %s\n", ok ? "saved" : "FAILED", path);
    return ok;
}

static gboolean take_shot(gpointer ud)
{
    GtkWidget *win = ud;
    gboolean ok = shot_window(win, hb_opt_screenshot);

    if (!ok) {
        static int retries;
        if (++retries < 10) {
            g_printerr("harbor: screenshot retry %d/10 -> %s\n",
                       retries, hb_opt_screenshot);
            g_timeout_add(500, take_shot, win);
            return G_SOURCE_REMOVE;
        }
    }
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

#ifndef NDEBUG
/* app.shot <png 路径>：dev 调试动作，运行中实例按控件树截图。 */
static void on_dev_shot(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    (void)a;
    if (!param) return;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (win)
        shot_window(GTK_WIDGET(win), g_variant_get_string(param, NULL));
}

/* app.feed <text>：dev 调试动作，把文本写进活动 tab 的会话。 */
static void on_dev_feed(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    GtkWidget *page;
    const char *text;
    (void)a;
    if (!param) return;
    text = g_variant_get_string(param, NULL);
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    /* workspace tab → 焦点 pane */
    page = hb_window_active_session_page(HB_WINDOW(win));
    if (HB_IS_TERM_PAGE(page))
        hb_term_page_feed_input(HB_TERM_PAGE(page), text);
    else if (HB_IS_SSH_PAGE(page))
        hb_ssh_page_feed_input(HB_SSH_PAGE(page), text);
}

/* app.open-host <host_id>：dev 调试动作，按主机 id 打开对应 tab。 */
static void on_dev_open_host(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    const char *id;
    (void)a;
    if (!param) return;
    id = g_variant_get_string(param, NULL);
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (HB_IS_WINDOW(win))
        hb_host_action_connect(GTK_WIDGET(win), hb_store_default(), id);
}
#endif /* !NDEBUG */

/* ------------------------------ 生命周期 ------------------------------ */

/* app.show-tab <tab_id>：监控通知的默认动作 —— 呈现窗口并切到该
 * tab；tab 已关则什么都不做（遍历所有窗口）。 */
static void on_show_tab(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GApplication *app = ud;
    GList *ws, *l;
    const char *id;
    (void)a;
    if (!param) return;
    id = g_variant_get_string(param, NULL);
    ws = gtk_application_get_windows(GTK_APPLICATION(app));
    for (l = ws; l; l = l->next) {
        if (HB_IS_WINDOW(l->data) &&
            hb_window_has_tab(HB_WINDOW(l->data), id)) {
            hb_window_switch_tab(HB_WINDOW(l->data), id);
            gtk_window_present(GTK_WINDOW(l->data));
            return;
        }
    }
}

static void hb_application_startup(GApplication *app)
{
    GtkCssProvider *css;
    GSimpleAction *sa;

    G_APPLICATION_CLASS(hb_application_parent_class)->startup(app);

    hb_store_default();          /* vault 打开 + 首次播种 */
    hb_notify_default();

    /* FR-10：启动时按 log_retention_days（默认 90；<=0 = 永久）清理日志 */
    {
        hb_obj *o = NULL;
        hb_vault *v = hb_store_vault(hb_store_default());
        if (hb_vault_settings(v, &o) == HB_OK && o) {
            hb_vault_log_purge(v, (int)hb_obj_get_int(
                                   o, "log_retention_days", 90));
            hb_obj_free(o);
        }
    }

    sa = g_simple_action_new("show-tab", G_VARIANT_TYPE_STRING);
    g_signal_connect(sa, "activate", G_CALLBACK(on_show_tab), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
    g_object_unref(sa);

#ifndef NDEBUG
    /* shot/feed/open-host 会经 org.gtk.Actions 导出到 session bus，
     * 任何同 bus 进程都能驱动会话 —— 仅 debug build + 显式开关。 */
    if (!g_strcmp0(g_getenv("HARBOR_DEV_ACTIONS"), "1")) {
        sa = g_simple_action_new("shot", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_shot), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("feed", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_feed), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("open-host", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_open_host), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);
    }
#endif

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

/* Hosts 页 connect(id)：ssh → SSH 页；local → Local 页；rdp/vnc → 占位 toast */
static void on_host_connect(GtkWidget *hv, const char *hid, gpointer ud)
{
    HbWindow *win = ud;
    hb_obj *o = NULL;
    (void)hv;
    if (hb_vault_get(hb_store_vault(hb_store_default()), hid, &o) != HB_OK ||
        !o)
        return;
    hb_host_connect_obj(win, hb_store_default(), o);
    hb_obj_free(o);
}

/* 快速连接：已有主机精确命中（name == 文本，或解析出的
 * address+port+user 相等 —— 不做子串匹配，10.0.0.1 不会误连
 * 10.0.0.12）则直接 connect；否则解析文本建主机。 */
static void quick_connect(HbWindow *win, const char *text)
{
    char *user = NULL, *host = NULL;
    int port = 0;
    hb_obj *h, **objs = NULL;
    size_t n = 0, i;
    hb_err pe;
    hb_vault *v = hb_store_vault(hb_store_default());

    pe = hb_ssh_parse_target(text, &user, &host, &port);
    objs = hb_store_list(hb_store_default(), "host", &n);
    for (i = 0; i < n; i++) {
        const char *nm = hb_obj_get_str(objs[i], "name");
        const char *ad = hb_obj_get_str(objs[i], "address");
        const char *hu = hb_obj_get_str(objs[i], "user");
        int hp = (int)hb_obj_get_int(objs[i], "port", 22);
        gboolean hit = FALSE;
        if (nm && !g_ascii_strcasecmp(nm, text))
            hit = TRUE;
        else if (pe == HB_OK && host && ad &&
                 !g_ascii_strcasecmp(ad, host) &&
                 (hp > 0 ? hp : 22) == (port > 0 ? port : 22) &&
                 (!user || !*user || (hu && !strcmp(hu, user))))
            hit = TRUE;
        if (hit) {
            hb_host_connect_obj(win, hb_store_default(), objs[i]);
            hb_obj_list_free(objs, n);
            g_free(user); g_free(host);
            return;
        }
    }
    if (objs) hb_obj_list_free(objs, n);

    if (pe != HB_OK || !host || !*host) {
        hb_toast(GTK_WIDGET(win), "格式：user@hostname[:port]");
        g_free(user); g_free(host);
        return;
    }
    h = hb_obj_new("host");
    hb_obj_set_str(h, "name", host);
    hb_obj_set_str(h, "address", host);
    hb_obj_set_str(h, "user", user && *user ? user : g_get_user_name());
    hb_obj_set_int(h, "port", port > 0 ? port : 22);
    hb_obj_set_str(h, "proto", "ssh");
    hb_obj_set_str(h, "os", "auto");
    hb_obj_set_str(h, "auth", "auto");
    hb_vault_put(v, h);
    hb_host_connect_obj(win, hb_store_default(), h);
    hb_obj_free(h);
    g_free(user); g_free(host);
}

static void on_quick_connect(GtkWidget *hv, const char *text, gpointer ud)
{
    (void)hv;
    quick_connect((HbWindow *)ud, text);
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
    g_signal_connect(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp)),
                     "quick-connect", G_CALLBACK(on_quick_connect), win);
    if (hb_opt_connect)
        quick_connect(win, hb_opt_connect);
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
