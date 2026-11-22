/* hb-tray.c — StatusNotifierItem + com.canonical.dbusmenu over GDBus.
 * 无 libappindicator：直接在 session bus 导出两个对象并向
 * org.kde.StatusNotifierWatcher 注册。watcher 消失→available=FALSE；
 * 重新出现→自动再注册。menu 事件经 HbTrayHooks 回调给 application。 */

#include "hb-tray.h"
#include <gio/gio.h>
#include <string.h>
#include <glib/gi18n.h>

#define SNI_PATH   "/StatusNotifierItem"
#define MENU_PATH  "/dev/harbor/Harbor/MenuBar"
#define WATCHER    "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJ "/StatusNotifierWatcher"

static const char *SNI_XML =
    "<node>"
    " <interface name='org.kde.StatusNotifierItem'>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='WindowId' type='u' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='OverlayIconName' type='s' access='read'/>"
    "  <property name='AttentionIconName' type='s' access='read'/>"
    "  <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='IconThemePath' type='s' access='read'/>"
    "  <method name='ContextMenu'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='i'/>"
    "  </method>"
    "  <method name='Activate'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='i'/>"
    "  </method>"
    "  <method name='SecondaryActivate'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='i'/>"
    "  </method>"
    "  <method name='Scroll'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='s'/>"
    "  </method>"
    "  <signal name='NewTitle'/><signal name='NewIcon'/>"
    "  <signal name='NewAttentionIcon'/><signal name='NewOverlayIcon'/>"
    "  <signal name='NewToolTip'/>"
    "  <signal name='NewStatus'><arg type='s'/></signal>"
    " </interface>"
    "</node>";

static const char *MENU_XML =
    "<node>"
    " <interface name='com.canonical.dbusmenu'>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <method name='GetLayout'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='i'/>"
    "   <arg direction='in' type='as'/>"
    "   <arg direction='out' type='u'/>"
    "   <arg direction='out' type='(ia{sv}av)'/>"
    "  </method>"
    "  <method name='GetGroupProperties'>"
    "   <arg direction='in' type='ai'/><arg direction='in' type='as'/>"
    "   <arg direction='out' type='a(ia{sv})'/>"
    "  </method>"
    "  <method name='GetProperty'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='s'/>"
    "   <arg direction='out' type='v'/>"
    "  </method>"
    "  <method name='Event'>"
    "   <arg direction='in' type='i'/><arg direction='in' type='s'/>"
    "   <arg direction='in' type='v'/><arg direction='in' type='u'/>"
    "  </method>"
    "  <method name='EventGroup'>"
    "   <arg direction='in' type='a(isvu)'/><arg direction='out' type='ai'/>"
    "  </method>"
    "  <method name='AboutToShow'>"
    "   <arg direction='in' type='i'/><arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='AboutToShowGroup'>"
    "   <arg direction='in' type='ai'/><arg direction='out' type='ai'/>"
    "  </method>"
    "  <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "  <signal name='ItemsPropertiesUpdated'>"
    "   <arg type='a(ia{sv})'/><arg type='a(ias)'/>"
    "  </signal>"
    " </interface>"
    "</node>";

enum { M_SHOW = 1, M_COUNT = 2, M_DISCONN = 3, M_SEP = 4, M_QUIT = 5 };

struct _HbTray {
    HbTrayHooks  hooks;
    GDBusConnection *bus;
    GDBusNodeInfo   *sni_info, *menu_info;
    guint           sni_reg, menu_reg;
    guint           name_own, watch_id;
    char           *bus_name;
    gboolean        registered;    /* watcher 收到 Register 后 TRUE */
    gboolean        watcher_up;
    guint           register_src;  /* 注册重试 timer（watcher 对象就绪竞态） */
    int             register_tries;
    int             online;        /* 已发出的 tooltip 计数 */
    guint           revision;
    char           *icon_theme_path; /* 非 NULL 时导出 */
    void          (*watcher_cb)(gboolean, void *);
    void           *watcher_ud;
};

/* ------------------------------ menu model ------------------------------ */

static const char *item_label(HbTray *t, int id, char *buf, size_t n)
{
    switch (id) {
    case M_SHOW:    return _("显示 Harbor");
    case M_COUNT:
        g_snprintf(buf, n, ngettext("%d 个会话在线", "%d 个会话在线", t->online), t->online);
        return buf;
    case M_DISCONN: return _("全部断开");
    case M_QUIT:    return _("退出");
    default:        return "";
    }
}

static GVariant *item_props(HbTray *t, int id)
{
    GVariantBuilder b;
    char buf[64];

    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (id == M_SEP) {
        g_variant_builder_add(&b, "{sv}", "type",
                              g_variant_new_string("separator"));
        g_variant_builder_add(&b, "{sv}", "visible",
                              g_variant_new_boolean(TRUE));
        return g_variant_builder_end(&b);
    }
    if (id == 0) {
        g_variant_builder_add(&b, "{sv}", "children-display",
                              g_variant_new_string("submenu"));
        return g_variant_builder_end(&b);
    }
    g_variant_builder_add(&b, "{sv}", "type",
                          g_variant_new_string("standard"));
    g_variant_builder_add(&b, "{sv}", "label",
        g_variant_new_string(item_label(t, id, buf, sizeof buf)));
    g_variant_builder_add(&b, "{sv}", "enabled",
        g_variant_new_boolean(id != M_COUNT && (id != M_DISCONN ||
                                               t->online > 0)));
    g_variant_builder_add(&b, "{sv}", "visible",
                          g_variant_new_boolean(TRUE));
    return g_variant_builder_end(&b);
}

static GVariant *item_struct(HbTray *t, int id, gboolean with_children)
{
    GVariantBuilder kids;
    static const int MENU_IDS[] = { M_SHOW, M_COUNT, M_DISCONN,
                                    M_SEP, M_QUIT };
    size_t i;

    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    if (id == 0 && with_children) {
        for (i = 0; i < G_N_ELEMENTS(MENU_IDS); i++)
            g_variant_builder_add(&kids, "v",
                item_struct(t, MENU_IDS[i], FALSE));
    }
    return g_variant_new("(i@a{sv}av)", id, item_props(t, id), &kids);
}

static GVariant *item_prop_one(HbTray *t, int id, const char *name)
{
    GVariant *props = item_props(t, id), *v;
    v = g_variant_lookup_value(props, name, NULL);
    g_variant_unref(props);
    return v;
}

/* ------------------------------ menu methods ---------------------------- */

static void menu_call(GDBusConnection *c, const gchar *sender,
                      const gchar *op, const gchar *iface,
                      const gchar *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer ud)
{
    HbTray *t = ud;
    (void)c; (void)sender; (void)op; (void)iface;

    if (!strcmp(method, "GetLayout")) {
        gint parent, depth;
        g_variant_get(params, "(ii^as)", &parent, &depth, NULL);
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(u@(ia{sv}av))", t->revision,
                          item_struct(t, parent, depth != 0)));
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariantIter *it;
        GVariantBuilder out;
        gint id;
        g_variant_get(params, "(aias)", &it, NULL);
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        while (g_variant_iter_next(it, "i", &id)) {
            if (id >= 0 && id <= M_QUIT)
                g_variant_builder_add(&out, "(i@a{sv})",
                                      id, item_props(t, id));
        }
        g_variant_iter_free(it);
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(a(ia{sv}))", &out));
    } else if (!strcmp(method, "GetProperty")) {
        const char *name;
        gint id;
        GVariant *v = NULL;
        g_variant_get(params, "(i&s)", &id, &name);
        if (id >= 0 && id <= M_QUIT)
            v = item_prop_one(t, id, name);
        if (v)
            g_dbus_method_invocation_return_value(inv,
                g_variant_new("(v)", v));
        else
            g_dbus_method_invocation_return_error(inv,
                G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                "no such item/property");
    } else if (!strcmp(method, "Event")) {
        const char *evid;
        gint id;
        g_variant_get(params, "(i&svu)", &id, &evid, NULL, NULL);
        if (!strcmp(evid, "clicked")) {
            if (id == M_SHOW && t->hooks.activate)
                t->hooks.activate(t->hooks.ud);
            else if (id == M_DISCONN && t->hooks.disconnect_all)
                t->hooks.disconnect_all(t->hooks.ud);
            else if (id == M_QUIT && t->hooks.quit)
                t->hooks.quit(t->hooks.ud);
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter *it;
        GVariantBuilder errs;
        gint id;
        const char *evid;
        GVariant *data;
        guint32 ts;
        g_variant_get(params, "(a(isvu))", &it);
        g_variant_builder_init(&errs, G_VARIANT_TYPE("ai"));
        while (g_variant_iter_loop(it, "(i&svu)", &id, &evid,
                                   &data, &ts)) {
            if (!strcmp(evid, "clicked")) {
                if (id == M_SHOW && t->hooks.activate)
                    t->hooks.activate(t->hooks.ud);
                else if (id == M_DISCONN && t->hooks.disconnect_all)
                    t->hooks.disconnect_all(t->hooks.ud);
                else if (id == M_QUIT && t->hooks.quit)
                    t->hooks.quit(t->hooks.ud);
            }
        }
        g_variant_iter_free(it);
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(ai)", &errs));
    } else if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(b)", FALSE));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(@ai)",
                g_variant_new_array(G_VARIANT_TYPE("ai"), NULL, 0)));
    } else {
        g_dbus_method_invocation_return_error(inv,
            G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
    }
}

static GVariant *menu_prop_get(GDBusConnection *c, const gchar *sender,
                               const gchar *op, const gchar *iface,
                               const gchar *prop, GError **err,
                               gpointer ud)
{
    (void)c; (void)sender; (void)op; (void)iface; (void)ud;
    if (!strcmp(prop, "Version"))
        return g_variant_new_uint32(4);
    if (!strcmp(prop, "TextDirection"))
        return g_variant_new_string("ltr");
    if (!strcmp(prop, "Status"))
        return g_variant_new_string("normal");
    if (!strcmp(prop, "IconThemePath")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        if (((HbTray *)ud)->icon_theme_path)
            g_variant_builder_add(&b, "s",
                ((HbTray *)ud)->icon_theme_path);
        return g_variant_builder_end(&b);
    }
    g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "%s", prop);
    return NULL;
}

/* ------------------------------ SNI ------------------------------------- */

static char *find_icon_theme_path(void)
{
    /* 未安装运行时让 SNI host 仍能解析 IconName：找含我们
     * hicolor/scalable/apps/dev.harbor.Harbor.svg 的 icons 根。 */
    const char *env = g_getenv("APPDIR");
    char *exe = NULL, *dir = NULL;
    char *cand[4] = {0}, *hit = NULL;
    int i;

    if (env)
        cand[0] = g_build_filename(env, "usr/share/icons", NULL);
    exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        char *d = g_path_get_dirname(exe);
        /* AppImage 内部 / 装好的 prefix/bin */
        cand[1] = g_build_filename(d, "..", "share", "icons", NULL);
        /* dev build: <repo>/build/linux/harbor → <repo>/linux/data/icons */
        cand[2] = g_build_filename(d, "..", "..", "linux", "data",
                                   "icons", NULL);
        g_free(d);
    }
    for (i = 0; i < 4; i++) {
        char *svg;
        if (!cand[i])
            continue;
        svg = g_build_filename(cand[i], "hicolor", "scalable", "apps",
                               "dev.harbor.Harbor.svg", NULL);
        if (g_file_test(svg, G_FILE_TEST_EXISTS)) {
            char *abs = realpath(cand[i], NULL);
            hit = abs ? abs : g_steal_pointer(&cand[i]);
            g_free(svg);
            break;
        }
        g_free(svg);
    }
    for (i = 0; i < 4; i++)
        g_free(cand[i]);
    g_free(exe);
    g_free(dir);
    return hit;
}

static GVariant *render_icon_pixmap(int size)
{
    /* IconPixmap: (i width, i height, ay argb32)。经 icon theme 渲染
     * dev.harbor.Harbor；未安装/找不到则返回空数组，host 退回
     * IconName/IconThemePath。 */
    GVariantBuilder arr;
    GtkIconTheme *theme;
    GtkIconPaintable *ip;
    GdkTexture *tex = NULL;
    GtkSnapshot *snap;
    GskRenderNode *node;
    GskRenderer *ren;
    graphene_rect_t bounds;
    gsize nb;
    guchar *px;

    g_variant_builder_init(&arr, G_VARIANT_TYPE("a(iiay)"));
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (!gtk_icon_theme_has_icon(theme, "dev.harbor.Harbor"))
        return g_variant_builder_end(&arr);
    ip = gtk_icon_theme_lookup_icon(theme, "dev.harbor.Harbor",
        NULL, size, 1, GTK_TEXT_DIR_LTR,
        GTK_ICON_LOOKUP_PRELOAD);
    if (!ip)
        return g_variant_builder_end(&arr);

    snap = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(ip), snap,
                           (double)size, (double)size);
    node = gtk_snapshot_free_to_node(snap);
    if (node) {
        graphene_rect_init(&bounds, 0, 0, size, size);
        /* cairo renderer 可在无窗口 surface 时离屏渲染 */
        ren = gsk_cairo_renderer_new();
        gsk_renderer_realize(ren, NULL, NULL);
        tex = gsk_renderer_render_texture(ren, node, &bounds);
        gsk_render_node_unref(node);
        gsk_renderer_unrealize(ren);
        g_object_unref(ren);
    }
    g_object_unref(ip);
    if (!tex)
        return g_variant_builder_end(&arr);

    nb = (gsize)size * size * 4;
    px = g_malloc(nb);
    gdk_texture_download(tex, px, size * 4);
    g_object_unref(tex);
    /* ARGB32 big-endian per spec（D-Bus pixmap 历来网络序 ARGB） */
    {
        gsize i;
        for (i = 0; i < (gsize)size * size; i++) {
            guchar r = px[i*4], g = px[i*4+1], b = px[i*4+2],
                   a = px[i*4+3];
            px[i*4]   = a; px[i*4+1] = r;
            px[i*4+2] = g; px[i*4+3] = b;
        }
    }
    {
        GVariant *bytes = g_variant_new_from_data(
            G_VARIANT_TYPE("ay"), px, nb, TRUE, g_free, px);
        g_variant_builder_add(&arr, "(ii@ay)", size, size, bytes);
    }
    return g_variant_builder_end(&arr);
}

static GVariant *tooltip_variant(HbTray *t)
{
    char title[64];
    g_snprintf(title, sizeof title, ngettext("Harbor · %d 个会话在线", "Harbor · %d 个会话在线",
               t->online), t->online);
    return g_variant_new("(s@a(iiay)ss)", "dev.harbor.Harbor",
                         render_icon_pixmap(24), title, "");
}

static void emit_tooltip(HbTray *t)
{
    if (!t->bus || !t->registered)
        return;
    g_dbus_connection_emit_signal(t->bus, NULL, SNI_PATH,
        "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL);
    g_dbus_connection_emit_signal(t->bus, NULL, MENU_PATH,
        "com.canonical.dbusmenu", "LayoutUpdated",
        g_variant_new("(ui)", ++t->revision, 0), NULL);
}

static void sni_call(GDBusConnection *c, const gchar *sender,
                     const gchar *op, const gchar *iface,
                     const gchar *method, GVariant *params,
                     GDBusMethodInvocation *inv, gpointer ud)
{
    HbTray *t = ud;
    (void)c; (void)sender; (void)op; (void)iface; (void)params;
    if (!strcmp(method, "Activate") ||
        !strcmp(method, "SecondaryActivate") ||
        !strcmp(method, "ContextMenu")) {
        if (!strcmp(method, "Activate") && t->hooks.activate)
            t->hooks.activate(t->hooks.ud);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "Scroll")) {
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv,
            G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
    }
}

static GVariant *sni_prop_get(GDBusConnection *c, const gchar *sender,
                              const gchar *op, const gchar *iface,
                              const gchar *prop, GError **err,
                              gpointer ud)
{
    HbTray *t = ud;
    (void)c; (void)sender; (void)op; (void)iface;
    if (!strcmp(prop, "Category"))
        return g_variant_new_string("ApplicationStatus");
    if (!strcmp(prop, "Id"))
        return g_variant_new_string("dev.harbor.Harbor");
    if (!strcmp(prop, "Title"))
        return g_variant_new_string("Harbor");
    if (!strcmp(prop, "Status"))
        return g_variant_new_string("Active");
    if (!strcmp(prop, "WindowId"))
        return g_variant_new_uint32(0);
    if (!strcmp(prop, "IconName"))
        return g_variant_new_string("dev.harbor.Harbor");
    if (!strcmp(prop, "IconPixmap"))
        return render_icon_pixmap(48);
    if (!strcmp(prop, "OverlayIconName") ||
        !strcmp(prop, "AttentionIconName"))
        return g_variant_new_string("");
    if (!strcmp(prop, "AttentionIconPixmap"))
        return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
    if (!strcmp(prop, "ToolTip"))
        return tooltip_variant(t);
    if (!strcmp(prop, "Menu"))
        return g_variant_new_object_path(MENU_PATH);
    if (!strcmp(prop, "ItemIsMenu"))
        return g_variant_new_boolean(FALSE);
    if (!strcmp(prop, "IconThemePath"))
        return g_variant_new_string(t->icon_theme_path ?
                                    t->icon_theme_path : "");
    g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "%s", prop);
    return NULL;
}

/* ------------------------- watcher registration ------------------------- */

static void register_done(GObject *src, GAsyncResult *res, gpointer ud);

static void tray_register(HbTray *t)
{
    if (!t->bus || t->registered)
        return;
    g_dbus_connection_call(t->bus, WATCHER, WATCHER_OBJ, WATCHER,
        "RegisterStatusNotifierItem",
        g_variant_new("(s)",
            g_dbus_connection_get_unique_name(t->bus)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        register_done, t);
}

static gboolean register_retry(gpointer ud)
{
    HbTray *t = ud;
    t->register_src = 0;
    /* watcher 名已拿到但其对象可能还没 export（NOC 先于
     * register_object 到达）：有界重试到对象就绪 */
    if (!t->watcher_up || t->registered || t->register_tries >= 10)
        return G_SOURCE_REMOVE;
    t->register_tries++;
    tray_register(t);
    return G_SOURCE_REMOVE;
}

static void register_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbTray *t = ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                res, &err);
    if (r) {
        t->registered = TRUE;
        t->register_tries = 0;
        t->online = t->hooks.count_online ?
                    t->hooks.count_online(t->hooks.ud) : 0;
        g_variant_unref(r);
        if (t->watcher_cb)
            t->watcher_cb(TRUE, t->watcher_ud);
        g_dbus_connection_emit_signal(t->bus, NULL, SNI_PATH,
            "org.kde.StatusNotifierItem", "NewStatus",
            g_variant_new("(s)", "Active"), NULL);
    } else {
        g_printerr("harbor: tray register failed: %s\n",
                   err->message);
        g_error_free(err);
        if (t->watcher_up && !t->register_src)
            t->register_src =
                g_timeout_add(300, register_retry, t);
    }
}

static void watcher_cb(GDBusConnection *c, const gchar *sender,
                       const gchar *op, const gchar *iface,
                       const gchar *sig, GVariant *params,
                       gpointer ud)
{
    HbTray *t = ud;
    const char *name, *oldw, *neww;
    (void)c; (void)sender; (void)op; (void)iface; (void)params;

    if (g_strcmp0(sig, "NameOwnerChanged") != 0)
        return;
    g_variant_get(params, "(&s&s&s)", &name, &oldw, &neww);
    if (g_strcmp0(name, WATCHER) != 0)
        return;

    if (*neww && !t->watcher_up) {
        t->watcher_up = TRUE;
        t->register_tries = 0;
        tray_register(t);
    } else if (!*neww && t->watcher_up) {
        gboolean had = t->registered;
        t->watcher_up = FALSE;
        t->registered = FALSE;
        if (t->register_src) {
            g_source_remove(t->register_src);
            t->register_src = 0;
        }
        if (had && t->watcher_cb)
            t->watcher_cb(FALSE, t->watcher_ud);
    }
}

/* ------------------------------ lifecycle ------------------------------- */

static void bus_ready(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbTray *t = ud;
    (void)src;
    GError *err = NULL;
    static const GDBusInterfaceVTable sni_vt = {
        sni_call, sni_prop_get, NULL, {0} };
    static const GDBusInterfaceVTable menu_vt = {
        menu_call, menu_prop_get, NULL, {0} };

    t->bus = g_bus_get_finish(res, &err);
    if (!t->bus) {
        g_printerr("harbor: tray: no session bus: %s\n", err->message);
        g_error_free(err);
        return;
    }
    t->sni_reg = g_dbus_connection_register_object(t->bus, SNI_PATH,
        t->sni_info->interfaces[0], &sni_vt, t, NULL, &err);
    if (!t->sni_reg) {
        g_printerr("harbor: tray SNI object: %s\n", err->message);
        g_clear_error(&err);
    }
    t->menu_reg = g_dbus_connection_register_object(t->bus, MENU_PATH,
        t->menu_info->interfaces[0], &menu_vt, t, NULL, &err);
    if (!t->menu_reg) {
        g_printerr("harbor: tray menu object: %s\n", err->message);
        g_clear_error(&err);
    }
    /* watcher 名字出现/消失：信号订阅（own_name 前也能收到） */
    t->watch_id = g_dbus_connection_signal_subscribe(t->bus,
        "org.freedesktop.DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", "/org/freedesktop/DBus", WATCHER,
        G_DBUS_SIGNAL_FLAGS_NONE, watcher_cb, t, NULL);
    /* 若 watcher 已在，立刻注册一次 */
    if (g_dbus_connection_get_unique_name(t->bus)) {
        GVariant *r;
        r = g_dbus_connection_call_sync(t->bus,
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner",
            g_variant_new("(s)", WATCHER), NULL,
            G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
        if (r) {
            gboolean has;
            g_variant_get(r, "(b)", &has);
            g_variant_unref(r);
            if (has) {
                t->watcher_up = TRUE;
                tray_register(t);
            }
        }
    }
}

static void name_acquired(GDBusConnection *c, const gchar *name,
                          gpointer ud)
{
    HbTray *t = ud;
    (void)c;
    g_free(t->bus_name);
    t->bus_name = g_strdup(name);
}

static gboolean poll_sessions(gpointer ud)
{
    hb_tray_sessions_changed(ud);
    return G_SOURCE_CONTINUE;
}

HbTray *hb_tray_new(const HbTrayHooks *hooks)
{
    HbTray *t = g_new0(HbTray, 1);
    GError *err = NULL;

    t->hooks = *hooks;
    t->icon_theme_path = find_icon_theme_path();
    t->sni_info = g_dbus_node_info_new_for_xml(SNI_XML, &err);
    if (!t->sni_info)
        g_error("tray SNI xml: %s", err->message);
    t->menu_info = g_dbus_node_info_new_for_xml(MENU_XML, &err);
    if (!t->menu_info)
        g_error("tray menu xml: %s", err->message);

    /* SNI host 习惯用 well-known 名注册 item */
    {
        char *nm = g_strdup_printf("org.kde.StatusNotifierItem-%d-1",
                                   (int)getpid());
        t->name_own = g_bus_own_name(G_BUS_TYPE_SESSION, nm,
            G_BUS_NAME_OWNER_FLAGS_NONE, NULL, name_acquired, NULL,
            t, NULL);
        g_free(nm);
    }
    g_bus_get(G_BUS_TYPE_SESSION, NULL, bus_ready, t);
    /* 安全网：转发启停/SFTP 断连等不经过 tab dot 的变化，2s 兜底刷新 */
    g_timeout_add_seconds(2, poll_sessions, t);
    return t;
}

void hb_tray_free(HbTray *t)
{
    if (!t)
        return;
    if (t->register_src)
        g_source_remove(t->register_src);
    if (t->bus) {
        if (t->watch_id)
            g_dbus_connection_signal_unsubscribe(t->bus, t->watch_id);
        if (t->sni_reg)
            g_dbus_connection_unregister_object(t->bus, t->sni_reg);
        if (t->menu_reg)
            g_dbus_connection_unregister_object(t->bus, t->menu_reg);
        g_object_unref(t->bus);
    }
    if (t->name_own)
        g_bus_unown_name(t->name_own);
    if (t->sni_info)
        g_dbus_node_info_unref(t->sni_info);
    if (t->menu_info)
        g_dbus_node_info_unref(t->menu_info);
    g_free(t->bus_name);
    g_free(t->icon_theme_path);
    g_free(t);
}

gboolean hb_tray_available(HbTray *t)
{
    return t && t->registered;
}

int hb_tray_online(HbTray *t)
{
    return t ? t->online : 0;
}

void hb_tray_set_watcher_cb(HbTray *t,
                            void (*cb)(gboolean, void *), void *ud)
{
    t->watcher_cb = cb;
    t->watcher_ud = ud;
}

void hb_tray_sessions_changed(HbTray *t)
{
    int n;
    if (!t)
        return;
    n = t->hooks.count_online ? t->hooks.count_online(t->hooks.ud) : 0;
    if (n != t->online) {
        t->online = n;
        emit_tooltip(t);
    }
}
