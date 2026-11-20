#include "hb-hosts-view.h"
#include "hb-host-actions.h"
#include "hb-host-form.h"
#include "hb-host-drawer.h"
#include "hb-dialogs.h"
#include "hb-window.h"
#include "hb-icons.h"

#include <adwaita.h>
#include <harbor/hostio.h>

#include <stdio.h>

/* ------------------------- HbHostItem（GObject 包装 hb_obj） ------------------------- */

#define HB_TYPE_HOST_ITEM (hb_host_item_get_type())
G_DECLARE_FINAL_TYPE(HbHostItem, hb_host_item, HB, HOST_ITEM, GObject)

struct _HbHostItem {
    GObject parent;
    char *id, *name, *address, *user, *proto, *os, *group, *via;
    char *via_name;          /* reload 时一次解析（bind 时 O(1)） */
    char *hay;               /* fold 过的检索字段拼串（一次 strstr） */
    char *coll;              /* g_utf8_collate_key(name)：排序比较不再建 key */
    int64_t port, last_connected, order;
    gboolean tun, local;
};

G_DEFINE_TYPE(HbHostItem, hb_host_item, G_TYPE_OBJECT)

static HbHostItem *host_item_new(hb_obj *o)
{
    HbHostItem *h = g_object_new(HB_TYPE_HOST_ITEM, NULL);
    const char *via = hb_obj_get_str(o, "via");
    const char **tags = NULL;
    size_t nt;
    GString *hay;
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
    /* 预折叠 haystack：name/address/user/group/proto/tags 一次 strstr */
    {
        const char *flds[] = { h->name, h->address, h->user,
                               h->group, h->proto, NULL };
        const char **fp;
        hay = g_string_new(NULL);
        for (fp = flds; *fp; fp++)
            if (**fp) {
                g_string_append(hay, *fp);
                g_string_append_c(hay, '\x1f');
            }
        nt = hb_obj_get_strv(o, "tags", &tags);
        for (size_t i = 0; i < nt; i++)
            if (tags[i] && *tags[i]) {
                g_string_append(hay, tags[i]);
                g_string_append_c(hay, '\x1f');
            }
        free(tags);
        h->hay = g_utf8_casefold(hay->str, hay->len);
        g_string_free(hay, TRUE);
    }
    h->coll = g_utf8_collate_key(h->name ? h->name : "", -1);
    return h;
}

static void hb_host_item_finalize(GObject *o)
{
    HbHostItem *h = HB_HOST_ITEM(o);
    g_free(h->id); g_free(h->name); g_free(h->address); g_free(h->user);
    g_free(h->proto); g_free(h->os); g_free(h->group); g_free(h->via);
    g_free(h->via_name); g_free(h->hay); g_free(h->coll);
    G_OBJECT_CLASS(hb_host_item_parent_class)->finalize(o);
}
static void hb_host_item_class_init(HbHostItemClass *k)
{ G_OBJECT_CLASS(k)->finalize = hb_host_item_finalize; }
static void hb_host_item_init(HbHostItem *h) { (void)h; }

/* ---------------- 分片发布模型 ----------------
 * GtkFilterListModel(incremental) 只把"加回来"分片；
 * 收窄时 (-1001/+0) 的移除一次性同步发出，item manager 在
 * items-changed 里同步拆掉整个跟踪窗口（实测 1000 项 ~50-110ms）。
 * 这里把源 items-changed 重算成 pub→tgt 的差量，按 tick 分片下发：
 * 窗口附近小片、远端整片一次发（细节见 slice_step）。同步路径
 * 恒定小，视图逐帧渐进更新。
 * pub = 已发布快照，tgt = 源最新快照；源一变就重算 diff 续发。 */
#define SLICE_K     24   /* 跟踪窗口内每片最多移除/插入的项数 */
#define SLICE_GUARD 384  /* 大于跟踪窗口的保守上界：此下标以上一次发 */
#define SLICE_MS    12

typedef struct _HbSliceModel HbSliceModel;
struct _HbSliceModel {
    GObject parent_instance;
    GListModel *src;
    GPtrArray *pub;             /* 已发布 item（owns refs） */
    GPtrArray *tgt;             /* src 最新快照（owns refs） */
    GHashTable *in_pub, *in_tgt;
    guint drain;
    int mode;                   /* SLICE_NARROW / _WIDEN / _FALLBACK */
    guint j;                    /* WIDEN 的 tgt 游标 */
};
typedef struct { GObjectClass parent_class; } HbSliceModelClass;

enum { SLICE_NARROW, SLICE_WIDEN, SLICE_FALLBACK };

#define HB_TYPE_SLICE_MODEL (hb_slice_model_get_type())
#define HB_SLICE_MODEL(o) (G_TYPE_CHECK_INSTANCE_CAST((o), \
                           HB_TYPE_SLICE_MODEL, HbSliceModel))
static void slice_model_iface_init(GListModelInterface *iface);
G_DEFINE_TYPE_WITH_CODE(HbSliceModel, hb_slice_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              slice_model_iface_init))

static GType slice_get_item_type(GListModel *m)
{ return g_list_model_get_item_type(HB_SLICE_MODEL(m)->src); }
static guint slice_get_n_items(GListModel *m)
{ return HB_SLICE_MODEL(m)->pub->len; }
static gpointer slice_get_item(GListModel *m, guint pos)
{
    HbSliceModel *s = HB_SLICE_MODEL(m);
    if (pos >= s->pub->len) return NULL;
    return g_object_ref(s->pub->pdata[pos]);
}
static void slice_model_iface_init(GListModelInterface *i)
{
    i->get_item_type = slice_get_item_type;
    i->get_n_items = slice_get_n_items;
    i->get_item = slice_get_item;
}

/* sub 是否为 super 的顺序子序列（super_set 已建好） */
static gboolean ptr_subseq(GPtrArray *sub, GPtrArray *super,
                           GHashTable *super_set)
{
    guint i, j = 0;
    for (i = 0; i < sub->len; i++) {
        if (!g_hash_table_contains(super_set, sub->pdata[i]))
            return FALSE;
        while (j < super->len && super->pdata[j] != sub->pdata[i]) j++;
        if (j == super->len) return FALSE;
        j++;
    }
    return TRUE;
}

static void slice_refresh_tgt(HbSliceModel *s)
{
    guint i, n = g_list_model_get_n_items(s->src);
    g_ptr_array_set_size(s->tgt, 0);
    for (i = 0; i < n; i++)
        g_ptr_array_add(s->tgt, g_list_model_get_item(s->src, i));
    g_hash_table_remove_all(s->in_tgt);
    for (i = 0; i < n; i++)
        g_hash_table_add(s->in_tgt, s->tgt->pdata[i]);
}

static void slice_rebuild_pub_set(HbSliceModel *s)
{
    guint i;
    g_hash_table_remove_all(s->in_pub);
    for (i = 0; i < s->pub->len; i++)
        g_hash_table_add(s->in_pub, s->pub->pdata[i]);
}

/* 发一片 items-changed，返回 TRUE=还有剩余。
 * 移除自下而上分两段策略：
 *  - SLICE_GUARD 以上（远离可视窗口）的整段一次发完——item manager
 *    只跟踪窗口附近，远端移除近乎免费，还能让 pub 立刻变小，
 *    下一帧 measure/render 的开销随 pub 线性下降（实测 list 900 项
 *    一帧 ~100ms，是首帧预算的主要来源）；
 *  - GUARD 以内（可能命中跟踪窗口）才切 SLICE_K 小片，窗口顶部的
 *    旧条目最后删。 */
static gboolean slice_step(HbSliceModel *s)
{
    guint r, a, k, e, x;

    if (s->mode == SLICE_FALLBACK) {
        if (s->pub->len) {      /* 先自下而上分片清空 */
            k = s->pub->len > SLICE_K ? s->pub->len - SLICE_K : 0;
            if (s->pub->len > SLICE_GUARD) k = SLICE_GUARD;
            r = s->pub->len - k;
            for (x = k; x < s->pub->len; x++)
                g_hash_table_remove(s->in_pub, s->pub->pdata[x]);
            g_ptr_array_remove_range(s->pub, k, r);
            g_list_model_items_changed(G_LIST_MODEL(s), k, r, 0);
            return TRUE;
        }
        s->mode = SLICE_WIDEN;  /* 清完 = 纯插入 */
        s->j = 0;
    }
    if (s->mode == SLICE_NARROW) {
        /* 从尾部往下找最高的一段"不在 tgt"的连续区间 */
        e = s->pub->len;
        while (e > 0 &&
               g_hash_table_contains(s->in_tgt, s->pub->pdata[e - 1]))
            e--;
        if (!e) return FALSE;
        k = e;
        while (k > 0 &&
               !g_hash_table_contains(s->in_tgt, s->pub->pdata[k - 1]))
            k--;
        /* [k,e) 全都要删：GUARD 以上一次发，GUARD 以内每次 <=SLICE_K */
        if (e > SLICE_GUARD)
            k = MAX(k, SLICE_GUARD);
        else if (e - k > SLICE_K)
            k = e - SLICE_K;
        r = e - k;
        for (x = k; x < e; x++)
            g_hash_table_remove(s->in_pub, s->pub->pdata[x]);
        g_ptr_array_remove_range(s->pub, k, r);
        g_list_model_items_changed(G_LIST_MODEL(s), k, r, 0);
        return TRUE;
    }
    /* SLICE_WIDEN：tgt[j..] 里还没进 pub 的连续段插到位置 j；
     * 窗口内小片（先让新结果顶上来），GUARD 以下一次发完 */
    while (s->j < s->tgt->len &&
           g_hash_table_contains(s->in_pub, s->tgt->pdata[s->j]))
        s->j++;
    if (s->j >= s->tgt->len) return FALSE;
    a = 0;
    while (s->j + a < s->tgt->len &&
           !g_hash_table_contains(s->in_pub, s->tgt->pdata[s->j + a]) &&
           (s->j >= SLICE_GUARD || a < SLICE_K))
        a++;
    if (!a) { s->j++; return TRUE; }
    for (x = 0; x < a; x++) {
        g_ptr_array_insert(s->pub, s->j + x, s->tgt->pdata[s->j + x]);
        g_object_ref(s->tgt->pdata[s->j + x]);
        g_hash_table_add(s->in_pub, s->tgt->pdata[s->j + x]);
    }
    g_list_model_items_changed(G_LIST_MODEL(s), s->j, 0, a);
    s->j += a;
    return TRUE;
}

static gboolean slice_tick(gpointer ud)
{
    HbSliceModel *s = ud;
    if (slice_step(s)) return G_SOURCE_CONTINUE;
    s->drain = 0;
    return G_SOURCE_REMOVE;
}

static void slice_schedule(HbSliceModel *s)
{
    if (s->drain) return;
    s->drain = g_timeout_add(SLICE_MS, slice_tick, s);
}

/* src 一变：重取快照、重算 diff 方向，先同步发一片再排程 */
static void slice_src_changed(GListModel *m, guint pos, guint rem,
                              guint add, gpointer ud)
{
    HbSliceModel *s = ud;
    (void)m; (void)pos; (void)rem; (void)add;
    slice_refresh_tgt(s);
    if (ptr_subseq(s->tgt, s->pub, s->in_pub))
        s->mode = SLICE_NARROW;
    else if (ptr_subseq(s->pub, s->tgt, s->in_tgt))
        s->mode = SLICE_WIDEN;
    else
        s->mode = SLICE_FALLBACK;
    s->j = 0;
    if (s->drain) {
        g_source_remove(s->drain);
        s->drain = 0;
    }
    if (slice_step(s))
        slice_schedule(s);
}

static void hb_slice_model_dispose(GObject *o)
{
    HbSliceModel *s = HB_SLICE_MODEL(o);
    if (s->drain) {
        g_source_remove(s->drain);
        s->drain = 0;
    }
    g_clear_object(&s->src);
    g_clear_pointer(&s->pub, g_ptr_array_unref);
    g_clear_pointer(&s->tgt, g_ptr_array_unref);
    g_clear_pointer(&s->in_pub, g_hash_table_unref);
    g_clear_pointer(&s->in_tgt, g_hash_table_unref);
    G_OBJECT_CLASS(hb_slice_model_parent_class)->dispose(o);
}
static void hb_slice_model_class_init(HbSliceModelClass *k)
{ G_OBJECT_CLASS(k)->dispose = hb_slice_model_dispose; }
static void hb_slice_model_init(HbSliceModel *s)
{
    s->pub = g_ptr_array_new_with_free_func(g_object_unref);
    s->tgt = g_ptr_array_new_with_free_func(g_object_unref);
    s->in_pub = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->in_tgt = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static GListModel *hb_slice_model_new(GListModel *src)
{
    HbSliceModel *s = g_object_new(HB_TYPE_SLICE_MODEL, NULL);
    guint i, n = g_list_model_get_n_items(src);
    s->src = g_object_ref(src);
    for (i = 0; i < n; i++)
        g_ptr_array_add(s->pub, g_list_model_get_item(src, i));
    slice_rebuild_pub_set(s);
    slice_refresh_tgt(s);
    g_signal_connect(src, "items-changed", G_CALLBACK(slice_src_changed), s);
    return G_LIST_MODEL(s);
}

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
    GListModel *visible;       /* filtered 的分片发布视图（喂给 view） */
    GtkSingleSelection *grid_sel, *list_sel;  /* 只给可见 view 挂模型 */

    char *search_text;         /* folded */
    char *chip;                /* NULL=全部, "tun", or group name */
    int   sort_mode;
    gboolean list_mode;
    gulong changed_handler;

    HbHostDrawer *drawer;
    guint   pending_reload;    /* coalesced store-changed → idle reload */
    char   *sel_id;            /* 最近点击的 host（Delete 键用） */
    char   *sel_restore;       /* reload 后要恢复的 selected id */
    char   *focus_key;         /* pending single-click→drawer token */
    GtkWidget *body_root;      /* overlay 内的内容列 */
};

G_DEFINE_TYPE(HbHostsView, hb_hosts_view, GTK_TYPE_BOX)

enum { SIG_CONNECT, SIG_QUICK, N_SIGS };
static guint sigs[N_SIGS];

static void emit_connect(HbHostsView *hv, const char *hid)
{
    g_free(hv->focus_key);
    hv->focus_key = NULL;
    if (hv->drawer) hb_host_drawer_close(hv->drawer);
    if (hid)
        g_signal_emit(hv, sigs[SIG_CONNECT], 0, hid);
}

static gboolean drawer_idle(gpointer p)
{
    HbHostsView *hv = p;
    if (hv->focus_key && hv->drawer)
        hb_host_drawer_show(hv->drawer, hv->focus_key);
    g_free(hv->focus_key);
    hv->focus_key = NULL;
    return G_SOURCE_REMOVE;
}

static char *fold(const char *s);

/* 快速连接：
 * 1. name/address 与输入完全相等（不分大小写）的可见卡片 → 直连；
 * 2. 否则输入像连接目标（含 @ : .）→ 交给应用层解析/建主机；
 * 3. 否则是裸名字片段：恰好一条可见卡片才直连；0 条交给应用层；
 *    >=2 条时保留输入不动，让用户继续缩小范围。 */
static gboolean looks_like_target(const char *s)
{
    return s && (strchr(s, '@') || strchr(s, ':') || strchr(s, '.'));
}

static void quick_connect_go(HbHostsView *hv)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(hv->search));
    guint n;
    char *ft;

    if (!text || !*text)
        return;
    ft = fold(text);
    n = g_list_model_get_n_items(G_LIST_MODEL(hv->filtered));
    for (guint i = 0; i < n; i++) {
        HbHostItem *h = g_list_model_get_item(G_LIST_MODEL(hv->filtered), i);
        char *fn, *fa;
        gboolean eq;

        if (!h) continue;
        fn = fold(h->name);
        fa = fold(h->address);
        eq = !strcmp(fn, ft) || !strcmp(fa, ft);
        g_free(fn);
        g_free(fa);
        if (eq) {
            emit_connect(hv, h->id);
            g_object_unref(h);
            g_free(ft);
            gtk_editable_set_text(GTK_EDITABLE(hv->search), "");
            return;
        }
        g_object_unref(h);
    }
    if (looks_like_target(text) || n == 0) {
        g_signal_emit(hv, sigs[SIG_QUICK], 0, text);
        gtk_editable_set_text(GTK_EDITABLE(hv->search), "");
    } else if (n == 1) {
        HbHostItem *h = g_list_model_get_item(G_LIST_MODEL(hv->filtered), 0);
        if (h) {
            emit_connect(hv, h->id);
            g_object_unref(h);
            gtk_editable_set_text(GTK_EDITABLE(hv->search), "");
        }
    }
    g_free(ft);
}

static void on_search_activate(GtkSearchEntry *e, gpointer ud)
{ (void)e; quick_connect_go(ud); }
static void on_connect_clicked(GtkButton *b, gpointer ud)
{ (void)b; quick_connect_go(ud); }

static void select_card(HbHostsView *hv, const char *hid)
{
    g_free(hv->sel_id);
    hv->sel_id = g_strdup(hid);
}

/* 单击选中；双击 → 连接。抽屉在 released 才排程（见下） */
static void on_card_click(GtkGestureClick *g, int n_press,
                          double x, double y, gpointer ud)
{
    GtkWidget *card = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(g));
    HbHostsView *hv = g_object_get_data(G_OBJECT(card), "hv");
    const char *hid = g_object_get_data(G_OBJECT(card), "hid");
    (void)x; (void)y; (void)ud;
    if (!hv || !hid) return;
    select_card(hv, hid);
    if (n_press == 2)
        emit_connect(hv, hid);
}

/* released 只在控件内松手时触发：拖拽拖到分组 chip 松手不会来这，
 * 抽屉就不会在拖动途中弹出、用 scrim 盖住 chip 让 drop 永远落空 */
static void on_card_released(GtkGestureClick *g, int n_press,
                             double x, double y, gpointer ud)
{
    GtkWidget *card = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(g));
    HbHostsView *hv = g_object_get_data(G_OBJECT(card), "hv");
    const char *hid = g_object_get_data(G_OBJECT(card), "hid");
    (void)x; (void)y; (void)ud;
    if (!hv || !hid) return;
    if (n_press == 1 && hv->drawer) {
        /* 延迟一拍：若紧跟着第 2 击会 connect（emit_connect 清
         * focus_key），抽屉就白开了 */
        g_free(hv->focus_key);
        hv->focus_key = g_strdup(hid);
        g_idle_add(drawer_idle, hv);
    }
}

static void on_card_right(GtkGestureClick *g, int n_press,
                          double x, double y, gpointer ud)
{
    GtkWidget *card = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(g));
    HbHostsView *hv = g_object_get_data(G_OBJECT(card), "hv");
    const char *hid = g_object_get_data(G_OBJECT(card), "hid");
    GMenu *m;
    GMenuItem *it;
    GtkWidget *pop;
    GdkRectangle r;
    (void)n_press; (void)ud;
    if (!hv || !hid) return;
    select_card(hv, hid);

    m = g_menu_new();
    it = g_menu_item_new("连接", NULL);
    g_menu_item_set_action_and_target_value(it, "host.connect",
        g_variant_new_string(hid));
    g_menu_append_item(m, it);
    g_object_unref(it);
    it = g_menu_item_new("SFTP", NULL);
    g_menu_item_set_action_and_target_value(it, "host.sftp",
        g_variant_new_string(hid));
    g_menu_append_item(m, it);
    g_object_unref(it);
    it = g_menu_item_new("编辑", NULL);
    g_menu_item_set_action_and_target_value(it, "host.edit",
        g_variant_new_string(hid));
    g_menu_append_item(m, it);
    g_object_unref(it);
    it = g_menu_item_new("复制", NULL);
    g_menu_item_set_action_and_target_value(it, "host.dup",
        g_variant_new_string(hid));
    g_menu_append_item(m, it);
    g_object_unref(it);
    it = g_menu_item_new("删除", NULL);
    g_menu_item_set_action_and_target_value(it, "host.del",
        g_variant_new_string(hid));
    g_menu_append_item(m, it);
    g_object_unref(it);

    pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(m));
    g_object_unref(m);
    gtk_widget_set_parent(pop, card);
    gtk_widget_add_css_class(pop, "hb-ctx-menu");
    r = (GdkRectangle){ (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_card_dblclick(GtkGestureClick *g, int n_press,
                             double x, double y, gpointer ud)
{
    on_card_click(g, n_press, x, y, ud);
}

static void on_go_clicked(GtkButton *b, gpointer ud)
{
    GtkWidget *card = gtk_widget_get_parent(GTK_WIDGET(b));
    (void)ud;
    emit_connect(g_object_get_data(G_OBJECT(card), "hv"),
                 g_object_get_data(G_OBJECT(card), "hid"));
}

/* 拖拽：把 host id 作为 string 提供（分组 chip 的 drop target 收） */
static GdkContentProvider *host_drag_prepare(GtkDragSource *s,
                                             double x, double y,
                                             gpointer ud)
{
    GtkWidget *w = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(s));
    const char *hid = g_object_get_data(G_OBJECT(w), "hid");
    GValue v = G_VALUE_INIT;
    (void)x; (void)y; (void)ud;
    g_value_init(&v, G_TYPE_STRING);
    g_value_set_string(&v, hid ? hid : "");
    return gdk_content_provider_new_for_value(&v);
}

/* 行内按钮（连接/编辑/删除）：hid/hv 数据在 list_bind 时挂到 row 上 */
static void on_row_btn(GtkButton *b, gpointer ud)
{
    GtkWidget *act = gtk_widget_get_parent(GTK_WIDGET(b));
    GtkWidget *row = gtk_widget_get_parent(act);
    HbHostsView *hv = g_object_get_data(G_OBJECT(row), "hv");
    const char *hid = g_object_get_data(G_OBJECT(row), "hid");
    const char *what = ud;
    if (!hv || !hid) return;
    select_card(hv, hid);
    if (!strcmp(what, "connect"))
        emit_connect(hv, hid);
    else if (!strcmp(what, "edit"))
        hb_host_action_edit(GTK_WIDGET(b), hv->store, hid);
    else
        hb_host_action_delete(GTK_WIDGET(b), hv->store, hid);
}

/* ------------------------- view 上的 GAction ------------------------- */

static void act_conn(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbHostsView *hv = ud;
    (void)a;
    emit_connect(hv, g_variant_get_string(p, NULL));
}
static void act_edit(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; hb_host_action_edit(GTK_WIDGET(ud),
      HB_HOSTS_VIEW(ud)->store, g_variant_get_string(p, NULL)); }
static void act_dup(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; hb_host_action_duplicate(GTK_WIDGET(ud),
      HB_HOSTS_VIEW(ud)->store, g_variant_get_string(p, NULL)); }
static void act_del(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; hb_host_action_delete(GTK_WIDGET(ud),
      HB_HOSTS_VIEW(ud)->store, g_variant_get_string(p, NULL)); }

static void on_new_group_done(const char *text, gpointer ud)
{
    HbHostsView *hv = ud;
    hb_obj *g;
    if (!text || !*text) return;
    g = hb_obj_new("group");
    hb_obj_set_str(g, "name", text);
    hb_obj_set_int(g, "order", g_get_real_time() / 1000);
    hb_vault_put(hb_store_vault(hv->store), g);
    hb_obj_free(g);
}
static void act_newgroup(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    hb_dialog_prompt(GTK_WIDGET(ud), "新建分组", "分组名称", NULL,
                     on_new_group_done, ud);
}

static void act_import_cfg(GSimpleAction *a, GVariant *p, gpointer ud);
static void act_import_json(GSimpleAction *a, GVariant *p, gpointer ud);
static void act_export_json(GSimpleAction *a, GVariant *p, gpointer ud);

static void add_view_actions(HbHostsView *hv)
{
    GSimpleActionGroup *g = g_simple_action_group_new();
    GSimpleAction *a;
    a = g_simple_action_new("connect", G_VARIANT_TYPE_STRING);
    g_signal_connect(a, "activate", G_CALLBACK(act_conn), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("edit", G_VARIANT_TYPE_STRING);
    g_signal_connect(a, "activate", G_CALLBACK(act_edit), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("dup", G_VARIANT_TYPE_STRING);
    g_signal_connect(a, "activate", G_CALLBACK(act_dup), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("del", G_VARIANT_TYPE_STRING);
    g_signal_connect(a, "activate", G_CALLBACK(act_del), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("sftp", G_VARIANT_TYPE_STRING);
    g_simple_action_set_enabled(a, FALSE);   /* SFTP 后续版本提供 */
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("newgroup", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_newgroup), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("importcfg", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_import_cfg), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("importjson", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_import_json), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("exportjson", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_export_json), hv);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    gtk_widget_insert_action_group(GTK_WIDGET(hv), "host",
                                   G_ACTION_GROUP(g));
    g_object_unref(g);
}

/* Delete 键：对最近点选的主机走删除流程 */
static gboolean on_view_key(GtkEventControllerKey *kc, guint keyval,
                            guint code, GdkModifierType st, gpointer ud)
{
    HbHostsView *hv = ud;
    (void)kc; (void)code; (void)st;
    if (keyval != GDK_KEY_Delete || !hv->sel_id) return FALSE;
    hb_host_action_delete(GTK_WIDGET(hv), hv->store, hv->sel_id);
    return TRUE;
}

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
    if (hv->search_text && *hv->search_text &&
        !strstr(h->hay ? h->hay : "", hv->search_text))
        return FALSE;
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
        r = strcmp(x->coll ? x->coll : "", y->coll ? y->coll : "");
    if (!r)
        r = (x->order < y->order) ? -1 : (x->order > y->order);
    return r;
}

static void update_body_and_title(HbHostsView *hv)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(hv->filtered));
    guint pending = gtk_filter_list_model_get_pending(hv->filtered);
    char buf[64];
    snprintf(buf, sizeof buf, "Hosts · %u", n);
    gtk_label_set_label(GTK_LABEL(hv->title_lbl), buf);
    /* incremental 过滤未结束时 n 会短暂到 0，pending>0 时别闪切空页 */
    if (!n && !pending) {
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

/* 原型 h.sub 规则 */
static void host_sub(const HbHostItem *h, char *buf, size_t buflen)
{
    size_t off = 0;
    if (h->via && *h->via) {
        off = (size_t)snprintf(buf, buflen, "%s · 经由 %s 隧道",
                               h->proto ? h->proto : "",
                               h->via_name ? h->via_name : "?");
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
    GtkGesture *dbl = gtk_gesture_click_new();
    GtkGesture *rgt = gtk_gesture_click_new();
    GtkDragSource *ds = gtk_drag_source_new();

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
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(dbl, "pressed", G_CALLBACK(on_card_dblclick), NULL);
    g_signal_connect(dbl, "released", G_CALLBACK(on_card_released), NULL);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(dbl));
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rgt),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(rgt, "pressed", G_CALLBACK(on_card_right), NULL);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(rgt));
    gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
    g_signal_connect(ds, "prepare", G_CALLBACK(host_drag_prepare), NULL);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(ds));
    g_signal_connect(go, "clicked", G_CALLBACK(on_go_clicked), NULL);
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
    GtkWidget *ch;
    char subbuf[256];

    /* 头像就地换键，不重建 widget */
    hb_os_avatar_set_key(av, host_avatar_key(h));

    gtk_label_set_label(GTK_LABEL(name), h->name ? h->name : "?");

    while ((ch = gtk_widget_get_first_child(tags)))
        gtk_box_remove(GTK_BOX(tags), ch);
    if (h->tun)
        gtk_box_append(GTK_BOX(tags), tag_lbl("SSH 隧道", TRUE));
    if (h->local)
        gtk_box_append(GTK_BOX(tags), tag_lbl("本地", TRUE));

    host_sub(h, subbuf, sizeof subbuf);
    gtk_label_set_label(GTK_LABEL(sub), subbuf);
    g_object_set_data_full(G_OBJECT(card), "hid", g_strdup(h->id), g_free);
    g_object_set_data(G_OBJECT(card), "hv", hv);
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
    GtkGesture *dbl = gtk_gesture_click_new();
    GtkGesture *rgt = gtk_gesture_click_new();
    GtkDragSource *ds = gtk_drag_source_new();
    GtkWidget *bc, *be, *bd;
    gtk_widget_add_css_class(row, "hb-table-row");
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(dbl, "pressed", G_CALLBACK(on_card_dblclick), NULL);
    g_signal_connect(dbl, "released", G_CALLBACK(on_card_released), NULL);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(dbl));
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rgt),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(rgt, "pressed", G_CALLBACK(on_card_right), NULL);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(rgt));
    gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
    g_signal_connect(ds, "prepare", G_CALLBACK(host_drag_prepare), NULL);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(ds));
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
    act = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(act, COL_ACT_W, -1);
    gtk_widget_set_margin_start(act, 8);
    gtk_widget_set_valign(act, GTK_ALIGN_CENTER);
    bc = gtk_button_new();
    gtk_widget_add_css_class(bc, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(bc), hb_icon_new("chev", 13));
    gtk_widget_set_tooltip_text(bc, "连接");
    g_signal_connect(bc, "clicked", G_CALLBACK(on_row_btn), "connect");
    gtk_box_append(GTK_BOX(act), bc);
    be = gtk_button_new();
    gtk_widget_add_css_class(be, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(be), hb_icon_new("pencil", 13));
    gtk_widget_set_tooltip_text(be, "编辑");
    g_signal_connect(be, "clicked", G_CALLBACK(on_row_btn), "edit");
    gtk_box_append(GTK_BOX(act), be);
    bd = gtk_button_new();
    gtk_widget_add_css_class(bd, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(bd), hb_icon_new("trash", 13));
    gtk_widget_set_tooltip_text(bd, "删除");
    g_signal_connect(bd, "clicked", G_CALLBACK(on_row_btn), "del");
    gtk_box_append(GTK_BOX(act), bd);
    gtk_box_append(GTK_BOX(row), act);
    return row;
}

static void row_bind(GtkWidget *row, HbHostItem *h)
{
    GtkWidget *av;
    av = g_object_get_data(G_OBJECT(row), "av");
    char port[16];
    /* 头像就地换键，不重建 widget */
    hb_os_avatar_set_key(av, host_avatar_key(h));

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

static void on_li_sel(GtkListItem *li, GParamSpec *ps, gpointer ud)
{
    GtkWidget *ch = gtk_list_item_get_child(li);
    (void)ps; (void)ud;
    if (!ch) return;
    if (gtk_list_item_get_selected(li))
        gtk_widget_add_css_class(ch, "sel");
    else
        gtk_widget_remove_css_class(ch, "sel");
}

static void grid_setup(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    gtk_list_item_set_child(li, card_shell());
    g_signal_connect(li, "notify::selected", G_CALLBACK(on_li_sel), NULL);
}
static void grid_bind(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f;
    card_bind(HB_HOSTS_VIEW(ud), gtk_list_item_get_child(li),
              HB_HOST_ITEM(gtk_list_item_get_item(li)));
}
static void list_setup(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
    (void)f; (void)ud;
    gtk_list_item_set_child(li, row_shell());
    g_signal_connect(li, "notify::selected", G_CALLBACK(on_li_sel), NULL);
}
static void list_bind(GtkListItemFactory *f, GtkListItem *li, gpointer ud)
{
    GtkWidget *row = gtk_list_item_get_child(li);
    HbHostItem *h = HB_HOST_ITEM(gtk_list_item_get_item(li));
    (void)f;
    g_object_set_data_full(G_OBJECT(row), "hid", g_strdup(h->id), g_free);
    g_object_set_data(G_OBJECT(row), "hv", ud);
    row_bind(row, h);
}

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

/* drop 到分组 chip：host.group = 该分组名 */
static gboolean chip_drop(GtkDropTarget *t, const GValue *v,
                          double x, double y, gpointer ud)
{
    HbHostsView *hv = ud;
    const char *grp = g_object_get_data(G_OBJECT(t), "grp");
    const char *hid = g_value_get_string(v);
    hb_obj *o = NULL;
    char *toast;
    (void)x; (void)y;
    if (!grp || !hid ||
        hb_vault_get(hb_store_vault(hv->store), hid, &o) != HB_OK || !o)
        return FALSE;
    hb_obj_set_str(o, "group", grp);
    hb_vault_put(hb_store_vault(hv->store), o);
    hb_obj_free(o);
    toast = g_strdup_printf("已移到分组「%s」", grp);
    hb_toast(GTK_WIDGET(hv), toast);
    g_free(toast);
    return TRUE;
}

static GtkWidget *chip_btn(HbHostsView *hv, const char *label,
                           const char *key)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "hb-chip");
    g_object_set_data(G_OBJECT(b), "chip",
                      (gpointer)(key ? g_intern_string(key) : NULL));
    g_signal_connect(b, "clicked", G_CALLBACK(on_chip), hv);
    /* 分组 chip（key != NULL 且 != "tun"）是 drop target */
    if (key && strcmp(key, "tun")) {
        GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_STRING,
                                                GDK_ACTION_MOVE);
        g_object_set_data(G_OBJECT(dt), "grp",
                          (gpointer)g_intern_string(key));
        g_signal_connect(dt, "drop", G_CALLBACK(chip_drop), hv);
        gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(dt));
    }
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

/* ==================== ssh_config / JSON 导入导出 ==================== */

typedef struct {
    HbHostsView *hv;
    hb_sshcfg   *cfg;
    AdwDialog  *dlg;
    GtkWidget  *body;
    GtkWidget  *import_btn;
} ImpUd;

static void on_imp_pick_other(GtkButton *b, gpointer ud);

static void imp_ud_free(ImpUd *u)
{
    if (u->cfg) hb_sshcfg_free(u->cfg);
    g_free(u);
}

/* 导入撤销：删除所有 created_ids（含跳板字面量） */
typedef struct {
    HbHostsView *hv;
    char       **ids;
    size_t       n;
} UndoImport;

static void imp_undo(gpointer ud)
{
    UndoImport *u = ud;
    size_t i;
    for (i = 0; i < u->n; i++)
        hb_vault_delete(hb_store_vault(u->hv->store), u->ids[i]);
    g_strfreev(u->ids);
    g_free(u);
}

static void imp_toast_result(HbHostsView *hv, hb_import_result *r)
{
    UndoImport *u;
    char *t;
    u = g_new0(UndoImport, 1);
    u->hv = hv;
    u->n = r->ncreated;
    u->ids = g_new0(char *, u->n + 1);
    for (size_t i = 0; i < u->n; i++)
        u->ids[i] = g_strdup(r->created_ids[i]);
    t = g_strdup_printf("已导入 %u 台主机（新建跳板 %u 台，跳过 %u 台）",
                        (unsigned)r->created, (unsigned)r->jumps_created,
                        (unsigned)r->skipped);
    {
        GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(hv));
        if (HB_IS_WINDOW(root))
            hb_window_toast_undo(HB_WINDOW(root), t,
                                 G_CALLBACK(imp_undo), u);
        else {
            hb_toast(GTK_WIDGET(hv), t);
            g_strfreev(u->ids); g_free(u);
        }
    }
    g_free(t);
}

/* 导入 N 台 */
static void imp_do(ImpUd *u)
{
    hb_import_result r = {0};
    hb_err e = hb_hosts_import_sshcfg(hb_store_vault(u->hv->store),
                                      u->cfg, &r);
    if (e != HB_OK) {
        hb_toast(GTK_WIDGET(u->hv), "导入失败");
    } else {
        imp_toast_result(u->hv, &r);
        if (r.nwarn) {
            /* warnings → toast "查看" */
            GString *w = g_string_new(NULL);
            for (size_t i = 0; i < r.nwarn && i < 20; i++)
                g_string_append_printf(w, "%s\n", r.warnings[i]);
            /* 警告含文件路径/主机名 —— 纯文本对话框，不解析 markup */
            hb_dialog_text(GTK_WIDGET(u->hv), "导入警告", w->str);
            g_string_free(w, TRUE);
        }
    }
    hb_import_result_free(&r);
    adw_dialog_close(u->dlg);
}
static void on_imp_ok(GtkButton *b, gpointer ud) { (void)b; imp_do(ud); }
static void on_imp_cancel(GtkButton *b, gpointer ud)
{ (void)b; adw_dialog_close(((ImpUd *)ud)->dlg); }

/* 预览 dialog 内容：摘要 + alias 列表 + warnings expander */
static void imp_build_preview(ImpUd *u)
{
    hb_sshcfg *c = u->cfg;
    hb_obj **ex = NULL;
    size_t ne = 0, i, j, nskip = 0;
    GtkWidget *sum, *list, *w;
    char *t;
    GtkWidget *f, *ok, *cancel, *ch;

    /* 已存在数（与 core 同名大小写不敏感规则一致） */
    ex = hb_store_list(u->hv->store, "host", &ne);
    for (i = 0; i < c->n; i++)
        for (j = 0; j < ne; j++) {
            const char *nm = hb_obj_get_str(ex[j], "name");
            if (nm && !g_ascii_strcasecmp(nm, c->hosts[i].alias))
                { nskip++; break; }
        }

    t = g_strdup_printf("发现 %u 个 Host（%u 个已存在，将跳过）",
                        (unsigned)c->n, (unsigned)nskip);
    sum = gtk_label_new(t);
    g_free(t);
    gtk_label_set_xalign(GTK_LABEL(sum), 0);
    gtk_widget_add_css_class(sum, "hb-modal-msg");
    gtk_box_append(GTK_BOX(u->body), sum);

    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(list, "hb-imp-list");
    for (i = 0; i < c->n; i++) {
        hb_sshcfg_host *h = &c->hosts[i];
        const char *hn = h->hostname ? h->hostname : h->alias;
        char *line = g_strdup_printf("%s → %s%s%s:%d",
            h->alias,
            h->user && *h->user ? h->user : "",
            h->user && *h->user ? "@" : "",
            hn, h->port ? h->port : 22);
        w = gtk_label_new(line);
        g_free(line);
        gtk_label_set_xalign(GTK_LABEL(w), 0);
        gtk_widget_add_css_class(w, "hb-imp-row");
        gtk_box_append(GTK_BOX(list), w);
        if (h->proxy_jump) {
            GString *j = g_string_new("    跳板: ");
            for (size_t k = 0; h->proxy_jump[k]; k++) {
                if (k) g_string_append(j, " → ");
                g_string_append(j, h->proxy_jump[k]);
            }
            w = gtk_label_new(j->str);
            g_string_free(j, TRUE);
            gtk_label_set_xalign(GTK_LABEL(w), 0);
            gtk_widget_add_css_class(w, "hb-imp-sub");
            gtk_box_append(GTK_BOX(list), w);
        }
        if (h->identity_file) {
            const char *base = strrchr(h->identity_file, '/');
            char *il = g_strdup_printf("    密钥: %s",
                                       base ? base + 1 : h->identity_file);
            w = gtk_label_new(il);
            g_free(il);
            gtk_label_set_xalign(GTK_LABEL(w), 0);
            gtk_widget_add_css_class(w, "hb-imp-sub");
            gtk_box_append(GTK_BOX(list), w);
        }
    }
    gtk_box_append(GTK_BOX(u->body), list);

    if (c->nwarn) {
        GtkWidget *exp = gtk_expander_new(NULL);
        GtkWidget *wb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        char *et = g_strdup_printf("警告（%u）", (unsigned)c->nwarn);
        gtk_expander_set_label(GTK_EXPANDER(exp), et);
        g_free(et);
        for (i = 0; i < c->nwarn; i++) {
            w = gtk_label_new(c->warnings[i]);
            gtk_label_set_xalign(GTK_LABEL(w), 0);
            gtk_label_set_wrap(GTK_LABEL(w), TRUE);
            gtk_widget_add_css_class(w, "hb-imp-sub");
            gtk_box_append(GTK_BOX(wb), w);
        }
        gtk_expander_set_child(GTK_EXPANDER(exp), wb);
        gtk_box_append(GTK_BOX(u->body), exp);
    }
    if (ex) hb_obj_list_free(ex, ne);

    /* footer：选择其它文件… / 取消 / 导入 N 台 */
    f = gtk_widget_get_parent(GTK_WIDGET(u->import_btn));
    while ((ch = gtk_widget_get_first_child(f)))
        gtk_box_remove(GTK_BOX(f), ch);
    {
        GtkWidget *other = gtk_button_new_with_label("选择其它文件…");
        gtk_widget_add_css_class(other, "hb-btn");
        g_signal_connect(other, "clicked",
                         G_CALLBACK(on_imp_pick_other), u);
        gtk_widget_set_halign(other, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(f), other);
    }
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(f), sp);
    }
    cancel = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(cancel, "hb-btn");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_imp_cancel), u);
    gtk_box_append(GTK_BOX(f), cancel);
    t = g_strdup_printf("导入 %u 台", (unsigned)(c->n - nskip));
    ok = gtk_button_new_with_label(t);
    g_free(t);
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(on_imp_ok), u);
    gtk_box_append(GTK_BOX(f), ok);
    u->import_btn = ok;
}

static void imp_rebuild(ImpUd *u)
{
    GtkWidget *ch;
    while ((ch = gtk_widget_get_first_child(u->body)))
        gtk_box_remove(GTK_BOX(u->body), ch);
    imp_build_preview(u);
    /* 内容在 present 之后才填充：强制 dialog 重新按内容计算高度 */
    gtk_widget_queue_resize(GTK_WIDGET(u->dlg));
}

/* 选文件 → 解析 → 填预览 */
static void imp_set_file(ImpUd *u, const char *path)
{
    hb_sshcfg tmp = {0};
    if (u->cfg) { hb_sshcfg_free(u->cfg); u->cfg = NULL; }
    if (hb_sshcfg_parse_file(path, &tmp) != HB_OK) {
        hb_toast(GTK_WIDGET(u->hv), "无法解析该 ssh_config 文件");
        return;
    }
    u->cfg = g_new0(hb_sshcfg, 1);
    *u->cfg = tmp;
    imp_rebuild(u);
}

static void cfg_open_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ImpUd *u = ud;
    GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    if (!gf) return;
    path = g_file_get_path(gf);
    if (path) imp_set_file(u, path);
    g_free(path);
    g_object_unref(gf);
}

static void on_imp_pick_other(GtkButton *b, gpointer ud)
{
    ImpUd *u = ud;
    GtkFileDialog *d = gtk_file_dialog_new();
    (void)b;
    gtk_file_dialog_open(d, NULL, NULL, cfg_open_done, u);
    g_object_unref(d);
}

static void imp_closed(AdwDialog *dlg, gpointer ud)
{ (void)dlg; imp_ud_free(ud); }

/* 入口：优先 $HOME/.ssh/config，不存在则弹文件选择 */
static void act_import_cfg(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbHostsView *hv = ud;
    ImpUd *u;
    GtkWidget *body, *foot, *footbox, *placeholder;
    char *path;
    GtkRoot *root;
    (void)a; (void)p;

    u = g_new0(ImpUd, 1);
    u->hv = hv;
    u->dlg = hb_modal_new("从 ssh_config 导入", &body, &foot);
    u->body = body;
    /* footer 容器（imp_build_preview 里重建按钮） */
    footbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(footbox, "hb-modal-footer");
    gtk_widget_set_margin_start(footbox, 20);
    gtk_widget_set_margin_end(footbox, 20);
    gtk_widget_set_margin_top(footbox, 4);
    gtk_widget_set_margin_bottom(footbox, 16);
    placeholder = gtk_button_new();
    gtk_box_append(GTK_BOX(footbox), placeholder);
    u->import_btn = placeholder;
    gtk_box_append(GTK_BOX(foot), footbox);

    g_signal_connect(u->dlg, "closed", G_CALLBACK(imp_closed), u);

    path = g_build_filename(g_get_home_dir(), ".ssh", "config", NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        imp_set_file(u, path);

    /* 已有内容再 present：AdwDialog 在 present 时按内容定大小，
     * 空 dialog 先弹出后续填充不会自动长高 */
    root = gtk_widget_get_root(GTK_WIDGET(hv));
    adw_dialog_present(u->dlg,
                       GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : NULL);

    if (!u->cfg) {
        GtkFileDialog *d = gtk_file_dialog_new();
        gtk_file_dialog_open(d, NULL, NULL, cfg_open_done, u);
        g_object_unref(d);
    }
    g_free(path);
}

/* ------------------------------ JSON 导出 ------------------------------ */

static void exp_save_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbHostsView *hv = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *json = NULL, *t;
    hb_obj **objs = NULL;
    size_t n = 0;
    GError *err = NULL;
    if (!gf) return;
    if (hb_hosts_export_json(hb_store_vault(hv->store), &json) != HB_OK ||
        !json ||
        !g_file_replace_contents(gf, json, strlen(json), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        hb_toast(GTK_WIDGET(hv), "导出失败");
        if (err) g_error_free(err);
    } else {
        objs = hb_store_list(hv->store, "host", &n);
        t = g_strdup_printf("已导出 %u 台主机（不含密码与私钥）", (unsigned)n);
        hb_toast(GTK_WIDGET(hv), t);
        g_free(t);
        if (objs) hb_obj_list_free(objs, n);
    }
    free(json);
    g_object_unref(gf);
}

static void act_export_json(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbHostsView *hv = ud;
    GtkFileDialog *d = gtk_file_dialog_new();
    GDateTime *dt = g_date_time_new_now_local();
    char *fn;
    GtkFileFilter *ff;
    GListStore *fs;
    (void)a; (void)p;
    fn = g_strdup_printf("harbor-hosts-%s.json",
                         g_date_time_format(dt, "%Y%m%d"));
    g_date_time_unref(dt);
    gtk_file_dialog_set_initial_name(d, fn);
    ff = gtk_file_filter_new();
    gtk_file_filter_add_suffix(ff, "json");
    gtk_file_filter_set_name(ff, "JSON (*.json)");
    fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(fs, ff);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    gtk_file_dialog_set_default_filter(d, ff);
    gtk_file_dialog_save(d, NULL, NULL, exp_save_done, hv);
    g_object_unref(ff);
    g_object_unref(fs);
    g_object_unref(d);
    g_free(fn);
}

/* ------------------------------ JSON 导入 ------------------------------ */

typedef struct { HbHostsView *hv; hb_import_result res; } JsonImp;

static void json_show_warnings(gpointer ud)
{
    JsonImp *j = ud;
    GString *w = g_string_new(NULL);
    size_t i;
    for (i = 0; i < j->res.nwarn; i++)
        g_string_append_printf(w, "%s\n", j->res.warnings[i]);
    hb_dialog_text(GTK_WIDGET(j->hv), "导入警告", w->str);
    g_string_free(w, TRUE);
}

static void json_import_text(HbHostsView *hv, const char *text)
{
    JsonImp *j = g_new0(JsonImp, 1);
    char *t;
    j->hv = hv;
    if (hb_hosts_import_json(hb_store_vault(hv->store), text,
                             &j->res) != HB_OK) {
        hb_toast(GTK_WIDGET(hv), "不是有效的 Harbor 主机文件");
        g_free(j);
        return;
    }
    t = g_strdup_printf("已导入 %u 台主机（跳过 %u 台）",
                        (unsigned)j->res.created,
                        (unsigned)j->res.skipped);
    if (j->res.nwarn) {
        GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(hv));
        if (HB_IS_WINDOW(root)) {
            /* 查看 → warnings dialog；结果对象随回调保留 */
            hb_window_toast_action(HB_WINDOW(root), t, "查看", 9,
                                   G_CALLBACK(json_show_warnings), j);
            /* 撤销需要单独一份 ids（res 归 j 管） */
            {
                UndoImport *u = g_new0(UndoImport, 1);
                u->hv = hv;
                u->n = j->res.ncreated;
                u->ids = g_new0(char *, u->n + 1);
                for (size_t i = 0; i < u->n; i++)
                    u->ids[i] = g_strdup(j->res.created_ids[i]);
                /* 查看按钮已占用 toast 按钮；撤销靠第二个 toast */
                hb_window_toast_undo(HB_WINDOW(root),
                                     "撤销导入？", G_CALLBACK(imp_undo), u);
            }
        } else {
            hb_toast(GTK_WIDGET(hv), t);
            hb_import_result_free(&j->res);
            g_free(j);
            g_free(t);
            return;
        }
    } else {
        imp_toast_result(hv, &j->res);
        hb_import_result_free(&j->res);
        g_free(j);
    }
    g_free(t);
}

static void json_open_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbHostsView *hv = ud;
    GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *text = NULL;
    gsize len = 0;
    if (!gf) return;
    if (g_file_load_contents(gf, NULL, &text, &len, NULL, NULL) && text)
        json_import_text(hv, text);
    else
        hb_toast(GTK_WIDGET(hv), "不是有效的 Harbor 主机文件");
    g_free(text);
    g_object_unref(gf);
}

static void act_import_json(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbHostsView *hv = ud;
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *ff;
    GListStore *fs;
    (void)a; (void)p;
    ff = gtk_file_filter_new();
    gtk_file_filter_add_suffix(ff, "json");
    gtk_file_filter_set_name(ff, "JSON (*.json)");
    fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(fs, ff);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    gtk_file_dialog_set_default_filter(d, ff);
    gtk_file_dialog_open(d, NULL, NULL, json_open_done, hv);
    g_object_unref(ff);
    g_object_unref(fs);
    g_object_unref(d);
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
    /* via 主机名一次解析（bind 时 O(1) 查 h->via_name） */
    {
        GHashTable *names = g_hash_table_new(g_str_hash, g_str_equal);
        guint m = g_list_model_get_n_items(G_LIST_MODEL(hv->all));
        for (i = 0; i < m; i++) {
            HbHostItem *h = g_list_model_get_item(G_LIST_MODEL(hv->all), i);
            g_hash_table_insert(names, h->id, h->name);
            g_object_unref(h);
        }
        for (i = 0; i < m; i++) {
            HbHostItem *h = g_list_model_get_item(G_LIST_MODEL(hv->all), i);
            const char *vn = h->via && *h->via
                ? g_hash_table_lookup(names, h->via) : NULL;
            g_free(h->via_name);
            h->via_name = g_strdup(vn);
            g_object_unref(h);
        }
        g_hash_table_unref(names);
    }
    rebuild_chips(hv);
    gtk_filter_changed(GTK_FILTER(hv->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_body_and_title(hv);
}

static gboolean reload_idle(gpointer ud)
{
    HbHostsView *hv = ud;
    hv->pending_reload = 0;
    reload(hv);
    /* 尽量恢复点击选中的卡（Delete 键目标） */
    if (hv->sel_restore) {
        guint i, n = g_list_model_get_n_items(G_LIST_MODEL(hv->all));
        gboolean alive = FALSE;
        for (i = 0; i < n; i++) {
            HbHostItem *h = g_list_model_get_item(
                G_LIST_MODEL(hv->all), i);
            if (!g_strcmp0(h->id, hv->sel_restore)) alive = TRUE;
            g_object_unref(h);
            if (alive) break;
        }
        if (!alive) { g_free(hv->sel_id); hv->sel_id = NULL; }
        g_free(hv->sel_restore);
        hv->sel_restore = NULL;
    }
    return G_SOURCE_REMOVE;
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbHostsView *hv = ud;
    (void)st; (void)id;
    if (strcmp(type, "host") && strcmp(type, "group")) return;
    if (hv->pending_reload) return;      /* 合并为一次 reload */
    g_free(hv->sel_restore);
    hv->sel_restore = g_strdup(hv->sel_id);
    hv->pending_reload = g_idle_add(reload_idle, hv);
}

/* ------------------------------ UI ------------------------------ */

static void on_search_changed(GtkSearchEntry *e, gpointer ud)
{
    HbHostsView *hv = ud;
    char *nt = fold(gtk_editable_get_text(GTK_EDITABLE(e)));
    GtkFilterChange ch = GTK_FILTER_CHANGE_DIFFERENT;
    const char *old = hv->search_text ? hv->search_text : "";
    /* 新串包含旧串 → 只会更严；反之更宽；否则算完全不同
     * （空串是任何串的子串：首字符=MORE_STRICT、清空=LESS_STRICT） */
    if (strstr(nt, old))
        ch = GTK_FILTER_CHANGE_MORE_STRICT;
    else if (strstr(old, nt))
        ch = GTK_FILTER_CHANGE_LESS_STRICT;
    g_free(hv->search_text);
    hv->search_text = nt;
    gtk_filter_changed(GTK_FILTER(hv->filter), ch);
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

/* 只让可见 view 持有模型：隐藏 view 的 item manager 不跟 items-changed */
static void attach_sel_model(HbHostsView *hv, gboolean list)
{
    GtkSingleSelection *show = list ? hv->list_sel : hv->grid_sel;
    GtkSingleSelection *hide = list ? hv->grid_sel : hv->list_sel;
    HbHostItem *cur = gtk_single_selection_get_selected_item(hide);
    const char *sid = cur ? cur->id : NULL;

    gtk_single_selection_set_model(hide, NULL);
    gtk_single_selection_set_model(show,
        G_LIST_MODEL(g_object_ref(hv->visible)));
    if (cur) {
        guint i, n = g_list_model_get_n_items(hv->visible);
        for (i = 0; i < n; i++) {
            HbHostItem *h = g_list_model_get_item(hv->visible, i);
            gboolean eq = h && !g_strcmp0(h->id, sid);
            g_object_unref(h);
            if (eq) {
                gtk_single_selection_set_selected(show, i);
                break;
            }
        }
        g_object_unref(cur);
    }
}

static void set_mode(HbHostsView *hv, gboolean list)
{
    hv->list_mode = list;
    attach_sel_model(hv, list);
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
    gtk_search_entry_set_search_delay(
        GTK_SEARCH_ENTRY(hv->search), 0);
    gtk_widget_set_hexpand(hv->search, TRUE);
    g_object_set(hv->search, "placeholder-text",
        "Find a host or ssh user@hostname…", NULL);
    g_signal_connect(hv->search, "search-changed",
                     G_CALLBACK(on_search_changed), hv);
    g_signal_connect(hv->search, "activate",
                     G_CALLBACK(on_search_activate), hv);
    gtk_box_append(GTK_BOX(bar), hv->search);

    kbd = gtk_label_new("Ctrl K");
    gtk_widget_add_css_class(kbd, "kbd");
    gtk_widget_set_visible(kbd, FALSE);       /* palette 单元再启用 */
    gtk_box_append(GTK_BOX(bar), kbd);

    conn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(conn, "hb-btn");
    gtk_widget_add_css_class(conn, "primary");
    g_signal_connect(conn, "clicked", G_CALLBACK(on_connect_clicked), hv);
    gtk_box_append(GTK_BOX(bar), conn);
    return bar;
}

static void on_new_click(GtkButton *b, gpointer ud)
{
    HbHostsView *hv = ud;
    (void)b;
    hb_host_form_open(GTK_WIDGET(hv), hv->store, NULL);
}

static GtkWidget *build_toolbar(HbHostsView *hv)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *newb;
    GtkWidget *sort;
    GtkWidget *spacer;
    const char *sort_opts[] = { "按名称", "按最近连接", "按协议", NULL };

    /* New host 分割按钮：主键 → 表单；▾ → 分组/导入/导出 */
    {
        GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *menub = gtk_menu_button_new();
        GMenu *m = g_menu_new();
        gtk_widget_add_css_class(split, "linked");
        newb = gtk_button_new_with_label("New host");
        gtk_widget_add_css_class(newb, "hb-btn");
        gtk_widget_add_css_class(newb, "primary");
        g_signal_connect(newb, "clicked", G_CALLBACK(on_new_click), hv);
        gtk_box_append(GTK_BOX(split), newb);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(menub), "▾");
        gtk_widget_add_css_class(menub, "hb-btn");
        gtk_widget_add_css_class(menub, "primary");
        g_menu_append(m, "新建分组…", "host.newgroup");
        g_menu_append(m, "从 ~/.ssh/config 导入…", "host.importcfg");
        g_menu_append(m, "导入 JSON…", "host.importjson");
        g_menu_append(m, "导出 JSON…", "host.exportjson");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menub),
                                       G_MENU_MODEL(m));
        g_object_unref(m);
        gtk_box_append(GTK_BOX(split), menub);
        gtk_box_append(GTK_BOX(bar), split);
    }

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
    g_clear_object(&hv->visible);
    g_clear_object(&hv->grid_sel);
    g_clear_object(&hv->list_sel);
    g_clear_object(&hv->filtered);
    g_clear_object(&hv->sorted);
    g_clear_object(&hv->filter);
    g_clear_object(&hv->sorter);
    if (hv->pending_reload)
        g_source_remove(hv->pending_reload);
    g_free(hv->search_text);
    g_free(hv->chip);
    g_free(hv->sel_id);
    g_free(hv->sel_restore);
    g_free(hv->focus_key);
    G_OBJECT_CLASS(hb_hosts_view_parent_class)->dispose(o);
}

static void hb_hosts_view_class_init(HbHostsViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_hosts_view_dispose;
    sigs[SIG_QUICK] = g_signal_new("quick-connect", HB_TYPE_HOSTS_VIEW,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    sigs[SIG_CONNECT] = g_signal_new("connect", HB_TYPE_HOSTS_VIEW,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

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
    hv->sorter = gtk_custom_sorter_new(sort_fn, hv, NULL);
    /* 先排后滤：filter 变化不碰 SortListModel（重排是它的大头开销） */
    hv->sorted = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(hv->all)),
        GTK_SORTER(g_object_ref(hv->sorter)));
    hv->filter = gtk_custom_filter_new(filter_fn, hv, NULL);
    hv->filtered = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(hv->sorted)),
        GTK_FILTER(hv->filter));
    /* 大列表：分片过滤 + 分片 items-changed，同步路径不进 item manager
     * 的大窗口重建（见 FR-01 验收 5 的 100ms 预算） */
    gtk_filter_list_model_set_incremental(hv->filtered, TRUE);
    hv->visible = hb_slice_model_new(G_LIST_MODEL(hv->filtered));
    g_signal_connect(hv->filtered, "items-changed",
                     G_CALLBACK(on_items_changed), hv);

    {
        /* 内容列 + drawer overlay：drawer 浮在列表右侧 */
        GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *ov;
        hv->drawer = hb_host_drawer_new(st);
        ov = gtk_overlay_new();
        gtk_widget_set_vexpand(ov, TRUE);
        g_object_set_data(G_OBJECT(hv), "inner", inner);
        gtk_overlay_set_child(GTK_OVERLAY(ov), inner);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov),
                                hb_host_drawer_widget(hv->drawer));
        gtk_box_append(GTK_BOX(hv), ov);
        hv->body_root = inner;
    }
    gtk_box_append(GTK_BOX(hv->body_root), build_quickbar(hv));
    gtk_box_append(GTK_BOX(hv->body_root), build_toolbar(hv));

    /* chip 行 */
    {
        GtkWidget *sc = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
            GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
        gtk_widget_add_css_class(sc, "hb-chiprow");
        hv->chiprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), hv->chiprow);
        gtk_box_append(GTK_BOX(hv->body_root), sc);
    }

    hv->title_lbl = gtk_label_new("Hosts · 0");
    gtk_widget_add_css_class(hv->title_lbl, "hb-section-title");
    gtk_label_set_xalign(GTK_LABEL(hv->title_lbl), 0);
    gtk_widget_set_margin_start(hv->title_lbl, 20);
    gtk_box_append(GTK_BOX(hv->body_root), hv->title_lbl);

    hv->body = gtk_stack_new();
    gtk_widget_set_vexpand(hv->body, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(hv->body),
        GTK_STACK_TRANSITION_TYPE_NONE);

    /* 网格：模型由 set_mode 按需挂上（隐藏的 view 不持有） */
    hv->grid_sel = gtk_single_selection_new(NULL);
    sel = GTK_SELECTION_MODEL(hv->grid_sel);
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
    hv->list_sel = gtk_single_selection_new(NULL);
    sel = GTK_SELECTION_MODEL(hv->list_sel);
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
    gtk_box_append(GTK_BOX(hv->body_root), hv->body);

    hv->changed_handler = g_signal_connect(hv->store, "changed",
        G_CALLBACK(on_store_changed), hv);

    add_view_actions(hv);
    {
        GtkEventController *kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed", G_CALLBACK(on_view_key), hv);
        gtk_widget_add_controller(GTK_WIDGET(hv), kc);
    }

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
