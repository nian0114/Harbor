/* hb-forward-view — 端口转发规则表（FR-06）
 *
 * 工具栏：New forwarding 分裂键 + ▾（新建/全部启动/全部停止/导出 JSON）
 *        + 搜索框。表格：类型 tag | 监听 | 目标 | 经由 | 流量 | 状态 pill
 *        | 开关/编辑/删除。行双击进入编辑。删除确认 + 9s 撤销。
 * 实时状态经 HbForwardMgr "changed" 单行刷新（STATS ≤1/s，不重建表）。
 * ================================================================== */

#include "hb-forward-view.h"
#include "hb-forward-form.h"
#include "hb-forward-mgr.h"
#include "hb-dialogs.h"
#include "hb-icons.h"

#include <harbor/pf.h>
#include <string.h>
#include <stdlib.h>

struct _HbForwardView {
    GtkBox parent;

    HbStore      *store;
    HbForwardMgr *mgr;

    GtkWidget    *search;
    GtkWidget    *body;         /* stack: table / empty / nomatch */
    GtkWidget    *rows_box;
    GPtrArray    *rows;         /* FwdRow* */

    gulong        store_changed_h, mgr_changed_h;
    guint         tick_src;     /* 1s 重连倒计时 */
    gboolean      dead;
};

typedef struct {
    HbForwardView *v;
    char          *id;
    GtkWidget     *row;
    GtkWidget     *bind_l, *traffic_l, *pill;
    GtkWidget     *sw;
    gboolean       sw_hold;    /* 程序化 set_active 期间屏蔽回调 */
} FwdRow;

G_DEFINE_TYPE(HbForwardView, hb_forward_view, GTK_TYPE_BOX)

static void fill_rows(HbForwardView *v);
static void update_row(FwdRow *r);

/* ------------------------------ 工具 ------------------------------ */

static char *fmt_kind(hb_obj *o, const char **tag_class)
{
    const char *k = hb_obj_get_str(o, "kind");
    *tag_class = "hb-tag";
    if (k && !strcmp(k, "remote"))
        return g_strdup("Remote");
    if (k && !strcmp(k, "dynamic"))
        return g_strdup("Dynamic");
    return g_strdup("Local");
}

/* 监听列：local/dynamic "addr:port"；remote "服务器 addr:port"，
 * 服务端挑端口（配 0 或与实际不同）时显示实际端口。 */
static char *fmt_bind(HbForwardView *v, hb_obj *o, FwdRow *r)
{
    const char *k = hb_obj_get_str(o, "kind");
    const char *ba = hb_obj_get_str(o, "bind_addr");
    int bp = (int)hb_obj_get_int(o, "bind_port", 0);
    hb_fwd_status st;
    int actual = bp;

    if (hb_forward_mgr_status(v->mgr, r->id, &st) &&
        st.bound_port > 0)
        actual = st.bound_port;
    if (k && !strcmp(k, "remote"))
        return g_strdup_printf("服务器 %s:%d",
                               ba && *ba ? ba : "127.0.0.1", actual);
    return g_strdup_printf("%s:%d",
                           ba && *ba ? ba : "127.0.0.1", actual);
}

static char *fmt_dest(hb_obj *o)
{
    const char *k = hb_obj_get_str(o, "kind");
    const char *da = hb_obj_get_str(o, "dest_addr");
    int dp = (int)hb_obj_get_int(o, "dest_port", 0);
    if (k && !strcmp(k, "dynamic"))
        return g_strdup("SOCKS5");
    return g_strdup_printf("%s:%d", da && *da ? da : "?", dp);
}

static char *fmt_via(HbForwardView *v, hb_obj *o)
{
    const char *via = hb_obj_get_str(o, "via");
    hb_obj *h = NULL;
    char *out;
    if (!via || !*via)
        return g_strdup("（未设置）");
    if (hb_vault_get(hb_store_vault(v->store), via, &h) == HB_OK &&
        h && !hb_obj_tombstone(h)) {
        const char *nm = hb_obj_get_str(h, "name");
        out = g_strdup(nm && *nm ? nm : via);
    } else {
        out = g_strdup("（已删除）");
    }
    if (h) hb_obj_free(h);
    return out;
}

static char *fmt_traffic(const hb_fwd_status *st)
{
    if (st->state == HB_FWD_STOPPED)
        return g_strdup("—");
    if (!st->bytes_up && !st->bytes_down && !st->active && !st->total)
        return g_strdup("↑ 0 B ↓ 0 B");
    {
        char *u = g_format_size(st->bytes_up);
        char *d = g_format_size(st->bytes_down);
        char *s = g_strdup_printf("↑ %s ↓ %s · %u 连接",
                                  u, d, st->active);
        g_free(u); g_free(d);
        return s;
    }
}

/* ------------------------------ 行刷新 ------------------------------ */

static void set_pill(FwdRow *r, const char *cls, const char *text)
{
    gtk_widget_remove_css_class(r->pill, "on");
    gtk_widget_remove_css_class(r->pill, "off");
    gtk_widget_remove_css_class(r->pill, "run");
    gtk_widget_remove_css_class(r->pill, "err");
    gtk_widget_remove_css_class(r->pill, "wait");
    gtk_widget_add_css_class(r->pill, cls);
    gtk_label_set_text(GTK_LABEL(r->pill), text);
}

static void update_row(FwdRow *r)
{
    HbForwardView *v = r->v;
    hb_fwd_status st;
    gboolean running;
    char *s;

    hb_forward_mgr_status(v->mgr, r->id, &st);
    running = st.state == HB_FWD_CONNECTING ||
              st.state == HB_FWD_LISTENING ||
              st.state == HB_FWD_RECONNECTING;

    /* 监听列（remote 可能拿到实际端口） */
    {
        hb_obj *o = NULL;
        if (hb_vault_get(hb_store_vault(v->store), r->id,
                         &o) == HB_OK && o) {
            s = fmt_bind(v, o, r);
            gtk_label_set_text(GTK_LABEL(r->bind_l), s);
            g_free(s);
            hb_obj_free(o);
        }
    }

    s = fmt_traffic(&st);
    gtk_label_set_text(GTK_LABEL(r->traffic_l), s);
    g_free(s);

    switch (st.state) {
    case HB_FWD_LISTENING:
        set_pill(r, "on", "监听中");
        gtk_widget_set_tooltip_text(r->row,
            st.message ? st.message : NULL);
        break;
    case HB_FWD_CONNECTING:
        set_pill(r, "run", "连接中…");
        gtk_widget_set_tooltip_text(r->row, NULL);
        break;
    case HB_FWD_RECONNECTING: {
        char buf[48];
        snprintf(buf, sizeof buf, "断开 · %ds 后重连", st.retry_in_s);
        set_pill(r, "wait", buf);
        gtk_widget_set_tooltip_text(r->row,
            st.message ? st.message : "连接断开，自动重连中");
        break;
    }
    case HB_FWD_FAILED:
        set_pill(r, "err", "失败");
        gtk_widget_set_tooltip_text(r->row,
            st.message ? st.message : "启动失败");
        break;
    case HB_FWD_STOPPED:
    default:
        set_pill(r, "off", "已停止");
        gtk_widget_set_tooltip_text(r->row,
            st.message ? st.message : NULL);
        break;
    }

    r->sw_hold = TRUE;
    gtk_switch_set_active(GTK_SWITCH(r->sw), running);
    r->sw_hold = FALSE;
}

/* 1s 倒计时 tick：只刷 RECONNECTING 行的 pill 文本 */
static gboolean tick_cb(gpointer ud)
{
    HbForwardView *v = ud;
    gboolean any = FALSE;
    guint i;
    if (v->dead)
        return G_SOURCE_REMOVE;
    for (i = 0; i < v->rows->len; i++) {
        FwdRow *r = g_ptr_array_index(v->rows, i);
        hb_fwd_status st;
        if (!hb_forward_mgr_status(v->mgr, r->id, &st))
            continue;
        if (st.state == HB_FWD_RECONNECTING) {
            char buf[48];
            snprintf(buf, sizeof buf, "断开 · %ds 后重连",
                     st.retry_in_s);
            gtk_label_set_text(GTK_LABEL(r->pill), buf);
            any = TRUE;
        }
    }
    if (!any) {
        v->tick_src = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void ensure_tick(HbForwardView *v)
{
    guint i;
    if (v->tick_src)
        return;
    for (i = 0; i < v->rows->len; i++) {
        FwdRow *r = g_ptr_array_index(v->rows, i);
        hb_fwd_status st;
        if (hb_forward_mgr_status(v->mgr, r->id, &st) &&
            st.state == HB_FWD_RECONNECTING) {
            v->tick_src = g_timeout_add_seconds(1, tick_cb, v);
            return;
        }
    }
}

/* ------------------------------ mgr / store 信号 ------------------------------ */

static FwdRow *find_row(HbForwardView *v, const char *id)
{
    guint i;
    for (i = 0; i < v->rows->len; i++) {
        FwdRow *r = g_ptr_array_index(v->rows, i);
        if (!strcmp(r->id, id))
            return r;
    }
    return NULL;
}

static void on_mgr_changed(HbForwardMgr *m, const char *id, gpointer ud)
{
    HbForwardView *v = ud;
    guint i;
    (void)m;
    if (v->dead)
        return;
    if (!id) {
        for (i = 0; i < v->rows->len; i++)
            update_row(g_ptr_array_index(v->rows, i));
    } else {
        FwdRow *r = find_row(v, id);
        if (r)
            update_row(r);
        /* 未找到的行（刚删除/过滤外）不管 */
    }
    ensure_tick(v);
}

static void on_store_changed(HbStore *st, const char *type,
                             const char *id, gpointer ud)
{
    HbForwardView *v = ud;
    (void)st; (void)id;
    if (v->dead)
        return;
    if (!strcmp(type, "forward") || !strcmp(type, "host"))
        fill_rows(v);
}

/* ------------------------------ 行交互 ------------------------------ */

static void on_sw(GtkSwitch *sw, GParamSpec *ps, gpointer ud)
{
    FwdRow *r = ud;
    gboolean on;
    (void)ps;
    if (r->sw_hold)
        return;
    on = gtk_switch_get_active(sw);
    if (on)
        hb_forward_mgr_start(r->v->mgr, r->id);
    else
        hb_forward_mgr_stop(r->v->mgr, r->id);
}

static void on_edit(GtkButton *b, gpointer ud)
{
    HbForwardView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "rid");
    hb_obj *o = NULL;
    (void)b;
    if (hb_vault_get(hb_store_vault(v->store), id, &o) == HB_OK && o &&
        !hb_obj_tombstone(o)) {
        hb_forward_form_open(GTK_WIDGET(v), v->store, o);
    }
    if (o) hb_obj_free(o);
}

static void on_row_dclick(GtkGestureClick *g, int n, double x, double y,
                          gpointer ud)
{
    FwdRow *r = ud;
    hb_obj *o = NULL;
    (void)g; (void)x; (void)y;
    if (n != 2)                       /* 双击才进编辑 */
        return;
    if (hb_vault_get(hb_store_vault(r->v->store), r->id,
                     &o) == HB_OK && o && !hb_obj_tombstone(o))
        hb_forward_form_open(GTK_WIDGET(r->v), r->v->store, o);
    if (o) hb_obj_free(o);
}

/* ------------------------------ 删除 + 撤销 ------------------------------ */

typedef struct {
    HbForwardView *v;
    char *id;
    char *desc;
    gboolean was_enabled;
} DelFwd;

static void fwd_undo_cb(gpointer ud)
{
    DelFwd *d = ud;
    if (hb_vault_undelete(hb_store_vault(d->v->store), d->id) == HB_OK &&
        d->was_enabled)
        hb_forward_mgr_start(d->v->mgr, d->id);
}
static void fwd_undo_free(gpointer ud)
{
    DelFwd *d = ud;
    g_free(d->id); g_free(d->desc); g_free(d);
}

static void del_done(gboolean ok, gpointer ud)
{
    DelFwd *d = ud;
    char *msg;
    if (!ok) { fwd_undo_free(d); return; }
    hb_forward_mgr_stop(d->v->mgr, d->id);
    hb_vault_delete(hb_store_vault(d->v->store), d->id);
    msg = g_strdup_printf("已删除规则 %s", d->desc ? d->desc : "");
    hb_toast_undo_destroy(GTK_WIDGET(d->v), msg,
                          fwd_undo_cb, fwd_undo_free, d);
    g_free(msg);
}

static void on_del(GtkButton *b, gpointer ud)
{
    HbForwardView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "rid");
    hb_obj *o = NULL;
    DelFwd *d;
    char *desc = NULL, *msg;
    (void)b;

    if (hb_vault_get(hb_store_vault(v->store), id, &o) == HB_OK && o) {
        const char *k = hb_obj_get_str(o, "kind");
        const char *ba = hb_obj_get_str(o, "bind_addr");
        int bp = (int)hb_obj_get_int(o, "bind_port", 0);
        const char *da = hb_obj_get_str(o, "dest_addr");
        int dp = (int)hb_obj_get_int(o, "dest_port", 0);
        char *bind = g_strdup_printf("%s:%d",
            ba && *ba ? ba : "127.0.0.1", bp);
        if (k && !strcmp(k, "dynamic"))
            desc = g_strdup_printf("%s → SOCKS5", bind);
        else if (k && !strcmp(k, "remote"))
            desc = g_strdup_printf("服务器 %s → %s:%d", bind,
                da && *da ? da : "127.0.0.1", dp);
        else
            desc = g_strdup_printf("%s → %s:%d", bind,
                da && *da ? da : "?", dp);
        g_free(bind);

        d = g_new0(DelFwd, 1);
        d->v = v;
        d->id = g_strdup(id);
        d->desc = g_strdup(desc);
        d->was_enabled = hb_obj_get_bool(o, "enabled", FALSE);
        msg = g_strdup_printf(
            "确定删除 <b>%s</b>？监听将立即停止。删除后 9 秒内可撤销。",
            desc);
        hb_dialog_confirm(GTK_WIDGET(v), "删除转发规则", msg, "删除",
                          TRUE, del_done, d);
        g_free(msg);
        g_free(desc);
        hb_obj_free(o);
    }
}

/* ------------------------------ 工具栏动作 ------------------------------ */

static void on_new(GtkButton *b, gpointer ud)
{
    HbForwardView *v = ud;
    (void)b;
    hb_forward_form_open(GTK_WIDGET(v), v->store, NULL);
}

static void on_start_all(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbForwardView *v = ud;
    hb_obj **objs;
    size_t n = 0, i;
    (void)a; (void)p;
    objs = hb_store_list(v->store, "forward", &n);
    for (i = 0; objs && i < n; i++)
        hb_forward_mgr_start(v->mgr, hb_obj_id(objs[i]));
    if (objs) hb_obj_list_free(objs, n);
}

static void on_stop_all(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbForwardView *v = ud;
    hb_obj **objs;
    size_t n = 0, i;
    (void)a; (void)p;
    objs = hb_store_list(v->store, "forward", &n);
    for (i = 0; objs && i < n; i++)
        hb_forward_mgr_stop(v->mgr, hb_obj_id(objs[i]));
    if (objs) hb_obj_list_free(objs, n);
}

static void exp_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbForwardView *v = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *out = NULL;
    GError *err = NULL;
    if (!gf) return;
    if (hb_vault_export_json(hb_store_vault(v->store), "forward",
                             &out) != HB_OK || !out ||
        !g_file_replace_contents(gf, out, strlen(out), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        hb_toast(GTK_WIDGET(v), "导出失败");
        if (err) g_error_free(err);
    } else {
        hb_toast(GTK_WIDGET(v), "转发规则已导出");
    }
    g_free(out);
    g_object_unref(gf);
}

static void on_export(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbForwardView *v = ud;
    GtkFileDialog *d;
    (void)a; (void)p;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "导出转发规则");
    gtk_file_dialog_set_initial_name(d, "harbor-forwards.json");
    gtk_file_dialog_save(d, NULL, NULL, exp_done, v);
    g_object_unref(d);
}

static void on_menu_new(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbForwardView *v = ud;
    (void)a; (void)p;
    hb_forward_form_open(GTK_WIDGET(v), v->store, NULL);
}

/* ------------------------------ 行构建 ------------------------------ */

static void fwd_row_free(FwdRow *r)
{
    g_free(r->id);
    g_free(r);
}

/* 双击打开编辑的手势挂在文本标签上：挂整行会 claim 每个 press，
 * 让行内的 switch/button 收不到事件。 */
static void add_dclick(GtkWidget *w, FwdRow *r)
{
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
    g_signal_connect(g, "pressed", G_CALLBACK(on_row_dclick), r);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(g));
}

static GtkWidget *build_row(HbForwardView *v, hb_obj *o)
{
    FwdRow *r = g_new0(FwdRow, 1);
    GtkWidget *row, *l, *eb, *db;
    char *s;
    const char *tc;

    r->v = v;
    r->id = g_strdup(hb_obj_id(o));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "hb-table-row");
    r->row = row;

    /* 类型 tag */
    s = fmt_kind(o, &tc);
    l = gtk_label_new(s);
    gtk_widget_add_css_class(l, tc);
    gtk_widget_set_margin_start(l, 14);
    gtk_widget_set_size_request(l, 64, -1);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), l);
    add_dclick(l, r);
    g_free(s);

    /* 监听 */
    s = fmt_bind(v, o, r);
    r->bind_l = gtk_label_new(s);
    gtk_widget_add_css_class(r->bind_l, "mono");
    gtk_label_set_xalign(GTK_LABEL(r->bind_l), 0);
    gtk_widget_set_hexpand(r->bind_l, TRUE);
    gtk_box_append(GTK_BOX(row), r->bind_l);
    add_dclick(r->bind_l, r);
    g_free(s);

    /* 目标 */
    s = fmt_dest(o);
    l = gtk_label_new(s);
    gtk_widget_add_css_class(l, "mono");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    add_dclick(l, r);
    g_free(s);

    /* 经由 */
    s = fmt_via(v, o);
    l = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_size_request(l, 150, -1);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), l);
    add_dclick(l, r);
    g_free(s);

    /* 流量 */
    r->traffic_l = gtk_label_new("—");
    gtk_widget_add_css_class(r->traffic_l, "mono");
    gtk_label_set_xalign(GTK_LABEL(r->traffic_l), 0);
    gtk_widget_set_size_request(r->traffic_l, 170, -1);
    gtk_box_append(GTK_BOX(row), r->traffic_l);
    add_dclick(r->traffic_l, r);

    /* 状态 pill */
    r->pill = gtk_label_new("已停止");
    gtk_widget_add_css_class(r->pill, "pill");
    gtk_widget_add_css_class(r->pill, "off");
    gtk_widget_set_size_request(r->pill, 120, -1);
    gtk_widget_set_halign(r->pill, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), r->pill);
    add_dclick(r->pill, r);

    /* 动作：switch / edit / delete */
    r->sw = gtk_switch_new();
    gtk_widget_set_valign(r->sw, GTK_ALIGN_CENTER);
    g_signal_connect(r->sw, "notify::active", G_CALLBACK(on_sw), r);
    gtk_box_append(GTK_BOX(row), r->sw);

    eb = gtk_button_new();
    gtk_widget_add_css_class(eb, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(eb), hb_icon_new("pencil", 13));
    gtk_widget_set_tooltip_text(eb, "编辑");
    g_object_set_data_full(G_OBJECT(eb), "rid",
                           g_strdup(r->id), g_free);
    g_signal_connect(eb, "clicked", G_CALLBACK(on_edit), v);
    gtk_box_append(GTK_BOX(row), eb);

    db = gtk_button_new();
    gtk_widget_add_css_class(db, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(db), hb_icon_new("trash", 13));
    gtk_widget_set_tooltip_text(db, "删除规则");
    g_object_set_data_full(G_OBJECT(db), "rid",
                           g_strdup(r->id), g_free);
    g_signal_connect(db, "clicked", G_CALLBACK(on_del), v);
    gtk_widget_set_margin_end(db, 10);
    gtk_box_append(GTK_BOX(row), db);

    update_row(r);
    g_ptr_array_add(v->rows, r);
    return row;
}

/* 双击过滤：gesture pressed 每击都发，n_press==2 才算编辑 */
/* (on_row_dclick 内 n 参数即 n_press) */

/* ------------------------------ 表构建 ------------------------------ */

static GtkWidget *build_head(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    struct { const char *t; int w; gboolean grow; } cols[] = {
        { "类型",         64,  FALSE },
        { "监听",         -1,  TRUE  },
        { "目标",         -1,  TRUE  },
        { "经由 SSH 主机", 150, FALSE },
        { "流量",         170, FALSE },
        { "状态",         120, FALSE },
        { "",             96,  FALSE },
    };
    int i;
    gtk_widget_add_css_class(row, "hb-table-head");
    for (i = 0; i < 7; i++) {
        GtkWidget *l = gtk_label_new(cols[i].t);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_margin_start(l, i == 0 ? 14 : 0);
        if (cols[i].grow) gtk_widget_set_hexpand(l, TRUE);
        if (cols[i].w > 0)
            gtk_widget_set_size_request(l, cols[i].w, -1);
        gtk_box_append(GTK_BOX(row), l);
    }
    return row;
}

static gboolean row_match(HbForwardView *v, hb_obj *o, const char *q)
{
    const char *k = hb_obj_get_str(o, "kind");
    const char *ba = hb_obj_get_str(o, "bind_addr");
    const char *da = hb_obj_get_str(o, "dest_addr");
    const char *via = hb_obj_get_str(o, "via");
    char *vname = NULL, *hay, *hay_l, *q_l;
    gboolean hit;
    int bp = (int)hb_obj_get_int(o, "bind_port", 0);
    int dp = (int)hb_obj_get_int(o, "dest_port", 0);
    hb_obj *h = NULL;

    if (via && *via &&
        hb_vault_get(hb_store_vault(v->store), via, &h) == HB_OK && h) {
        const char *nm = hb_obj_get_str(h, "name");
        const char *ad = hb_obj_get_str(h, "address");
        vname = g_strdup_printf("%s %s", nm ? nm : "", ad ? ad : "");
        hb_obj_free(h);
    }
    hay = g_strdup_printf("%s %s:%d %s:%d %s",
        k ? k : "", ba ? ba : "", bp, da ? da : "", dp,
        vname ? vname : "");
    hay_l = g_utf8_strdown(hay, -1);
    q_l = g_utf8_strdown(q, -1);
    hit = strstr(hay_l, q_l) != NULL;
    g_free(vname); g_free(hay); g_free(hay_l); g_free(q_l);
    return hit;
}

static void fill_rows(HbForwardView *v)
{
    GtkWidget *c;
    hb_obj **objs;
    size_t n = 0, i;
    guint shown = 0;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search));

    /* 先拆 widget 行（其回调还引用 FwdRow），再释放 FwdRow 数组 */
    while ((c = gtk_widget_get_first_child(v->rows_box)))
        gtk_box_remove(GTK_BOX(v->rows_box), c);
    g_ptr_array_set_size(v->rows, 0);

    objs = hb_store_list(v->store, "forward", &n);
    for (i = 0; objs && i < n; i++) {
        if (q && *q && !row_match(v, objs[i], q))
            continue;
        gtk_box_append(GTK_BOX(v->rows_box), build_row(v, objs[i]));
        shown++;
    }
    if (objs) hb_obj_list_free(objs, n);

    if (n == 0)
        gtk_stack_set_visible_child_name(GTK_STACK(v->body), "empty");
    else if (!shown)
        gtk_stack_set_visible_child_name(GTK_STACK(v->body), "nomatch");
    else
        gtk_stack_set_visible_child_name(GTK_STACK(v->body), "table");
    ensure_tick(v);
}

static void on_search(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    fill_rows((HbForwardView *)ud);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void hb_forward_view_dispose(GObject *o)
{
    HbForwardView *v = HB_FORWARD_VIEW(o);
    v->dead = TRUE;
    if (v->store && v->store_changed_h)
        g_signal_handler_disconnect(v->store, v->store_changed_h);
    if (v->mgr && v->mgr_changed_h)
        g_signal_handler_disconnect(v->mgr, v->mgr_changed_h);
    v->store_changed_h = v->mgr_changed_h = 0;
    if (v->tick_src) {
        g_source_remove(v->tick_src);
        v->tick_src = 0;
    }
    g_clear_pointer(&v->rows, g_ptr_array_unref);
    G_OBJECT_CLASS(hb_forward_view_parent_class)->dispose(o);
}

static void hb_forward_view_class_init(HbForwardViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_forward_view_dispose;
}

static void hb_forward_view_init(HbForwardView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
    v->rows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)fwd_row_free);
}

GtkWidget *hb_forward_view_new(HbStore *st)
{
    HbForwardView *v = g_object_new(HB_TYPE_FORWARD_VIEW, NULL);
    GtkWidget *toolbar, *split, *newb, *menub, *table, *scrolled;
    GtkWidget *hint, *hl, *empty, *nomatch;
    GMenu *mm;
    GSimpleActionGroup *ag;
    GSimpleAction *sa;

    v->store = st;
    v->mgr = hb_forward_mgr_default();

    /* ---- 工具栏 ---- */
    toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(toolbar, "hb-toolbar");
    split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(split, "hb-btn-split");
    newb = gtk_button_new();
    gtk_widget_add_css_class(newb, "hb-btn");
    {
        GtkWidget *rr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_append(GTK_BOX(rr), hb_icon_new("plus", 13));
        gtk_box_append(GTK_BOX(rr), gtk_label_new("New forwarding"));
        gtk_button_set_child(GTK_BUTTON(newb), rr);
    }
    g_signal_connect(newb, "clicked", G_CALLBACK(on_new), v);
    gtk_box_append(GTK_BOX(split), newb);
    menub = gtk_menu_button_new();
    gtk_widget_add_css_class(menub, "hb-btn");
    gtk_menu_button_set_label(GTK_MENU_BUTTON(menub), "▾");
    mm = g_menu_new();
    g_menu_append(mm, "新建规则", "fwd.new");
    g_menu_append(mm, "全部启动", "fwd.startall");
    g_menu_append(mm, "全部停止", "fwd.stopall");
    g_menu_append(mm, "导出规则 JSON", "fwd.export");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menub),
                                   G_MENU_MODEL(mm));
    g_object_unref(mm);
    gtk_box_append(GTK_BOX(split), menub);
    gtk_box_append(GTK_BOX(toolbar), split);

    ag = g_simple_action_group_new();
    sa = g_simple_action_new("new", NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(on_menu_new), v);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
    g_object_unref(sa);
    sa = g_simple_action_new("startall", NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(on_start_all), v);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
    g_object_unref(sa);
    sa = g_simple_action_new("stopall", NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(on_stop_all), v);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
    g_object_unref(sa);
    sa = g_simple_action_new("export", NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(on_export), v);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
    g_object_unref(sa);
    gtk_widget_insert_action_group(GTK_WIDGET(v), "fwd",
                                   G_ACTION_GROUP(ag));
    g_object_unref(ag);

    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(toolbar), sp);
    }
    v->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
        "搜索监听 / 目标 / 主机…");
    gtk_widget_set_size_request(v->search, 240, -1);
    gtk_widget_set_valign(v->search, GTK_ALIGN_CENTER);
    g_signal_connect(v->search, "search-changed",
                     G_CALLBACK(on_search), v);
    gtk_box_append(GTK_BOX(toolbar), v->search);
    gtk_widget_set_margin_start(toolbar, 16);
    gtk_widget_set_margin_end(toolbar, 16);
    gtk_widget_set_margin_top(toolbar, 14);
    gtk_box_append(GTK_BOX(v), toolbar);

    /* ---- 表 ---- */
    v->body = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(v->body),
                                  GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_vexpand(v->body, TRUE);
    gtk_widget_set_margin_start(v->body, 16);
    gtk_widget_set_margin_end(v->body, 16);
    gtk_widget_set_margin_top(v->body, 10);

    table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(table, "hb-table");
    gtk_box_append(GTK_BOX(table), build_head());
    v->rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                  v->rows_box);
    gtk_box_append(GTK_BOX(table), scrolled);
    gtk_stack_add_named(GTK_STACK(v->body), table, "table");

    /* 空态 */
    empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(empty, "hb-empty");
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    {
        GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *ic = hb_icon_new("fwd", 24);
        gtk_widget_add_css_class(ico, "hb-e-ico");
        gtk_widget_set_size_request(ico, 58, 58);
        gtk_widget_set_halign(ico, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(ic, TRUE);
        gtk_box_append(GTK_BOX(ico), ic);
        gtk_box_append(GTK_BOX(empty), ico);
    }
    {
        GtkWidget *t = gtk_label_new("还没有端口转发规则");
        gtk_widget_add_css_class(t, "hb-e-title");
        gtk_box_append(GTK_BOX(empty), t);
        t = gtk_label_new("Local / Remote / Dynamic SOCKS5 三种隧道，"
                          "一键建立、自动重连");
        gtk_widget_add_css_class(t, "hb-e-sub");
        gtk_box_append(GTK_BOX(empty), t);
    }
    gtk_stack_add_named(GTK_STACK(v->body), empty, "empty");

    nomatch = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(nomatch, "hb-empty");
    gtk_widget_set_valign(nomatch, GTK_ALIGN_CENTER);
    {
        GtkWidget *t = gtk_label_new("无匹配规则");
        gtk_widget_add_css_class(t, "hb-e-title");
        gtk_box_append(GTK_BOX(nomatch), t);
    }
    gtk_stack_add_named(GTK_STACK(v->body), nomatch, "nomatch");
    gtk_box_append(GTK_BOX(v), v->body);

    /* hintbox */
    hint = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hint, "hintbox");
    gtk_widget_add_css_class(hint, "plain");
    hl = gtk_label_new(
        "VNC / RDP 主机勾选「SSH 隧道」后会自动创建临时 Local 规则："
        "本机随机端口 → 服务器回环 127.0.0.1:3389/5901，"
        "桌面会话关闭即拆除（FR-07）。");
    gtk_label_set_wrap(GTK_LABEL(hl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_widget_set_hexpand(hl, TRUE);
    gtk_box_append(GTK_BOX(hint), hb_icon_new("info", 15));
    gtk_box_append(GTK_BOX(hint), hl);
    gtk_widget_set_margin_start(hint, 16);
    gtk_widget_set_margin_end(hint, 16);
    gtk_widget_set_margin_top(hint, 6);
    gtk_widget_set_margin_bottom(hint, 14);
    gtk_box_append(GTK_BOX(v), hint);

    /* 信号 */
    v->store_changed_h = g_signal_connect(st, "changed",
        G_CALLBACK(on_store_changed), v);
    v->mgr_changed_h = g_signal_connect(v->mgr, "changed",
        G_CALLBACK(on_mgr_changed), v);

    fill_rows(v);
    return GTK_WIDGET(v);
}
