#include "hb-hosts-view.h"
#include "hb-icons.h"

#include <stdio.h>

/* ------------------------- HbHostItem（GObject 包装 hb_obj） ------------------------- */

#define HB_TYPE_HOST_ITEM (hb_host_item_get_type())
G_DECLARE_FINAL_TYPE(HbHostItem, hb_host_item, HB, HOST_ITEM, GObject)

struct _HbHostItem {
    GObject parent;
    char *id, *name, *address, *user, *proto, *os, *group, *via;
    int64_t port, last_connected, order;
    gboolean tun, local;
};

G_DEFINE_TYPE(HbHostItem, hb_host_item, G_TYPE_OBJECT)

static HbHostItem *host_item_new(hb_obj *o)
{
    HbHostItem *h = g_object_new(HB_TYPE_HOST_ITEM, NULL);
    const char *via = hb_obj_get_str(o, "via");
    h->id = g_strdup(hb_obj_id(o));
    h->name = g_strdup(hb_obj_get_str(o, "name"));
    h->address = g_strdup(hb_obj_get_str(o, "address"));
    h->user = g_strdup(hb_obj_get_str(o, "user"));
    h->proto = g_strdup(hb_obj_get_str(o, "proto"));
    h->os = g_strdup(hb_obj_get_str(o, "os"));
    h->group = g_strdup(hb_obj_get_str(o, "group"));
    h->via = g_strdup(via);
    h->port = hb_obj_get_int(o, "port", 0);
    h->last_connected = hb_obj_get_int(o, "last_connected", 0);
    h->order = hb_obj_get_int(o, "order", 0);
    h->tun = (via && *via) || hb_obj_get_bool(o, "tunnel", false);
    h->local = h->proto && !strcmp(h->proto, "local");
    return h;
}

static void hb_host_item_finalize(GObject *o)
{
    HbHostItem *h = HB_HOST_ITEM(o);
    g_free(h->id); g_free(h->name); g_free(h->address); g_free(h->user);
    g_free(h->proto); g_free(h->os); g_free(h->group); g_free(h->via);
    G_OBJECT_CLASS(hb_host_item_parent_class)->finalize(o);
}
static void hb_host_item_class_init(HbHostItemClass *k)
{ G_OBJECT_CLASS(k)->finalize = hb_host_item_finalize; }
static void hb_host_item_init(HbHostItem *h) { (void)h; }

/* ------------------------------- 视图 ------------------------------- */

enum { SORT_NAME, SORT_RECENT, SORT_PROTO };

struct _HbHostsView {
    GtkBox parent;
    HbStore *store;            /* borrowed */

    GtkWidget *search;
    GtkWidget *title_lbl;
    GtkWidget *chiprow;
    GtkWidget *body;           /* stack: grid/list/empty */
    GtkWidget *grid, *list;
    GtkWidget *btn_grid, *btn_list;

    GListStore *all;           /* HbHostItem */
    GtkFilterListModel *filtered;
    GtkSortListModel *sorted;
    GtkCustomFilter *filter;
    GtkCustomSorter *sorter;

    char *search_text;         /* folded */
    char *chip;                /* NULL=全部, "tun", or group name */
    int   sort_mode;
    gboolean list_mode;
    gulong changed_handler;
};

G_DEFINE_TYPE(HbHostsView, hb_hosts_view, GTK_TYPE_BOX)

/* ------------------------------ 过滤/排序 ------------------------------ */

static char *fold(const char *s)
{
    return s ? g_utf8_casefold(s, -1) : g_strdup("");
}

static gboolean filter_fn(gpointer item, gpointer ud)
{
    HbHostsView *hv = ud;
    HbHostItem *h = item;

    if (hv->chip) {
        if (!strcmp(hv->chip, "tun")) {
            if (!h->tun) return FALSE;
        } else if (g_strcmp0(h->group, hv->chip)) {
            return FALSE;
        }
    }
    if (hv->search_text && *hv->search_text) {
        char *fn = fold(h->name), *fa = fold(h->address),
             *fu = fold(h->user), *fg = fold(h->group),
             *fp = fold(h->proto);
        gboolean hit = strstr(fn, hv->search_text) ||
                       strstr(fa, hv->search_text) ||
                       strstr(fu, hv->search_text) ||
                       strstr(fg, hv->search_text) ||
                       strstr(fp, hv->search_text);
        g_free(fn); g_free(fa); g_free(fu); g_free(fg); g_free(fp);
        if (!hit) return FALSE;
    }
    return TRUE;
}

static int sort_fn(gconstpointer a, gconstpointer b, gpointer ud)
{
    HbHostsView *hv = ud;
    const HbHostItem *x = a, *y = b;
    int r = 0;
    switch (hv->sort_mode) {
    case SORT_RECENT:
        r = (x->last_connected < y->last_connected) ? 1 :
            (x->last_connected > y->last_connected) ? -1 : 0;
        break;
    case SORT_PROTO:
        r = g_strcmp0(x->proto, y->proto);
        break;
    default: break;
    }
    if (!r)
        r = g_utf8_collate(x->name ? x->name : "", y->name ? y->name : "");
    if (!r)
        r = (x->order < y->order) ? -1 : (x->order > y->order);
    return r;
}

static void update_body_and_title(HbHostsView *hv)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(hv->filtered));
    char buf[64];
    snprintf(buf, sizeof buf, "Hosts · %u", n);
    gtk_label_set_label(GTK_LABEL(hv->title_lbl), buf);
    if (!n) {
        gtk_stack_set_visible_child_name(GTK_STACK(hv->body), "empty");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(hv->body),
                                       hv->list_mode ? "list" : "grid");
    }
}

/* 头像 key：proto ∈ {local,rdp,vnc} 时优先取 proto，否则取 os */
static const char *host_avatar_key(const HbHostItem *h)
{
    if (h->proto && (!strcmp(h->proto, "local") ||
                     !strcmp(h->proto, "rdp") ||
                     !strcmp(h->proto, "vnc")))
        return h->proto;
    if (h->os && *h->os && strcmp(h->os, "auto"))
        return h->os;
    return "generic";
}

/* via 主机名：按对象 id 在全量列表里查 */
static const char *via_host_name(HbHostsView *hv, const HbHostItem *h)
{
    GListModel *m = G_LIST_MODEL(hv->all);
    guint i, n = g_list_model_get_n_items(m);
    for (i = 0; i < n; i++) {
        HbHostItem *o = g_list_model_get_item(m, i);
        gboolean hit = o != (HbHostItem *)h &&
                       !g_strcmp0(o->id, h->via);
        const char *name = hit ? o->name : NULL;
        g_object_unref(o);
        if (hit)
            return name;
    }
    return NULL;
}

/* 原型 h.sub 规则 */
static void host_sub(HbHostsView *hv, const HbHostItem *h,
                     char *buf, size_t buflen)
{
    size_t off = 0;
    if (h->via && *h->via) {
        const char *vn = via_host_name(hv, h);
        off = (size_t)snprintf(buf, buflen, "%s · 经由 %s 隧道",
                               h->proto ? h->proto : "",
                               vn ? vn : "?");
    } else if (h->local) {
        const char *sh = g_getenv("SHELL");
        const char *base = sh ? g_strrstr(sh, "/") : NULL;
        base = base ? base + 1 : (sh && *sh ? sh : "sh");
        off = (size_t)snprintf(buf, buflen, "本机 shell · %s", base);
    } else {
        off = (size_t)snprintf(buf, buflen, "%s, %s",
                               h->proto ? h->proto : "",
                               h->user ? h->user : "");
    }
    if (h->group && *h->group && off < buflen)
        snprintf(buf + off, buflen - off, " · %s", h->group);
}

/* ------------------------------ 卡片/行构建 ------------------------------ */

static void on_card_motion(GtkEventControllerMotion *m, double x, double y,
                           gpointer ud)
{
    (void)m; (void)x; (void)y;
    gtk_widget_set_visible(GTK_WIDGET(ud), TRUE);
}
static void on_card_leave(GtkEventControllerMotion *m, gpointer ud)
{
    (void)m;
    gtk_widget_set_visible(GTK_WIDGET(ud), FALSE);
}

/* 卡片内容（bind 时填充） */
static GtkWidget *card_shell(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *av = hb_os_avatar_new("generic", 38);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name = gtk_label_new("");
    GtkWidget *tags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sub = gtk_label_new("");
    GtkWidget *go = gtk_button_new();
    GtkEventController *mo = gtk_event_controller_motion_new();

    gtk_widget_add_css_class(card, "hb-host-card");
    gtk_widget_set_size_request(card, 260, -1);
    gtk_widget_set_margin_start(card, 6);
    gtk_widget_set_margin_end(card, 6);
    gtk_widget_set_margin_top(card, 6);
    gtk_widget_set_margin_bottom(card, 6);
    g_object_set_data(G_OBJECT(card), "av", av);
    g_object_set_data(G_OBJECT(card), "name", name);
    g_object_set_data(G_OBJECT(card), "tags", tags);
    g_object_set_data(G_OBJECT(card), "sub", sub);
    g_object_set_data(G_OBJECT(card), "go", go);

    gtk_widget_set_valign(av, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), av);
    gtk_widget_set_hexpand(vb, TRUE);
    gtk_widget_set_valign(vb, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(name, "hb-h-name");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vb), name);
    gtk_widget_set_halign(tags, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vb), tags);
    gtk_widget_add_css_class(sub, "hb-h-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vb), sub);
    gtk_box_append(GTK_BOX(card), vb);

    gtk_widget_add_css_class(go, "hb-card-go");
    gtk_widget_set_valign(go, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(go, "连接");
    gtk_button_set_child(GTK_BUTTON(go), hb_icon_new("chev", 15));
    gtk_widget_set_visible(go, FALSE);         /* hover 显示 */
    gtk_box_append(GTK_BOX(card), go);

    g_signal_connect(mo, "enter", G_CALLBACK(on_card_motion), go);
    g_signal_connect(mo, "leave", G_CALLBACK(on_card_leave), go);
    gtk_widget_add_controller(card, mo);
    return card;
}

static GtkWidget *tag_lbl(const char *txt, gboolean tun)
{
    GtkWidget *l = gtk_label_new(txt);
    gtk_widget_add_css_class(l, "hb-tag");
    if (tun) gtk_widget_add_css_class(l, "tun");
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static void card_bind(HbHostsView *hv, GtkWidget *card, HbHostItem *h)
{
    GtkWidget *av = g_object_get_data(G_OBJECT(card), "av");
    GtkWidget *name = g_object_get_data(G_OBJECT(card), "name");
    GtkWidget *tags = g_object_get_data(G_OBJECT(card), "tags");
    GtkWidget *sub = g_object_get_data(G_OBJECT(card), "sub");
    GtkWidget *newav;
    GtkWidget *ch;
    char subbuf[256];

    /* 头像按 proto/os 重建（avatar widget 类型不可变） */
    newav = hb_os_avatar_new(host_avatar_key(h), 38);
    gtk_widget_set_valign(newav, GTK_ALIGN_START);
    gtk_box_insert_child_after(GTK_BOX(card), newav, NULL);
    gtk_widget_unparent(av);
    g_object_set_data(G_OBJECT(card), "av", newav);

    gtk_label_set_label(GTK_LABEL(name), h->name ? h->name : "?");

    while ((ch = gtk_widget_get_first_child(tags)))
        gtk_box_remove(GTK_BOX(tags), ch);
    if (h->tun)
        gtk_box_append(GTK_BOX(tags), tag_lbl("SSH 隧道", TRUE));
    if (h->local)
        gtk_box_append(GTK_BOX(tags), tag_lbl("本地", TRUE));

    host_sub(hv, h, subbuf, sizeof subbuf);
    gtk_label_set_label(GTK_LABEL(sub), subbuf);
}

/* 列表行：名称(头像+名) / 用户 / 端口 / 协议 / 分组 / 操作（86px） */
#define COL_NAME_MIN 260
#define COL_USER_W   150
#define COL_PORT_W   70
#define COL_PROTO_W  90
#define COL_GROUP_W  150
#define COL_ACT_W    86

static GtkWidget *row_shell(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *c1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name = gtk_label_new("");
    GtkWidget *av = hb_os_avatar_new("generic", 22);
    GtkWidget *gcell, *act;
    const char *keys[] = { "user", "port", "proto" };
    int widths[] = { COL_USER_W, COL_PORT_W, COL_PROTO_W };
    int i;
    gtk_widget_add_css_class(row, "hb-table-row");
    gtk_widget_set_size_request(c1, COL_NAME_MIN, -1);
    gtk_widget_set_hexpand(c1, TRUE);
    gtk_widget_set_margin_start(c1, 14);
    gtk_widget_add_css_class(name, "hb-h-name");
    gtk_widget_add_css_class(name, "hb-c0");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_widget_set_valign(name, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(av, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(c1), av);
    gtk_box_append(GTK_BOX(c1), name);
    gtk_box_append(GTK_BOX(row), c1);
    g_object_set_data(G_OBJECT(row), "av", av);
    g_object_set_data(G_OBJECT(row), "name", name);
    for (i = 0; i < 3; i++) {
        GtkWidget *l = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_size_request(l, widths[i], -1);
        gtk_widget_set_margin_start(l, 14);
        if (i == 1) gtk_widget_add_css_class(l, "mono");
        gtk_box_append(GTK_BOX(row), l);
        g_object_set_data(G_OBJECT(row), keys[i], l);
    }
    gcell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(gcell, COL_GROUP_W, -1);
    gtk_widget_set_margin_start(gcell, 14);
    gtk_widget_set_valign(gcell, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), gcell);
    g_object_set_data(G_OBJECT(row), "groupcell", gcell);
    act = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(act, COL_ACT_W, -1);
    gtk_widget_set_margin_start(act, 14);
    gtk_box_append(GTK_BOX(row), act);
    return row;
}

static void row_bind(GtkWidget *row, HbHostItem *h)
{
    GtkWidget *av;
    av = g_object_get_data(G_OBJECT(row), "av");
    char port[16];
    GtkWidget *newav = hb_os_avatar_new(host_avatar_key(h), 22);
    GtkWidget *c1 = gtk_widget_get_parent(av);
    gtk_widget_set_valign(newav, GTK_ALIGN_CENTER);
    gtk_box_prepend(GTK_BOX(c1), newav);
    gtk_widget_unparent(av);
    g_object_set_data(G_OBJECT(row), "av", newav);

    GtkWidget *gcell = g_object_get_data(G_OBJECT(row), "groupcell");
    GtkWidget *gch;
    char *pup;
    gtk_label_set_label(GTK_LABEL(g_object_get_data(G_OBJECT(row), "name")),
                        h->name ? h->name : "?");
    gtk_label_set_label(GTK_LABEL(g_object_get_data(G_OBJECT(row), "user")),
                        h->user && *h->user ? h->user : "—");
    snprintf(port, sizeof port, "%d", (int)h->port);
    gtk_label_set_label(GTK_LABEL(g_object_get_data(G_OBJECT(row), "port")),
                        h->port ? port : "—");
    pup = g_ascii_strup(h->proto ? h->proto : "", -1);
    gtk_label_set_label(GTK_LABEL(g_object_get_data(G_OBJECT(row), "proto")), pup);
    g_free(pup);
    while ((gch = gtk_widget_get_first_child(gcell)))
        gtk_box_remove(GTK_BOX(gcell), gch);
    if (h->group && *h->group)
        gtk_box_append(GTK_BOX(gcell), tag_lbl(h->group, FALSE));
    else {
        GtkWidget *l = gtk_label_new("—");
        gtk_widget_add_css_class(l, "hb-c0");
        gtk_box_append(GTK_BOX(gcell), l);
    }
}

/* ------------------------------ model/factory ------------------------------ */

static void grid_setup(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{ (void)f; (void)ud; gtk_list_item_set_child(li, card_shell()); }
static void grid_bind(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{ (void)f;
  card_bind(HB_HOSTS_VIEW(ud), gtk_list_item_get_child(li),
            HB_HOST_ITEM(gtk_list_item_get_item(li))); }
static void list_setup(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{ (void)f; (void)ud; gtk_list_item_set_child(li, row_shell()); }
static void list_bind(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{ (void)f; (void)ud;
  row_bind(gtk_list_item_get_child(li),
           HB_HOST_ITEM(gtk_list_item_get_item(li))); }

/* ------------------------------ chips ------------------------------ */

static void on_chip(GtkButton *b, gpointer ud)
{
    HbHostsView *hv = ud;
    GtkWidget *ch;
    g_free(hv->chip);
    hv->chip = g_strdup(g_object_get_data(G_OBJECT(b), "chip"));
    for (ch = gtk_widget_get_first_child(hv->chiprow); ch;
         ch = gtk_widget_get_next_sibling(ch))
        gtk_widget_remove_css_class(ch, "on");
    gtk_widget_add_css_class(GTK_WIDGET(b), "on");
    gtk_filter_changed(GTK_FILTER(hv->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static GtkWidget *chip_btn(HbHostsView *hv, const char *label,
                           const char *key)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "hb-chip");
    g_object_set_data(G_OBJECT(b), "chip",
                      (gpointer)(key ? g_intern_string(key) : NULL));
    g_signal_connect(b, "clicked", G_CALLBACK(on_chip), hv);
    return b;
}

static void rebuild_chips(HbHostsView *hv)
{
    GtkWidget *ch;
    GListModel *m = G_LIST_MODEL(hv->all);
    guint i, n = g_list_model_get_n_items(m);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *groups = g_ptr_array_new();
    hb_obj **gobjs = NULL;
    size_t gn = 0;

    while ((ch = gtk_widget_get_first_child(hv->chiprow)))
        gtk_box_remove(GTK_BOX(hv->chiprow), ch);

    {
        GtkWidget *b = chip_btn(hv, "全部", NULL);
        if (!hv->chip) gtk_widget_add_css_class(b, "on");
        gtk_box_append(GTK_BOX(hv->chiprow), b);
    }

    /* group 对象的 name + host.group 值，去重 */
    gobjs = hb_store_list(hv->store, "group", &gn);
    if (gobjs) {
        size_t k;
        for (k = 0; k < gn; k++) {
            const char *nm = hb_obj_get_str(gobjs[k], "name");
            if (nm && *nm && !g_hash_table_contains(seen, nm)) {
                g_hash_table_add(seen, (gpointer)nm);
                g_ptr_array_add(groups, g_strdup(nm));
            }
        }
        hb_obj_list_free(gobjs, gn);
    }
    for (i = 0; i < n; i++) {
        HbHostItem *h = g_list_model_get_item(m, i);
        if (h->group && *h->group &&
            !g_hash_table_contains(seen, h->group)) {
            g_hash_table_add(seen, h->group);
            g_ptr_array_add(groups, g_strdup(h->group));
        }
        g_object_unref(h);
    }
    g_hash_table_unref(seen);

    for (i = 0; i < groups->len; i++) {
        GtkWidget *b = chip_btn(hv, groups->pdata[i], groups->pdata[i]);
        if (g_strcmp0(hv->chip, groups->pdata[i]) == 0)
            gtk_widget_add_css_class(b, "on");
        gtk_box_append(GTK_BOX(hv->chiprow), b);
        g_free(groups->pdata[i]);
    }
    g_ptr_array_unref(groups);

    {
        GtkWidget *b = chip_btn(hv, "SSH 隧道", "tun");
        if (g_strcmp0(hv->chip, "tun") == 0)
            gtk_widget_add_css_class(b, "on");
        gtk_box_append(GTK_BOX(hv->chiprow), b);
    }
}

/* ------------------------------ 数据 ------------------------------ */

static void reload(HbHostsView *hv)
{
    hb_obj **objs = NULL;
    size_t n = 0, i;
    g_list_store_remove_all(hv->all);
    objs = hb_store_list(hv->store, "host", &n);
    if (objs) {
        for (i = 0; i < n; i++) {
            HbHostItem *h = host_item_new(objs[i]);
            g_list_store_append(hv->all, h);
            g_object_unref(h);
        }
        hb_obj_list_free(objs, n);
    }
    rebuild_chips(hv);
    gtk_filter_changed(GTK_FILTER(hv->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_body_and_title(hv);
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbHostsView *hv = ud;
    (void)st; (void)id;
    if (!strcmp(type, "host") || !strcmp(type, "group"))
        reload(hv);
}

/* ------------------------------ UI ------------------------------ */

static void on_search_changed(GtkSearchEntry *e, gpointer ud)
{
    HbHostsView *hv = ud;
    g_free(hv->search_text);
    hv->search_text = fold(gtk_editable_get_text(GTK_EDITABLE(e)));
    gtk_filter_changed(GTK_FILTER(hv->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void on_clear_filter(GtkButton *b, gpointer ud)
{
    HbHostsView *hv = ud;
    (void)b;
    gtk_editable_set_text(GTK_EDITABLE(hv->search), "");
    g_free(hv->chip); hv->chip = NULL;
    rebuild_chips(hv);
    gtk_filter_changed(GTK_FILTER(hv->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void on_sort_sel(GtkDropDown *dd, GParamSpec *ps, gpointer ud)
{
    HbHostsView *hv = ud;
    (void)ps;
    hv->sort_mode = (int)gtk_drop_down_get_selected(dd);
    gtk_sorter_changed(GTK_SORTER(hv->sorter), GTK_SORTER_CHANGE_DIFFERENT);
}

static void set_mode(HbHostsView *hv, gboolean list)
{
    hv->list_mode = list;
    if (list) {
        gtk_widget_add_css_class(hv->btn_list, "on");
        gtk_widget_remove_css_class(hv->btn_grid, "on");
    } else {
        gtk_widget_add_css_class(hv->btn_grid, "on");
        gtk_widget_remove_css_class(hv->btn_list, "on");
    }
    update_body_and_title(hv);
}
static void on_mode_grid(GtkButton *b, gpointer ud)
{ (void)b; set_mode(ud, FALSE); }
static void on_mode_list(GtkButton *b, gpointer ud)
{ (void)b; set_mode(ud, TRUE); }

static void on_items_changed(GListModel *m, guint pos, guint rem, guint add,
                             gpointer ud)
{
    (void)m; (void)pos; (void)rem; (void)add;
    update_body_and_title(ud);
}

static GtkWidget *build_quickbar(HbHostsView *hv)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *kbd, *conn;

    gtk_widget_add_css_class(bar, "hb-quickbar");

    hv->search = gtk_search_entry_new();
    gtk_widget_set_hexpand(hv->search, TRUE);
    g_object_set(hv->search, "placeholder-text",
        "Find a host or ssh user@hostname…", NULL);
    g_signal_connect(hv->search, "search-changed",
                     G_CALLBACK(on_search_changed), hv);
    gtk_box_append(GTK_BOX(bar), hv->search);

    kbd = gtk_label_new("Ctrl K");
    gtk_widget_add_css_class(kbd, "kbd");
    gtk_widget_set_visible(kbd, FALSE);       /* palette 单元再启用 */
    gtk_box_append(GTK_BOX(bar), kbd);

    conn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(conn, "hb-btn");
    gtk_widget_add_css_class(conn, "primary");
    gtk_widget_set_visible(conn, FALSE);      /* ssh 单元再启用 */
    gtk_box_append(GTK_BOX(bar), conn);
    return bar;
}

static GtkWidget *build_toolbar(HbHostsView *hv)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *newb = gtk_menu_button_new();
    GtkWidget *sort;
    GtkWidget *spacer;
    const char *sort_opts[] = { "按名称", "按最近连接", "按协议", NULL };

    /* New host（分割按钮样式 —— 主机 CRUD 单元再启用） */
    gtk_widget_add_css_class(newb, "hb-btn");
    gtk_widget_add_css_class(newb, "primary");
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(row), gtk_label_new("New host"));
        gtk_box_append(GTK_BOX(row), hb_icon_new("chev", 12));
        gtk_menu_button_set_child(GTK_MENU_BUTTON(newb), row);
    }
    gtk_widget_set_visible(newb, FALSE);
    gtk_box_append(GTK_BOX(bar), newb);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    sort = gtk_drop_down_new_from_strings(sort_opts);
    gtk_widget_add_css_class(sort, "hb-sortsel");
    gtk_widget_set_tooltip_text(sort, "排序");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(sort), hv->sort_mode);
    g_signal_connect(sort, "notify::selected",
                     G_CALLBACK(on_sort_sel), hv);
    gtk_widget_set_valign(sort, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bar), sort);

    {
        GtkWidget *tog = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(tog, "hb-viewtoggle");
        gtk_widget_set_valign(tog, GTK_ALIGN_CENTER);
        hv->btn_grid = gtk_button_new();
        gtk_widget_add_css_class(hv->btn_grid, "hb-viewbtn");
        gtk_button_set_child(GTK_BUTTON(hv->btn_grid), hb_icon_new("grid", 15));
        gtk_widget_set_tooltip_text(hv->btn_grid, "网格视图");
        g_signal_connect(hv->btn_grid, "clicked", G_CALLBACK(on_mode_grid), hv);
        gtk_box_append(GTK_BOX(tog), hv->btn_grid);
        hv->btn_list = gtk_button_new();
        gtk_widget_add_css_class(hv->btn_list, "hb-viewbtn");
        gtk_button_set_child(GTK_BUTTON(hv->btn_list), hb_icon_new("list", 15));
        gtk_widget_set_tooltip_text(hv->btn_list, "列表视图");
        g_signal_connect(hv->btn_list, "clicked", G_CALLBACK(on_mode_list), hv);
        gtk_box_append(GTK_BOX(tog), hv->btn_list);
        gtk_box_append(GTK_BOX(bar), tog);
    }
    gtk_widget_add_css_class(bar, "hb-toolbar");
    return bar;
}

static GtkWidget *build_empty(HbHostsView *hv)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ic = hb_icon_new("search", 24);
    GtkWidget *t = gtk_label_new("没有匹配的主机");
    GtkWidget *s = gtk_label_new("试试其他关键词，或清除筛选");
    GtkWidget *btn = gtk_button_new_with_label("清除筛选");

    gtk_widget_add_css_class(box, "hb-empty");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(ico, "hb-e-ico");
    gtk_widget_set_size_request(ico, 58, 58);
    gtk_widget_set_halign(ico, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand_set(ico, TRUE);
    gtk_widget_set_hexpand(ico, FALSE);
    gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(ic, TRUE);
    gtk_box_append(GTK_BOX(ico), ic);
    gtk_widget_add_css_class(t, "hb-e-title");
    gtk_widget_add_css_class(s, "hb-e-sub");
    gtk_widget_add_css_class(btn, "hb-btn");
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(btn, 8);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_clear_filter), hv);
    gtk_box_append(GTK_BOX(box), ico);
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), s);
    gtk_box_append(GTK_BOX(box), btn);
    return box;
}

static GtkWidget *build_table_head(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    struct { const char *txt; int w; gboolean grow; } cols[] = {
        { "名称", COL_NAME_MIN, TRUE }, { "用户", COL_USER_W, FALSE },
        { "端口", COL_PORT_W, FALSE }, { "协议", COL_PROTO_W, FALSE },
        { "分组", COL_GROUP_W, FALSE }, { "", COL_ACT_W, FALSE },
    };
    int i;
    gtk_widget_add_css_class(row, "hb-table-head");
    for (i = 0; i < 6; i++) {
        GtkWidget *l = gtk_label_new(cols[i].txt);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_margin_start(l, 14);
        if (cols[i].grow) gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_set_size_request(l, cols[i].w, -1);
        gtk_box_append(GTK_BOX(row), l);
    }
    return row;
}

static void hb_hosts_view_dispose(GObject *o)
{
    HbHostsView *hv = HB_HOSTS_VIEW(o);
    if (hv->store && hv->changed_handler)
        g_signal_handler_disconnect(hv->store, hv->changed_handler);
    g_clear_object(&hv->all);
    g_clear_object(&hv->filtered);
    g_clear_object(&hv->sorted);
    g_clear_object(&hv->filter);
    g_clear_object(&hv->sorter);
    g_free(hv->search_text);
    g_free(hv->chip);
    G_OBJECT_CLASS(hb_hosts_view_parent_class)->dispose(o);
}

static void hb_hosts_view_class_init(HbHostsViewClass *klass)
{ G_OBJECT_CLASS(klass)->dispose = hb_hosts_view_dispose; }

static void hb_hosts_view_init(HbHostsView *hv)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(hv),
                                   GTK_ORIENTATION_VERTICAL);
}

GtkWidget *hb_hosts_view_new(HbStore *st)
{
    HbHostsView *hv = g_object_new(HB_TYPE_HOSTS_VIEW, NULL);
    GtkSelectionModel *sel;
    GtkListItemFactory *f;
    GtkWidget *scrolled, *listbox;

    hv->store = st;
    hv->all = g_list_store_new(HB_TYPE_HOST_ITEM);
    hv->filter = gtk_custom_filter_new(filter_fn, hv, NULL);
    hv->filtered = gtk_filter_list_model_new(
        G_LIST_MODEL(hv->all), GTK_FILTER(hv->filter));
    hv->sorter = gtk_custom_sorter_new(sort_fn, hv, NULL);
    hv->sorted = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(hv->filtered)),
        GTK_SORTER(g_object_ref(hv->sorter)));
    g_signal_connect(hv->filtered, "items-changed",
                     G_CALLBACK(on_items_changed), hv);

    gtk_box_append(GTK_BOX(hv), build_quickbar(hv));
    gtk_box_append(GTK_BOX(hv), build_toolbar(hv));

    /* chip 行 */
    {
        GtkWidget *sc = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
            GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
        gtk_widget_add_css_class(sc, "hb-chiprow");
        hv->chiprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), hv->chiprow);
        gtk_box_append(GTK_BOX(hv), sc);
    }

    hv->title_lbl = gtk_label_new("Hosts · 0");
    gtk_widget_add_css_class(hv->title_lbl, "hb-section-title");
    gtk_label_set_xalign(GTK_LABEL(hv->title_lbl), 0);
    gtk_widget_set_margin_start(hv->title_lbl, 20);
    gtk_box_append(GTK_BOX(hv), hv->title_lbl);

    hv->body = gtk_stack_new();
    gtk_widget_set_vexpand(hv->body, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(hv->body),
        GTK_STACK_TRANSITION_TYPE_NONE);

    /* 网格 */
    sel = GTK_SELECTION_MODEL(
        gtk_single_selection_new(G_LIST_MODEL(g_object_ref(hv->sorted))));
    gtk_single_selection_set_autoselect(GTK_SINGLE_SELECTION(sel), FALSE);
    f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(grid_setup), NULL);
    g_signal_connect(f, "bind", G_CALLBACK(grid_bind), hv);
    hv->grid = gtk_grid_view_new(sel, f);
    gtk_widget_set_margin_start(hv->grid, 16);
    gtk_widget_set_margin_end(hv->grid, 16);
    gtk_widget_set_margin_bottom(hv->grid, 16);
    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), hv->grid);
    gtk_stack_add_named(GTK_STACK(hv->body), scrolled, "grid");
    /* sel/f ownership is transferred to the view */

    /* 列表 */
    sel = GTK_SELECTION_MODEL(
        gtk_single_selection_new(G_LIST_MODEL(g_object_ref(hv->sorted))));
    gtk_single_selection_set_autoselect(GTK_SINGLE_SELECTION(sel), FALSE);
    f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(list_setup), NULL);
    g_signal_connect(f, "bind", G_CALLBACK(list_bind), hv);
    hv->list = gtk_list_view_new(sel, f);
    gtk_widget_set_margin_start(hv->list, 16);
    gtk_widget_set_margin_end(hv->list, 16);
    listbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(listbox, "hb-table");
    {
        GtkWidget *headwrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_start(headwrap, 16);
        gtk_widget_set_margin_end(headwrap, 16);
        gtk_widget_set_margin_top(headwrap, 8);
        gtk_box_append(GTK_BOX(headwrap), build_table_head());
        gtk_box_append(GTK_BOX(listbox), headwrap);
    }
    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), hv->list);
    gtk_box_append(GTK_BOX(listbox), scrolled);
    gtk_stack_add_named(GTK_STACK(hv->body), listbox, "list");

    gtk_stack_add_named(GTK_STACK(hv->body), build_empty(hv), "empty");
    gtk_box_append(GTK_BOX(hv), hv->body);

    hv->changed_handler = g_signal_connect(hv->store, "changed",
        G_CALLBACK(on_store_changed), hv);

    set_mode(hv, FALSE);
    reload(hv);
    return GTK_WIDGET(hv);
}

void hb_hosts_view_set_search(HbHostsView *hv, const char *text)
{
    gtk_editable_set_text(GTK_EDITABLE(hv->search), text);
}

void hb_hosts_view_set_list_mode(HbHostsView *hv, gboolean list)
{
    set_mode(hv, list);
}
