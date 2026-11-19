#include "hb-icons.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

/* SVG path 表：与 prototype/app.js 的 P 表逐键对应（stroke 图标，
 * 内嵌 fill="currentColor" 在渲染时替换为目标色）。 */
static const char *const ICON_PATHS[] = {
    "server", "<rect x=\"3\" y=\"4\" width=\"18\" height=\"7\" rx=\"2\"/><rect x=\"3\" y=\"13\" width=\"18\" height=\"7\" rx=\"2\"/><path d=\"M7 7.5h.01M7 16.5h.01\"/>",
    "key", "<circle cx=\"8\" cy=\"15.5\" r=\"4\"/><path d=\"M10.9 12.6 20 3.5M15 8.5l3 3M18 5.5l2 2\"/>",
    "fwd", "<path d=\"M4 7.5h13.5l-3.2-3.2M20 16.5H6.5l3.2 3.2\"/>",
    "fingerprint", "<path d=\"M6.5 11a5.5 5.5 0 0 1 11 0v3.5M9.2 11a2.8 2.8 0 0 1 5.6 0v4a7 7 0 0 1-1 3.6M12 11v5.2a10 10 0 0 1-1.4 4.6M6.5 14.5a13 13 0 0 1-.7 3.7M17.5 17a12 12 0 0 1-.6 2.6M4 8.5A8.5 8.5 0 0 1 12 4a8.4 8.4 0 0 1 6.6 3.2\"/>",
    "clock", "<circle cx=\"12\" cy=\"12\" r=\"8.6\"/><path d=\"M12 7.2v4.8l3.1 2\"/>",
    "broadcast", "<circle cx=\"12\" cy=\"12\" r=\"2\"/><path d=\"M8.6 8.6a4.8 4.8 0 0 0 0 6.8M15.4 8.8a4.8 4.8 0 0 1 0 6.8M5.8 5.8a8.8 8.8 0 0 0 0 12.4M18.2 5.8a8.8 8.8 0 0 1 0 12.4\"/>",
    "cloud", "<path d=\"M7 18a4.5 4.5 0 1 1 .7-8.95A6 6 0 0 1 19.2 10.6 3.8 3.8 0 0 1 18.3 18H7z\"/>",
    "bell", "<path d=\"M18 9.5a6 6 0 1 0-12 0c0 5.5-2.3 6.8-2.3 6.8h16.6S18 15 18 9.5M10.4 19.8a1.9 1.9 0 0 0 3.2 0\"/>",
    "panel", "<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"M9.5 4v16\"/>",
    "search", "<circle cx=\"11\" cy=\"11\" r=\"6.8\"/><path d=\"m20.3 20.3-4.4-4.4\"/>",
    "grid", "<rect x=\"4\" y=\"4\" width=\"7\" height=\"7\" rx=\"1.8\"/><rect x=\"13\" y=\"4\" width=\"7\" height=\"7\" rx=\"1.8\"/><rect x=\"4\" y=\"13\" width=\"7\" height=\"7\" rx=\"1.8\"/><rect x=\"13\" y=\"13\" width=\"7\" height=\"7\" rx=\"1.8\"/>",
    "tag", "<path d=\"M3.5 10.8V4.5a1 1 0 0 1 1-1h6.3L20.5 13l-7.5 7.5L3.5 10.8z\"/><circle cx=\"8\" cy=\"8\" r=\"1.4\"/>",
    "list", "<path d=\"M8.5 6h12M8.5 12h12M8.5 18h12M3.8 6h.01M3.8 12h.01M3.8 18h.01\"/>",
    "plug", "<path d=\"M9 3v5M15 3v5M6 8h12v3a6 6 0 0 1-12 0V8zM12 17v4\"/>",
    "cert", "<rect x=\"4\" y=\"3\" width=\"16\" height=\"13.5\" rx=\"2\"/><path d=\"M8 7h8M8 10.5h4.5\"/><circle cx=\"15.8\" cy=\"16.8\" r=\"2.8\"/><path d=\"M14.4 19.2 13.8 22l2-1.1 2 1.1-.6-2.8\"/>",
    "fido", "<rect x=\"5\" y=\"10\" width=\"14\" height=\"10\" rx=\"2.5\"/><path d=\"M8.2 10V7.2a3.8 3.8 0 0 1 7.6 0V10M12 14v2.6\"/>",
    "lock", "<rect x=\"5\" y=\"10\" width=\"14\" height=\"10\" rx=\"2.5\"/><path d=\"M8.2 10V7.2a3.8 3.8 0 0 1 7.6 0V10M12 14v2.6\"/>",
    "folder", "<path d=\"M3 7a2 2 0 0 1 2-2h4.2l2 2.5H19a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7z\"/>",
    "file", "<path d=\"M6 3h8l4 4v14H6V3zM14 3v4h4M9.2 12.5h5.6M9.2 16h5.6\"/>",
    "ubuntu", "<circle cx=\"12\" cy=\"12\" r=\"8.2\" stroke-dasharray=\"11.5 5.7\"/><circle cx=\"12\" cy=\"3.8\" r=\"1.9\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"4.9\" cy=\"16.1\" r=\"1.9\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"19.1\" cy=\"16.1\" r=\"1.9\" fill=\"currentColor\" stroke=\"none\"/>",
    "asterisk", "<path d=\"M12 4.5v15M5.5 8.25l13 7.5M18.5 8.25l-13 7.5\"/>",
    "win", "<path d=\"M4 4h7.4v7.4H4zM12.6 4H20v7.4h-7.4zM4 12.6h7.4V20H4zM12.6 12.6H20V20h-7.4z\" fill=\"currentColor\" stroke=\"none\"/>",
    "monitor", "<rect x=\"3\" y=\"4\" width=\"18\" height=\"12\" rx=\"2\"/><path d=\"M9 20h6M12 16v4\"/>",
    "terminal", "<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"m7 9 3 3-3 3M13 15h4\"/>",
    "cols", "<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"M12 4v16\"/>",
    "laptop", "<rect x=\"4.5\" y=\"5\" width=\"15\" height=\"10\" rx=\"1.8\"/><path d=\"M2.5 19h19l-1.8-3.4H4.3L2.5 19z\"/>",
    "phone", "<rect x=\"7\" y=\"3\" width=\"10\" height=\"18\" rx=\"2.6\"/><path d=\"M11 17.8h2\"/>",
    "check", "<path d=\"m4.8 12.6 4.8 4.8L19.4 6.6\"/>",
    "x", "<path d=\"M6 6l12 12M18 6 6 18\"/>",
    "info", "<circle cx=\"12\" cy=\"12\" r=\"8.6\"/><path d=\"M12 11v5M12 7.8h.01\"/>",
    "user", "<circle cx=\"12\" cy=\"8\" r=\"3.8\"/><path d=\"M4.5 20.5c1.4-3.8 4.2-5.7 7.5-5.7s6.1 1.9 7.5 5.7\"/>",
    "eye", "<path d=\"M2.8 12S6.2 5.8 12 5.8 21.2 12 21.2 12 17.8 18.2 12 18.2 2.8 12 2.8 12z\"/><circle cx=\"12\" cy=\"12\" r=\"2.9\"/>",
    "focus", "<path d=\"M4 9V5.5A1.5 1.5 0 0 1 5.5 4H9M15 4h3.5A1.5 1.5 0 0 1 20 5.5V9M20 15v3.5a1.5 1.5 0 0 1-1.5 1.5H15M9 20H5.5A1.5 1.5 0 0 1 4 18.5V15\"/>",
    "wand", "<path d=\"M4.5 19.5 15 9M15.5 3.5v4M13.5 5.5h4M19 11.5v3M17.5 13h3\"/>",
    "palette", "<path d=\"M12 3a9 9 0 1 0 0 18h1.6a2 2 0 0 0 1.4-3.4c-.8-.8-.2-2.1 1-2.1h1.9A3.6 3.6 0 0 0 21 11.6 9 9 0 0 0 12 3z\"/><circle cx=\"8\" cy=\"10\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"12\" cy=\"7.3\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"16\" cy=\"10\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/>",
    "refresh", "<path d=\"M20.5 12a8.5 8.5 0 1 1-2.5-6M20.5 3.5V8H16\"/>",
    "bookmark", "<path d=\"M6.5 3.5h11V20.5l-5.5-3.8-5.5 3.8V3.5z\"/>",
    "link", "<path d=\"M10.2 13.8a4.3 4.3 0 0 0 6.1 0l2.9-2.9a4.3 4.3 0 0 0-6.1-6.1l-1.4 1.4M13.8 10.2a4.3 4.3 0 0 0-6.1 0l-2.9 2.9a4.3 4.3 0 0 0 6.1 6.1l1.4-1.4\"/>",
    "globe", "<circle cx=\"12\" cy=\"12\" r=\"8.6\"/><path d=\"M3.4 12h17.2M12 3.4a13.4 13.4 0 0 1 0 17.2 13.4 13.4 0 0 1 0-17.2z\"/>",
    "trash", "<path d=\"M4.5 7h15M9.5 7V4h5v3M6.5 7l1 13.5h9l1-13.5M10.2 11v6M13.8 11v6\"/>",
    "keyboard", "<rect x=\"3\" y=\"7\" width=\"18\" height=\"11\" rx=\"2\"/><path d=\"M6.8 10.8h.01M10.4 10.8h.01M14 10.8h.01M17.4 10.8h.01M7.5 14.4h9\"/>",
    "back", "<path d=\"M19 12H5.5M11 6l-6 6 6 6\"/>",
    "chev", "<path d=\"m9.5 5.5 6.5 6.5-6.5 6.5\"/>",
    "more", "<circle cx=\"12\" cy=\"5.2\" r=\"1.4\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"12\" cy=\"12\" r=\"1.4\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"12\" cy=\"18.8\" r=\"1.4\" fill=\"currentColor\" stroke=\"none\"/>",
    "wifi", "<path d=\"M2.8 9.3a13.5 13.5 0 0 1 18.4 0M5.8 12.6a9.2 9.2 0 0 1 12.4 0M8.9 15.9a4.8 4.8 0 0 1 6.2 0M12 19.2h.01\"/>",
    "battery", "<rect x=\"2.5\" y=\"8\" width=\"16.5\" height=\"8\" rx=\"2\"/><path d=\"M21.8 10.8v2.4\"/><rect x=\"5\" y=\"10.4\" width=\"9.5\" height=\"3.2\" rx=\".8\" fill=\"currentColor\" stroke=\"none\"/>",
    "shield", "<path d=\"M12 3l7.8 2.9v5.9c0 4.8-3.3 7.7-7.8 8.9-4.5-1.2-7.8-4.1-7.8-8.9V5.9L12 3z\"/><path d=\"m9 11.8 2.1 2.1 4-4.4\"/>",
    "plus", "<path d=\"M12 5.5v13M5.5 12h13\"/>",
    NULL,
};

static const char *lookup_path(const char *name)
{
    int i;
    if (!name)
        return "";
    for (i = 0; ICON_PATHS[i]; i += 2)
        if (strcmp(ICON_PATHS[i], name) == 0)
            return ICON_PATHS[i + 1];
    return "";
}

/* ---------------- HbIcon：自绘 SVG 图标 widget ---------------- */

struct _HbIcon {
    GtkWidget parent;
    char *name;
    int   px;
};

G_DEFINE_TYPE(HbIcon, hb_icon, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_NAME, PROP_PIXEL_SIZE, N_PROPS };
static GParamSpec *props[N_PROPS];

/* (name,color,px) → GdkTexture 缓存 */
static GHashTable *icon_cache;

static GdkTexture *render_icon(const char *name, const GdkRGBA *col, int px)
{
    char key[160];
    char hex[8];
    char *svg, *body;
    GBytes *bytes;
    GInputStream *st;
    GdkPixbuf *pb;
    GdkTexture *tex;
    int scale = 2; /* HiDPI 冗余 */

    snprintf(hex, sizeof hex, "#%02x%02x%02x",
             (int)(col->red * 255), (int)(col->green * 255),
             (int)(col->blue * 255));
    snprintf(key, sizeof key, "%s|%s|%d", name, hex, px * scale);
    tex = g_hash_table_lookup(icon_cache, key);
    if (tex)
        return tex;

    {
        /* currentColor → 目标色 */
        char **parts = g_strsplit(lookup_path(name), "currentColor", -1);
        body = g_strjoinv(hex, parts);
        g_strfreev(parts);
    }
    svg = g_strdup_printf(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
        "width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"%s\" "
        "stroke-width=\"1.7\" stroke-linecap=\"round\" "
        "stroke-linejoin=\"round\">%s</svg>",
        px * scale, px * scale, hex, body);
    g_free(body);

    bytes = g_bytes_new_take(svg, strlen(svg));
    st = g_memory_input_stream_new_from_bytes(bytes);
    pb = gdk_pixbuf_new_from_stream_at_scale(st, px * scale, px * scale,
                                             TRUE, NULL, NULL);
    g_object_unref(st);
    g_bytes_unref(bytes);
    if (!pb)
        return NULL;
    tex = gdk_texture_new_for_pixbuf(pb);
    g_object_unref(pb);
    if (tex)
        g_hash_table_insert(icon_cache, g_strdup(key), tex);
    return tex;
}

static void hb_icon_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    HbIcon *self = HB_ICON(w);
    GdkRGBA col;
    GdkTexture *tex;
    int wpx = gtk_widget_get_width(w), hpx = gtk_widget_get_height(w);
    int size = MIN(wpx, hpx);
    graphene_rect_t r;

    gtk_widget_get_color(w, &col);
    tex = render_icon(self->name, &col, self->px);
    if (!tex)
        return;
    graphene_rect_init(&r, (wpx - size) / 2.0f, (hpx - size) / 2.0f,
                       (float)size, (float)size);
    gtk_snapshot_append_texture(snap, tex, &r);
}

static void hb_icon_measure(GtkWidget *w, GtkOrientation o, int for_size,
                            int *min, int *nat, int *minb, int *natb)
{
    HbIcon *self = HB_ICON(w);
    (void)for_size; (void)o;
    *min = *nat = self->px;
    if (minb) *minb = -1;
    if (natb) *natb = -1;
}

static void hb_icon_get_property(GObject *o, guint pid, GValue *v, GParamSpec *ps)
{
    HbIcon *self = HB_ICON(o);
    switch (pid) {
    case PROP_NAME: g_value_set_string(v, self->name); break;
    case PROP_PIXEL_SIZE: g_value_set_int(v, self->px); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, pid, ps);
    }
}

static void hb_icon_set_property(GObject *o, guint pid, const GValue *v,
                                 GParamSpec *ps)
{
    HbIcon *self = HB_ICON(o);
    switch (pid) {
    case PROP_NAME:
        g_free(self->name);
        self->name = g_strdup(g_value_get_string(v));
        gtk_widget_queue_draw(GTK_WIDGET(self));
        break;
    case PROP_PIXEL_SIZE:
        self->px = MAX(1, g_value_get_int(v));
        gtk_widget_queue_resize(GTK_WIDGET(self));
        break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, pid, ps);
    }
}

static void hb_icon_dispose(GObject *o)
{
    HbIcon *self = HB_ICON(o);
    g_free(self->name);
    G_OBJECT_CLASS(hb_icon_parent_class)->dispose(o);
}

static void hb_icon_class_init(HbIconClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    oc->dispose = hb_icon_dispose;
    oc->get_property = hb_icon_get_property;
    oc->set_property = hb_icon_set_property;
    wc->snapshot = hb_icon_snapshot;
    wc->measure = hb_icon_measure;
    props[PROP_NAME] = g_param_spec_string("name", NULL, NULL, "",
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_PIXEL_SIZE] = g_param_spec_int("pixel-size", NULL, NULL,
        1, 256, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(oc, N_PROPS, props);
    icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       g_object_unref);
}

static void hb_icon_init(HbIcon *self)
{
    self->px = 16;
}

GtkWidget *hb_icon_new(const char *name, int px)
{
    return g_object_new(HB_TYPE_ICON, "name", name, "pixel-size", px, NULL);
}

void hb_icon_set_name(HbIcon *ico, const char *name)
{
    g_object_set(ico, "name", name, NULL);
}

void hb_icon_set_pixel_size(HbIcon *ico, int px)
{
    g_object_set(ico, "pixel-size", px, NULL);
}

GtkWidget *hb_mono_icon_new(const char *glyph)
{
    GtkWidget *l = gtk_label_new(glyph);
    gtk_widget_add_css_class(l, "mono-ico");
    return l;
}

/* ---------------- OS 头像 ---------------- */

const char *hb_os_avatar_key(const char *os_or_proto)
{
    if (!os_or_proto || !*os_or_proto)
        return "generic";
    if (!strcmp(os_or_proto, "ubuntu"))   return "ubuntu";
    if (!strcmp(os_or_proto, "debian"))   return "debian";
    if (!strcmp(os_or_proto, "local"))    return "local";
    if (!strcmp(os_or_proto, "vnc"))      return "vnc";
    if (!strcmp(os_or_proto, "rdp") ||
        !strcmp(os_or_proto, "windows") ||
        !strcmp(os_or_proto, "win"))      return "rdp";
    if (!strcmp(os_or_proto, "ssh") ||
        !strcmp(os_or_proto, "generic") ||
        !strcmp(os_or_proto, "auto"))     return "generic";
    return "generic";
}

GtkWidget *hb_os_avatar_new(const char *os_or_proto, int px)
{
    const char *key = hb_os_avatar_key(os_or_proto);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *glyph;
    int gpx = px >= 38 ? 20 : MAX(10, px * 53 / 100);

    gtk_widget_add_css_class(box, "hb-os-avatar");
    gtk_widget_add_css_class(box, key);
    if (px <= 26)
        gtk_widget_add_css_class(box, "sm");
    gtk_widget_set_size_request(box, px, px);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    /* keep the box at its natural size even though the glyph expands */
    gtk_widget_set_hexpand_set(box, TRUE);
    gtk_widget_set_hexpand(box, FALSE);
    gtk_widget_set_vexpand_set(box, TRUE);
    gtk_widget_set_vexpand(box, FALSE);

    if (!strcmp(key, "local"))
        glyph = hb_mono_icon_new(">_");
    else if (!strcmp(key, "vnc"))
        glyph = hb_icon_new("monitor", gpx);
    else if (!strcmp(key, "rdp") || !strcmp(key, "win"))
        glyph = hb_icon_new("win", gpx);
    else if (!strcmp(key, "ubuntu"))
        glyph = hb_icon_new("ubuntu", gpx);
    else
        glyph = hb_icon_new("asterisk", gpx);
    gtk_widget_set_halign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(glyph, TRUE);
    gtk_box_append(GTK_BOX(box), glyph);
    return box;
}
