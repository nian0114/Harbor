/* hb-snippets-view — Snippets 页 + Shell History 子页（FR-08）
 *
 * Snippets：卡片列表（title/scope/正文预览），点击 → 共享发送流；
 * 新建/编辑模态（标题/正文 {{var}}/作用范围/执行开关）；导入/导出 JSON；
 * 删除确认 + 9s 撤销。
 * Shell History：命令列表（mono + 主机/Local + 相对时间），搜索、
 * 主机过滤、行操作（转为 Snippet/复制/删除）、清空、history_enabled 开关。
 * ================================================================== */

#include "hb-snippets-view.h"
#include "hb-dialogs.h"
#include "hb-icons.h"
#include "hb-snippet-send.h"
#include "hb-window.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <glib/gi18n.h>

struct _HbSnippetsView {
    GtkBox parent;

    HbStore   *store;
    GtkWidget *stack;          /* "snips" | "hist" */

    /* snippets page */
    GtkWidget *search;
    GtkWidget *list;

    /* history page */
    GtkWidget *h_search;
    GtkWidget *h_host_sel;     /* GtkDropDown */
    GtkWidget *h_list;
    GPtrArray *h_hosts;        /* 与 dropdown 对齐的过滤值：NULL=全部,""=Local */
    GtkWidget *hist_sw;
    gboolean   hist_loading;

    gulong   changed_h;
    gboolean dead;         /* dispose 中：dropdown notify 不再重建 */
};

G_DEFINE_TYPE(HbSnippetsView, hb_snippets_view, GTK_TYPE_BOX)

static void fill_snips(HbSnippetsView *v);
static void fill_hist(HbSnippetsView *v);

static GtkWidget *win_of(HbSnippetsView *v)
{
    return gtk_widget_get_ancestor(GTK_WIDGET(v), HB_TYPE_WINDOW);
}

/* ============================ 编辑 / 新建对话框 ============================ */

typedef struct {
    HbSnippetsView *v;
    AdwDialog  *dlg;
    GtkWidget  *title_e;
    GtkWidget  *body_tv;
    GtkWidget  *scope_dd;      /* 0=全局 1=主机 2=分组 */
    GtkWidget  *host_dd;
    GtkWidget  *group_e;
    GtkWidget  *host_row, *group_row;
    GtkWidget  *run_sw;
    GPtrArray  *hosts;         /* 借用 host id 数组（与 host_dd 对齐） */
    char       *edit_id;       /* NULL = 新建 */
    gulong      scope_h;       /* scope_dd notify::selected —— 关闭时断开 */
} EditCtx;

static void edit_ctx_free(EditCtx *e)
{
    if (e->hosts) g_ptr_array_unref(e->hosts);
    g_free(e->edit_id);
    g_free(e);
}

static void on_scope_sel(GObject *dd, GParamSpec *ps, gpointer ud)
{
    EditCtx *e = ud;
    guint s;
    (void)dd; (void)ps;
    /* 对话框销毁中 model 可能先被摘掉 —— get_selected 会崩 */
    if (!e || !gtk_drop_down_get_model(GTK_DROP_DOWN(e->scope_dd)))
        return;
    s = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->scope_dd));
    gtk_widget_set_visible(e->host_row, s == 1);
    gtk_widget_set_visible(e->group_row, s == 2);
}

static char *tv_text(GtkTextView *tv)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(tv);
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(b, &a, &z);
    return gtk_text_buffer_get_text(b, &a, &z, FALSE);
}

static void edit_save(GtkButton *b, gpointer ud)
{
    EditCtx *e = ud;
    HbSnippetsView *v = e->v;
    hb_obj *o = NULL;
    const char *title;
    char *body, *scope = NULL;
    guint s;
    (void)b;

    title = gtk_editable_get_text(GTK_EDITABLE(e->title_e));
    body = tv_text(GTK_TEXT_VIEW(e->body_tv));
    s = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->scope_dd));
    if (s == 1) {
        guint h = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->host_dd));
        if (e->hosts && h < e->hosts->len)
            scope = g_strdup_printf("host:%s",
                                    (char *)e->hosts->pdata[h]);
    } else if (s == 2) {
        const char *g = gtk_editable_get_text(GTK_EDITABLE(e->group_e));
        scope = g_strdup_printf("group:%s", g ? g : "");
    } else {
        scope = g_strdup("global");
    }

    if (e->edit_id &&
        hb_vault_get(hb_store_vault(v->store), e->edit_id, &o) == HB_OK && o) {
        /* 编辑现有 */
    } else {
        if (o) hb_obj_free(o);
        o = hb_obj_new("snippet");
    }
    hb_obj_set_str(o, "title", title && *title ? title : _("未命名"));
    hb_obj_set_str(o, "body", body);
    hb_obj_set_str(o, "scope", scope);
    hb_obj_set_bool(o, "run",
                    gtk_switch_get_active(GTK_SWITCH(e->run_sw)));
    hb_vault_put(hb_store_vault(v->store), o);
    hb_obj_free(o);
    g_free(body);
    g_free(scope);
    adw_dialog_close(e->dlg);
}

static void edit_closed(AdwDialog *d, gpointer ud)
{
    EditCtx *e = ud;
    (void)d;
    /* closed 可能在 widget 拆卸前触发 —— 先断开 notify::selected，
     * 否则拆卸时 dropdown 再发信号会摸到已释放的 EditCtx。 */
    if (e->scope_h)
        g_signal_handler_disconnect(e->scope_dd, e->scope_h);
    e->scope_h = 0;
    edit_ctx_free(e);
}

/* obj NULL → 新建（pref_body/pref_title 可预填）；非 NULL → 编辑该对象 */
static void open_edit_dialog(HbSnippetsView *v, hb_obj *obj,
                             const char *pref_title, const char *pref_body)
{
    EditCtx *e = g_new0(EditCtx, 1);
    GtkWidget *body, *foot, *f, *cancel, *ok, *row, *l, *sw, *tv_sw;
    GtkStringList *sm;
    hb_obj **hosts;
    size_t nh = 0, i;
    const char *scope;
    guint sidx = 0;
    char want_host[64] = "", want_group[128] = "";

    e->v = v;
    e->dlg = hb_modal_new(obj ? _("编辑 Snippet") : _("新建 Snippet"),
                          &body, &foot);
    if (obj)
        e->edit_id = g_strdup(hb_obj_id(obj));

    /* 标题 */
    l = gtk_label_new(_("标题"));
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    e->title_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e->title_e), _("重启服务"));
    gtk_editable_set_text(GTK_EDITABLE(e->title_e),
        obj ? (hb_obj_get_str(obj, "title") ?: "") :
              (pref_title ? pref_title : ""));
    gtk_box_append(GTK_BOX(body), e->title_e);

    /* 正文 */
    l = gtk_label_new(_("命令（支持 {{变量}} 占位）"));
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    tv_sw = gtk_scrolled_window_new();
    gtk_widget_set_size_request(tv_sw, -1, 96);
    e->body_tv = gtk_text_view_new();
    gtk_widget_add_css_class(e->body_tv, "mono");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(e->body_tv), GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(e->body_tv)),
        obj ? (hb_obj_get_str(obj, "body") ?: "") :
              (pref_body ? pref_body : ""), -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tv_sw), e->body_tv);
    gtk_box_append(GTK_BOX(body), tv_sw);

    /* 解析现有 scope */
    scope = obj ? hb_obj_get_str(obj, "scope") : NULL;
    if (scope && !strncmp(scope, "host:", 5)) {
        sidx = 1;
        snprintf(want_host, sizeof want_host, "%s", scope + 5);
    } else if (scope && !strncmp(scope, "group:", 6)) {
        sidx = 2;
        snprintf(want_group, sizeof want_group, "%s", scope + 6);
    }

    /* 作用范围 */
    l = gtk_label_new(_("作用范围"));
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    sm = gtk_string_list_new(NULL);
    gtk_string_list_append(sm, _("全局"));
    gtk_string_list_append(sm, _("指定主机"));
    gtk_string_list_append(sm, _("分组"));
    e->scope_dd = gtk_drop_down_new(G_LIST_MODEL(sm), NULL);
    /* sm 不 unref：GTK 拆卸 dropdown 时仍会读 model（host-form 同约定） */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(e->scope_dd), sidx);
    gtk_box_append(GTK_BOX(body), e->scope_dd);

    /* 主机选择 */
    e->host_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    l = gtk_label_new(_("主机"));
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(e->host_row), l);
    sm = gtk_string_list_new(NULL);
    e->hosts = g_ptr_array_new();
    hosts = hb_store_list(v->store, "host", &nh);
    {
        guint sel = 0;
        for (i = 0; hosts && i < nh; i++) {
            const char *nm = hb_obj_get_str(hosts[i], "name");
            const char *id = hb_obj_id(hosts[i]);
            gtk_string_list_append(sm, nm && *nm ? nm : id);
            g_ptr_array_add(e->hosts, (gpointer)id);   /* 借用：objs 活到函数外？不——见下 */
            if (*want_host && !strcmp(id, want_host))
                sel = (guint)e->hosts->len - 1;
        }
        /* hosts[] 里 id 指针随 hb_obj_list_free 释放 —— 先深拷贝进 e->hosts */
        for (i = 0; i < e->hosts->len; i++)
            e->hosts->pdata[i] = g_strdup(e->hosts->pdata[i]);
        g_ptr_array_set_free_func(e->hosts, g_free);
        if (hosts) hb_obj_list_free(hosts, nh);
        e->host_dd = gtk_drop_down_new(G_LIST_MODEL(sm), NULL);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(e->host_dd), sel);
    }
    gtk_box_append(GTK_BOX(e->host_row), e->host_dd);
    gtk_widget_set_visible(e->host_row, sidx == 1);
    gtk_box_append(GTK_BOX(body), e->host_row);

    /* 分组名 */
    e->group_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    l = gtk_label_new(_("分组名"));
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(e->group_row), l);
    e->group_e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e->group_e), want_group);
    gtk_box_append(GTK_BOX(e->group_row), e->group_e);
    gtk_widget_set_visible(e->group_row, sidx == 2);
    gtk_box_append(GTK_BOX(body), e->group_row);

    e->scope_h = g_signal_connect(e->scope_dd, "notify::selected",
                                  G_CALLBACK(on_scope_sel), e);

    /* 执行开关 */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    sw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    l = gtk_label_new(_("执行（追加回车）"));
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(sw), l);
    {
        GtkWidget *sub = gtk_label_new(_("发送后自动回车执行命令"));
        gtk_widget_add_css_class(sub, "hb-dim");
        gtk_label_set_xalign(GTK_LABEL(sub), 0);
        gtk_box_append(GTK_BOX(sw), sub);
    }
    gtk_box_append(GTK_BOX(row), sw);
    e->run_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(e->run_sw),
        obj ? hb_obj_get_bool(obj, "run", FALSE) : FALSE);
    gtk_widget_set_valign(e->run_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(e->run_sw, TRUE);
    gtk_widget_set_halign(e->run_sw, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), e->run_sw);
    gtk_box_append(GTK_BOX(body), row);

    /* footer */
    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label(_("取消"));
    gtk_widget_add_css_class(cancel, "hb-btn");
    g_signal_connect_swapped(cancel, "clicked",
                             G_CALLBACK(adw_dialog_close), e->dlg);
    gtk_box_append(GTK_BOX(f), cancel);
    ok = gtk_button_new_with_label(_("保存"));
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(edit_save), e);
    gtk_box_append(GTK_BOX(f), ok);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(e->dlg, "closed", G_CALLBACK(edit_closed), e);
    adw_dialog_present(e->dlg, GTK_WIDGET(v));
}

/* ============================ 删除 + 撤销 ============================ */

typedef struct {
    HbSnippetsView *v;
    char *id;
    char *title;
} DelSnip;

static void snip_undo_cb(gpointer ud)
{
    DelSnip *d = ud;
    hb_vault_undelete(hb_store_vault(d->v->store), d->id);
}
static void snip_undo_free(gpointer ud)
{
    DelSnip *d = ud;
    g_free(d->id); g_free(d->title); g_free(d);
}

static void del_done(gboolean ok, gpointer ud)
{
    DelSnip *d = ud;
    char *msg;
    if (!ok) { snip_undo_free(d); return; }
    hb_vault_delete(hb_store_vault(d->v->store), d->id);
    msg = g_strdup_printf(_("已删除 Snippet「%s」"),
                          d->title ? d->title : "");
    hb_toast_undo_destroy(GTK_WIDGET(d->v), msg,
                          snip_undo_cb, snip_undo_free, d);
    g_free(msg);
}

static void on_del_snip(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "sid");
    const char *title = g_object_get_data(G_OBJECT(b), "stitle");
    DelSnip *d;
    char *msg;
    (void)b;

    d = g_new0(DelSnip, 1);
    d->v = v;
    d->id = g_strdup(id);
    d->title = g_strdup(title);
    msg = g_strdup_printf(_("确定删除 <b>%s</b>？删除后 9 秒内可撤销。"),
                          title ? title : "");
    hb_dialog_confirm(GTK_WIDGET(v), _("删除 Snippet"), msg, _("删除"), TRUE,
                      del_done, d);
    g_free(msg);
}

static void on_edit_snip(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "sid");
    hb_obj *o = NULL;
    (void)b;
    if (hb_vault_get(hb_store_vault(v->store), id, &o) == HB_OK && o) {
        open_edit_dialog(v, o, NULL, NULL);
        hb_obj_free(o);
    }
}

static void on_new_snip(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    (void)b;
    open_edit_dialog(v, NULL, NULL, NULL);
}

/* ============================ 卡片 ============================ */

static void on_card_click(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "sid");
    GtkWidget *win = win_of(v);
    (void)b;
    if (win)
        hb_snippet_send(win, v->store, id);
}

static GtkWidget *snip_card(HbSnippetsView *v, hb_obj *s)
{
    const char *title = hb_obj_get_str(s, "title");
    const char *body  = hb_obj_get_str(s, "body");
    const char *scope = hb_obj_get_str(s, "scope");
    GtkWidget *card, *r1, *t, *tag, *sp, *eb, *db, *bl;

    card = gtk_button_new();
    gtk_widget_add_css_class(card, "snip-card");

    r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    t = gtk_label_new(title && *title ? title : _("未命名"));
    gtk_widget_add_css_class(t, "snip-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(r1), t);

    if (scope && *scope) {
        const char *disp = scope;
        if (!strcmp(scope, "global")) disp = _("全局");
        else if (!strncmp(scope, "host:", 5)) disp = _("主机");
        else if (!strncmp(scope, "group:", 6)) disp = scope + 6;
        /* host:<uuid> 显示 uuid 没意义 → 解析主机名 */
        if (!strncmp(scope, "host:", 5)) {
            hb_obj *h = NULL;
            if (hb_vault_get(hb_store_vault(v->store), scope + 5,
                             &h) == HB_OK && h) {
                const char *nm = hb_obj_get_str(h, "name");
                if (nm && *nm) {
                    tag = gtk_label_new(nm);
                    goto have_tag;
                }
                hb_obj_free(h);
            }
        }
        tag = gtk_label_new(disp);
    have_tag:
        gtk_widget_add_css_class(tag, "hb-tag");
        gtk_box_append(GTK_BOX(r1), tag);
    }

    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(r1), sp);

    eb = gtk_button_new();
    gtk_widget_add_css_class(eb, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(eb), hb_icon_new("pencil", 13));
    gtk_widget_set_tooltip_text(eb, _("编辑"));
    g_object_set_data_full(G_OBJECT(eb), "sid",
                           g_strdup(hb_obj_id(s)), g_free);
    g_signal_connect(eb, "clicked", G_CALLBACK(on_edit_snip), v);
    gtk_box_append(GTK_BOX(r1), eb);

    db = gtk_button_new();
    gtk_widget_add_css_class(db, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(db), hb_icon_new("trash", 13));
    gtk_widget_set_tooltip_text(db, _("删除"));
    g_object_set_data_full(G_OBJECT(db), "sid",
                           g_strdup(hb_obj_id(s)), g_free);
    g_object_set_data_full(G_OBJECT(db), "stitle",
                           g_strdup(title ? title : ""), g_free);
    g_signal_connect(db, "clicked", G_CALLBACK(on_del_snip), v);
    gtk_box_append(GTK_BOX(r1), db);

    /* card 本体 = 垂直盒：标题行 + 代码预览 */
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(box), r1);
        bl = gtk_label_new(body ? body : "");
        gtk_widget_add_css_class(bl, "mono");
        gtk_widget_add_css_class(bl, "snip-code");
        gtk_label_set_xalign(GTK_LABEL(bl), 0);
        gtk_label_set_lines(GTK_LABEL(bl), 4);
        gtk_label_set_ellipsize(GTK_LABEL(bl), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), bl);
        gtk_button_set_child(GTK_BUTTON(card), box);
    }

    g_object_set_data_full(G_OBJECT(card), "sid",
                           g_strdup(hb_obj_id(s)), g_free);
    g_signal_connect(card, "clicked", G_CALLBACK(on_card_click), v);
    gtk_widget_set_tooltip_text(card, _("点击发送到当前终端"));
    return card;
}

static void fill_snips(HbSnippetsView *v)
{
    GtkWidget *c;
    hb_obj **objs;
    size_t n = 0, i, shown = 0;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search));

    while ((c = gtk_widget_get_first_child(v->list)))
        gtk_box_remove(GTK_BOX(v->list), c);

    objs = hb_store_list(v->store, "snippet", &n);
    for (i = 0; objs && i < n; i++) {
        hb_obj *s = objs[i];
        const char *title = hb_obj_get_str(s, "title");
        const char *body  = hb_obj_get_str(s, "body");
        const char *scope = hb_obj_get_str(s, "scope");
        if (q && *q) {
            char *hay = g_strdup_printf("%s %s %s",
                title ? title : "", body ? body : "",
                scope ? scope : "");
            char *hay_l = g_utf8_strdown(hay, -1);
            char *q_l = g_utf8_strdown(q, -1);
            gboolean hit = strstr(hay_l, q_l) != NULL;
            g_free(hay); g_free(hay_l); g_free(q_l);
            if (!hit) continue;
        }
        gtk_box_append(GTK_BOX(v->list), snip_card(v, s));
        shown++;
    }
    if (objs) hb_obj_list_free(objs, n);
    if (!shown) {
        GtkWidget *l = gtk_label_new(q && *q ?
            _("无匹配 Snippet") : _("暂无 Snippet — 新建一个开始"));
        gtk_widget_add_css_class(l, "hb-e-sub");
        gtk_widget_set_margin_top(l, 30);
        gtk_box_append(GTK_BOX(v->list), l);
    }
}

static void on_search(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    fill_snips((HbSnippetsView *)ud);
}

/* ============================ 导入 / 导出 JSON ============================ */

static void imp_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbSnippetsView *v = ud;
    GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *data = NULL;
    gsize len;
    size_t n = 0;
    char *msg;
    if (!gf) return;
    if (!g_file_load_contents(gf, NULL, &data, &len, NULL, NULL)) {
        hb_toast(GTK_WIDGET(v), _("读取文件失败"));
        g_object_unref(gf);
        return;
    }
    hb_vault_import_json(hb_store_vault(v->store), "snippet", data, &n);
    msg = g_strdup_printf(_("已导入 %zu 条 Snippet"), n);
    hb_toast(GTK_WIDGET(v), msg);
    g_free(msg);
    g_free(data);
    g_object_unref(gf);
}

static void on_import(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    GtkFileDialog *d;
    (void)b;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, _("导入 Snippets JSON"));
    gtk_file_dialog_open(d, NULL, NULL, imp_done, v);
    g_object_unref(d);
}

static void exp_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbSnippetsView *v = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *out = NULL;
    GError *err = NULL;
    if (!gf) return;
    if (hb_vault_export_json(hb_store_vault(v->store), "snippet",
                             &out) != HB_OK || !out ||
        !g_file_replace_contents(gf, out, strlen(out), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        hb_toast(GTK_WIDGET(v), _("导出失败"));
        if (err) g_error_free(err);
    } else {
        hb_toast(GTK_WIDGET(v), _("Snippets 已导出"));
    }
    g_free(out);
    g_object_unref(gf);
}

static void on_export(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    GtkFileDialog *d;
    (void)b;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, _("导出 Snippets"));
    gtk_file_dialog_set_initial_name(d, "harbor-snippets.json");
    gtk_file_dialog_save(d, NULL, NULL, exp_done, v);
    g_object_unref(d);
}

/* ============================ Shell History ============================ */

static char *rel_time(int64_t ts_ms)
{
    int64_t now = g_get_real_time() / 1000;
    int64_t d = now - ts_ms;
    if (d < 60 * 1000) return g_strdup(_("刚刚"));
    if (d < 3600 * 1000) return g_strdup_printf(ngettext("%d 分钟前", "%d 分钟前", d / 60000), (int)(d / 60000));
    if (d < 86400 * 1000) return g_strdup_printf(ngettext("%d 小时前", "%d 小时前", d / 3600000), (int)(d / 3600000));
    if (d < 30 * 86400LL * 1000)
        return g_strdup_printf(ngettext("%d 天前", "%d 天前", d / 86400000), (int)(d / 86400000));
    {
        time_t t = (time_t)(ts_ms / 1000);
        char buf[32];
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
        return g_strdup(buf);
    }
}

static const char *hist_host_filter(HbSnippetsView *v)
{
    guint s = gtk_drop_down_get_selected(GTK_DROP_DOWN(v->h_host_sel));
    if (!v->h_hosts || s >= v->h_hosts->len)
        return NULL;
    return v->h_hosts->pdata[s];   /* NULL=全部, ""=Local, id=主机 */
}

typedef struct {
    HbSnippetsView *v;
    GHashTable   *names;     /* host_id → name */
    const char   *q;
    guint         shown;
} HistFill;

static const char *host_disp(HistFill *hf, const char *host_id)
{
    const char *nm;
    if (!host_id || !*host_id) return _("Local");
    nm = g_hash_table_lookup(hf->names, host_id);
    if (nm) return nm;
    {
        hb_obj *h = NULL;
        nm = NULL;
        if (hb_vault_get(hb_store_vault(hf->v->store), host_id,
                         &h) == HB_OK && h) {
            const char *n = hb_obj_get_str(h, "name");
            nm = n && *n ? n : host_id;
            g_hash_table_insert(hf->names, g_strdup(host_id),
                                g_strdup(nm));
            hb_obj_free(h);
        } else {
            nm = host_id;
            g_hash_table_insert(hf->names, g_strdup(host_id),
                                g_strdup(nm));
        }
    }
    return nm;
}

static void on_hist_copy(GtkButton *b, gpointer ud)
{
    const char *cmd = g_object_get_data(G_OBJECT(b), "cmd");
    (void)ud;
    GdkClipboard *cb = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(b)));
    gdk_clipboard_set_text(cb, cmd ? cmd : "");
}

static void on_hist_to_snip(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    const char *cmd = g_object_get_data(G_OBJECT(b), "cmd");
    (void)b;
    open_edit_dialog(v, NULL, NULL, cmd);
}

static void on_hist_del(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    int64_t id = (int64_t)(intptr_t)g_object_get_data(G_OBJECT(b), "hid");
    (void)b;
    hb_vault_history_delete(hb_store_vault(v->store), id);
}

static bool hist_row_cb(const hb_history_entry *e, void *ud)
{
    HistFill *hf = ud;
    HbSnippetsView *v = hf->v;
    GtkWidget *row, *cl, *hl, *tl, *b1, *b2, *b3;
    char *rt;

    if (hf->q && *hf->q && e->cmd &&
        !strstr(e->cmd, hf->q))
        return true;

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(row, "hb-table-row");
    gtk_widget_set_margin_start(row, 10);
    gtk_widget_set_margin_end(row, 10);

    cl = gtk_label_new(e->cmd ? e->cmd : "");
    gtk_widget_add_css_class(cl, "mono");
    gtk_label_set_xalign(GTK_LABEL(cl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(cl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_single_line_mode(GTK_LABEL(cl), TRUE);
    gtk_label_set_selectable(GTK_LABEL(cl), TRUE);
    gtk_widget_set_hexpand(cl, TRUE);
    gtk_box_append(GTK_BOX(row), cl);

    hl = gtk_label_new(host_disp(hf, e->host_id));
    gtk_widget_add_css_class(hl, "hb-dim");
    gtk_box_append(GTK_BOX(row), hl);

    rt = rel_time(e->ts);
    tl = gtk_label_new(rt);
    gtk_widget_add_css_class(tl, "hb-dim");
    gtk_box_append(GTK_BOX(row), tl);
    g_free(rt);

    b1 = gtk_button_new();
    gtk_widget_add_css_class(b1, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(b1), hb_icon_new("snippet", 13));
    gtk_widget_set_tooltip_text(b1, _("转为 Snippet"));
    g_object_set_data_full(G_OBJECT(b1), "cmd",
                         g_strdup(e->cmd ? e->cmd : ""), g_free);
    g_signal_connect(b1, "clicked", G_CALLBACK(on_hist_to_snip), v);
    gtk_box_append(GTK_BOX(row), b1);

    b2 = gtk_button_new();
    gtk_widget_add_css_class(b2, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(b2), hb_icon_new("copy", 13));
    gtk_widget_set_tooltip_text(b2, _("复制"));
    g_object_set_data_full(G_OBJECT(b2), "cmd",
                         g_strdup(e->cmd ? e->cmd : ""), g_free);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_hist_copy), NULL);
    gtk_box_append(GTK_BOX(row), b2);

    b3 = gtk_button_new();
    gtk_widget_add_css_class(b3, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(b3), hb_icon_new("trash", 13));
    gtk_widget_set_tooltip_text(b3, _("删除"));
    g_object_set_data(G_OBJECT(b3), "hid", (gpointer)(intptr_t)e->id);
    g_signal_connect(b3, "clicked", G_CALLBACK(on_hist_del), v);
    gtk_box_append(GTK_BOX(row), b3);

    gtk_box_append(GTK_BOX(v->h_list), row);
    hf->shown++;
    return true;
}

static bool collect_host_id(const hb_history_entry *e, void *ud)
{
    if (e->host_id)
        g_hash_table_add((GHashTable *)ud, (gpointer)e->host_id);
    return true;
}

static void rebuild_host_dropdown(HbSnippetsView *v)
{
    /* 从 vault 收集出现过的 host_id → dropdown [全部, Local, 各主机名] */
    GtkStringList *sm = gtk_string_list_new(NULL);
    GPtrArray *ids;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    hb_obj **hosts;
    size_t nh = 0, i;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(v->h_host_sel));
    char *prev = (!v->h_hosts || sel >= v->h_hosts->len)
                 ? NULL : g_strdup(v->h_hosts->pdata[sel]);
    guint newsel = 0;

    /* 先抓历史里的 host_id 集合 */
    hb_vault_history_foreach(hb_store_vault(v->store), NULL, NULL,
                             collect_host_id, seen);

    ids = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ids, NULL);                /* 全部 */
    g_ptr_array_add(ids, g_strdup(""));        /* Local */
    gtk_string_list_append(sm, _("全部主机"));
    gtk_string_list_append(sm, _("Local"));
    if (prev && *prev) newsel = GTK_INVALID_LIST_POSITION;

    hosts = hb_store_list(v->store, "host", &nh);
    for (i = 0; hosts && i < nh; i++) {
        const char *id = hb_obj_id(hosts[i]);
        const char *nm = hb_obj_get_str(hosts[i], "name");
        if (!g_hash_table_contains(seen, id))
            continue;
        g_ptr_array_add(ids, g_strdup(id));
        gtk_string_list_append(sm, nm && *nm ? nm : id);
        if (prev && *prev && !strcmp(prev, id))
            newsel = ids->len - 1;
    }
    if (hosts) hb_obj_list_free(hosts, nh);
    g_hash_table_unref(seen);
    g_free(prev);

    if (newsel == GTK_INVALID_LIST_POSITION) newsel = 0;
    if (v->h_hosts) g_ptr_array_unref(v->h_hosts);
    v->h_hosts = ids;
    gtk_drop_down_set_model(GTK_DROP_DOWN(v->h_host_sel),
                            G_LIST_MODEL(sm));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v->h_host_sel), newsel);
}

static void fill_hist(HbSnippetsView *v)
{
    GtkWidget *c;
    HistFill hf;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->h_search));

    while ((c = gtk_widget_get_first_child(v->h_list)))
        gtk_box_remove(GTK_BOX(v->h_list), c);

    memset(&hf, 0, sizeof hf);
    hf.v = v;
    hf.q = q;
    hf.names = g_hash_table_new_full(g_str_hash, g_str_equal,
                                   g_free, g_free);
    hb_vault_history_foreach(hb_store_vault(v->store),
                             NULL, hist_host_filter(v),
                             hist_row_cb, &hf);
    g_hash_table_unref(hf.names);

    if (!hf.shown) {
        GtkWidget *l = gtk_label_new(_("暂无命令历史"));
        gtk_widget_add_css_class(l, "hb-e-sub");
        gtk_widget_set_margin_top(l, 30);
        gtk_box_append(GTK_BOX(v->h_list), l);
    }
}

static void on_hist_search(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    fill_hist((HbSnippetsView *)ud);
}

static void on_hist_host_sel(GObject *dd, GParamSpec *ps, gpointer ud)
{
    HbSnippetsView *v = ud;
    (void)dd; (void)ps;
    if (v->dead ||
        !gtk_drop_down_get_model(GTK_DROP_DOWN(v->h_host_sel)))
        return;
    fill_hist(v);
}

static void clear_done(gboolean ok, gpointer ud)
{
    HbSnippetsView *v = ud;
    if (ok)
        hb_vault_history_clear(hb_store_vault(v->store));
}

static void on_clear_hist(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    (void)b;
    hb_dialog_confirm(GTK_WIDGET(v), _("清空命令历史"),
                      _("确定删除全部 Shell History？此操作不可撤销。"),
                      _("清空"), TRUE, clear_done, v);
}

static void on_hist_enabled(GObject *sw, GParamSpec *ps, gpointer ud)
{
    HbSnippetsView *v = ud;
    hb_obj *o = NULL;
    (void)ps;
    if (v->hist_loading) return;
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        hb_obj_set_bool(o, "history_enabled",
                        gtk_switch_get_active(GTK_SWITCH(sw)));
        hb_vault_put(hb_store_vault(v->store), o);
        hb_obj_free(o);
    }
}

static void on_show_hist(GtkButton *b, gpointer ud)
{
    HbSnippetsView *v = ud;
    gboolean hist;
    (void)b;
    hist = !strcmp(gtk_stack_get_visible_child_name(GTK_STACK(v->stack)),
                   "snips");
    if (hist) {
        rebuild_host_dropdown(v);
        fill_hist(v);
        gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "hist");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "snips");
    }
}

/* dev 钩子：等同点击 “Shell History”/返回按钮。 */
void hb_snippets_view_show_hist(HbSnippetsView *v, gboolean hist)
{
    gboolean cur;
    g_return_if_fail(HB_IS_SNIPPETS_VIEW(v));
    cur = !strcmp(gtk_stack_get_visible_child_name(GTK_STACK(v->stack)),
                  "hist");
    if (cur != hist)
        on_show_hist(NULL, v);
}

/* ============================ 构建 ============================ */

static GtkWidget *build_snips_page(HbSnippetsView *v)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *split, *newb, *menub, *hb, *hb_r, *spacer, *sw;

    /* New snippet split ▾ */
    split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(split, "linked");
    newb = gtk_button_new_with_label(_("New snippet"));
    gtk_widget_add_css_class(newb, "hb-btn");
    gtk_widget_add_css_class(newb, "primary");
    g_signal_connect(newb, "clicked", G_CALLBACK(on_new_snip), v);
    gtk_box_append(GTK_BOX(split), newb);
    menub = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(menub), "▾");
    gtk_widget_add_css_class(menub, "hb-btn");
    gtk_widget_add_css_class(menub, "primary");
    {
        GMenu *m = g_menu_new();
        g_menu_append(m, _("新建 Snippet"), "snip.new");
        g_menu_append(m, _("导入 JSON…"), "snip.import");
        g_menu_append(m, _("导出 JSON"), "snip.export");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menub),
                                       G_MENU_MODEL(m));
        g_object_unref(m);
    }
    gtk_box_append(GTK_BOX(split), menub);
    gtk_box_append(GTK_BOX(bar), split);

    /* actions 供 menu model 派发 */
    {
        GSimpleActionGroup *ag = g_simple_action_group_new();
        GSimpleAction *a;
        a = g_simple_action_new("new", NULL);
        g_signal_connect_swapped(a, "activate",
                                 G_CALLBACK(on_new_snip), v);
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(a));
        a = g_simple_action_new("import", NULL);
        g_signal_connect_swapped(a, "activate",
                                 G_CALLBACK(on_import), v);
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(a));
        a = g_simple_action_new("export", NULL);
        g_signal_connect_swapped(a, "activate",
                                 G_CALLBACK(on_export), v);
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(a));
        gtk_widget_insert_action_group(GTK_WIDGET(v), "snip",
                                       G_ACTION_GROUP(ag));
        g_object_unref(ag);
    }

    /* Shell History 切换 */
    hb = gtk_button_new();
    gtk_widget_add_css_class(hb, "hb-btn");
    gtk_widget_add_css_class(hb, "ghost");
    hb_r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(hb_r), hb_icon_new("clock", 13));
    gtk_box_append(GTK_BOX(hb_r), gtk_label_new(_("Shell History")));
    gtk_button_set_child(GTK_BUTTON(hb), hb_r);
    g_signal_connect(hb, "clicked", G_CALLBACK(on_show_hist), v);
    gtk_box_append(GTK_BOX(bar), hb);

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    v->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
                                          _("搜索 Snippets…"));
    gtk_widget_set_size_request(v->search, 220, -1);
    gtk_widget_set_valign(v->search, GTK_ALIGN_CENTER);
    g_signal_connect(v->search, "search-changed",
                     G_CALLBACK(on_search), v);
    gtk_box_append(GTK_BOX(bar), v->search);

    gtk_widget_add_css_class(bar, "hb-toolbar");
    gtk_box_append(GTK_BOX(page), bar);

    sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    v->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(v->list, "snip-list");
    gtk_widget_set_margin_start(v->list, 18);
    gtk_widget_set_margin_end(v->list, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), v->list);
    gtk_box_append(GTK_BOX(page), sw);
    return page;
}

static GtkWidget *build_hist_page(HbSnippetsView *v)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *back, *br, *row, *l, *sub, *sw, *clr;
    hb_obj *o = NULL;
    gboolean enabled = TRUE;

    /* 返回 */
    back = gtk_button_new();
    gtk_widget_add_css_class(back, "hb-btn");
    gtk_widget_add_css_class(back, "ghost");
    br = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(br), hb_icon_new("back", 13));
    gtk_box_append(GTK_BOX(br), gtk_label_new(_("Snippets")));
    gtk_button_set_child(GTK_BUTTON(back), br);
    g_signal_connect(back, "clicked", G_CALLBACK(on_show_hist), v);
    gtk_box_append(GTK_BOX(bar), back);

    /* history_enabled 开关 */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    l = gtk_label_new(_("记录命令历史"));
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), l);
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        enabled = hb_obj_get_bool(o, "history_enabled", true);
        hb_obj_free(o);
    }
    v->hist_sw = gtk_switch_new();
    v->hist_loading = TRUE;
    gtk_switch_set_active(GTK_SWITCH(v->hist_sw), enabled);
    v->hist_loading = FALSE;
    gtk_widget_set_valign(v->hist_sw, GTK_ALIGN_CENTER);
    g_signal_connect(v->hist_sw, "notify::active",
                     G_CALLBACK(on_hist_enabled), v);
    gtk_box_append(GTK_BOX(row), v->hist_sw);
    gtk_box_append(GTK_BOX(bar), row);

    /* history_sync（BETA，insensitive） */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    l = gtk_label_new(_("同步"));
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), l);
    sw = gtk_switch_new();
    gtk_widget_set_sensitive(sw, FALSE);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(sw, _("同步上线后可选"));
    gtk_box_append(GTK_BOX(row), sw);
    sub = gtk_label_new(_("同步上线后可选"));
    gtk_widget_add_css_class(sub, "hb-dim");
    gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), sub);
    gtk_box_append(GTK_BOX(bar), row);

    /* 主机过滤 */
    v->h_host_sel = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_size_request(v->h_host_sel, 130, -1);
    gtk_widget_set_valign(v->h_host_sel, GTK_ALIGN_CENTER);
    g_signal_connect(v->h_host_sel, "notify::selected",
                     G_CALLBACK(on_hist_host_sel), v);
    gtk_box_append(GTK_BOX(bar), v->h_host_sel);

    /* 搜索 */
    v->h_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->h_search),
                                          _("搜索命令…"));
    gtk_widget_set_size_request(v->h_search, 180, -1);
    gtk_widget_set_valign(v->h_search, GTK_ALIGN_CENTER);
    g_signal_connect(v->h_search, "search-changed",
                     G_CALLBACK(on_hist_search), v);
    gtk_box_append(GTK_BOX(bar), v->h_search);

    {
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(bar), spacer);
    }

    clr = gtk_button_new_with_label(_("清空历史"));
    gtk_widget_add_css_class(clr, "hb-btn");
    g_signal_connect(clr, "clicked", G_CALLBACK(on_clear_hist), v);
    gtk_box_append(GTK_BOX(bar), clr);

    gtk_widget_add_css_class(bar, "hb-toolbar");
    gtk_box_append(GTK_BOX(page), bar);

    sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    v->h_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(v->h_list, "hb-table");
    gtk_widget_set_margin_start(v->h_list, 18);
    gtk_widget_set_margin_end(v->h_list, 18);
    gtk_widget_set_margin_top(v->h_list, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), v->h_list);
    gtk_box_append(GTK_BOX(page), sw);
    return page;
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbSnippetsView *v = ud;
    (void)st; (void)id;
    if (v->dead) return;
    if (!strcmp(type, "snippet")) {
        fill_snips(v);
    } else if (!strcmp(type, "history") &&
               !strcmp(gtk_stack_get_visible_child_name(
                           GTK_STACK(v->stack)), "hist")) {
        fill_hist(v);
    }
}

static void hb_snippets_view_init(HbSnippetsView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
}

/* store 指派后构建（init 时 store 尚未设置） */
static void build_ui(HbSnippetsView *v)
{
    v->stack = gtk_stack_new();
    gtk_widget_set_vexpand(v->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(v->stack),
                        build_snips_page(v), "snips");
    gtk_stack_add_named(GTK_STACK(v->stack),
                        build_hist_page(v), "hist");
    gtk_box_append(GTK_BOX(v), v->stack);
    fill_snips(v);
    v->changed_h = g_signal_connect(v->store, "changed",
                                    G_CALLBACK(on_store_changed), v);
}

static void hb_snippets_view_dispose(GObject *o)
{
    HbSnippetsView *v = HB_SNIPPETS_VIEW(o);
    v->dead = TRUE;
    if (v->changed_h)
        g_signal_handler_disconnect(v->store, v->changed_h);
    v->changed_h = 0;
    G_OBJECT_CLASS(hb_snippets_view_parent_class)->dispose(o);
}

static void hb_snippets_view_finalize(GObject *o)
{
    HbSnippetsView *v = HB_SNIPPETS_VIEW(o);
    if (v->h_hosts) g_ptr_array_unref(v->h_hosts);
    G_OBJECT_CLASS(hb_snippets_view_parent_class)->finalize(o);
}

static void hb_snippets_view_class_init(HbSnippetsViewClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_snippets_view_dispose;
    oc->finalize = hb_snippets_view_finalize;
}

GtkWidget *hb_snippets_view_new(HbStore *store)
{
    HbSnippetsView *v = g_object_new(HB_TYPE_SNIPPETS_VIEW, NULL);
    v->store = store;
    build_ui(v);
    return GTK_WIDGET(v);
}
