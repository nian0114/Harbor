/* hb-palette — 命令面板（FR-20）：窗口顶部浮层，
 * 搜索 hosts / Vault sections / snippets / commands，Enter 执行。
 * 结构照 prototype/app.js palItems()/palette()/renderPalList()。
 * ctx/items 生命周期：挂为 scrim 的 qdata（data_full），
 * pal_close → unparent → scrim finalize → pal_ctx_free。 */
#include "hb-palette.h"
#include "hb-window.h"
#include "hb-icons.h"
#include "hb-store.h"
#include "hb-host-actions.h"
#include "hb-vault-page.h"
#include "hb-term-page.h"
#include "hb-ssh-page.h"
#include "hb-workspace.h"
#include "hb-session-page.h"
#include "hb-snippet-send.h"

#include <harbor/session.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */

enum { PAL_HOST, PAL_SECTION, PAL_SNIPPET, PAL_CMD };

typedef struct {
    int   kind;
    int   score;          /* 0 prefix-title, 1 infix-title, 2 infix-kw, 3 fuzzy */
    int   ord;            /* 组内原始顺序 —— 排序稳定键 */
    char *icon;           /* 非 host 项的图标名 */
    char *avatar;         /* host 项的 os/proto key */
    char *title;
    char *sub;
    char *kw;
    char *id;             /* host id / section id / snippet id / cmd key */
} PalItem;

typedef struct {
    HbWindow  *win;
    HbStore   *store;
    GtkWidget *root;      /* scrim（覆盖整窗），NULL = 已关 */
    GtkWidget *entry;
    GtkWidget *list;      /* GtkListBox */
    GPtrArray *items;     /* PalItem*，当前过滤结果 ≤9 */
    int        sel;
    GtkWidget *prev_focus;
} PalCtx;

static void pal_close(PalCtx *c);

/* ------------------------- items 构造 ------------------------- */

static void item_free(gpointer p)
{
    PalItem *it = p;
    if (!it) return;               /* apply_filter 里被偷走的槽位是 NULL */
    g_free(it->icon);  g_free(it->avatar);
    g_free(it->title); g_free(it->sub);
    g_free(it->kw);    g_free(it->id);
    g_free(it);
}

static PalItem *item_new(int kind, const char *icon, const char *avatar,
                         const char *title, const char *sub,
                         const char *kw, const char *id, int ord)
{
    PalItem *it = g_new0(PalItem, 1);
    it->kind   = kind;
    it->icon   = g_strdup(icon);
    it->avatar = g_strdup(avatar);
    it->title  = g_strdup(title);
    it->sub    = g_strdup(sub);
    it->kw     = g_strdup(kw);
    it->id     = g_strdup(id);
    it->ord    = ord;
    return it;
}

/* host 的 avatar key（与 hb-hosts-view host_avatar_key 同规则） */
static const char *host_avatar(const hb_obj *h)
{
    const char *proto = hb_obj_get_str(h, "proto");
    const char *os    = hb_obj_get_str(h, "os");
    if (proto && (!strcmp(proto, "local") || !strcmp(proto, "rdp") ||
                  !strcmp(proto, "vnc")))
        return proto;
    if (os && *os && strcmp(os, "auto"))
        return os;
    return "generic";
}

/* Vault sections —— 与 hb-vault-page.c SECTIONS 保持一致 */
static const struct { const char *id, *label, *icon; } PAL_SECTIONS[] = {
    { "hosts",       "Hosts",          "grid"      },
    { "keys",        "Keychain",       "key"       },
    { "fwd",         "Port Forwarding","fwd"       },
    { "snippets",    "Snippets",       "snippet"   },
    { "known_hosts", "Known Hosts",    "shield"    },
    { "logs",        "Logs",           "clock"     },
    { "share",       "共享会话",        "broadcast" },
    { "appearance",  "外观与终端",      "palette"   },
    { "account",     "账号与同步",      "cloud"     },
};

static char *str_fold(const char *s)
{
    return s ? g_utf8_strdown(s, -1) : g_strdup("");
}

/* 子序列（fuzzy）匹配：q 的字符是否按序散落于 s（gunichar 粒度） */
static gboolean is_subseq(const char *q, const char *s)
{
    while (*q) {
        gunichar qc = g_utf8_get_char(q);
        s = g_utf8_strchr(s, -1, qc);
        if (!s) return FALSE;
        s = g_utf8_next_char(s);
        q = g_utf8_next_char(q);
    }
    return TRUE;
}

/* 0=title 前缀, 1=title 子串, 2=kw 子串, 3=fuzzy; -1 不匹配。
 * q/title/kw 均已小写化；UTF-8 自同步，strstr 安全。 */
static int score_item(const char *q, const char *title, const char *kw)
{
    char *hay;
    int r;
    if (!*q) return 0;
    if (g_str_has_prefix(title, q)) return 0;
    if (strstr(title, q)) return 1;
    if (strstr(kw, q))    return 2;
    hay = g_strconcat(title, " ", kw, NULL);
    r = is_subseq(q, hay) ? 3 : -1;
    g_free(hay);
    return r;
}

static void collect_items(PalCtx *c, GPtrArray *out)
{
    hb_obj **objs;
    size_t n = 0, i;
    int ord = 0;

    /* 1. hosts */
    objs = hb_store_list(c->store, "host", &n);
    for (i = 0; objs && i < n; i++) {
        hb_obj *h = objs[i];
        const char *id    = hb_obj_id(h);
        const char *name  = hb_obj_get_str(h, "name");
        const char *addr  = hb_obj_get_str(h, "address");
        const char *user  = hb_obj_get_str(h, "user");
        const char *proto = hb_obj_get_str(h, "proto");
        const char *group = hb_obj_get_str(h, "group");
        const char *tags  = hb_obj_get_str(h, "tags");
        char *sub, *kw, *pup;
        pup = g_ascii_strup(proto && *proto ? proto : "ssh", -1);
        sub = g_strdup_printf("连接 · %s%s%s", pup,
                              user && *user ? " " : "",
                              user && *user ? user : "");
        kw  = g_strdup_printf("%s %s %s %s %s",
                              name ? name : "", addr ? addr : "",
                              user ? user : "", group ? group : "",
                              tags ? tags : "");
        g_ptr_array_add(out, item_new(PAL_HOST, NULL, host_avatar(h),
            name && *name ? name : (addr ? addr : "host"),
            sub, kw, id, ord++));
        g_free(pup); g_free(sub); g_free(kw);
    }
    if (objs) hb_obj_list_free(objs, n);

    /* 2. sections */
    for (i = 0; i < G_N_ELEMENTS(PAL_SECTIONS); i++) {
        char *kw = g_strdup_printf("%s %s", PAL_SECTIONS[i].label,
                                   PAL_SECTIONS[i].id);
        g_ptr_array_add(out, item_new(PAL_SECTION,
            PAL_SECTIONS[i].icon, NULL, PAL_SECTIONS[i].label,
            "Vault 页面", kw, PAL_SECTIONS[i].id, ord++));
        g_free(kw);
    }

    /* 3. snippets（scope 匹配焦点会话 host：global / host:<id> / group:<g>） */
    {
        const char *fhost = NULL, *fgroup = NULL;
        GtkWidget *pg = hb_window_active_session_page(c->win);
        hb_obj *fh = NULL;
        static char fgrp[128];
        fgrp[0] = 0;
        if (pg && HB_IS_SESSION_PAGE(pg))
            fhost = hb_session_page_host_id(HB_SESSION_PAGE(pg));
        if (fhost &&
            hb_vault_get(hb_store_vault(c->store), fhost,
                         &fh) == HB_OK && fh) {
            const char *g = hb_obj_get_str(fh, "group");
            if (g && *g) {
                snprintf(fgrp, sizeof fgrp, "%s", g);
                fgroup = fgrp;
            }
            hb_obj_free(fh);
        }
        objs = hb_store_list(c->store, "snippet", &n);
        for (i = 0; objs && i < n; i++) {
            hb_obj *s = objs[i];
            const char *id    = hb_obj_id(s);
            const char *title = hb_obj_get_str(s, "title");
            const char *body  = hb_obj_get_str(s, "body");
            const char *scope = hb_obj_get_str(s, "scope");
            char *kw;
            if (!hb_snippet_scope_match(scope, fhost, fgroup))
                continue;
            kw = g_strdup_printf("%s %s", title ? title : "",
                                 body ? body : "");
            g_ptr_array_add(out, item_new(PAL_SNIPPET, "snippet", NULL,
                title && *title ? title : "snippet",
                "发送 Snippet", kw, id, ord++));
            g_free(kw);
        }
        if (objs) hb_obj_list_free(objs, n);
    }

    /* 4. commands */
    g_ptr_array_add(out, item_new(PAL_CMD, "terminal", NULL,
        "Local 终端", "命令", "Local 终端 local terminal",
        "local", ord++));
    g_ptr_array_add(out, item_new(PAL_CMD, "clock", NULL,
        "导出连接日志 CSV", "命令", "导出连接日志 CSV export logs csv",
        "export-logs", ord++));
    g_ptr_array_add(out, item_new(PAL_CMD, "palette", NULL,
        "外观与终端", "命令", "外观与终端 appearance settings theme",
        "appearance", ord++));
    g_ptr_array_add(out, item_new(PAL_CMD, "cols", NULL,
        "Workspace 分屏", "命令", "Workspace 分屏 workspace split panes",
        "workspace", ord++));
}

/* ------------------------- 过滤 + 排序 ------------------------- */

static int item_cmp(gconstpointer pa, gconstpointer pb)
{
    const PalItem *a = *(PalItem * const *)pa;
    const PalItem *b = *(PalItem * const *)pb;
    if (a->score != b->score) return a->score - b->score;
    return a->ord - b->ord;
}

static void apply_filter(PalCtx *c)
{
    const char *raw = gtk_editable_get_text(GTK_EDITABLE(c->entry));
    GPtrArray *all = g_ptr_array_new_with_free_func(item_free);
    char *q;
    guint i;

    g_ptr_array_set_size(c->items, 0);   /* 释放旧结果 */
    collect_items(c, all);

    q = str_fold(raw);
    g_strstrip(q);
    for (i = 0; i < all->len; i++) {
        PalItem *it = g_ptr_array_index(all, i);
        char *title = str_fold(it->title);
        char *kw    = str_fold(it->kw);
        it->score = score_item(q, title, kw);
        g_free(title); g_free(kw);
        if (it->score >= 0) {
            g_ptr_array_add(c->items, it);
            g_ptr_array_index(all, i) = NULL;   /* 所有权转移 */
        }
    }
    g_ptr_array_sort(c->items, item_cmp);
    while (c->items->len > 9)
        g_ptr_array_remove_index(c->items, c->items->len - 1);
    g_ptr_array_unref(all);
    g_free(q);

    if (c->sel >= (int)c->items->len)
        c->sel = c->items->len ? (int)c->items->len - 1 : 0;
    if (c->sel < 0)
        c->sel = 0;
}

/* ------------------------- 渲染 ------------------------- */

static void on_row_enter(GtkEventControllerMotion *m, double x, double y,
                         gpointer ud);
/* 前向声明：row_widget 在 render_list 前定义 */

static GtkWidget *row_widget(const PalItem *it, gboolean on)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 11);
    GtkWidget *ico, *badge;
    gtk_widget_add_css_class(row, "pal-item");
    if (on) gtk_widget_add_css_class(row, "on");

    if (it->kind == PAL_HOST)
        ico = hb_os_avatar_new(it->avatar, 22);
    else {
        ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(ico, "pi-ico");
        gtk_box_append(GTK_BOX(ico), hb_icon_new(it->icon, 15));
    }
    gtk_box_append(GTK_BOX(row), ico);

    {
        GtkWidget *t = gtk_label_new(it->title);
        gtk_widget_add_css_class(t, "pi-title");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(t), 26);
        gtk_box_append(GTK_BOX(row), t);
    }
    {
        GtkWidget *s = gtk_label_new(it->sub);
        gtk_widget_add_css_class(s, "pi-sub");
        gtk_widget_set_hexpand(s, TRUE);
        gtk_label_set_xalign(GTK_LABEL(s), 1);
        gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(row), s);
    }
    badge = gtk_label_new("⏎");
    gtk_widget_add_css_class(badge, "kbd");
    gtk_widget_set_visible(badge, on);
    gtk_box_append(GTK_BOX(row), badge);
    return row;
}

static void render_list(PalCtx *c)
{
    GtkWidget *ch;
    guint i;

    while ((ch = gtk_widget_get_first_child(c->list)))
        gtk_list_box_remove(GTK_LIST_BOX(c->list), ch);

    if (!c->items->len) {
        GtkWidget *lr = gtk_list_box_row_new();
        GtkWidget *l = gtk_label_new("无匹配结果 — 试试主机名或「日志」");
        gtk_widget_add_css_class(l, "pal-empty");
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lr), FALSE);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(lr), FALSE);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lr), l);
        gtk_list_box_append(GTK_LIST_BOX(c->list), lr);
        return;
    }
    for (i = 0; i < c->items->len; i++) {
        GtkListBoxRow *lr = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
        GtkEventController *mo = gtk_event_controller_motion_new();
        gtk_list_box_row_set_child(lr,
            row_widget(g_ptr_array_index(c->items, i), (int)i == c->sel));
        g_signal_connect(mo, "enter", G_CALLBACK(on_row_enter), c);
        gtk_widget_add_controller(GTK_WIDGET(lr), mo);
        gtk_list_box_append(GTK_LIST_BOX(c->list), GTK_WIDGET(lr));
    }
    {
        GtkListBoxRow *sel =
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(c->list), c->sel);
        if (sel) gtk_list_box_select_row(GTK_LIST_BOX(c->list), sel);
    }
}

static void on_row_enter(GtkEventControllerMotion *m, double x, double y,
                         gpointer ud)
{
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(m));
    PalCtx *c = ud;
    int idx;
    (void)x; (void)y;
    if (!w || !c->root) return;
    idx = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(w));
    if (idx >= 0 && idx < (int)c->items->len && idx != c->sel) {
        c->sel = idx;
        render_list(c);
    }
}

/* ------------------------- 执行 ------------------------- */

static void csv_save_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbWindow *win = HB_WINDOW(ud);
    HbStore *store = hb_store_default();
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *csv = NULL;
    GError *err = NULL;
    if (!gf) return;
    if (hb_vault_log_export_csv(hb_store_vault(store), &csv) != HB_OK ||
        !csv ||
        !g_file_replace_contents(gf, csv, strlen(csv), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        hb_window_toast(win, "导出失败");
        if (err) g_error_free(err);
    } else {
        hb_window_toast(win, "连接日志已导出");
    }
    free(csv);
    g_object_unref(gf);
}

static void run_export_logs(HbWindow *win)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    GtkFileFilter *ff = gtk_file_filter_new();
    GListStore *fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    gtk_file_dialog_set_title(d, "导出连接日志 CSV");
    gtk_file_dialog_set_initial_name(d, "harbor-logs.csv");
    gtk_file_filter_add_suffix(ff, "csv");
    gtk_file_filter_set_name(ff, "CSV 文件");
    g_list_store_append(fs, ff);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fs));
    gtk_file_dialog_save(d, GTK_WINDOW(win), NULL, csv_save_done, win);
    g_object_unref(ff);
    g_object_unref(fs);
    g_object_unref(d);
}

/* pal_close 会释放 c 与全部 items —— 需要的东西先拷到局部 */
static void run_item(PalCtx *c, PalItem *it)
{
    HbWindow *win = c->win;
    HbStore  *st  = c->store;
    int   kind    = it->kind;
    char *id      = g_strdup(it->id);

    if (kind == PAL_HOST) {
        pal_close(c);
        hb_host_action_connect(GTK_WIDGET(win), st, id);
    } else if (kind == PAL_SECTION) {
        GtkWidget *vp = hb_window_vault_page(win);
        pal_close(c);
        hb_window_switch_tab(win, "vault");
        if (vp)
            hb_vault_page_set_section(HB_VAULT_PAGE(vp), id);
    } else if (kind == PAL_SNIPPET) {
        pal_close(c);
        /* 共享发送流：变量对话框 / run / broadcast 确认 / 无终端 toast */
        hb_snippet_send(GTK_WIDGET(win), st, id);
    } else if (kind == PAL_CMD) {
        if (!strcmp(id, "local")) {
            pal_close(c);
            hb_window_open_local(win, NULL);
        } else if (!strcmp(id, "export-logs")) {
            pal_close(c);
            run_export_logs(win);
        } else if (!strcmp(id, "appearance")) {
            GtkWidget *vp = hb_window_vault_page(win);
            pal_close(c);
            hb_window_switch_tab(win, "vault");
            if (vp)
                hb_vault_page_set_section(HB_VAULT_PAGE(vp), "appearance");
        } else if (!strcmp(id, "workspace")) {
            pal_close(c);
            hb_workspace_pick_targets(GTK_WIDGET(win),
                                      hb_store_default());
        }
    }
    g_free(id);
}

/* ------------------------- 事件 ------------------------- */

static gboolean on_key(GtkEventControllerKey *kc, guint keyval,
                       guint code, GdkModifierType state, gpointer ud)
{
    PalCtx *c = ud;
    (void)kc; (void)code;
    if (!c->root) return FALSE;

    if (keyval == GDK_KEY_Escape) {
        pal_close(c);
        return TRUE;
    }
    if (keyval == GDK_KEY_Down ||
        (keyval == GDK_KEY_n && (state & GDK_CONTROL_MASK))) {
        if (c->items->len)
            c->sel = (c->sel + 1) % (int)c->items->len;
        render_list(c);
        return TRUE;
    }
    if (keyval == GDK_KEY_Up ||
        (keyval == GDK_KEY_p && (state & GDK_CONTROL_MASK))) {
        if (c->items->len)
            c->sel = (c->sel - 1 + (int)c->items->len) % (int)c->items->len;
        render_list(c);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (c->sel < (int)c->items->len)
            run_item(c, g_ptr_array_index(c->items, c->sel));
        return TRUE;
    }
    return FALSE;
}

static void on_changed(GtkEditable *e, gpointer ud)
{
    PalCtx *c = ud;
    (void)e;
    c->sel = 0;
    apply_filter(c);
    render_list(c);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    PalCtx *c = ud;
    int idx = gtk_list_box_row_get_index(row);
    (void)lb;
    if (c->root && idx >= 0 && idx < (int)c->items->len)
        run_item(c, g_ptr_array_index(c->items, idx));
}

static void on_scrim_click(GtkGestureClick *g, int n, double x, double y,
                           gpointer ud)
{
    PalCtx *c = ud;
    (void)g; (void)n; (void)x; (void)y;
    pal_close(c);
}

/* ------------------------- 生命周期 ------------------------- */

static void pal_ctx_free(gpointer p)
{
    PalCtx *c = p;
    if (c->items) g_ptr_array_unref(c->items);
    g_free(c);
}

/* scrim finalize → 清掉 win 上的"已开"标记（win 此刻必仍存活：
 * scrim 是它的子级，先走一步） */
static void scrim_dead(gpointer data, GObject *dead)
{
    (void)dead;
    g_object_set_data(G_OBJECT(data), "hb-palette", NULL);
}

/* unparent 挪到 idle：按键派发途中直接拆 widget 会触发
 * GTK "broken accounting of active state" 警告 */
static gboolean close_idle(gpointer p)
{
    GtkWidget *root = p;
    PalCtx    *c    = g_object_get_data(G_OBJECT(root), "pal-ctx");
    GtkWidget *prev = c ? c->prev_focus : NULL;
    HbWindow  *win  = c ? c->win : NULL;
    gtk_widget_unparent(root);   /* scrim finalize → 释放 c + items */
    if (prev && win &&
        GTK_WIDGET(gtk_widget_get_root(prev)) == GTK_WIDGET(win))
        gtk_widget_grab_focus(prev);
    g_object_unref(root);
    return G_SOURCE_REMOVE;
}

static void pal_close(PalCtx *c)
{
    GtkWidget *root = c->root;
    if (!root) return;
    c->root = NULL;            /* 防 reenter */
    g_idle_add(close_idle, g_object_ref(root));
}

/* ------------------------- open ------------------------- */

void hb_palette_open(HbWindow *win)
{
    PalCtx *c;
    GtkWidget *scrim, *box, *input_row, *scroll, *esc;
    GtkEventController *key;
    GtkGesture *click, *eat;

    {
        GtkWidget *existing = g_object_get_data(G_OBJECT(win), "hb-palette");
        if (existing) {          /* 已开：聚焦输入框 */
            GtkWidget *e = g_object_get_data(G_OBJECT(existing), "pal-entry");
            if (e) gtk_widget_grab_focus(e);
            return;
        }
    }

    c = g_new0(PalCtx, 1);
    c->win = win;
    c->store = hb_store_default();
    c->items = g_ptr_array_new_with_free_func(item_free);
    c->sel = 0;
    c->prev_focus = gtk_window_get_focus(GTK_WINDOW(win));

    scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(scrim, "pal-scrim");
    gtk_widget_set_hexpand(scrim, TRUE);   /* overlay 子件要 expand 才铺满整窗 */
    gtk_widget_set_vexpand(scrim, TRUE);
    g_object_set_data_full(G_OBJECT(scrim), "pal-ctx", c,
                           pal_ctx_free);
    g_object_set_data(G_OBJECT(win), "hb-palette", scrim);
    g_object_weak_ref(G_OBJECT(scrim), scrim_dead, win);

    /* 点击 scrim 空白关闭；box 内的点击被 eat 手势吞掉不外传 */
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_scrim_click), c);
    gtk_widget_add_controller(scrim, GTK_EVENT_CONTROLLER(click));

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "pal-box");
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_START);
    gtk_widget_set_margin_top(box, 90);
    gtk_widget_set_size_request(box, 560, -1);
    eat = gtk_gesture_click_new();
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(eat));
    gtk_box_append(GTK_BOX(scrim), box);

    input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(input_row, "pal-input");
    gtk_box_append(GTK_BOX(input_row), hb_icon_new("search", 15));
    c->entry = gtk_entry_new();
    gtk_widget_add_css_class(c->entry, "pal-entry");
    gtk_widget_set_hexpand(c->entry, TRUE);
    g_object_set(c->entry, "placeholder-text",
        "搜索主机、页面、Snippet 或命令…", NULL);
    g_signal_connect(c->entry, "changed", G_CALLBACK(on_changed), c);
    gtk_box_append(GTK_BOX(input_row), c->entry);
    g_object_set_data(G_OBJECT(scrim), "pal-entry", c->entry);
    esc = gtk_label_new("esc");
    gtk_widget_add_css_class(esc, "kbd");
    gtk_box_append(GTK_BOX(input_row), esc);
    gtk_box_append(GTK_BOX(box), input_row);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(scroll), 340);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);
    c->list = gtk_list_box_new();
    gtk_widget_add_css_class(c->list, "pal-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(c->list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(c->list, "row-activated",
                     G_CALLBACK(on_row_activated), c);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), c->list);
    gtk_box_append(GTK_BOX(box), scroll);

    /* 键盘：capture 挂在 box 上 —— entry 会吃掉 Return（emit activate），
     * 等 bubble 就晚了，下行途中先截 */
    key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), c);
    gtk_widget_add_controller(box, key);

    hb_window_overlay_add(win, scrim);
    c->root = scrim;

    apply_filter(c);
    render_list(c);
    gtk_widget_grab_focus(c->entry);
}
