/* hb-workspace.c — Workspace 分屏容器（FR-03）
 *
 * 布局：WsNode 二叉树。叶子 = pane box（pane-head + compact session page），
 * 内部节点 = GtkPaned。root widget 是 root_host 唯一可见子。
 * 分屏 = 把目标叶子的位置换成新 paned（旧叶子 start、新叶子 end），50%。
 * 关 pane = 叶子摘除 + paned 用兄弟顶替（递归到根）。
 * Focus 模式 = 把焦点叶子摘出树挂进 root_host，整棵树根隐藏；还原时回插
 * 原槽位（paned 位置未动 → 尺寸复原）。
 * ================================================================== */
#include "hb-workspace.h"
#include "hb-session-page.h"
#include "hb-term-page.h"
#include "hb-ssh-page.h"
#include "hb-find-bar.h"
#include "hb-icons.h"
#include "hb-window.h"
#include "hb-dialogs.h"
#include "hb-snippet-send.h"

#include <harbor/vault.h>
#include <string.h>

#define MAX_PANES 6

typedef struct WsNode WsNode;
struct WsNode {
    gboolean   is_leaf;
    GtkWidget *w;             /* leaf pane box 或 GtkPaned */
    WsNode    *parent;
    WsNode    *a, *b;         /* paned 孩子（a=start, b=end） */
    double     init_frac;     /* paned 首次布局的目标比例 */
    /* 叶子： */
    GtkWidget *page;          /* HbSessionPage 实现 */
    GtkWidget *head_ic;
    GtkWidget *head_lbl;
    GtkWidget *reconn_btn;
    gulong     meta_id;
};

struct _HbWorkspace {
    GtkBox parent;

    HbStore  *store;          /* borrowed */
    HbWindow *win;            /* borrowed */
    char     *tab_id;

    GtkWidget *tt_label;
    GtkWidget *bcast_btn;
    GtkWidget *assist_btn;
    GtkWidget *focus_btn;
    GtkWidget *find_bar;
    GtkWidget *root_host;     /* 布局根 widget 的容器 */
    GtkWidget *assist;
    GtkWidget *snip_list;
    GtkWidget *footer;
    GtkWidget *hosted_status; /* 当前借到 footer 的 status widget（borrowed） */

    WsNode   *root;
    WsNode   *focused;
    GList    *leaves;         /* WsNode*（仅叶子） */
    int       n_panes;
    gboolean  broadcast;
    gboolean  focus_mode;
    WsNode   *fm_slot_parent; /* focus 模式下叶子原槽位 */
    gboolean  fm_slot_start;
    char     *theme_cls;      /* 当前 "theme-<id>" class（随 settings 变） */

    gulong    store_sig;
};

G_DEFINE_TYPE(HbWorkspace, hb_workspace, GTK_TYPE_BOX)

static void focus_pane(HbWorkspace *ws, WsNode *n, gboolean grab);
static void fill_snippets(HbWorkspace *ws);

/* ------------------------------ 小工具 ------------------------------ */

static HbSessionPage *page_of(WsNode *n)
{
    return HB_SESSION_PAGE(n->page);
}

static WsNode *leaf_of_page(HbWorkspace *ws, GtkWidget *page)
{
    GList *l;
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n = l->data;
        if (n->page == page) return n;
    }
    return NULL;
}

static void update_title(HbWorkspace *ws)
{
    char *t = g_strdup_printf("Workspace · %d 分屏", ws->n_panes);
    gtk_label_set_label(GTK_LABEL(ws->tt_label), t);
    g_free(t);
}

/* ------------------------- Broadcast 输入镜像 ------------------------- */

static void mirror_forward(HbTerminal *src,
                           const hb_terminal_mirror_event *ev, gpointer ud)
{
    HbWorkspace *ws = ud;
    GList *l;
    if (!ws->broadcast) return;
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n = l->data;
        HbTerminal *tt = hb_session_page_terminal(page_of(n));
        hb_session *s;
        if (!tt || tt == src) continue;
        s = hb_terminal_get_session(tt);
        if (!s || !hb_session_alive(s)) continue;
        switch (ev->kind) {
        case HB_MIRROR_KEY:
            hb_session_key(s, ev->key, ev->mods);
            break;
        case HB_MIRROR_UNICHAR:
            hb_session_unichar(s, ev->unichar, ev->mods);
            break;
        case HB_MIRROR_PASTE:
            hb_session_paste(s, ev->text);
            break;
        case HB_MIRROR_RAW:
        default:
            hb_session_write(s, ev->text, ev->len);
            break;
        }
    }
}

static void apply_broadcast(HbWorkspace *ws)
{
    GList *l;
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n = l->data;
        HbTerminal *t = hb_session_page_terminal(page_of(n));
        if (!t) continue;
        hb_terminal_set_input_mirror(t,
            ws->broadcast ? mirror_forward : NULL, ws);
        if (ws->broadcast)
            gtk_widget_add_css_class(n->w, "broadcast");
        else
            gtk_widget_remove_css_class(n->w, "broadcast");
    }
}

/* ------------------------------ pane-head ------------------------------ */

static void update_head(WsNode *n)
{
    HbSessionPage *sp = page_of(n);
    const char *ic = hb_session_page_icon(sp);
    const char *tt = hb_session_page_title(sp);
    if (ic) hb_icon_set_name(HB_ICON(n->head_ic), ic);
    gtk_label_set_label(GTK_LABEL(n->head_lbl), tt ? tt : "");
    gtk_widget_set_visible(n->reconn_btn, hb_session_page_is_dead(sp));
}

static void on_meta(HbSessionPage *p, gpointer ud)
{
    (void)p;
    update_head(ud);
}

static void on_head_watch(GtkButton *b, gpointer ud)
{
    WsNode *n = ud;
    (void)b;
    hb_session_page_open_watch(page_of(n));
}

static void on_head_reconn(GtkButton *b, gpointer ud)
{
    WsNode *n = ud;
    (void)b;
    hb_session_page_reconnect(page_of(n));
}

static void on_head_close(GtkButton *b, gpointer ud)
{
    WsNode *n = ud;
    HbWorkspace *ws = HB_WORKSPACE(gtk_widget_get_ancestor(n->w,
        HB_TYPE_WORKSPACE));
    (void)b;
    if (ws && ws->win)
        hb_workspace_close_page(ws, n->page);
}

static GtkWidget *build_head(HbWorkspace *ws, WsNode *n)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *eye, *x;
    (void)ws;

    gtk_widget_add_css_class(h, "pane-head");
    n->head_ic = hb_icon_new("terminal", 12);
    gtk_widget_add_css_class(n->head_ic, "pane-head-ic");
    gtk_box_append(GTK_BOX(h), n->head_ic);

    n->head_lbl = gtk_label_new("");
    gtk_widget_add_css_class(n->head_lbl, "pane-head-tt");
    gtk_label_set_xalign(GTK_LABEL(n->head_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(n->head_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(n->head_lbl, TRUE);
    gtk_box_append(GTK_BOX(h), n->head_lbl);

    eye = gtk_button_new();
    gtk_widget_add_css_class(eye, "pane-head-btn");
    gtk_widget_set_tooltip_text(eye, "监控设置");
    gtk_button_set_child(GTK_BUTTON(eye), hb_icon_new("eye", 11));
    g_signal_connect(eye, "clicked", G_CALLBACK(on_head_watch), n);
    gtk_box_append(GTK_BOX(h), eye);

    n->reconn_btn = gtk_button_new();
    gtk_widget_add_css_class(n->reconn_btn, "pane-head-btn");
    gtk_widget_set_tooltip_text(n->reconn_btn, "重新连接");
    gtk_button_set_child(GTK_BUTTON(n->reconn_btn), hb_icon_new("refresh", 11));
    g_signal_connect(n->reconn_btn, "clicked",
                     G_CALLBACK(on_head_reconn), n);
    gtk_widget_set_visible(n->reconn_btn, FALSE);
    gtk_box_append(GTK_BOX(h), n->reconn_btn);

    x = gtk_button_new();
    gtk_widget_add_css_class(x, "pane-head-btn");
    gtk_widget_set_tooltip_text(x, "关闭分屏 (Ctrl+Shift+W)");
    gtk_button_set_child(GTK_BUTTON(x), hb_icon_new("x", 11));
    g_signal_connect(x, "clicked", G_CALLBACK(on_head_close), n);
    gtk_box_append(GTK_BOX(h), x);
    return h;
}

/* ------------------------------ 树操作 ------------------------------ */

/* 把 widget 从它当前的 GTK 父容器里摘除（调用方持 ref 保活或任其销毁） */
static void detach_widget(HbWorkspace *ws, WsNode *n)
{
    WsNode *pn = n->parent;
    if (!pn) {
        /* 根：直接孩子在 root_host 或 focus 模式下悬在 root_host */
        if (gtk_widget_get_parent(n->w) == ws->root_host)
            gtk_box_remove(GTK_BOX(ws->root_host), n->w);
        return;
    }
    if (pn->a == n) gtk_paned_set_start_child(GTK_PANED(pn->w), NULL);
    else            gtk_paned_set_end_child(GTK_PANED(pn->w), NULL);
}

static WsNode *make_leaf(HbWorkspace *ws, GtkWidget *page);
static void remove_leaf(HbWorkspace *ws, WsNode *n);

/* paned 首帧布局后按比例落位 */
static gboolean first_alloc(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    GtkPaned *p = GTK_PANED(w);
    double frac = GPOINTER_TO_INT(ud) / 1000.0;
    int s;
    (void)fc;
    s = gtk_orientable_get_orientation(GTK_ORIENTABLE(p)) ==
            GTK_ORIENTATION_HORIZONTAL
        ? gtk_widget_get_width(w) : gtk_widget_get_height(w);
    if (s > 4) {
        gtk_paned_set_position(p, (int)(s * frac + 0.5));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_init_pos(GtkWidget *paned, double frac)
{
    gtk_widget_add_tick_callback(paned, first_alloc,
        GINT_TO_POINTER((int)(frac * 1000 + 0.5)), NULL);
}

/* 把 leaf 挂到 target 的 o 侧：target 原位换成新 paned */
static void insert_leaf(HbWorkspace *ws, WsNode *target, WsNode *leaf,
                        GtkOrientation o, double frac)
{
    WsNode *pn = g_new0(WsNode, 1);
    GtkWidget *paned = gtk_paned_new(o);

    pn->is_leaf = FALSE;
    pn->w = paned;
    pn->parent = target->parent;
    pn->init_frac = frac;
    gtk_widget_add_css_class(paned, "ws-paned");
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);

    g_object_ref(target->w);
    detach_widget(ws, target);
    pn->a = target;
    pn->b = leaf;
    target->parent = leaf->parent = pn;
    gtk_paned_set_start_child(GTK_PANED(paned), target->w);
    g_object_unref(target->w);
    gtk_paned_set_end_child(GTK_PANED(paned), leaf->w);

    if (pn->parent) {
        if (pn->parent->a == target) {
            pn->parent->a = pn;
            gtk_paned_set_start_child(GTK_PANED(pn->parent->w), paned);
        } else {
            pn->parent->b = pn;
            gtk_paned_set_end_child(GTK_PANED(pn->parent->w), paned);
        }
    } else {
        ws->root = pn;
        gtk_box_append(GTK_BOX(ws->root_host), paned);
    }
    set_init_pos(paned, frac);
    ws->leaves = g_list_append(ws->leaves, leaf);
    ws->n_panes++;
    update_title(ws);
}

/* 摘除并销毁叶子（page 随之销毁 → 会话结束）。不处理 n_panes<=1 的收尾。 */
static void remove_leaf(HbWorkspace *ws, WsNode *n)
{
    WsNode *pn = n->parent, *sib;
    GtkWidget *st;
    HbTerminal *t;

    ws->leaves = g_list_remove(ws->leaves, n);
    ws->n_panes--;

    /* footer 托管的状态条先还回（widget 有 page 的 ref，unparent 不销毁） */
    st = hb_session_page_status_widget(page_of(n));
    if (ws->hosted_status == st) {
        gtk_widget_unparent(st);
        ws->hosted_status = NULL;
    }
    t = hb_session_page_terminal(page_of(n));
    if (t) hb_terminal_set_input_mirror(t, NULL, NULL);
    g_signal_handler_disconnect(n->page, n->meta_id);

    if (!pn) {
        /* 唯一叶子（理论上调用方先 take_last_page；兜底直接销毁） */
        ws->root = NULL;
        detach_widget(ws, n);       /* n->w unparent → 销毁 */
        g_free(n);
        update_title(ws);
        return;
    }

    /* 销毁叶子 widget（page 一同销毁） */
    detach_widget(ws, n);
    g_free(n);

    /* 兄弟顶替 paned 的位置 */
    sib = (pn->a == n) ? pn->b : pn->a;
    g_object_ref(sib->w);
    detach_widget(ws, sib);         /* sib->parent 仍是 pn：从旧 paned 摘除 */
    sib->parent = pn->parent;
    if (pn->parent) {
        if (pn->parent->a == pn) {
            pn->parent->a = sib;
            gtk_paned_set_start_child(GTK_PANED(pn->parent->w), sib->w);
        } else {
            pn->parent->b = sib;
            gtk_paned_set_end_child(GTK_PANED(pn->parent->w), sib->w);
        }
    } else {
        ws->root = sib;
        gtk_box_append(GTK_BOX(ws->root_host), sib->w);
    }
    g_object_unref(sib->w);
    gtk_widget_unparent(pn->w);     /* 空 paned 销毁 */
    g_free(pn);

    if (ws->focused == n)
        ws->focused = NULL;
    update_title(ws);
}

/* ------------------------------ 焦点 ------------------------------ */

static void rehost_status(HbWorkspace *ws, WsNode *n)
{
    GtkWidget *st = hb_session_page_status_widget(page_of(n));
    if (ws->hosted_status == st) return;
    if (ws->hosted_status)
        gtk_widget_unparent(ws->hosted_status);
    ws->hosted_status = st;
    if (st && gtk_widget_get_parent(st) != ws->footer)
        gtk_box_append(GTK_BOX(ws->footer), st);
}

static void focus_pane(HbWorkspace *ws, WsNode *n, gboolean grab)
{
    if (ws->focused == n) {
        rehost_status(ws, n);
        if (grab) {
            HbTerminal *t = hb_session_page_terminal(page_of(n));
            if (t) gtk_widget_grab_focus(GTK_WIDGET(t));
        }
        return;
    }
    if (ws->focused)
        gtk_widget_remove_css_class(ws->focused->w, "focused");
    ws->focused = n;
    gtk_widget_add_css_class(n->w, "focused");
    rehost_status(ws, n);
    /* find bar 改绑焦点 pane 的终端 */
    {
        HbTerminal *t = hb_session_page_terminal(page_of(n));
        hb_find_bar_bind(HB_FIND_BAR(ws->find_bar), t);
    }
    if (ws->snip_list)
        fill_snippets(ws);   /* scope 跟随焦点 host */
    if (grab) {
        HbTerminal *t = hb_session_page_terminal(page_of(n));
        if (t) gtk_widget_grab_focus(GTK_WIDGET(t));
    }
}

static void on_pane_click(GtkGesture *g, int npress, double x, double y,
                          gpointer ud)
{
    WsNode *n = ud;
    HbWorkspace *ws = HB_WORKSPACE(gtk_widget_get_ancestor(n->w,
        HB_TYPE_WORKSPACE));
    (void)g; (void)npress; (void)x; (void)y;
    if (ws) focus_pane(ws, n, TRUE);
}

static void on_pane_focus_enter(GtkEventControllerFocus *fc, gpointer ud)
{
    WsNode *n = ud;
    HbWorkspace *ws = HB_WORKSPACE(gtk_widget_get_ancestor(n->w,
        HB_TYPE_WORKSPACE));
    (void)fc;
    if (ws) focus_pane(ws, n, FALSE);
}

/* Alt+方向：按 pane 中心几何找最近候选 */
void hb_workspace_move_focus(HbWorkspace *ws, int dir)
{
    static const graphene_point_t zero = { 0, 0 };
    graphene_point_t p0;
    double cx0 = 0, cy0 = 0;
    GList *l;
    WsNode *best = NULL;
    double best_d = 0;
    if (!ws->focused || ws->focus_mode) return;
    if (!gtk_widget_compute_point(ws->focused->w, GTK_WIDGET(ws),
                                  &zero, &p0))
        return;
    cx0 = p0.x + gtk_widget_get_width(ws->focused->w) / 2.0;
    cy0 = p0.y + gtk_widget_get_height(ws->focused->w) / 2.0;
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n = l->data;
        graphene_point_t p;
        double cx, cy, dx, dy, d;
        if (n == ws->focused) continue;
        if (!gtk_widget_compute_point(n->w, GTK_WIDGET(ws), &zero, &p))
            continue;
        cx = p.x + gtk_widget_get_width(n->w) / 2.0;
        cy = p.y + gtk_widget_get_height(n->w) / 2.0;
        dx = cx - cx0; dy = cy - cy0;
        /* 只取目标方向上的 pane；距离 = 正交偏离 + 前向距离 */
        if ((dir == 0 && dx >= -1) || (dir == 1 && dx <= 1) ||
            (dir == 2 && dy >= -1) || (dir == 3 && dy <= 1))
            continue;
        if (dir <= 1) d = fabs(dx) + fabs(dy) * 2;
        else          d = fabs(dy) + fabs(dx) * 2;
        if (!best || d < best_d) { best = n; best_d = d; }
    }
    if (best) focus_pane(ws, best, TRUE);
}

/* ------------------------------ 叶子创建 ------------------------------ */

static WsNode *make_leaf(HbWorkspace *ws, GtkWidget *page)
{
    WsNode *n = g_new0(WsNode, 1);
    GtkWidget *leaf, *head;
    GtkEventController *fc;
    GtkGesture *click;
    HbTerminal *t;

    n->is_leaf = TRUE;
    n->page = page;
    hb_session_page_set_compact(HB_SESSION_PAGE(page), TRUE);

    leaf = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(leaf, "pane");
    gtk_widget_set_hexpand(leaf, TRUE);
    gtk_widget_set_vexpand(leaf, TRUE);
    head = build_head(ws, n);
    gtk_box_append(GTK_BOX(leaf), head);
    gtk_widget_set_vexpand(page, TRUE);
    gtk_box_append(GTK_BOX(leaf), page);
    n->w = leaf;

    n->meta_id = g_signal_connect(page, "meta-changed",
        G_CALLBACK(on_meta), n);
    update_head(n);

    fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "enter", G_CALLBACK(on_pane_focus_enter), n);
    gtk_widget_add_controller(leaf, fc);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pane_click), n);
    gtk_widget_add_controller(leaf, GTK_EVENT_CONTROLLER(click));

    /* broadcast 已开 → 新 pane 也装镜像 */
    t = hb_session_page_terminal(page_of(n));
    if (t && ws->broadcast) {
        hb_terminal_set_input_mirror(t, mirror_forward, ws);
        gtk_widget_add_css_class(leaf, "broadcast");
    }
    return n;
}

/* ------------------------------ 公开 API ------------------------------ */

void hb_workspace_seed(HbWorkspace *ws, GtkWidget *page)
{
    WsNode *n;
    g_return_if_fail(HB_IS_SESSION_PAGE(page));
    g_return_if_fail(ws->root == NULL);
    n = make_leaf(ws, page);
    ws->root = n;
    ws->leaves = g_list_append(ws->leaves, n);
    ws->n_panes = 1;
    gtk_box_append(GTK_BOX(ws->root_host), n->w);
    if (ws->tab_id)
        hb_session_page_attach_tab(page_of(n), GTK_WIDGET(ws->win),
                                   ws->tab_id);
    focus_pane(ws, n, FALSE);
    update_title(ws);
}

gboolean hb_workspace_can_split(HbWorkspace *ws)
{
    return ws->n_panes < MAX_PANES;
}

gboolean hb_workspace_split_focused(HbWorkspace *ws, GtkOrientation o)
{
    GtkWidget *clone;
    WsNode *leaf;
    if (!ws->focused || ws->n_panes >= MAX_PANES) return FALSE;
    clone = hb_session_page_clone(page_of(ws->focused));
    if (!clone) return FALSE;
    leaf = make_leaf(ws, clone);
    if (ws->tab_id)
        hb_session_page_attach_tab(page_of(leaf), GTK_WIDGET(ws->win),
                                   ws->tab_id);
    insert_leaf(ws, ws->focused, leaf, o, 0.5);
    focus_pane(ws, leaf, TRUE);
    return TRUE;
}

void hb_workspace_add_pane(HbWorkspace *ws, GtkWidget *page,
                           GtkOrientation o, double frac)
{
    WsNode *leaf, *target;
    g_return_if_fail(HB_IS_SESSION_PAGE(page));
    leaf = make_leaf(ws, page);
    if (ws->tab_id)
        hb_session_page_attach_tab(page_of(leaf), GTK_WIDGET(ws->win),
                                   ws->tab_id);
    target = ws->leaves ? g_list_last(ws->leaves)->data : NULL;
    if (!target) {
        ws->root = leaf;
        ws->leaves = g_list_append(ws->leaves, leaf);
        ws->n_panes = 1;
        gtk_box_append(GTK_BOX(ws->root_host), leaf->w);
        update_title(ws);
    } else {
        insert_leaf(ws, target, leaf, o, frac);
    }
    focus_pane(ws, leaf, TRUE);
}

/* ws 需要收尾时通知 window（close_page/close_focused/close_host 共用） */
static void sync_window(HbWorkspace *ws)
{
    if (ws->win)
        hb_window_workspace_panes_changed(ws->win, GTK_WIDGET(ws));
}

gboolean hb_workspace_close_page(HbWorkspace *ws, GtkWidget *page)
{
    WsNode *n = leaf_of_page(ws, page);
    int np;
    if (!n) return FALSE;
    if (ws->n_panes <= 1) { sync_window(ws); return TRUE; }
    if (ws->focus_mode)
        hb_workspace_toggle_focus_mode(ws);       /* 先还原布局再摘 */
    remove_leaf(ws, n);
    if (!ws->focused && ws->leaves)
        focus_pane(ws, ws->leaves->data, TRUE);
    np = ws->n_panes;
    sync_window(ws);          /* 可能塌/销毁 ws —— 之后不能再碰 ws */
    return np <= 1;
}

gboolean hb_workspace_close_focused(HbWorkspace *ws)
{
    if (!ws->focused) return FALSE;
    return hb_workspace_close_page(ws, ws->focused->page);
}

gboolean hb_workspace_close_host(HbWorkspace *ws, const char *host_id)
{
    GList *kill = NULL, *l;
    if (!host_id) return FALSE;
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n = l->data;
        const char *hid = hb_session_page_host_id(page_of(n));
        if (hid && !strcmp(hid, host_id))
            kill = g_list_prepend(kill, n);
    }
    if (ws->focus_mode && kill)
        hb_workspace_toggle_focus_mode(ws);
    for (l = kill; l; l = l->next) {
        WsNode *n = l->data;
        if (g_list_find(ws->leaves, n) && ws->n_panes > 0)
            remove_leaf(ws, n);
    }
    g_list_free(kill);
    if (!ws->focused && ws->leaves)
        focus_pane(ws, ws->leaves->data, TRUE);
    {
        int np = ws->n_panes;
        sync_window(ws);      /* 可能塌/销毁 ws */
        return np <= 1;
    }
}

GtkWidget *hb_workspace_take_last_page(HbWorkspace *ws)
{
    WsNode *n;
    GtkWidget *page, *st;
    if (ws->n_panes != 1 || !ws->leaves) return NULL;
    n = ws->leaves->data;
    page = n->page;
    g_object_ref(page);                 /* 跨 unparent 保活，引用转交调用方 */

    st = hb_session_page_status_widget(page_of(n));
    if (ws->hosted_status == st) {
        gtk_widget_unparent(st);
        ws->hosted_status = NULL;
    }
    /* 恢复成独立页：工具栏回来、状态条重新 append 回页内 */
    hb_session_page_set_compact(HB_SESSION_PAGE(page), FALSE);
    g_signal_handler_disconnect(page, n->meta_id);
    {
        HbTerminal *t = hb_session_page_terminal(page_of(n));
        if (t) hb_terminal_set_input_mirror(t, NULL, NULL);
    }
    gtk_box_remove(GTK_BOX(n->w), page);     /* 摘出 page */
    detach_widget(ws, n);                    /* 销毁空 pane 壳 */
    ws->leaves = NULL;
    ws->root = NULL;
    ws->focused = NULL;
    ws->n_panes = 0;
    g_free(n);
    return page;
}

guint hb_workspace_pane_count(HbWorkspace *ws)
{
    return (guint)ws->n_panes;
}

GtkWidget *hb_workspace_focused_page(HbWorkspace *ws)
{
    return ws->focused ? ws->focused->page : NULL;
}

GtkWidget *hb_workspace_resolve_page(GtkWidget *page)
{
    if (page && HB_IS_WORKSPACE(page))
        return hb_workspace_focused_page(HB_WORKSPACE(page));
    return page;
}

void hb_workspace_attach_tab(HbWorkspace *ws, GtkWidget *win,
                             const char *tab_id)
{
    GList *l;
    ws->win = HB_WINDOW(win);
    g_free(ws->tab_id);
    ws->tab_id = g_strdup(tab_id);
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n = l->data;
        hb_session_page_attach_tab(page_of(n), win, tab_id);
    }
}

/* ------------------------------ Focus 模式 ------------------------------ */

void hb_workspace_toggle_focus_mode(HbWorkspace *ws)
{
    if (!ws->focus_mode && (ws->n_panes < 2 || !ws->focused)) return;
    ws->focus_mode = !ws->focus_mode;
    if (ws->focus_mode) {
        WsNode *n = ws->focused;
        /* 记住槽位 → 摘出 → 独占 root_host */
        ws->fm_slot_parent = n->parent;
        ws->fm_slot_start = n->parent ? (n->parent->a == n) : TRUE;
        g_object_ref(n->w);
        detach_widget(ws, n);
        gtk_widget_set_visible(ws->root->w, FALSE);
        gtk_box_append(GTK_BOX(ws->root_host), n->w);
        g_object_unref(n->w);
        gtk_widget_add_css_class(GTK_WIDGET(ws), "focusmode");
    } else {
        WsNode *n = ws->focused;
        g_object_ref(n->w);
        if (gtk_widget_get_parent(n->w) == ws->root_host)
            gtk_box_remove(GTK_BOX(ws->root_host), n->w);
        /* 回插原槽位（树结构未动） */
        if (ws->fm_slot_parent) {
            if (ws->fm_slot_start)
                gtk_paned_set_start_child(
                    GTK_PANED(ws->fm_slot_parent->w), n->w);
            else
                gtk_paned_set_end_child(
                    GTK_PANED(ws->fm_slot_parent->w), n->w);
        } else {
            gtk_box_append(GTK_BOX(ws->root_host), n->w);
        }
        g_object_unref(n->w);
        gtk_widget_set_visible(ws->root->w, TRUE);
        ws->fm_slot_parent = NULL;
        gtk_widget_remove_css_class(GTK_WIDGET(ws), "focusmode");
    }
    gtk_widget_set_visible(ws->focus_btn, TRUE);
    if (ws->focus_mode)
        gtk_widget_add_css_class(ws->focus_btn, "on");
    else
        gtk_widget_remove_css_class(ws->focus_btn, "on");
}

gboolean hb_workspace_is_focus_mode(HbWorkspace *ws)
{
    return ws->focus_mode;
}

gboolean hb_workspace_is_broadcast(HbWorkspace *ws)
{
    return ws->broadcast;
}

void hb_workspace_open_find(HbWorkspace *ws)
{
    hb_find_bar_open(HB_FIND_BAR(ws->find_bar));
}

/* ------------------------------ 工具栏按钮 ------------------------------ */

static void on_bcast(GtkButton *b, gpointer ud)
{
    HbWorkspace *ws = ud;
    ws->broadcast = !ws->broadcast;
    apply_broadcast(ws);
    if (ws->broadcast) {
        gtk_button_set_label(b, "Broadcast 开");
        gtk_widget_add_css_class(GTK_WIDGET(b), "on");
    } else {
        gtk_button_set_label(b, "Broadcast");
        gtk_widget_remove_css_class(GTK_WIDGET(b), "on");
    }
}

static void on_focus_btn(GtkButton *b, gpointer ud)
{
    (void)b;
    hb_workspace_toggle_focus_mode(HB_WORKSPACE(ud));
}

static void on_assist_btn(GtkButton *b, gpointer ud);
static void on_search_btn(GtkButton *b, gpointer ud)
{
    HbWorkspace *ws = ud;
    (void)b;
    hb_workspace_open_find(ws);
}

/* ------------------------------ 助手栏 ------------------------------ */

guint hb_workspace_paste_all(HbWorkspace *ws, const char *text, gboolean run)
{
    GList *l;
    guint n = 0;
    if (!text || !*text) return 0;
    for (l = ws->leaves; l; l = l->next) {
        WsNode *n2 = l->data;
        HbTerminal *t;
        hb_session *s;
        t = hb_session_page_terminal(page_of(n2));
        s = t ? hb_terminal_get_session(t) : NULL;
        if (s && hb_session_alive(s)) {
            hb_session_paste(s, text);      /* 不带换行：留在提示符 */
            if (run)
                hb_session_key(s, HB_KEY_ENTER, 0);
            n++;
        }
    }
    return n;
}

static void on_snip_btn(GtkButton *b, gpointer ud)
{
    HbWorkspace *ws = ud;
    const char *sid = g_object_get_data(G_OBJECT(b), "sid");
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(ws),
                                             HB_TYPE_WINDOW);
    if (win)
        hb_snippet_send(win, ws->store, sid);
}

/* 焦点 pane 的 host_id / host.group（scope 过滤用）；local → NULL/NULL */
static void focused_host_info(HbWorkspace *ws,
                              const char **host_id, const char **group)
{
    static char grp[128];
    hb_obj *h = NULL;
    *host_id = NULL;
    *group = NULL;
    grp[0] = 0;
    if (!ws->focused)
        return;
    *host_id = hb_session_page_host_id(page_of(ws->focused));
    if (!*host_id || !ws->store)
        return;
    if (hb_vault_get(hb_store_vault(ws->store), *host_id, &h) == HB_OK && h) {
        const char *g = hb_obj_get_str(h, "group");
        if (g && *g) {
            snprintf(grp, sizeof grp, "%s", g);
            *group = grp;
        }
        hb_obj_free(h);
    }
}

static void fill_snippets(HbWorkspace *ws)
{
    GtkWidget *c;
    hb_obj **objs;
    size_t n = 0, i, shown = 0;
    const char *host_id, *group;
    while ((c = gtk_widget_get_first_child(ws->snip_list)))
        gtk_box_remove(GTK_BOX(ws->snip_list), c);
    if (!ws->store) return;
    focused_host_info(ws, &host_id, &group);
    objs = hb_store_list(ws->store, "snippet", &n);
    for (i = 0; objs && i < n; i++) {
        hb_obj *s = objs[i];
        const char *title = hb_obj_get_str(s, "title");
        const char *body = hb_obj_get_str(s, "body");
        const char *scope = hb_obj_get_str(s, "scope");
        GtkWidget *b, *r, *t, *d;
        if (!hb_snippet_scope_match(scope, host_id, group))
            continue;
        shown++;
        b = gtk_button_new();
        gtk_widget_add_css_class(b, "assist-item");
        r = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        t = gtk_label_new(title && *title ? title : "snippet");
        gtk_widget_add_css_class(t, "assist-item-tt");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_box_append(GTK_BOX(r), t);
        d = gtk_label_new(body ? body : "");
        gtk_widget_add_css_class(d, "assist-item-sub");
        gtk_label_set_xalign(GTK_LABEL(d), 0);
        gtk_label_set_ellipsize(GTK_LABEL(d), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(d), TRUE);
        gtk_box_append(GTK_BOX(r), d);
        gtk_button_set_child(GTK_BUTTON(b), r);
        g_object_set_data_full(G_OBJECT(b), "sid",
                               g_strdup(hb_obj_id(s)), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_snip_btn), ws);
        gtk_box_append(GTK_BOX(ws->snip_list), b);
    }
    if (objs) hb_obj_list_free(objs, n);
    if (!shown) {
        GtkWidget *l = gtk_label_new("暂无匹配 Snippet");
        gtk_widget_add_css_class(l, "assist-item-sub");
        gtk_box_append(GTK_BOX(ws->snip_list), l);
    }
}

static void on_theme_pick(GtkButton *b, gpointer ud)
{
    HbWorkspace *ws = ud;
    hb_obj *o = NULL;
    const char *id = g_object_get_data(G_OBJECT(b), "tid");
    if (hb_vault_settings(hb_store_vault(ws->store), &o) == HB_OK && o) {
        hb_obj_set_str(o, "theme", id);
        hb_vault_put(hb_store_vault(ws->store), o);
        hb_obj_free(o);
    }
}

static GtkWidget *build_assist(HbWorkspace *ws)
{
    GtkWidget *box, *stack, *sw, *page, *scr;
    static const struct { const char *id, *name; } TH[] = {
        { "harbor", "Harbor Dark" }, { "ubuntu", "Ubuntu" },
        { "solarized", "Solarized" }, { "light", "Paper Light" },
    };
    size_t i;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "assist");
    gtk_widget_set_size_request(box, 250, -1);

    stack = gtk_stack_new();
    sw = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(sw), GTK_STACK(stack));
    gtk_widget_add_css_class(sw, "assist-switch");
    gtk_box_append(GTK_BOX(box), sw);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_append(GTK_BOX(box), stack);

    /* Snippets */
    scr = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ws->snip_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), ws->snip_list);
    gtk_stack_add_titled(GTK_STACK(stack), scr, "snip", "Snippets");

    /* 主题 */
    {
        GtkWidget *tp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(tp, 8);
        for (i = 0; i < G_N_ELEMENTS(TH); i++) {
            GtkWidget *b = gtk_button_new_with_label(TH[i].name);
            gtk_widget_add_css_class(b, "assist-item");
            g_object_set_data_full(G_OBJECT(b), "tid",
                                   g_strdup(TH[i].id), g_free);
            g_signal_connect(b, "clicked", G_CALLBACK(on_theme_pick), ws);
            gtk_box_append(GTK_BOX(tp), b);
        }
        page = tp;
    }
    gtk_stack_add_titled(GTK_STACK(stack), page, "theme", "主题");

    /* 自动补全（未实现） */
    {
        GtkWidget *ap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *l = gtk_label_new("即将推出");
        GtkWidget *tgl = gtk_switch_new();
        gtk_widget_set_margin_top(ap, 8);
        gtk_widget_set_sensitive(tgl, FALSE);
        gtk_widget_set_halign(tgl, GTK_ALIGN_START);
        gtk_widget_add_css_class(l, "assist-item-sub");
        gtk_box_append(GTK_BOX(ap), tgl);
        gtk_box_append(GTK_BOX(ap), l);
        page = ap;
    }
    gtk_stack_add_titled(GTK_STACK(stack), page, "ac", "自动补全");

    fill_snippets(ws);
    return box;
}

static void on_assist_btn(GtkButton *b, gpointer ud)
{
    HbWorkspace *ws = ud;
    gboolean on = !gtk_widget_get_visible(ws->assist);
    gtk_widget_set_visible(ws->assist, on);
    if (on)
        gtk_widget_add_css_class(GTK_WIDGET(b), "on");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(b), "on");
}

/* ws 自身带 hb-term-view + theme-<id>：pane-head/assist/footer 的
 * 主题底色规则（hb-term-theme.c 生成）因此覆盖整层 chrome */
static void apply_theme(HbWorkspace *ws)
{
    hb_obj *o = NULL;
    const char *tid = "harbor";
    char *cls;
    if (hb_vault_settings(hb_store_vault(ws->store), &o) == HB_OK && o) {
        const char *t2 = hb_obj_get_str(o, "theme");
        if (t2 && *t2) tid = t2;
        hb_obj_free(o);
    }
    cls = g_strdup_printf("theme-%s", tid);
    if (g_strcmp0(cls, ws->theme_cls)) {
        if (ws->theme_cls)
            gtk_widget_remove_css_class(GTK_WIDGET(ws), ws->theme_cls);
        gtk_widget_add_css_class(GTK_WIDGET(ws), cls);
        g_free(ws->theme_cls);
        ws->theme_cls = cls;
    } else {
        g_free(cls);
    }
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbWorkspace *ws = ud;
    (void)st; (void)id;
    if (type && !strcmp(type, "snippet"))
        fill_snippets(ws);
    if (type && !strcmp(type, "settings"))
        apply_theme(ws);
}

/* ------------------------------ 构建 ------------------------------ */

static void hb_workspace_init(HbWorkspace *ws)
{
    GtkWidget *bar, *ic, *sp, *sb, *body;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(ws),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(ws), "workspace");
    gtk_widget_add_css_class(GTK_WIDGET(ws), "hb-term-view");

    /* ---- 工具栏 ---- */
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "hb-term-toolbar");
    ic = hb_icon_new("cols", 15);
    gtk_widget_add_css_class(ic, "hb-term-tt");
    gtk_box_append(GTK_BOX(bar), ic);
    ws->tt_label = gtk_label_new("Workspace");
    gtk_widget_add_css_class(ws->tt_label, "hb-term-tt");
    gtk_label_set_xalign(GTK_LABEL(ws->tt_label), 0);
    gtk_box_append(GTK_BOX(bar), ws->tt_label);
    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(bar), sp);

    sb = gtk_button_new();
    gtk_widget_add_css_class(sb, "tbtn");
    gtk_widget_set_tooltip_text(sb, "在 scrollback 中搜索 (Ctrl+Shift+F)");
    gtk_button_set_child(GTK_BUTTON(sb), hb_icon_new("search", 13));
    g_signal_connect(sb, "clicked", G_CALLBACK(on_search_btn), ws);
    gtk_box_append(GTK_BOX(bar), sb);

    ws->bcast_btn = gtk_button_new_with_label("Broadcast");
    gtk_widget_add_css_class(ws->bcast_btn, "tbtn");
    gtk_widget_set_tooltip_text(ws->bcast_btn,
        "向所有 pane 广播输入（各 pane 按自己的终端模式编码）");
    g_signal_connect(ws->bcast_btn, "clicked", G_CALLBACK(on_bcast), ws);
    gtk_box_append(GTK_BOX(bar), ws->bcast_btn);

    ws->assist_btn = gtk_button_new_with_label("助手栏");
    gtk_widget_add_css_class(ws->assist_btn, "tbtn");
    g_signal_connect(ws->assist_btn, "clicked",
                     G_CALLBACK(on_assist_btn), ws);
    gtk_box_append(GTK_BOX(bar), ws->assist_btn);

    ws->focus_btn = gtk_button_new();
    gtk_widget_add_css_class(ws->focus_btn, "tbtn");
    gtk_widget_set_tooltip_text(ws->focus_btn,
        "Focus 模式：只看焦点 pane (Ctrl+Shift+M)");
    {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_append(GTK_BOX(r), hb_icon_new("focus", 13));
        gtk_box_append(GTK_BOX(r), gtk_label_new("Focus"));
        gtk_button_set_child(GTK_BUTTON(ws->focus_btn), r);
    }
    g_signal_connect(ws->focus_btn, "clicked",
                     G_CALLBACK(on_focus_btn), ws);
    gtk_box_append(GTK_BOX(bar), ws->focus_btn);
    gtk_box_append(GTK_BOX(ws), bar);

    /* ---- find bar（绑定焦点 pane） ---- */
    ws->find_bar = hb_find_bar_new();
    gtk_box_append(GTK_BOX(ws), ws->find_bar);

    /* ---- body：panes + 助手栏 ---- */
    body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    ws->root_host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(ws->root_host, TRUE);
    gtk_widget_set_vexpand(ws->root_host, TRUE);
    gtk_box_append(GTK_BOX(body), ws->root_host);

    ws->assist = build_assist(ws);
    gtk_widget_set_visible(ws->assist, FALSE);
    gtk_box_append(GTK_BOX(body), ws->assist);
    gtk_box_append(GTK_BOX(ws), body);

    /* ---- footer：焦点 pane 的状态条 ---- */
    ws->footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ws->footer, "ws-footer");
    gtk_box_append(GTK_BOX(ws), ws->footer);

    /* 窄窗默认关助手栏（<1100px 由 window 决定，这里默认关） */
}

GtkWidget *hb_workspace_new(HbStore *st)
{
    HbWorkspace *ws = g_object_new(HB_TYPE_WORKSPACE, NULL);
    ws->store = st;
    ws->store_sig = g_signal_connect(st, "changed",
        G_CALLBACK(on_store_changed), ws);
    apply_theme(ws);
    fill_snippets(ws);
    return GTK_WIDGET(ws);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void free_node_tree(WsNode *n)
{
    if (!n) return;
    if (!n->is_leaf) {
        free_node_tree(n->a);
        free_node_tree(n->b);
    }
    g_free(n);
}

static void hb_workspace_dispose(GObject *o)
{
    HbWorkspace *ws = HB_WORKSPACE(o);
    if (ws->store_sig && ws->store) {
        g_signal_handler_disconnect(ws->store, ws->store_sig);
        ws->store_sig = 0;
    }
    /* page 销毁在之后：先摘掉它们回调到 WsNode 的 meta handler */
    {
        GList *l;
        for (l = ws->leaves; l; l = l->next) {
            WsNode *n = l->data;
            if (n->page && n->meta_id)
                g_signal_handler_disconnect(n->page, n->meta_id);
        }
    }
    free_node_tree(ws->root);
    ws->root = NULL;
    g_list_free(ws->leaves);
    ws->leaves = NULL;
    G_OBJECT_CLASS(hb_workspace_parent_class)->dispose(o);
}

static void hb_workspace_finalize(GObject *o)
{
    HbWorkspace *ws = HB_WORKSPACE(o);
    g_free(ws->tab_id);
    g_free(ws->theme_cls);
    G_OBJECT_CLASS(hb_workspace_parent_class)->finalize(o);
}

static void hb_workspace_class_init(HbWorkspaceClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_workspace_dispose;
    oc->finalize = hb_workspace_finalize;
}

/* ------------------------------ 目标选择对话框 ------------------------------ */

typedef struct {
    GtkWidget *win;
    HbStore *st;
    GList *checks;       /* GtkCheckButton*，data "kind": local|host_id */
    GtkWidget *create;
    AdwDialog *dlg;
} PickCtx;

static void pick_ctx_free(gpointer data, GObject *dead)
{
    PickCtx *c = data;
    (void)dead;
    g_list_free(c->checks);
    g_free(c);
}

static void pick_update(PickCtx *c)
{
    GList *l;
    int n = 0;
    for (l = c->checks; l; l = l->next)
        if (gtk_check_button_get_active(l->data)) n++;
    gtk_widget_set_sensitive(c->create, n >= 2 && n <= 4);
}

static void on_pick_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    pick_update(ud);
}

static void on_pick_create(GtkButton *b, gpointer ud)
{
    PickCtx *c = ud;
    GList *pages = NULL, *l;
    (void)b;
    for (l = c->checks; l; l = l->next) {
        const char *kind;
        if (!gtk_check_button_get_active(l->data)) continue;
        kind = g_object_get_data(l->data, "kind");
        if (!strcmp(kind, "local")) {
            pages = g_list_append(pages,
                hb_term_page_new_local(c->st));
        } else {
            hb_obj *h = NULL;
            if (hb_vault_get(hb_store_vault(c->st), kind, &h) == HB_OK && h) {
                pages = g_list_append(pages, hb_ssh_page_new(c->st, h));
                hb_obj_free(h);
            }
        }
    }
    adw_dialog_close(c->dlg);
    if (pages)
        hb_window_open_workspace(HB_WINDOW(c->win), pages);
    g_list_free(pages);
}

void hb_workspace_pick_targets(GtkWidget *win, HbStore *st)
{
    PickCtx *c = g_new0(PickCtx, 1);
    AdwDialog *dlg;
    GtkWidget *box, *foot, *l, *list, *row;
    hb_obj **hosts;
    size_t n = 0, i;

    c->win = win;
    c->st = st;
    dlg = hb_modal_new("新建 Workspace", &box, &foot);
    c->dlg = dlg;

    l = gtk_label_new("选择 2–4 个分屏目标（并排一行）");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "assist-item-sub");
    gtk_box_append(GTK_BOX(box), l);

    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(box), list);

    /* Local */
    {
        GtkWidget *ck = gtk_check_button_new_with_label("Local · 本地终端");
        gtk_widget_add_css_class(ck, "pick-row");
        g_object_set_data_full(G_OBJECT(ck), "kind",
                               g_strdup("local"), g_free);
        g_signal_connect(ck, "toggled", G_CALLBACK(on_pick_toggled), c);
        gtk_box_append(GTK_BOX(list), ck);
        c->checks = g_list_append(c->checks, ck);
    }
    /* hosts */
    hosts = hb_store_list(st, "host", &n);
    for (i = 0; hosts && i < n; i++) {
        hb_obj *h = hosts[i];
        const char *name = hb_obj_get_str(h, "name");
        const char *user = hb_obj_get_str(h, "user");
        char *lbl = g_strdup_printf("%s · ssh, %s",
            name && *name ? name : "host", user ? user : "?");
        GtkWidget *ck = gtk_check_button_new_with_label(lbl);
        g_free(lbl);
        gtk_widget_add_css_class(ck, "pick-row");
        g_object_set_data_full(G_OBJECT(ck), "kind",
                               g_strdup(hb_obj_id(h)), g_free);
        g_signal_connect(ck, "toggled", G_CALLBACK(on_pick_toggled), c);
        gtk_box_append(GTK_BOX(list), ck);
        c->checks = g_list_append(c->checks, ck);
    }
    if (hosts) hb_obj_list_free(hosts, n);

    row = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(row, "hb-btn");
    g_signal_connect_swapped(row, "clicked",
        G_CALLBACK(adw_dialog_close), dlg);
    gtk_box_append(GTK_BOX(foot), row);
    c->create = gtk_button_new_with_label("创建 Workspace");
    gtk_widget_add_css_class(c->create, "hb-btn");
    gtk_widget_add_css_class(c->create, "primary");
    gtk_widget_set_sensitive(c->create, FALSE);
    g_signal_connect(c->create, "clicked", G_CALLBACK(on_pick_create), c);
    gtk_box_append(GTK_BOX(foot), c->create);

    g_object_weak_ref(G_OBJECT(dlg), pick_ctx_free, c);
    adw_dialog_present(dlg, win);
}
