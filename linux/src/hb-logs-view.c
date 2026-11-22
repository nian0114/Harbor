/* hb-logs-view — 连接日志页（FR-10）
 *
 * 工具栏：过滤（host/user/proto/result 子串）、仅看 Saved、
 * Date 排序 ↓/↑、保留期 dropdown、导出 CSV/JSON、清空日志。
 * 表格：Date/User/Host/协议/结果 pill/原因/时长/Saved 书签。
 * log_retention_days（默认 90，0=永久）改动即 purge；store "log" 信号刷新。
 * ================================================================== */

#include "hb-logs-view.h"
#include "hb-dialogs.h"
#include "hb-icons.h"

#include <string.h>
#include <stdio.h>
#include <glib/gi18n.h>

struct _HbLogsView {
    GtkBox parent;

    HbStore   *store;
    GtkWidget *search;
    GtkWidget *saved_btn;    /* 仅看 Saved toggle */
    GtkWidget *sort_btn;     /* Date ↓/↑ */
    GtkWidget *ret_dd;       /* retention */
    GtkWidget *rows;
    gboolean   only_saved;
    gboolean   desc;         /* TRUE = 新→旧 */
    gboolean   ret_loading;
    gboolean   dead;         /* dispose 中不再重建 */

    gulong changed_h;
};

G_DEFINE_TYPE(HbLogsView, hb_logs_view, GTK_TYPE_BOX)

static void fill_rows(HbLogsView *v);

static const int RET_CHOICES[] = { 30, 90, 180, 365, 0 };   /* 0=永久 */
#define N_RET 5

/* ============================ 行 ============================ */

/* result → pill 文本 + css 变体 */
static void result_disp(const hb_log *l, const char **txt,
                        const char **css)
{
    const char *r = l->result ? l->result : "";
    if (l->duration_s < 0 || !strcmp(r, "connecting")) {
        *txt = _("进行中"); *css = "run";
    } else if (!strcmp(r, "ok")) {
        *txt = _("成功"); *css = "on";
    } else if (!strcmp(r, "cancelled")) {
        *txt = _("已取消"); *css = "off";
    } else {
        *txt = _("失败"); *css = "err";
    }
}

/* reason（或 error 型 result）→ 人类可读 */
static const char *reason_disp(const hb_log *l, char *buf, size_t bl)
{
    const char *r = l->reason && *l->reason ? l->reason
                                            : (l->result ? l->result : "");
    struct { const char *k, *zh; } map[] = {
        { "auth_failed",        _("认证失败")   },
        { "unreachable",        _("无法连接")   },
        { "resolve",            _("无法连接")   },
        { "refused",            _("无法连接")   },
        { "hostkey_changed",    _("主机密钥变更") },
        { "hostkey_rejected",   _("主机密钥被拒绝") },
        { "timeout",            _("超时")      },
        { "cancelled",          _("已取消")    },
        { "key confirm denied", _("已取消")    },
        { "ok",                 ""          },
        { "connecting",         _("进行中")    },
    };
    size_t i;
    if (!*r) return "";
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(r, map[i].k))
            return map[i].zh;
    /* 未识别 → 原样（tooltip 也是原文）。短串直接返回省一次拷贝；
     * 长串按 UTF-8 字符边界截断 —— snprintf 按字节截会把多字节
     * 字符切断成半个，产生无效 UTF-8（Pango 告警）。 */
    if (strlen(r) < bl)
        return r;
    {
        const gchar *end = g_utf8_find_prev_char(r, r + bl - 1);
        size_t n = end ? (size_t)(end - r) : 0;
        memcpy(buf, r, n);
        buf[n] = 0;
    }
    return buf;
}

static char *dur_disp(int64_t s, char *buf, size_t bl)
{
    if (s < 0) { snprintf(buf, bl, _("进行中")); return buf; }
    if (s < 60) { snprintf(buf, bl, "%llds", (long long)s); return buf; }
    snprintf(buf, bl, "%lldm %llds",
             (long long)(s / 60), (long long)(s % 60));
    return buf;
}

static void on_saved_toggle(GtkButton *b, gpointer ud)
{
    HbLogsView *v = ud;
    int64_t id = (int64_t)(intptr_t)g_object_get_data(G_OBJECT(b), "lid");
    gboolean cur = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "sv"));
    (void)b;
    hb_vault_log_set_saved(hb_store_vault(v->store), id, !cur);
}

typedef struct { HbLogsView *v; guint shown; } Fill;

static bool log_row_cb(const hb_log *l, void *ud)
{
    Fill *f = ud;
    HbLogsView *v = f->v;
    GtkWidget *row, *dl, *ul, *hl, *pl, *pill, *rl, *durl, *bm;
    char dbuf[24], durb[24], rbuf[128];
    const char *rtxt, *rcss;
    struct tm tm;
    time_t t;

    if (v->only_saved && !l->saved)
        return true;

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "hb-table-row");

    /* Date */
    t = (time_t)(l->ts / 1000);
    localtime_r(&t, &tm);
    strftime(dbuf, sizeof dbuf, "%Y-%m-%d %H:%M", &tm);
    dl = gtk_label_new(dbuf);
    gtk_widget_add_css_class(dl, "mono");
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_widget_set_margin_start(dl, 14);
    gtk_widget_set_size_request(dl, 140, -1);
    gtk_box_append(GTK_BOX(row), dl);

    /* User */
    ul = gtk_label_new(l->user ? l->user : "");
    gtk_label_set_xalign(GTK_LABEL(ul), 0);
    gtk_widget_set_margin_start(ul, 14);
    gtk_widget_set_size_request(ul, 90, -1);
    gtk_box_append(GTK_BOX(row), ul);

    /* Host */
    hl = gtk_label_new(l->host ? l->host : "");
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(hl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(hl, 14);
    gtk_widget_set_size_request(hl, 130, -1);
    gtk_widget_set_hexpand(hl, TRUE);
    gtk_box_append(GTK_BOX(row), hl);

    /* 协议 */
    pl = gtk_label_new(l->proto ? l->proto : "");
    gtk_label_set_xalign(GTK_LABEL(pl), 0);
    gtk_widget_set_margin_start(pl, 14);
    gtk_widget_set_size_request(pl, 70, -1);
    gtk_box_append(GTK_BOX(row), pl);

    /* 结果 pill */
    result_disp(l, &rtxt, &rcss);
    pill = gtk_label_new(rtxt);
    gtk_widget_add_css_class(pill, "pill");
    gtk_widget_add_css_class(pill, rcss);
    gtk_widget_set_margin_start(pill, 14);
    gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
    {
        GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(wrap, 80, -1);
        gtk_box_append(GTK_BOX(wrap), pill);
        gtk_box_append(GTK_BOX(row), wrap);
    }

    /* 原因 */
    rl = gtk_label_new(reason_disp(l, rbuf, sizeof rbuf));
    gtk_label_set_xalign(GTK_LABEL(rl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(rl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(rl, 14);
    gtk_widget_set_size_request(rl, 130, -1);
    gtk_widget_set_hexpand(rl, TRUE);
    if (l->reason && *l->reason)
        gtk_widget_set_tooltip_text(rl, l->reason);
    gtk_box_append(GTK_BOX(row), rl);

    /* 时长 */
    durl = gtk_label_new(dur_disp(l->duration_s, durb, sizeof durb));
    gtk_label_set_xalign(GTK_LABEL(durl), 0);
    gtk_widget_set_margin_start(durl, 14);
    gtk_widget_set_size_request(durl, 80, -1);
    gtk_box_append(GTK_BOX(row), durl);

    /* Saved 书签 */
    bm = gtk_button_new();
    gtk_widget_add_css_class(bm, "hb-iconbtn");
    if (l->saved) gtk_widget_add_css_class(bm, "saved");
    gtk_button_set_child(GTK_BUTTON(bm), hb_icon_new("bookmark", 13));
    gtk_widget_set_tooltip_text(bm, l->saved ? _("取消 Saved") : _("标记 Saved"));
    gtk_widget_set_margin_start(bm, 14);
    gtk_widget_set_size_request(bm, 50, -1);
    gtk_widget_set_valign(bm, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(bm), "lid", (gpointer)(intptr_t)l->id);
    g_object_set_data(G_OBJECT(bm), "sv", GINT_TO_POINTER(l->saved ? 1 : 0));
    g_signal_connect(bm, "clicked", G_CALLBACK(on_saved_toggle), v);
    gtk_box_append(GTK_BOX(row), bm);

    gtk_box_append(GTK_BOX(v->rows), row);
    f->shown++;
    return true;
}

static GtkWidget *row_head(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    struct { const char *txt; int w; gboolean grow; } cols[] = {
        { _("Date"),   140, FALSE },
        { _("User"),    90, FALSE },
        { _("Host"),   130, TRUE  },
        { _("协议"),    70, FALSE },
        { _("结果"),    80, FALSE },
        { _("原因"),   130, TRUE  },
        { _("时长"),    80, FALSE },
        { _("Saved"),   50, FALSE },
    };
    int i;
    gtk_widget_add_css_class(row, "hb-table-head");
    for (i = 0; i < 8; i++) {
        GtkWidget *l = gtk_label_new(cols[i].txt);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_margin_start(l, 14);
        if (cols[i].grow) gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_set_size_request(l, cols[i].w, -1);
        gtk_box_append(GTK_BOX(row), l);
    }
    return row;
}

static void fill_rows(HbLogsView *v)
{
    GtkWidget *c;
    Fill f = { v, 0 };
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search));

    while ((c = gtk_widget_get_first_child(v->rows)))
        gtk_box_remove(GTK_BOX(v->rows), c);

    hb_vault_log_foreach(hb_store_vault(v->store),
                         (q && *q) ? q : NULL, v->desc,
                         log_row_cb, &f);

    if (!f.shown) {
        GtkWidget *l = gtk_label_new(_("无匹配记录"));
        gtk_widget_add_css_class(l, "hb-e-sub");
        gtk_widget_set_margin_top(l, 26);
        gtk_widget_set_margin_bottom(l, 26);
        gtk_box_append(GTK_BOX(v->rows), l);
    }
}

static void on_search(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    fill_rows((HbLogsView *)ud);
}

static void on_saved_filter(GtkToggleButton *b, gpointer ud)
{
    HbLogsView *v = ud;
    v->only_saved = gtk_toggle_button_get_active(b);
    fill_rows(v);
}

static void on_sort(GtkButton *b, gpointer ud)
{
    HbLogsView *v = ud;
    char *lbl;
    (void)b;
    v->desc = !v->desc;
    lbl = g_strdup_printf("Date %s", v->desc ? "↓" : "↑");
    gtk_button_set_label(GTK_BUTTON(v->sort_btn), lbl);
    g_free(lbl);
    fill_rows(v);
}

/* ============================ 导出 / 清空 ============================ */

typedef struct { HbLogsView *v; gboolean json; } ExpCtx;

static void exp_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ExpCtx *c = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *out = NULL;
    GError *err = NULL;
    hb_err e;
    if (!gf) { g_free(c); return; }
    e = c->json ? hb_vault_log_export_json(hb_store_vault(c->v->store),
                                           &out)
                : hb_vault_log_export_csv(hb_store_vault(c->v->store),
                                          &out);
    if (e != HB_OK || !out ||
        !g_file_replace_contents(gf, out, strlen(out), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        hb_toast(GTK_WIDGET(c->v), _("导出失败"));
        if (err) g_error_free(err);
    } else {
        hb_toast(GTK_WIDGET(c->v),
                 c->json ? _("日志已导出 JSON") : _("日志已导出 CSV"));
    }
    free(out);
    g_object_unref(gf);
    g_free(c);
}

static void on_export(GtkButton *b, gpointer ud)
{
    HbLogsView *v = ud;
    gboolean json = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "json"));
    GtkFileDialog *d;
    ExpCtx *c = g_new0(ExpCtx, 1);
    c->v = v;
    c->json = json;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, json ? _("导出日志 JSON") : _("导出日志 CSV"));
    gtk_file_dialog_set_initial_name(d,
        json ? "harbor-logs.json" : "harbor-logs.csv");
    gtk_file_dialog_save(d, NULL, NULL, exp_done, c);
    g_object_unref(d);
}

static void clear_done(gboolean ok, gpointer ud)
{
    HbLogsView *v = ud;
    if (ok)
        hb_vault_log_clear(hb_store_vault(v->store));
}

static void on_clear(GtkButton *b, gpointer ud)
{
    HbLogsView *v = ud;
    (void)b;
    hb_dialog_confirm(GTK_WIDGET(v), _("清空连接日志"),
                      _("确定删除全部连接日志？此操作不可撤销。"),
                      _("清空"), TRUE, clear_done, v);
}

/* ============================ retention ============================ */

static void apply_retention(HbLogsView *v, int days)
{
    hb_obj *o = NULL;
    if (v->ret_loading) return;
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        hb_obj_set_int(o, "log_retention_days", days);
        hb_vault_put(hb_store_vault(v->store), o);
        hb_obj_free(o);
    }
    hb_vault_log_purge(hb_store_vault(v->store), days);
}

static void on_ret_sel(GObject *dd, GParamSpec *ps, gpointer ud)
{
    HbLogsView *v = ud;
    guint s;
    (void)ps;
    /* teardown 中 model 可能先被摘 —— get_selected 会崩 */
    if (v->dead || !gtk_drop_down_get_model(GTK_DROP_DOWN(dd)))
        return;
    s = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (s >= N_RET) return;
    apply_retention(v, RET_CHOICES[s]);
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbLogsView *v = ud;
    (void)st; (void)id;
    if (!v->dead && !strcmp(type, "log"))
        fill_rows(v);
}

/* ============================ 构建 ============================ */

static void build(HbLogsView *v)
{
    GtkWidget *bar, *sv, *sp, *csv, *js, *clr, *sw, *sc, *table, *rl;
    GtkStringList *sm;
    hb_obj *o = NULL;
    int days = 90, i;

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    v->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
                                          _("按主机 / 用户 / 结果筛选…"));
    gtk_widget_set_size_request(v->search, 260, -1);
    gtk_widget_set_valign(v->search, GTK_ALIGN_CENTER);
    g_signal_connect(v->search, "search-changed",
                     G_CALLBACK(on_search), v);
    gtk_box_append(GTK_BOX(bar), v->search);

    /* 仅看 Saved */
    v->saved_btn = gtk_toggle_button_new_with_label(_("仅看 Saved"));
    gtk_widget_add_css_class(v->saved_btn, "hb-btn");
    gtk_widget_add_css_class(v->saved_btn, "ghost");
    g_signal_connect(v->saved_btn, "toggled",
                     G_CALLBACK(on_saved_filter), v);
    gtk_box_append(GTK_BOX(bar), v->saved_btn);

    /* Date 排序 */
    v->sort_btn = gtk_button_new_with_label("Date ↓");
    gtk_widget_add_css_class(v->sort_btn, "hb-btn");
    gtk_widget_add_css_class(v->sort_btn, "ghost");
    g_signal_connect(v->sort_btn, "clicked", G_CALLBACK(on_sort), v);
    gtk_box_append(GTK_BOX(bar), v->sort_btn);

    /* retention dropdown */
    rl = gtk_label_new(_("保留"));
    gtk_widget_add_css_class(rl, "hb-dim");
    gtk_widget_set_valign(rl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bar), rl);
    sm = gtk_string_list_new(NULL);
    gtk_string_list_append(sm, _("30 天"));
    gtk_string_list_append(sm, _("90 天"));
    gtk_string_list_append(sm, _("180 天"));
    gtk_string_list_append(sm, _("365 天"));
    gtk_string_list_append(sm, _("永久"));
    v->ret_dd = gtk_drop_down_new(G_LIST_MODEL(sm), NULL);
    /* sm 不 unref：dropdown 拆卸时 GTK 仍会读 model */
    gtk_widget_set_size_request(v->ret_dd, 100, -1);
    gtk_widget_set_valign(v->ret_dd, GTK_ALIGN_CENTER);
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        days = (int)hb_obj_get_int(o, "log_retention_days", 90);
        hb_obj_free(o);
    }
    v->ret_loading = TRUE;
    {
        guint sel = 1;   /* 90 */
        for (i = 0; i < N_RET; i++)
            if (RET_CHOICES[i] == days) { sel = (guint)i; break; }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(v->ret_dd), sel);
    }
    v->ret_loading = FALSE;
    g_signal_connect(v->ret_dd, "notify::selected",
                     G_CALLBACK(on_ret_sel), v);
    gtk_box_append(GTK_BOX(bar), v->ret_dd);

    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(bar), sp);

    csv = gtk_button_new_with_label(_("导出 CSV"));
    gtk_widget_add_css_class(csv, "hb-btn");
    g_signal_connect(csv, "clicked", G_CALLBACK(on_export), v);
    gtk_box_append(GTK_BOX(bar), csv);

    js = gtk_button_new_with_label(_("导出 JSON"));
    gtk_widget_add_css_class(js, "hb-btn");
    g_object_set_data(G_OBJECT(js), "json", GINT_TO_POINTER(1));
    g_signal_connect(js, "clicked", G_CALLBACK(on_export), v);
    gtk_box_append(GTK_BOX(bar), js);

    clr = gtk_button_new_with_label(_("清空日志"));
    gtk_widget_add_css_class(clr, "hb-btn");
    gtk_widget_add_css_class(clr, "ghost");
    g_signal_connect(clr, "clicked", G_CALLBACK(on_clear), v);
    gtk_box_append(GTK_BOX(bar), clr);

    gtk_widget_add_css_class(bar, "hb-toolbar");
    gtk_box_append(GTK_BOX(v), bar);

    /* 表格 */
    sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    sc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(sc, 18);
    gtk_widget_set_margin_end(sc, 18);
    gtk_widget_set_margin_top(sc, 4);
    gtk_widget_set_margin_bottom(sc, 14);
    table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(table, "hb-table");
    gtk_box_append(GTK_BOX(table), row_head());
    v->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(table), v->rows);
    gtk_box_append(GTK_BOX(sc), table);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), sc);
    gtk_box_append(GTK_BOX(v), sw);

    v->desc = TRUE;
    (void)sv;
}

static void hb_logs_view_init(HbLogsView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
}

static void hb_logs_view_dispose(GObject *o)
{
    HbLogsView *v = HB_LOGS_VIEW(o);
    v->dead = TRUE;
    if (v->changed_h)
        g_signal_handler_disconnect(v->store, v->changed_h);
    v->changed_h = 0;
    G_OBJECT_CLASS(hb_logs_view_parent_class)->dispose(o);
}

static void hb_logs_view_class_init(HbLogsViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_logs_view_dispose;
}

GtkWidget *hb_logs_view_new(HbStore *store)
{
    HbLogsView *v = g_object_new(HB_TYPE_LOGS_VIEW, NULL);
    v->store = store;
    build(v);
    fill_rows(v);
    v->changed_h = g_signal_connect(v->store, "changed",
                                    G_CALLBACK(on_store_changed), v);
    return GTK_WIDGET(v);
}
