/* hb-known-view — Known Hosts 页（FR-09）
 *
 * 工具栏：导入（默认 ~/.ssh/known_hosts + 选择文件…）、导出、
 * 写入 ~/.ssh/known_hosts…（先备份 .harbor-bak，精确行去重追加）。
 * 表格：主机（hashed 前缀）/ 端口 / 密钥类型 / 指纹(mono,中截断) /
 * 添加时间 / 删除(确认+撤销)。导入计数 toast + 警告查看器。
 * ================================================================== */

#include "hb-known-view.h"
#include "hb-dialogs.h"
#include "hb-icons.h"

#include <harbor/knownhosts.h>

#include <glib/gstdio.h>

#include <string.h>
#include <stdio.h>
#include <glib/gi18n.h>

struct _HbKnownView {
    GtkBox parent;

    HbStore   *store;
    GtkWidget *search;
    GtkWidget *table;      /* hb-table 内容（含 head 由 build 添加） */
    GtkWidget *rows;       /* 数据行容器 */

    gulong   changed_h;
    gboolean dead;
};

G_DEFINE_TYPE(HbKnownView, hb_known_view, GTK_TYPE_BOX)

static void fill_rows(HbKnownView *v);

static char *default_kh_path(void)
{
    return g_build_filename(g_get_home_dir(), ".ssh", "known_hosts", NULL);
}

/* ============================ 导入 ============================ */

static void show_import_result(HbKnownView *v, hb_import_result *r)
{
    char *msg = g_strdup_printf(_("已导入 %zu 条，跳过 %zu 条"),
                                r->created, r->skipped);
    hb_toast(GTK_WIDGET(v), msg);
    g_free(msg);
    if (r->nwarn) {
        GString *s = g_string_new(NULL);
        size_t i;
        for (i = 0; i < r->nwarn; i++)
            g_string_append_printf(s, "• %s\n", r->warnings[i]);
        hb_dialog_text(GTK_WIDGET(v), _("导入警告"), s->str);
        g_string_free(s, TRUE);
    }
}

static void import_path(HbKnownView *v, const char *path)
{
    hb_import_result r;
    memset(&r, 0, sizeof r);
    if (hb_known_import_file(hb_store_vault(v->store), path, &r) != HB_OK) {
        hb_toast(GTK_WIDGET(v), _("导入失败（无法读取文件）"));
        return;
    }
    show_import_result(v, &r);
    hb_import_result_free(&r);
}

static void on_import_default(GtkButton *b, gpointer ud)
{
    HbKnownView *v = ud;
    char *p = default_kh_path();
    (void)b;
    import_path(v, p);
    g_free(p);
}

static void imp_file_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbKnownView *v = ud;
    GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *p;
    if (!gf) return;
    p = g_file_get_path(gf);
    if (p) import_path(v, p);
    else   hb_toast(GTK_WIDGET(v), _("无法读取该文件"));
    g_free(p);
    g_object_unref(gf);
}

static void on_import_file(GtkButton *b, gpointer ud)
{
    HbKnownView *v = ud;
    GtkFileDialog *d;
    (void)b;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, _("选择 known_hosts 文件"));
    gtk_file_dialog_open(d, NULL, NULL, imp_file_done, v);
    g_object_unref(d);
}

/* ============================ 导出 ============================ */

static void exp_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbKnownView *v = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *out = NULL;
    GError *err = NULL;
    if (!gf) return;
    if (hb_known_export_text(hb_store_vault(v->store), &out) != HB_OK ||
        !out ||
        !g_file_replace_contents(gf, out, strlen(out), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        hb_toast(GTK_WIDGET(v), _("导出失败"));
        if (err) g_error_free(err);
    } else {
        hb_toast(GTK_WIDGET(v), _("Known hosts 已导出"));
    }
    g_free(out);
    g_object_unref(gf);
}

static void on_export(GtkButton *b, gpointer ud)
{
    HbKnownView *v = ud;
    GtkFileDialog *d;
    (void)b;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, _("导出 known_hosts"));
    gtk_file_dialog_set_initial_name(d, "known_hosts");
    gtk_file_dialog_save(d, NULL, NULL, exp_done, v);
    g_object_unref(d);
}

/* ==================== 写入 ~/.ssh/known_hosts ==================== */

/* 对象缺 line 字段时按 host/port/keytype/key 重建 */
static char *known_line(hb_obj *o)
{
    const char *line = hb_obj_get_str(o, "line");
    const char *host = hb_obj_get_str(o, "host");
    const char *kt   = hb_obj_get_str(o, "keytype");
    const char *key  = hb_obj_get_str(o, "key");
    int port = (int)hb_obj_get_int(o, "port", 22);
    if (line && *line) return g_strdup(line);
    if (!host || !kt || !key) return NULL;
    if (port == 22 || port <= 0)
        return g_strdup_printf("%s %s %s", host, kt, key);
    return g_strdup_printf("[%s]:%d %s %s", host, port, kt, key);
}

/* known_hosts 可能是符号链接（dotfiles 仓库）：解析到真实目标再写入，
 * 避免用普通文件替换链接本身；悬空链接解析到目标路径后继续。 */
static char *kh_resolve(const char *path)
{
    char *p = g_strdup(path);
    int hops;
    for (hops = 0; hops < 40; hops++) {
        GError *err = NULL;
        char *link = g_file_read_link(p, &err);
        if (!link) {
            if (err) g_error_free(err);
            break;
        }
        if (g_path_is_absolute(link)) {
            g_free(p);
            p = link;
        } else {
            char *dir = g_path_get_dirname(p);
            char *np  = g_build_filename(dir, link, NULL);
            g_free(dir);
            g_free(link);
            g_free(p);
            p = np;
        }
    }
    return p;
}

static void do_write_known(HbKnownView *v)
{
    char *link_path = default_kh_path();
    char *path = kh_resolve(link_path);   /* 真实目标（可能尚不存在） */
    gchar *cur = NULL;
    gsize clen = 0;
    GHashTable *have = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GString *add = g_string_new(NULL);
    hb_obj **objs;
    size_t n = 0, i, added = 0;
    GStatBuf st;
    int mode = 0600;    /* 新建 known_hosts 默认权限 */
    char *msg;

    if (g_stat(path, &st) == 0)
        mode = st.st_mode & 0777;
    else {
        /* 目标不存在：确保父目录（如 ~/.ssh）存在，0700 */
        char *dir = g_path_get_dirname(path);
        if (g_mkdir_with_parents(dir, 0700) != 0) {
            hb_toast(GTK_WIDGET(v), _("无法创建 .ssh 目录"));
            g_free(dir);
            goto out;
        }
        g_free(dir);
    }

    /* 现有文件 → 行集合（精确匹配）+ 备份（同权限） */
    if (g_file_get_contents(path, &cur, &clen, NULL)) {
        char **lines = g_strsplit(cur, "\n", -1), **p;
        for (p = lines; *p; p++)
            if (**p) g_hash_table_add(have, g_strdup(*p));
        g_strfreev(lines);
        {
            char *bak = g_strconcat(path, ".harbor-bak", NULL);
            GError *err = NULL;
            if (!g_file_set_contents_full(bak, cur, (gssize)clen,
                                          G_FILE_SET_CONTENTS_CONSISTENT,
                                          mode, &err)) {
                hb_toast(GTK_WIDGET(v), _("备份失败，已中止写入"));
                g_error_free(err);
                g_free(bak); goto out;
            }
            g_free(bak);
        }
    }

    objs = hb_store_list(v->store, "known", &n);
    for (i = 0; objs && i < n; i++) {
        char *l = known_line(objs[i]);
        if (!l) continue;
        if (!g_hash_table_contains(have, l)) {
            g_string_append(add, l);
            g_string_append_c(add, '\n');
            g_hash_table_add(have, g_strdup(l));
            added++;
        }
        g_free(l);
    }
    if (objs) hb_obj_list_free(objs, n);

    if (!added) {
        hb_toast(GTK_WIDGET(v), _("无新条目需要写入"));
        goto out;
    }
    {
        GString *all = g_string_new(cur ? cur : "");
        GError *err = NULL;
        if (all->len && all->str[all->len - 1] != '\n')
            g_string_append_c(all, '\n');
        g_string_append(all, add->str);
        /* CONSISTENT：写临时文件再 rename；已存在文件保留其权限，
         * mode 仅用于新建文件 */
        if (!g_file_set_contents_full(path, all->str, (gssize)all->len,
                                      G_FILE_SET_CONTENTS_CONSISTENT,
                                      mode, &err)) {
            hb_toast(GTK_WIDGET(v), _("写入失败"));
            g_error_free(err);
            g_string_free(all, TRUE);
            goto out;
        }
        g_string_free(all, TRUE);
    }
    msg = g_strdup_printf(ngettext("已向 known_hosts 追加 %zu 条（备份 .harbor-bak）", "已向 known_hosts 追加 %zu 条（备份 .harbor-bak）",
                          added), added);
    hb_toast(GTK_WIDGET(v), msg);
    g_free(msg);

out:
    g_string_free(add, TRUE);
    g_hash_table_unref(have);
    g_free(cur);
    g_free(path);
    g_free(link_path);
}

static void write_known_done(gboolean ok, gpointer ud)
{
    if (ok) do_write_known((HbKnownView *)ud);
}

static void on_write_known(GtkButton *b, gpointer ud)
{
    HbKnownView *v = ud;
    (void)b;
    hb_dialog_confirm(GTK_WIDGET(v), _("写入 ~/.ssh/known_hosts"),
                      _("将把 Vault 中尚未出现的指纹追加到 "
                      "<tt>~/.ssh/known_hosts</tt>；"
                      "原文件先备份为 <tt>known_hosts.harbor-bak</tt>。"),
                      _("写入"), FALSE, write_known_done, v);
}

/* ============================ 删除 ============================ */

typedef struct {
    HbKnownView *v;
    char *id;
    char *host;
} DelKnown;

static void known_undo_cb(gpointer ud)
{
    DelKnown *d = ud;
    hb_vault_undelete(hb_store_vault(d->v->store), d->id);
}
static void known_undo_free(gpointer ud)
{
    DelKnown *d = ud;
    g_free(d->id); g_free(d->host); g_free(d);
}

static void known_del_done(gboolean ok, gpointer ud)
{
    DelKnown *d = ud;
    char *msg;
    if (!ok) { known_undo_free(d); return; }
    hb_vault_delete(hb_store_vault(d->v->store), d->id);
    msg = g_strdup_printf(_("已删除 %s 的指纹记录"), d->host ? d->host : "");
    hb_toast_undo_destroy(GTK_WIDGET(d->v), msg,
                          known_undo_cb, known_undo_free, d);
    g_free(msg);
}

static void on_del(GtkButton *b, gpointer ud)
{
    HbKnownView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "kid");
    const char *host = g_object_get_data(G_OBJECT(b), "khost");
    DelKnown *d;
    char *msg;
    (void)b;

    d = g_new0(DelKnown, 1);
    d->v = v;
    d->id = g_strdup(id);
    d->host = g_strdup(host);
    msg = g_strdup_printf(_("确定删除 <b>%s</b> 的指纹记录？"
                          "删除后 9 秒内可撤销。"),
                          host ? host : "");
    hb_dialog_confirm(GTK_WIDGET(v), _("删除 known hosts 条目"), msg,
                      _("删除"), TRUE, known_del_done, d);
    g_free(msg);
}

/* ============================ 表格 ============================ */

static GtkWidget *row_head(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    struct { const char *txt; int w; gboolean grow; } cols[] = {
        { _("主机"),    200, TRUE  },
        { _("端口"),     60, FALSE },
        { _("密钥类型"), 110, FALSE },
        { _("指纹"),    200, TRUE  },
        { _("添加时间"), 110, FALSE },
        { "",         50, FALSE },
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

/* hashed 条目 → "（hashed）|1|Ab3d…" */
static char *host_disp(const char *host)
{
    if (host && !strncmp(host, "|1|", 3)) {
        char *pre = g_strndup(host, 12);
        char *out = g_strdup_printf(_("（hashed）%s…"), pre);
        g_free(pre);
        return out;
    }
    return g_strdup(host ? host : "");
}

static GtkWidget *known_row(HbKnownView *v, hb_obj *o)
{
    const char *host = hb_obj_get_str(o, "host");
    const char *kt   = hb_obj_get_str(o, "keytype");
    const char *fp   = hb_obj_get_str(o, "fingerprint");
    int64_t added    = hb_obj_get_int(o, "added", 0);
    int port         = (int)hb_obj_get_int(o, "port", 22);
    GtkWidget *row, *hl, *pl, *kl, *fl, *al, *db;
    char *hd, pbuf[16], abuf[24];

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "hb-table-row");

    /* 主机 */
    hd = host_disp(host);
    hl = gtk_label_new(hd);
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(hl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(hl, host ? host : "");
    gtk_widget_set_margin_start(hl, 14);
    gtk_widget_set_size_request(hl, 200, -1);
    gtk_widget_set_hexpand(hl, TRUE);
    gtk_box_append(GTK_BOX(row), hl);
    g_free(hd);

    /* 端口 */
    if (port > 0) snprintf(pbuf, sizeof pbuf, "%d", port);
    else          snprintf(pbuf, sizeof pbuf, "—");
    pl = gtk_label_new(pbuf);
    gtk_label_set_xalign(GTK_LABEL(pl), 0);
    gtk_widget_set_margin_start(pl, 14);
    gtk_widget_set_size_request(pl, 60, -1);
    gtk_box_append(GTK_BOX(row), pl);

    /* 密钥类型 */
    kl = gtk_label_new(kt ? kt : "");
    gtk_widget_add_css_class(kl, "mono");
    gtk_label_set_xalign(GTK_LABEL(kl), 0);
    gtk_widget_set_margin_start(kl, 14);
    gtk_widget_set_size_request(kl, 110, -1);
    gtk_box_append(GTK_BOX(row), kl);

    /* 指纹 */
    fl = gtk_label_new(fp ? fp : "");
    gtk_widget_add_css_class(fl, "mono");
    gtk_label_set_xalign(GTK_LABEL(fl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(fl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_tooltip_text(fl, fp ? fp : "");
    gtk_widget_set_margin_start(fl, 14);
    gtk_widget_set_size_request(fl, 200, -1);
    gtk_widget_set_hexpand(fl, TRUE);
    gtk_box_append(GTK_BOX(row), fl);

    /* 添加时间 */
    if (added > 0) {
        time_t t = (time_t)(added / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(abuf, sizeof abuf, "%Y-%m-%d", &tm);
    } else {
        abuf[0] = 0;
    }
    al = gtk_label_new(abuf);
    gtk_label_set_xalign(GTK_LABEL(al), 0);
    gtk_widget_set_margin_start(al, 14);
    gtk_widget_set_size_request(al, 110, -1);
    gtk_box_append(GTK_BOX(row), al);

    /* 删除 */
    db = gtk_button_new();
    gtk_widget_add_css_class(db, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(db), hb_icon_new("trash", 13));
    gtk_widget_set_tooltip_text(db, _("删除"));
    gtk_widget_set_margin_start(db, 14);
    gtk_widget_set_size_request(db, 50, -1);
    gtk_widget_set_valign(db, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(db), "kid",
                           g_strdup(hb_obj_id(o)), g_free);
    g_object_set_data_full(G_OBJECT(db), "khost",
                           g_strdup(host ? host : ""), g_free);
    g_signal_connect(db, "clicked", G_CALLBACK(on_del), v);
    gtk_box_append(GTK_BOX(row), db);

    return row;
}

static void fill_rows(HbKnownView *v)
{
    GtkWidget *c;
    hb_obj **objs;
    size_t n = 0, i, shown = 0;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search));

    while ((c = gtk_widget_get_first_child(v->rows)))
        gtk_box_remove(GTK_BOX(v->rows), c);

    objs = hb_store_list(v->store, "known", &n);
    for (i = 0; objs && i < n; i++) {
        hb_obj *o = objs[i];
        const char *host = hb_obj_get_str(o, "host");
        const char *kt   = hb_obj_get_str(o, "keytype");
        const char *fp   = hb_obj_get_str(o, "fingerprint");
        if (q && *q) {
            char *hay = g_strdup_printf("%s %s %s",
                host ? host : "", kt ? kt : "", fp ? fp : "");
            char *hay_l = g_utf8_strdown(hay, -1);
            char *q_l = g_utf8_strdown(q, -1);
            gboolean hit = strstr(hay_l, q_l) != NULL;
            g_free(hay); g_free(hay_l); g_free(q_l);
            if (!hit) continue;
        }
        gtk_box_append(GTK_BOX(v->rows), known_row(v, o));
        shown++;
    }
    if (objs) hb_obj_list_free(objs, n);
    if (!shown) {
        GtkWidget *l = gtk_label_new(q && *q ?
            _("无匹配条目") : _("暂无 known hosts — 导入或首次连接时记录"));
        gtk_widget_add_css_class(l, "hb-e-sub");
        gtk_widget_set_margin_top(l, 26);
        gtk_widget_set_margin_bottom(l, 26);
        gtk_box_append(GTK_BOX(v->rows), l);
    }
}

static void on_search(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    fill_rows((HbKnownView *)ud);
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbKnownView *v = ud;
    (void)st; (void)id;
    if (!v->dead && !strcmp(type, "known"))
        fill_rows(v);
}

/* ============================ 构建 ============================ */

static void build(HbKnownView *v)
{
    GtkWidget *bar, *split, *ib, *mb, *wb, *eb, *sw, *sc;

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    /* Import split ▾：主按钮 = 默认 ~/.ssh/known_hosts */
    split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(split, "linked");
    ib = gtk_button_new_with_label(_("导入 known_hosts"));
    gtk_widget_add_css_class(ib, "hb-btn");
    gtk_widget_add_css_class(ib, "primary");
    gtk_widget_set_tooltip_text(ib, _("默认 ~/.ssh/known_hosts"));
    g_signal_connect(ib, "clicked", G_CALLBACK(on_import_default), v);
    gtk_box_append(GTK_BOX(split), ib);
    mb = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "▾");
    gtk_widget_add_css_class(mb, "hb-btn");
    gtk_widget_add_css_class(mb, "primary");
    {
        GMenu *m = g_menu_new();
        g_menu_append(m, _("选择文件…"), "known.pick");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb),
                                       G_MENU_MODEL(m));
        g_object_unref(m);
        {
            GSimpleActionGroup *ag = g_simple_action_group_new();
            GSimpleAction *a = g_simple_action_new("pick", NULL);
            g_signal_connect_swapped(a, "activate",
                                     G_CALLBACK(on_import_file), v);
            g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(a));
            gtk_widget_insert_action_group(GTK_WIDGET(v), "known",
                                           G_ACTION_GROUP(ag));
            g_object_unref(ag);
        }
    }
    gtk_box_append(GTK_BOX(split), mb);
    gtk_box_append(GTK_BOX(bar), split);

    /* 搜索 */
    v->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
                                          _("搜索主机 / 指纹…"));
    gtk_widget_set_size_request(v->search, 240, -1);
    gtk_widget_set_valign(v->search, GTK_ALIGN_CENTER);
    g_signal_connect(v->search, "search-changed",
                     G_CALLBACK(on_search), v);
    gtk_box_append(GTK_BOX(bar), v->search);

    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(bar), sp);
    }

    /* 写入 ~/.ssh/known_hosts… */
    wb = gtk_button_new_with_label(_("写入 ~/.ssh/known_hosts…"));
    gtk_widget_add_css_class(wb, "hb-btn");
    gtk_widget_add_css_class(wb, "ghost");
    g_signal_connect(wb, "clicked", G_CALLBACK(on_write_known), v);
    gtk_box_append(GTK_BOX(bar), wb);

    /* 导出 */
    eb = gtk_button_new_with_label(_("导出"));
    gtk_widget_add_css_class(eb, "hb-btn");
    gtk_widget_add_css_class(eb, "ghost");
    g_signal_connect(eb, "clicked", G_CALLBACK(on_export), v);
    gtk_box_append(GTK_BOX(bar), eb);

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

    v->table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(v->table, "hb-table");
    gtk_box_append(GTK_BOX(v->table), row_head());
    v->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(v->table), v->rows);
    gtk_box_append(GTK_BOX(sc), v->table);

    /* hint box（prototype hintbox） */
    {
        GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *txt;
        gtk_widget_add_css_class(hb2, "hintbox");
        gtk_widget_add_css_class(hb2, "plain");
        gtk_box_append(GTK_BOX(hb2), hb_icon_new("info", 14));
        txt = gtk_label_new(_("首次连接未知主机时会弹出指纹确认（TOFU）；"
                            "指纹变更会红色告警并阻断连接，防中间人攻击。"));
        gtk_label_set_wrap(GTK_LABEL(txt), TRUE);
        gtk_label_set_xalign(GTK_LABEL(txt), 0);
        gtk_box_append(GTK_BOX(hb2), txt);
        gtk_widget_set_margin_top(hb2, 12);
        gtk_box_append(GTK_BOX(sc), hb2);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), sc);
    gtk_box_append(GTK_BOX(v), sw);
}

static void hb_known_view_init(HbKnownView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
}

static void hb_known_view_dispose(GObject *o)
{
    HbKnownView *v = HB_KNOWN_VIEW(o);
    v->dead = TRUE;
    if (v->changed_h)
        g_signal_handler_disconnect(v->store, v->changed_h);
    v->changed_h = 0;
    G_OBJECT_CLASS(hb_known_view_parent_class)->dispose(o);
}

static void hb_known_view_class_init(HbKnownViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_known_view_dispose;
}

GtkWidget *hb_known_view_new(HbStore *store)
{
    HbKnownView *v = g_object_new(HB_TYPE_KNOWN_VIEW, NULL);
    v->store = store;
    build(v);
    fill_rows(v);
    v->changed_h = g_signal_connect(v->store, "changed",
                                    G_CALLBACK(on_store_changed), v);
    return GTK_WIDGET(v);
}
