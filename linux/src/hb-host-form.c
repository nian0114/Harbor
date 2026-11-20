#include "hb-host-form.h"
#include "hb-keychain-view.h"
#include "hb-dialogs.h"
#include "hb-window.h"

#include <adwaita.h>
#include <harbor/harbor_core.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

/* 颜色标签 palette（与 data-model color 字段值一致：存色彩名） */
static const struct { const char *name, *css, *hex; } kColors[] = {
    { "red",    "c-red",    "#e5484d" },
    { "orange", "c-orange", "#f0a13a" },
    { "green",  "c-green",  "#22a06b" },
    { "blue",   "c-blue",   "#2f7cf6" },
    { "purple", "c-purple", "#8b5cf6" },
    { "pink",   "c-pink",   "#ec4899" },
};
#define N_COLORS (sizeof(kColors) / sizeof(kColors[0]))

static const char *kProtos[]  = { "ssh", "rdp", "vnc" };
static const char *kProtoLbl[] = { "SSH", "RDP", "VNC" };
static const char *kAuths[]   = { "auto", "password", "key", "agent", "none" };
static const char *kAuthLbl[] = { "自动（agent → 密钥 → 密码）",
                                  "密码", "密钥", "Agent", "无" };
static const char *kOses[]    = { "auto", "ubuntu", "debian", "generic", "windows" };
static const char *kOsLbl[]   = { "自动检测", "Ubuntu", "Debian", "通用", "Windows" };

typedef struct {
    HbStore  *st;
    hb_vault *v;
    char     *host_id;       /* NULL = 新建 */
    hb_obj   *obj;           /* 编辑时加载的对象（关闭时 free） */
    gboolean  is_local;      /* 编辑 local 主机 */
    AdwDialog *dlg;
    GtkWidget *body;
    char      proto[8];      /* 当前类型 */
    int       prev_proto;    /* port 默认值联动 */

    GtkWidget *type_dd;
    GtkWidget *name_e, *addr_e, *port_sb, *user_e;
    GtkWidget *sec_addr, *sec_auth, *sec_jump, *sec_tunnel, *sec_keep, *sec_os;

    GtkWidget *auth_dd, *pw_box, *key_box;
    GtkWidget *pw_e, *pw_clear;
    gboolean   had_secret, pw_cleared;
    GtkWidget *key_dd;
    GPtrArray *key_ids;      /* 与 key_dd model 对齐（末位 NULL = 生成新密钥） */
    guint      key_prev;     /* 最后一个有效选择（取消生成时回退） */
    gboolean   key_building;
    GtkWidget *parent_w;     /* 打开方 widget（嵌套 dialog 的 present 父级） */
    GtkWidget *ident_e;

    GtkWidget *jump_rows;
    GtkWidget *jump_dd;
    GPtrArray *jump_cand;    /* dropdown model 对应的 host id */
    GPtrArray *jump_sel;     /* 已选链（strdup'd id） */
    gboolean   jump_building;
    guint      jump_idle;    /* deferred add (model swap must not run
                              * inside the dropdown's notify::selected) */
    guint      jump_pending; /* index chosen for the deferred add */

    GtkWidget *tunnel_sw, *via_dd;
    GPtrArray *via_ids;

    GtkWidget *group_dd;
    GPtrArray *group_names;  /* [0]="无" … [N-1]="新建分组…" */
    gboolean   group_building;

    GtkWidget *snip_dd;
    GPtrArray *snip_ids;     /* [0]="无" */

    GtkWidget *keep_sb;
    GtkWidget *color_row;
    char      *color;        /* NULL = 无 */
    GtkWidget *tags_e;
    GtkWidget *os_dd;
    GtkWidget *err_l;
    GtkWidget *save_btn;
} HostForm;

/* ------------------------------- 小工具 ------------------------------- */

static GtkWidget *lbl(const char *t)
{
    GtkWidget *l = gtk_label_new(t);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "hb-form-label");
    return l;
}

static GtkWidget *sec_box(HostForm *f, const char *title, GtkWidget **sec_out)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *t = lbl(title);
    gtk_box_append(GTK_BOX(b), t);
    gtk_box_append(GTK_BOX(f->body), b);
    if (sec_out) *sec_out = b;
    return b;
}

static int dd_idx(GtkDropDown *dd) { return (int)gtk_drop_down_get_selected(dd); }

static const char *local_user(void)
{
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_name ? pw->pw_name : "user";
}

/* ------------------------------ 分组下拉 ------------------------------ */

static void group_rebuild(HostForm *f, const char *select_name)
{
    GtkStringList *sl;
    hb_obj **gs = NULL, **hs = NULL;
    size_t ng = 0, nh = 0, i, j;
    guint sel = 0;

    f->group_building = TRUE;
    g_ptr_array_set_size(f->group_names, 0);
    sl = gtk_string_list_new(NULL);
    gtk_string_list_append(sl, "无");
    g_ptr_array_add(f->group_names, g_strdup(""));

    gs = hb_store_list(f->st, "group", &ng);
    for (i = 0; i < ng; i++) {
        const char *n = hb_obj_get_str(gs[i], "name");
        if (n && *n) {
            gtk_string_list_append(sl, n);
            g_ptr_array_add(f->group_names, g_strdup(n));
        }
    }
    /* host.group 里出现过但无对象的分组名也列出 */
    hs = hb_store_list(f->st, "host", &nh);
    for (i = 0; i < nh; i++) {
        const char *n = hb_obj_get_str(hs[i], "group");
        gboolean known = FALSE;
        if (!n || !*n) continue;
        for (j = 0; j < f->group_names->len; j++)
            if (!strcmp(g_ptr_array_index(f->group_names, j), n))
                { known = TRUE; break; }
        if (!known) {
            gtk_string_list_append(sl, n);
            g_ptr_array_add(f->group_names, g_strdup(n));
        }
    }
    gtk_string_list_append(sl, "新建分组…");
    g_ptr_array_add(f->group_names, g_strdup("\x01new"));

    if (select_name)
        for (i = 0; i < f->group_names->len; i++)
            if (!strcmp(g_ptr_array_index(f->group_names, i), select_name))
                sel = (guint)i;
    gtk_drop_down_set_model(GTK_DROP_DOWN(f->group_dd), G_LIST_MODEL(sl));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->group_dd), sel);
    f->group_building = FALSE;
    if (gs) hb_obj_list_free(gs, ng);
    if (hs) hb_obj_list_free(hs, nh);
}

typedef struct { HostForm *f; } GrpUd;

static void group_prompt_done(const char *text, gpointer ud)
{
    HostForm *f = ud;
    hb_obj *g;
    if (!text || !*text) { group_rebuild(f, NULL); return; }
    g = hb_obj_new("group");
    hb_obj_set_str(g, "name", text);
    hb_obj_set_int(g, "order", (int64_t)g_get_real_time() / 1000000);
    hb_vault_put(f->v, g);
    hb_obj_free(g);
    group_rebuild(f, text);
}

static void on_group_sel(GtkDropDown *dd, GParamSpec *ps, HostForm *f)
{
    guint i;
    const char *val;
    (void)ps;
    if (f->group_building) return;
    i = gtk_drop_down_get_selected(dd);
    if (i >= f->group_names->len) return;
    val = g_ptr_array_index(f->group_names, i);
    if (!strcmp(val, "\x01new"))
        hb_dialog_prompt(GTK_WIDGET(f->dlg), "新建分组", "分组名称", NULL,
                         group_prompt_done, f);
}

/* ------------------------------ 跳板编辑 ------------------------------ */

static void jump_rebuild_add(HostForm *f)
{
    GtkStringList *sl;
    hb_obj **hs = NULL;
    size_t n = 0, i, j;

    f->jump_building = TRUE;
    g_ptr_array_set_size(f->jump_cand, 0);
    sl = gtk_string_list_new(NULL);
    gtk_string_list_append(sl, "+ 添加跳板");
    g_ptr_array_add(f->jump_cand, NULL);
    hs = hb_store_list(f->st, "host", &n);
    for (i = 0; i < n; i++) {
        const char *pr = hb_obj_get_str(hs[i], "proto");
        const char *id = hb_obj_id(hs[i]);
        const char *nm = hb_obj_get_str(hs[i], "name");
        const char *ad = hb_obj_get_str(hs[i], "address");
        gboolean inchain = FALSE;
        if (!pr || strcmp(pr, "ssh")) continue;
        if (f->host_id && !strcmp(id, f->host_id)) continue;
        for (j = 0; j < f->jump_sel->len; j++)
            if (!strcmp(g_ptr_array_index(f->jump_sel, j), id))
                { inchain = TRUE; break; }
        if (inchain) continue;
        {
            const char *u = hb_obj_get_str(hs[i], "user");
            char *lbltxt = g_strdup_printf(
                "%s%s%s", u && *u ? u : "", u && *u ? "@" : "",
                nm && *nm ? nm : (ad ? ad : id));
            gtk_string_list_append(sl, lbltxt);
            g_free(lbltxt);
            g_ptr_array_add(f->jump_cand, g_strdup(id));
        }
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(f->jump_dd), G_LIST_MODEL(sl));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->jump_dd), 0);
    f->jump_building = FALSE;
    if (hs) hb_obj_list_free(hs, n);
}

static void jump_rows_rebuild(HostForm *f);

static void jump_move(HostForm *f, guint idx, int dir)
{
    guint dst = idx + dir;
    gpointer tmp;
    if ((dir < 0 && idx == 0) || dst >= f->jump_sel->len) return;
    tmp = g_ptr_array_index(f->jump_sel, idx);
    g_ptr_array_index(f->jump_sel, idx) =
        g_ptr_array_index(f->jump_sel, dst);
    g_ptr_array_index(f->jump_sel, dst) = tmp;
    jump_rows_rebuild(f);
}

static void jump_del(GtkButton *b, gpointer ud)
{
    HostForm *f = g_object_get_data(G_OBJECT(b), "form");
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx"));
    (void)ud;
    g_ptr_array_remove_index(f->jump_sel, idx);   /* free_func 负责 g_free */
    jump_rows_rebuild(f);
    jump_rebuild_add(f);
}
static void jump_up(GtkButton *b, gpointer ud)
{
    HostForm *f = g_object_get_data(G_OBJECT(b), "form");
    jump_move(f, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx")), -1);
    (void)ud;
}
static void jump_down(GtkButton *b, gpointer ud)
{
    HostForm *f = g_object_get_data(G_OBJECT(b), "form");
    jump_move(f, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx")), +1);
    (void)ud;
}

static GtkWidget *jump_btn(const char *label, GCallback cb, HostForm *f,
                           guint idx)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "hb-btn");
    gtk_widget_add_css_class(b, "hb-jump-btn");
    g_object_set_data(G_OBJECT(b), "form", f);
    g_object_set_data(G_OBJECT(b), "idx", GUINT_TO_POINTER(idx));
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void jump_rows_rebuild(HostForm *f)
{
    GtkWidget *c;
    guint i;
    while ((c = gtk_widget_get_first_child(f->jump_rows)))
        gtk_box_remove(GTK_BOX(f->jump_rows), c);
    for (i = 0; i < f->jump_sel->len; i++) {
        const char *id = g_ptr_array_index(f->jump_sel, i);
        hb_obj *h = NULL;
        const char *nm = "?", *u = NULL;
        GtkWidget *row, *t;
        if (hb_vault_get(f->v, id, &h) == HB_OK && h) {
            nm = hb_obj_get_str(h, "name");
            u = hb_obj_get_str(h, "user");
            if (!nm || !*nm) nm = hb_obj_get_str(h, "address");
        }
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row, "hb-jump-row");
        t = gtk_label_new(NULL);
        if (u && *u) {
            char *s = g_strdup_printf("%s@%s", u, nm ? nm : "?");
            gtk_label_set_text(GTK_LABEL(t), s);
            g_free(s);
        } else {
            gtk_label_set_text(GTK_LABEL(t), nm ? nm : "?");
        }
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_widget_set_hexpand(t, TRUE);
        gtk_box_append(GTK_BOX(row), t);
        gtk_box_append(GTK_BOX(row), jump_btn("↑", G_CALLBACK(jump_up), f, i));
        gtk_box_append(GTK_BOX(row), jump_btn("↓", G_CALLBACK(jump_down), f, i));
        gtk_box_append(GTK_BOX(row), jump_btn("×", G_CALLBACK(jump_del), f, i));
        gtk_box_append(GTK_BOX(f->jump_rows), row);
        if (h) hb_obj_free(h);
    }
}

static gboolean jump_add_idle(gpointer ud)
{
    HostForm *f = ud;
    guint i = f->jump_pending;
    f->jump_idle = 0;
    if (i == 0 || i >= f->jump_cand->len) return G_SOURCE_REMOVE;
    g_ptr_array_add(f->jump_sel,
                    g_strdup(g_ptr_array_index(f->jump_cand, i)));
    jump_rows_rebuild(f);
    jump_rebuild_add(f);
    return G_SOURCE_REMOVE;
}

static void on_jump_add_sel(GtkDropDown *dd, GParamSpec *ps, HostForm *f)
{
    guint i;
    (void)ps;
    if (f->jump_building) return;
    i = gtk_drop_down_get_selected(dd);
    if (i == 0 || i >= f->jump_cand->len) return;
    f->jump_pending = i;
    if (f->jump_idle) g_source_remove(f->jump_idle);
    f->jump_idle = g_idle_add(jump_add_idle, f);
}

/* 沿 jump 链 DFS：从 id 出发能否到达 target（成环检测）。 */
static gboolean jump_reaches(hb_vault *v, const char *id, const char *target,
                             int depth)
{
    const char **jump = NULL;
    size_t n, i;
    gboolean hit = FALSE;
    hb_obj *h = NULL;
    if (depth > 16 || !id) return FALSE;
    if (!strcmp(id, target)) return TRUE;
    if (hb_vault_get(v, id, &h) != HB_OK || !h) return FALSE;
    n = hb_obj_get_strv(h, "jump", &jump);
    for (i = 0; i < n && !hit; i++)
        hit = jump_reaches(v, jump[i], target, depth + 1);
    free(jump);
    hb_obj_free(h);
    return hit;
}

/* ------------------------------ via 下拉 ------------------------------ */

static void via_rebuild(HostForm *f, const char *select_id)
{
    GtkStringList *sl;
    hb_obj **hs = NULL;
    size_t n = 0, i;
    guint sel = 0;

    g_ptr_array_set_size(f->via_ids, 0);
    sl = gtk_string_list_new(NULL);
    gtk_string_list_append(sl, "选择 SSH 主机…");
    g_ptr_array_add(f->via_ids, NULL);
    hs = hb_store_list(f->st, "host", &n);
    for (i = 0; i < n; i++) {
        const char *pr = hb_obj_get_str(hs[i], "proto");
        const char *id = hb_obj_id(hs[i]);
        const char *nm = hb_obj_get_str(hs[i], "name");
        if (!pr || strcmp(pr, "ssh")) continue;
        if (f->host_id && !strcmp(id, f->host_id)) continue;
        gtk_string_list_append(sl, nm && *nm ? nm : id);
        g_ptr_array_add(f->via_ids, g_strdup(id));
        if (select_id && !strcmp(id, select_id))
            sel = f->via_ids->len - 1;
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(f->via_dd), G_LIST_MODEL(sl));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->via_dd), sel);
    if (hs) hb_obj_list_free(hs, n);
}

/* ------------------------------ 密钥/片段下拉 ------------------------------ */

static void key_rebuild(HostForm *f, const char *select_id)
{
    GtkStringList *sl;
    hb_obj **ks = NULL;
    size_t n = 0, i;
    guint sel = 0;

    f->key_building = TRUE;
    g_ptr_array_set_size(f->key_ids, 0);
    sl = gtk_string_list_new(NULL);
    ks = hb_store_list(f->st, "key", &n);
    if (n == 0)
        gtk_string_list_append(sl, "Keychain 中暂无密钥");
    for (i = 0; i < n; i++) {
        const char *nm = hb_obj_get_str(ks[i], "name");
        const char *fp = hb_obj_get_str(ks[i], "fingerprint");
        char *t = g_strdup_printf("%s%s%s",
                                  nm && *nm ? nm : "key",
                                  fp && *fp ? " · " : "",
                                  fp && *fp ? fp : "");
        gtk_string_list_append(sl, t);
        g_free(t);
        g_ptr_array_add(f->key_ids, g_strdup(hb_obj_id(ks[i])));
        if (select_id && !strcmp(hb_obj_id(ks[i]), select_id))
            sel = i;
    }
    /* 末位："生成新密钥…"（key_ids 里为 NULL 哨兵） */
    gtk_string_list_append(sl, "生成新密钥…");
    g_ptr_array_add(f->key_ids, NULL);
    gtk_widget_set_sensitive(f->key_dd, TRUE);
    gtk_drop_down_set_model(GTK_DROP_DOWN(f->key_dd), G_LIST_MODEL(sl));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->key_dd), sel);
    f->key_prev = sel;
    f->key_building = FALSE;
    if (ks) hb_obj_list_free(ks, n);
}

/* "生成新密钥…" 完成回调：选中新 key；取消 → 回到之前的选择 */
static void genkey_done(const char *new_id, gpointer ud)
{
    HostForm *f = ud;
    if (new_id)
        key_rebuild(f, new_id);
    else
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->key_dd),
                                   f->key_prev);
}

static void on_keydd_sel(GtkDropDown *dd, GParamSpec *ps, gpointer ud)
{
    HostForm *f = ud;
    guint i = gtk_drop_down_get_selected(dd);
    (void)ps;
    if (f->key_building || i >= f->key_ids->len)
        return;
    if (g_ptr_array_index(f->key_ids, i) == NULL)
        hb_keychain_generate_dialog(f->parent_w, f->st,
                                    genkey_done, f);
    else
        f->key_prev = i;
}

static void snip_rebuild(HostForm *f, const char *select_id)
{
    GtkStringList *sl;
    hb_obj **ss = NULL;
    size_t n = 0, i;
    guint sel = 0;

    g_ptr_array_set_size(f->snip_ids, 0);
    sl = gtk_string_list_new(NULL);
    gtk_string_list_append(sl, "无");
    g_ptr_array_add(f->snip_ids, NULL);
    ss = hb_store_list(f->st, "snippet", &n);
    for (i = 0; i < n; i++) {
        const char *nm = hb_obj_get_str(ss[i], "name");
        gtk_string_list_append(sl, nm && *nm ? nm : "snippet");
        g_ptr_array_add(f->snip_ids, g_strdup(hb_obj_id(ss[i])));
        if (select_id && !strcmp(hb_obj_id(ss[i]), select_id))
            sel = f->snip_ids->len - 1;
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(f->snip_dd), G_LIST_MODEL(sl));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->snip_dd), sel);
    if (ss) hb_obj_list_free(ss, n);
}

/* ------------------------------ 浏览文件 ------------------------------ */

static void on_browse_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HostForm *f = ud;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) gtk_editable_set_text(GTK_EDITABLE(f->ident_e), path);
    g_free(path);
    g_object_unref(file);
}

static void on_browse(GtkButton *b, HostForm *f)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    (void)b;
    gtk_file_dialog_set_title(d, "选择私钥文件");
    gtk_file_dialog_open(d, NULL, NULL, on_browse_done, f);
    g_object_unref(d);
}

/* ------------------------------ 颜色标签 ------------------------------ */

static void color_select(HostForm *f, const char *name, GtkWidget *btn)
{
    GtkWidget *c;
    if (!name) name = "";
    if (!btn && f->color_row) {
        /* 按名字找回按钮（编辑模式灌入初始值用） */
        for (c = gtk_widget_get_first_child(f->color_row); c;
             c = gtk_widget_get_next_sibling(c)) {
            const char *n = g_object_get_data(G_OBJECT(c), "color");
            if (n && !strcmp(n, name)) { btn = c; break; }
        }
    }
    g_free(f->color);
    f->color = *name ? g_strdup(name) : NULL;
    for (c = gtk_widget_get_first_child(f->color_row); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (c == btn) gtk_widget_add_css_class(c, "sel");
        else          gtk_widget_remove_css_class(c, "sel");
    }
}

static void on_color_click(GtkButton *b, gpointer ud)
{
    HostForm *f = g_object_get_data(G_OBJECT(b), "form");
    const char *nm = g_object_get_data(G_OBJECT(b), "color");
    color_select(f, nm && *nm ? nm : NULL, GTK_WIDGET(b));
    (void)ud;
}

static GtkWidget *color_row_new(HostForm *f, const char *initial)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sel_btn = NULL;
    size_t i;
    /* "无" */
    {
        GtkWidget *b = gtk_button_new_with_label("无");
        gtk_widget_add_css_class(b, "hb-btn");
        gtk_widget_add_css_class(b, "hb-color-none");
        g_object_set_data(G_OBJECT(b), "form", f);
        g_object_set_data(G_OBJECT(b), "color", (gpointer)"");
        g_signal_connect(b, "clicked", G_CALLBACK(on_color_click), NULL);
        gtk_box_append(GTK_BOX(row), b);
        if (!initial) sel_btn = b;
    }
    for (i = 0; i < N_COLORS; i++) {
        GtkWidget *b = gtk_button_new();
        gtk_widget_add_css_class(b, "hb-color-swatch");
        gtk_widget_add_css_class(b, kColors[i].css);
        gtk_widget_set_tooltip_text(b, kColors[i].name);
        gtk_widget_set_size_request(b, 26, 26);
        g_object_set_data(G_OBJECT(b), "form", f);
        g_object_set_data(G_OBJECT(b), "color", (gpointer)kColors[i].name);
        g_signal_connect(b, "clicked", G_CALLBACK(on_color_click), NULL);
        gtk_box_append(GTK_BOX(row), b);
        if (initial && !strcmp(initial, kColors[i].name)) sel_btn = b;
    }
    if (!sel_btn)
        sel_btn = gtk_widget_get_first_child(row);
    f->color_row = row;
    color_select(f, initial && *initial ? initial : NULL, sel_btn);
    return row;
}

/* ------------------------------ 类型联动 ------------------------------ */

static int proto_default_port(const char *proto)
{
    if (!strcmp(proto, "rdp")) return 3389;
    if (!strcmp(proto, "vnc")) return 5901;
    return 22;
}

static void apply_proto_vis(HostForm *f)
{
    gboolean ssh = !strcmp(f->proto, "ssh");
    gboolean remote = ssh || !strcmp(f->proto, "rdp") ||
                      !strcmp(f->proto, "vnc");
    gtk_widget_set_visible(f->sec_addr, remote);
    gtk_widget_set_visible(f->sec_auth, ssh);
    gtk_widget_set_visible(f->sec_jump, ssh);
    gtk_widget_set_visible(f->sec_tunnel,
                           remote && !ssh);
    gtk_widget_set_visible(f->sec_keep, ssh);
    gtk_widget_set_visible(f->sec_os, remote);
}

static void on_type_sel(GtkDropDown *dd, GParamSpec *ps, HostForm *f)
{
    guint i = gtk_drop_down_get_selected(dd);
    int prev_port;
    (void)ps;
    if (i >= 3) return;                  /* local 项只在编辑时出现 */
    prev_port = proto_default_port(kProtos[f->prev_proto]);
    strcpy(f->proto, kProtos[i]);
    /* 端口仍停在上一类型默认值 → 跟着换 */
    if ((int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(f->port_sb))
            == prev_port)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->port_sb),
                                  proto_default_port(f->proto));
    /* rdp 默认 windows */
    if (!strcmp(f->proto, "rdp") &&
        dd_idx(GTK_DROP_DOWN(f->os_dd)) == 0 && !f->host_id)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->os_dd), 4);
    f->prev_proto = (int)i;
    apply_proto_vis(f);
}

static void on_auth_sel(GtkDropDown *dd, GParamSpec *ps, HostForm *f)
{
    guint i = gtk_drop_down_get_selected(dd);
    (void)ps;
    gtk_widget_set_visible(f->pw_box, i == 1);
    gtk_widget_set_visible(f->key_box, i == 2);
}

/* ------------------------------ 校验/保存 ------------------------------ */

static void form_err(HostForm *f, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(f->err_l), msg ? msg : "");
    gtk_widget_set_visible(f->err_l, msg != NULL);
}

static char **tags_parse(const char *text, size_t *n_out)
{
    char **parts, **out;
    size_t n = 0, i;
    if (!text || !*text) { *n_out = 0; return NULL; }
    parts = g_strsplit(text, ",", -1);
    out = g_new0(char *, g_strv_length(parts) + 1);
    for (i = 0; parts[i]; i++) {
        char *t = g_strstrip(parts[i]);
        if (*t) out[n++] = g_strdup(t);
    }
    g_strfreev(parts);
    *n_out = n;
    return out;
}

static void do_save(HostForm *f)
{
    hb_obj *o = f->obj;
    const char *addr = gtk_editable_get_text(GTK_EDITABLE(f->addr_e));
    const char *name = gtk_editable_get_text(GTK_EDITABLE(f->name_e));
    guint i;

    /* 校验 */
    if (strcmp(f->proto, "local") && (!addr || !*addr)) {
        form_err(f, "地址不能为空");
        gtk_widget_grab_focus(f->addr_e);
        return;
    }
    if (f->host_id && !strcmp(f->proto, "ssh")) {
        for (i = 0; i < f->jump_sel->len; i++)
            if (jump_reaches(f->v, g_ptr_array_index(f->jump_sel, i),
                             f->host_id, 0)) {
                form_err(f, "跳板链存在循环：经该跳板最终会回到本机");
                return;
            }
    }

    if (!o) {
        o = hb_obj_new("host");
        f->obj = o;
    }
    hb_obj_set_str(o, "proto", f->proto);
    hb_obj_set_str(o, "name",
                   name && *name ? name : (addr && *addr ? addr : "local"));
    if (strcmp(f->proto, "local")) {
        hb_obj_set_str(o, "address", addr);
        hb_obj_set_int(o, "port",
                       (int64_t)gtk_spin_button_get_value(
                           GTK_SPIN_BUTTON(f->port_sb)));
        {
            const char *u = gtk_editable_get_text(GTK_EDITABLE(f->user_e));
            hb_obj_set_str(o, "user", u && *u ? u : NULL);
        }
    }

    /* OS */
    if (strcmp(f->proto, "local")) {
        guint oi = dd_idx(GTK_DROP_DOWN(f->os_dd));
        if (oi < G_N_ELEMENTS(kOses))
            hb_obj_set_str(o, "os", kOses[oi]);
    }

    if (!strcmp(f->proto, "ssh")) {
        guint ai = dd_idx(GTK_DROP_DOWN(f->auth_dd));
        const char *auth = ai < G_N_ELEMENTS(kAuths) ? kAuths[ai] : "auto";
        hb_obj_set_str(o, "auth", auth);
        /* key */
        if (!strcmp(auth, "key")) {
            guint ki = dd_idx(GTK_DROP_DOWN(f->key_dd));
            const char *kid = ki < f->key_ids->len
                ? g_ptr_array_index(f->key_ids, ki) : NULL;
            hb_obj_set_str(o, "key_id", kid && *kid ? kid : NULL);
        } else {
            hb_obj_set_str(o, "key_id", NULL);
        }
        {
            const char *idf =
                gtk_editable_get_text(GTK_EDITABLE(f->ident_e));
            hb_obj_set_str(o, "identity_file", idf && *idf ? idf : NULL);
        }
        /* jump */
        {
            const char **ids = g_new0(const char *, f->jump_sel->len + 1);
            for (i = 0; i < f->jump_sel->len; i++)
                ids[i] = g_ptr_array_index(f->jump_sel, i);
            hb_obj_set_strv(o, "jump", ids, f->jump_sel->len);
            g_free(ids);
        }
        /* keepalive */
        hb_obj_set_int(o, "keepalive",
                       (int64_t)gtk_spin_button_get_value(
                           GTK_SPIN_BUTTON(f->keep_sb)));
        /* startup_snippet */
        {
            guint si = dd_idx(GTK_DROP_DOWN(f->snip_dd));
            const char *sid = si < f->snip_ids->len
                ? g_ptr_array_index(f->snip_ids, si) : NULL;
            hb_obj_set_str(o, "startup_snippet", sid);
        }
        hb_obj_set_str(o, "tunnel", NULL);
        hb_obj_set_str(o, "via", NULL);
    } else if (!strcmp(f->proto, "rdp") || !strcmp(f->proto, "vnc")) {
        gboolean tun = gtk_switch_get_active(GTK_SWITCH(f->tunnel_sw));
        guint vi = dd_idx(GTK_DROP_DOWN(f->via_dd));
        const char *vid = vi < f->via_ids->len
            ? g_ptr_array_index(f->via_ids, vi) : NULL;
        hb_obj_set_str(o, "tunnel", tun && vid ? "ssh" : NULL);
        hb_obj_set_str(o, "via", tun ? vid : NULL);
        hb_obj_set_str(o, "auth", NULL);
        hb_obj_set_str(o, "key_id", NULL);
        hb_obj_set_strv(o, "jump", NULL, 0);
        hb_obj_set_str(o, "keepalive", NULL);
    }

    /* 分组 */
    {
        guint gi = dd_idx(GTK_DROP_DOWN(f->group_dd));
        const char *gv = gi < f->group_names->len
            ? g_ptr_array_index(f->group_names, gi) : "";
        if (!strcmp(gv, "\x01new")) gv = "";
        hb_obj_set_str(o, "group", *gv ? gv : NULL);
    }
    /* 颜色 / 标签 */
    hb_obj_set_str(o, "color", f->color);
    {
        size_t nt = 0;
        char **tags = tags_parse(
            gtk_editable_get_text(GTK_EDITABLE(f->tags_e)), &nt);
        hb_obj_set_strv(o, "tags", (const char *const *)tags, nt);
        g_strfreev(tags);
    }

    hb_vault_put(f->v, o);

    /* 密码 secret（仅 ssh/password 语义；编辑时空=保留） */
    if (!strcmp(f->proto, "ssh")) {
        guint ai = dd_idx(GTK_DROP_DOWN(f->auth_dd));
        if (ai == 1) {
            const char *pw =
                gtk_editable_get_text(GTK_EDITABLE(f->pw_e));
            if (f->pw_cleared)
                hb_vault_set_secret(f->v, hb_obj_id(o), "password", NULL);
            if (pw && *pw)
                hb_vault_set_secret(f->v, hb_obj_id(o), "password", pw);
        } else if (f->pw_cleared) {
            hb_vault_set_secret(f->v, hb_obj_id(o), "password", NULL);
        }
    }

    hb_toast(GTK_WIDGET(f->dlg), "主机已保存");
    adw_dialog_close(f->dlg);
}

static void on_save(GtkButton *b, HostForm *f) { (void)b; do_save(f); }
static void on_cancel(GtkButton *b, HostForm *f)
{ (void)b; adw_dialog_close(f->dlg); }

static gboolean on_key(GtkEventControllerKey *kc, guint keyval,
                       guint code, GdkModifierType state, HostForm *f)
{
    (void)kc; (void)code;
    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        do_save(f);
        return TRUE;
    }
    return FALSE;
}

static void on_pw_clear(GtkButton *b, HostForm *f)
{
    (void)b;
    f->pw_cleared = TRUE;
    gtk_widget_set_visible(f->pw_clear, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(f->pw_e), "");
    gtk_widget_set_tooltip_text(f->pw_e, "保存后将清除已保存密码");
}

static void on_closed(AdwDialog *dlg, HostForm *f)
{
    (void)dlg;
    if (f->jump_idle) g_source_remove(f->jump_idle);
    if (f->obj) hb_obj_free(f->obj);
    g_free(f->host_id);
    g_free(f->color);
    g_ptr_array_free(f->key_ids, TRUE);
    g_ptr_array_free(f->jump_cand, TRUE);
    g_ptr_array_free(f->jump_sel, TRUE);
    g_ptr_array_free(f->via_ids, TRUE);
    g_ptr_array_free(f->group_names, TRUE);
    g_ptr_array_free(f->snip_ids, TRUE);
    g_free(f);
}

/* ------------------------------ 构建 ------------------------------ */

static GtkDropDown *dd_new(const char *const *items, guint n, guint sel)
{
    GtkStringList *sl = gtk_string_list_new(NULL);
    guint i;
    GtkDropDown *dd;
    for (i = 0; i < n; i++)
        gtk_string_list_append(sl, items[i]);
    dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
    gtk_drop_down_set_selected(dd, sel);
    return dd;
}

static guint find_idx(const char *const *arr, guint n, const char *val,
                      guint def)
{
    guint i;
    if (!val) return def;
    for (i = 0; i < n; i++)
        if (!strcmp(arr[i], val)) return i;
    return def;
}

void hb_host_form_open(GtkWidget *parent, HbStore *st, const char *host_id)
{
    HostForm *f = g_new0(HostForm, 1);
    GtkWidget *dlg, *body, *foot, *f_, *ok, *cancel, *b, *row;
    const char *proto = "ssh";
    GtkRoot *root = gtk_widget_get_root(parent);
    GtkEventController *kc;

    f->st = st;
    f->v = hb_store_vault(st);
    f->parent_w = parent;
    f->key_ids = g_ptr_array_new_with_free_func(g_free);
    f->jump_cand = g_ptr_array_new_with_free_func(g_free);
    f->jump_sel = g_ptr_array_new_with_free_func(g_free);
    f->via_ids = g_ptr_array_new_with_free_func(g_free);
    f->group_names = g_ptr_array_new_with_free_func(g_free);
    f->snip_ids = g_ptr_array_new_with_free_func(g_free);

    if (host_id) {
        if (hb_vault_get(f->v, host_id, &f->obj) != HB_OK || !f->obj) {
            g_free(f);
            return;
        }
        f->host_id = g_strdup(host_id);
        proto = hb_obj_get_str(f->obj, "proto");
        if (!proto) proto = "ssh";
        f->is_local = !strcmp(proto, "local");
    }
    strcpy(f->proto, proto);
    f->prev_proto = (int)find_idx(kProtos, 3, proto, 0);

    dlg = GTK_WIDGET(hb_modal_new(f->host_id ? "编辑主机" : "New host",
                                  &body, &foot));
    f->dlg = ADW_DIALOG(dlg);
    f->body = body;

    /* 类型（编辑 local 才出现 Local 项） */
    {
        GtkStringList *sl = gtk_string_list_new(NULL);
        guint i;
        b = sec_box(f, "类型", NULL);
        for (i = 0; i < 3; i++)
            gtk_string_list_append(sl, kProtoLbl[i]);
        if (f->is_local)
            gtk_string_list_append(sl, "Local");
        f->type_dd = GTK_WIDGET(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->type_dd),
            f->is_local ? 3 : (guint)f->prev_proto);
        gtk_widget_set_sensitive(f->type_dd, !f->is_local);
        g_signal_connect(f->type_dd, "notify::selected",
                         G_CALLBACK(on_type_sel), f);
        gtk_box_append(GTK_BOX(b), f->type_dd);
    }

    /* 名称/地址/端口/用户 */
    b = sec_box(f, "基本信息", &f->sec_addr);
    f->name_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->name_e), "别名（默认=地址）");
    gtk_box_append(GTK_BOX(b), lbl("别名"));
    gtk_box_append(GTK_BOX(b), f->name_e);
    f->addr_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->addr_e), "hostname 或 IP");
    gtk_box_append(GTK_BOX(b), lbl("地址"));
    gtk_box_append(GTK_BOX(b), f->addr_e);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    {
        GtkWidget *pc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(pc, TRUE);
        gtk_box_append(GTK_BOX(pc), lbl("端口"));
        {
            /* 用带初值的 adjustment 创建，避免初始显示 lower bound */
            GtkAdjustment *pa = gtk_adjustment_new(
                proto_default_port(f->proto), 1, 65535, 1, 10, 0);
            f->port_sb = gtk_spin_button_new(pa, 1, 0);
        }
        gtk_box_append(GTK_BOX(pc), f->port_sb);
        gtk_box_append(GTK_BOX(row), pc);
    }
    {
        GtkWidget *uc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(uc, TRUE);
        gtk_box_append(GTK_BOX(uc), lbl("用户名"));
        f->user_e = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(f->user_e), local_user());
        gtk_box_append(GTK_BOX(uc), f->user_e);
        gtk_box_append(GTK_BOX(row), uc);
    }
    gtk_box_append(GTK_BOX(b), row);

    /* SSH 认证 */
    b = sec_box(f, "认证", &f->sec_auth);
    f->auth_dd = GTK_WIDGET(dd_new(kAuthLbl, 5, 0));
    g_signal_connect(f->auth_dd, "notify::selected",
                     G_CALLBACK(on_auth_sel), f);
    gtk_box_append(GTK_BOX(b), f->auth_dd);
    f->pw_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    {
        GtkWidget *pr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        f->pw_e = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(
            GTK_PASSWORD_ENTRY(f->pw_e), TRUE);
        gtk_widget_set_hexpand(f->pw_e, TRUE);
        gtk_box_append(GTK_BOX(pr), f->pw_e);
        f->pw_clear = gtk_button_new_with_label("清除已保存密码");
        gtk_widget_add_css_class(f->pw_clear, "hb-link");
        gtk_widget_set_visible(f->pw_clear, FALSE);
        g_signal_connect(f->pw_clear, "clicked",
                         G_CALLBACK(on_pw_clear), f);
        gtk_box_append(GTK_BOX(pr), f->pw_clear);
        gtk_box_append(GTK_BOX(f->pw_box), pr);
    }
    gtk_box_append(GTK_BOX(b), f->pw_box);
    f->key_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    f->key_dd = GTK_WIDGET(gtk_drop_down_new(NULL, NULL));
    g_signal_connect(f->key_dd, "notify::selected",
                     G_CALLBACK(on_keydd_sel), f);
    gtk_box_append(GTK_BOX(f->key_box), f->key_dd);
    {
        GtkWidget *kr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *bt;
        gtk_box_append(GTK_BOX(kr), lbl("私钥文件"));
        f->ident_e = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(f->ident_e),
                                       "~/.ssh/id_ed25519");
        gtk_widget_set_hexpand(f->ident_e, TRUE);
        gtk_box_append(GTK_BOX(kr), f->ident_e);
        bt = gtk_button_new_with_label("浏览…");
        gtk_widget_add_css_class(bt, "hb-btn");
        g_signal_connect(bt, "clicked", G_CALLBACK(on_browse), f);
        gtk_box_append(GTK_BOX(kr), bt);
        gtk_box_append(GTK_BOX(f->key_box), kr);
    }
    gtk_box_append(GTK_BOX(b), f->key_box);

    /* SSH 跳板链 */
    b = sec_box(f, "跳板机（外 → 内）", &f->sec_jump);
    f->jump_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(b), f->jump_rows);
    f->jump_dd = GTK_WIDGET(gtk_drop_down_new(NULL, NULL));
    g_signal_connect(f->jump_dd, "notify::selected",
                     G_CALLBACK(on_jump_add_sel), f);
    gtk_box_append(GTK_BOX(b), f->jump_dd);

    /* RDP/VNC 隧道 */
    b = sec_box(f, "隧道", &f->sec_tunnel);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *t = gtk_label_new("通过 SSH 隧道");
        gtk_widget_set_hexpand(t, TRUE);
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_box_append(GTK_BOX(row), t);
        f->tunnel_sw = gtk_switch_new();
        gtk_box_append(GTK_BOX(row), f->tunnel_sw);
    }
    gtk_box_append(GTK_BOX(b), row);
    f->via_dd = GTK_WIDGET(gtk_drop_down_new(NULL, NULL));
    gtk_box_append(GTK_BOX(b), f->via_dd);

    /* 分组 */
    b = sec_box(f, "分组", NULL);
    f->group_dd = GTK_WIDGET(gtk_drop_down_new(NULL, NULL));
    g_signal_connect(f->group_dd, "notify::selected",
                     G_CALLBACK(on_group_sel), f);
    gtk_box_append(GTK_BOX(b), f->group_dd);

    /* 启动片段 + 保活 */
    b = sec_box(f, "启动片段", NULL);
    f->snip_dd = GTK_WIDGET(gtk_drop_down_new(NULL, NULL));
    gtk_box_append(GTK_BOX(b), f->snip_dd);
    b = sec_box(f, "保活间隔（秒，0 = 关闭）", &f->sec_keep);
    f->keep_sb = gtk_spin_button_new_with_range(0, 3600, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->keep_sb), 30);
    gtk_box_append(GTK_BOX(b), f->keep_sb);

    /* 颜色 / 标签 / OS */
    b = sec_box(f, "颜色标签", NULL);
    gtk_box_append(GTK_BOX(b), color_row_new(f, NULL));
    b = sec_box(f, "标签（逗号分隔）", NULL);
    f->tags_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->tags_e), "prod, web, …");
    gtk_box_append(GTK_BOX(b), f->tags_e);
    b = sec_box(f, "操作系统", &f->sec_os);
    f->os_dd = GTK_WIDGET(dd_new(kOsLbl, 5, 0));
    gtk_box_append(GTK_BOX(b), f->os_dd);

    /* 内联错误 */
    f->err_l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(f->err_l), 0);
    gtk_label_set_wrap(GTK_LABEL(f->err_l), TRUE);
    gtk_widget_add_css_class(f->err_l, "hb-red");
    gtk_widget_set_visible(f->err_l, FALSE);
    gtk_box_append(GTK_BOX(f->body), f->err_l);

    /* footer */
    f_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f_, "hb-modal-footer");
    gtk_widget_set_halign(f_, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(cancel, "hb-btn");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), f);
    gtk_box_append(GTK_BOX(f_), cancel);
    ok = gtk_button_new_with_label("保存");
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(on_save), f);
    gtk_box_append(GTK_BOX(f_), ok);
    f->save_btn = ok;
    gtk_widget_set_margin_start(f_, 20);
    gtk_widget_set_margin_end(f_, 20);
    gtk_widget_set_margin_top(f_, 4);
    gtk_widget_set_margin_bottom(f_, 16);
    gtk_box_append(GTK_BOX(foot), f_);

    /* Ctrl+Enter 保存 */
    kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), f);
    gtk_widget_add_controller(GTK_WIDGET(dlg), kc);

    /* ---------------- 编辑模式：灌入现有值 ---------------- */
    if (f->obj) {
        const char *s;
        const char **jump = NULL;
        size_t nj, i;
        s = hb_obj_get_str(f->obj, "name");
        gtk_editable_set_text(GTK_EDITABLE(f->name_e), s ? s : "");
        s = hb_obj_get_str(f->obj, "address");
        gtk_editable_set_text(GTK_EDITABLE(f->addr_e), s ? s : "");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->port_sb),
            (double)hb_obj_get_int(f->obj, "port",
                                   proto_default_port(f->proto)));
        s = hb_obj_get_str(f->obj, "user");
        gtk_editable_set_text(GTK_EDITABLE(f->user_e), s ? s : "");
        s = hb_obj_get_str(f->obj, "auth");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->auth_dd),
                                 find_idx(kAuths, 5, s, 0));
        {
            char *sec = hb_vault_get_secret(f->v, host_id, "password");
            f->had_secret = sec != NULL;
            gtk_widget_set_visible(f->pw_clear, f->had_secret);
            if (sec) hb_secret_free(sec);
        }
        s = hb_obj_get_str(f->obj, "key_id");
        key_rebuild(f, s);
        s = hb_obj_get_str(f->obj, "identity_file");
        gtk_editable_set_text(GTK_EDITABLE(f->ident_e), s ? s : "");
        nj = hb_obj_get_strv(f->obj, "jump", &jump);
        for (i = 0; i < nj; i++)
            g_ptr_array_add(f->jump_sel, g_strdup(jump[i]));
        free(jump);
        gtk_switch_set_active(GTK_SWITCH(f->tunnel_sw),
            hb_obj_get_str(f->obj, "via") != NULL ||
            hb_obj_get_str(f->obj, "tunnel") != NULL);
        via_rebuild(f, hb_obj_get_str(f->obj, "via"));
        group_rebuild(f, hb_obj_get_str(f->obj, "group"));
        snip_rebuild(f, hb_obj_get_str(f->obj, "startup_snippet"));
        if (hb_obj_has(f->obj, "keepalive"))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->keep_sb),
                (double)hb_obj_get_int(f->obj, "keepalive", 30));
        s = hb_obj_get_str(f->obj, "os");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->os_dd),
                                 find_idx(kOses, 5, s, 0));
        {
            const char **tags = NULL;
            size_t nt = hb_obj_get_strv(f->obj, "tags", &tags);
            if (nt) {
                char *jt = g_strjoinv(", ", (char **)tags);
                gtk_editable_set_text(GTK_EDITABLE(f->tags_e), jt);
                g_free(jt);
            }
            free(tags);
        }
        color_select(f, hb_obj_get_str(f->obj, "color"), NULL);
    } else {
        key_rebuild(f, NULL);
        via_rebuild(f, NULL);
        group_rebuild(f, NULL);
        snip_rebuild(f, NULL);
    }
    jump_rows_rebuild(f);
    jump_rebuild_add(f);
    on_auth_sel(GTK_DROP_DOWN(f->auth_dd), NULL, f);
    apply_proto_vis(f);

    g_signal_connect(dlg, "closed", G_CALLBACK(on_closed), f);
    adw_dialog_present(ADW_DIALOG(dlg),
                       GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : NULL);
}
