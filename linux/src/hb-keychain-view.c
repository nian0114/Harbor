#include "hb-keychain-view.h"
#include "hb-icons.h"
#include "hb-dialogs.h"
#include "hb-window.h"

#include <harbor/keys.h>
#include <harbor/ssh.h>
#include <harbor/vault.h>

#include <openssl/crypto.h>
#include <sodium.h>
#include <string.h>
#include <stdio.h>

/* ==================================================================
 * Keychain（PRD FR-02）：表格列出密钥对象 + 生成 / 导入 / 详情 /
 * 删除-撤销 / 导出公钥。私钥本体只走 vault secret。
 *
 * key 对象字段（data-model.md）：
 *   name, kind(ed25519|ecdsa|rsa), bits, format(openssh|pem|pkcs8|pkcs12),
 *   public, fingerprint, comment, confirm, created
 *   secrets: private（私钥文本）, passphrase（可选记住的口令）
 * ================================================================== */

#define COL_TYPE_W  118
#define COL_FP_W    200
#define COL_CONF_W   84
#define COL_REF_W    76
#define COL_ACT_W    80

struct _HbKeychainView {
    GtkBox      parent;
    HbStore    *store;
    GtkWidget  *search;
    GtkWidget  *rows;        /* 数据行容器 */
    GtkWidget  *body;        /* stack: table / empty / nomatch */
    GtkWidget  *hint;
    hb_obj    **keys;        /* 当前 vault key 列表（拥有） */
    size_t      nkeys;
    guint       pending_reload;
    gulong      changed_handler;
};

G_DEFINE_TYPE(HbKeychainView, hb_keychain_view, GTK_TYPE_BOX)

/* ------------------------------ 工具 ------------------------------ */

static const char *key_type_label(hb_obj *k, char *buf, size_t n)
{
    const char *kind = hb_obj_get_str(k, "kind");
    int bits = (int)hb_obj_get_int(k, "bits", 0);
    if (!kind || !*kind) {
        g_strlcpy(buf, "—", n);
    } else if (!strcmp(kind, "ed25519")) {
        g_strlcpy(buf, "ED25519", n);
    } else if (!strcmp(kind, "ecdsa")) {
        g_snprintf(buf, n, "ECDSA P-%d", bits);
    } else if (!strcmp(kind, "rsa")) {
        g_snprintf(buf, n, bits ? "RSA %d" : "RSA", bits);
    } else {
        g_strlcpy(buf, kind, n);
    }
    return buf;
}

static const char *format_label(hb_obj *k)
{
    const char *f = hb_obj_get_str(k, "format");
    if (!f) return "openssh";
    return f;
}

static void copy_text(GtkWidget *any, const char *text)
{
    GdkClipboard *cl = gtk_widget_get_clipboard(any);
    gdk_clipboard_set_text(cl, text);
}

/* ------------------------------ 行构建 ------------------------------ */

static void on_row_copy(GtkButton *b, gpointer ud);
static void on_row_del(GtkButton *b, gpointer ud);
static void on_row_click(GtkGestureClick *g, int n, double x, double y,
                         gpointer ud);
static void open_detail(HbKeychainView *v, const char *key_id);

static GtkWidget *key_row_new(HbKeychainView *v, hb_obj *k, size_t refs)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *c1, *ic, *name, *typ, *fp, *pill, *ref, *act, *bc, *bd;
    char tbuf[32], rbuf[32];
    const char *nm = hb_obj_get_str(k, "name");
    const char *pub = hb_obj_get_str(k, "public");
    const char *fpv = hb_obj_get_str(k, "fingerprint");
    gboolean conf = hb_obj_get_bool(k, "confirm", false);
    GtkGesture *click;

    gtk_widget_add_css_class(row, "hb-table-row");

    /* 名称列 */
    c1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(c1, TRUE);
    gtk_widget_set_size_request(c1, 160, -1);
    gtk_widget_set_margin_start(c1, 14);
    ic = hb_icon_new("key", 15);
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(ic, "hb-dim");
    name = gtk_label_new(nm && *nm ? nm : "key");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(name, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(c1), ic);
    gtk_box_append(GTK_BOX(c1), name);
    gtk_box_append(GTK_BOX(row), c1);

    typ = gtk_label_new(key_type_label(k, tbuf, sizeof tbuf));
    gtk_label_set_xalign(GTK_LABEL(typ), 0);
    gtk_widget_set_size_request(typ, COL_TYPE_W, -1);
    gtk_widget_set_margin_start(typ, 14);
    gtk_box_append(GTK_BOX(row), typ);

    fp = gtk_label_new(fpv && *fpv ? fpv : "加密 · 首次使用后显示");
    gtk_widget_add_css_class(fp, "mono");
    gtk_label_set_ellipsize(GTK_LABEL(fp), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(fp), 0);
    gtk_widget_set_size_request(fp, COL_FP_W, -1);
    gtk_widget_set_margin_start(fp, 14);
    gtk_box_append(GTK_BOX(row), fp);

    pill = gtk_label_new(conf ? "开" : "关");
    gtk_widget_add_css_class(pill, "pill");
    gtk_widget_add_css_class(pill, conf ? "on" : "off");
    gtk_widget_set_size_request(pill, COL_CONF_W, -1);
    gtk_widget_set_halign(pill, GTK_ALIGN_START);
    gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(pill, 14);
    gtk_box_append(GTK_BOX(row), pill);

    g_snprintf(rbuf, sizeof rbuf, "%d 台主机", (int)refs);
    ref = gtk_label_new(rbuf);
    gtk_label_set_xalign(GTK_LABEL(ref), 0);
    gtk_widget_set_size_request(ref, COL_REF_W, -1);
    gtk_widget_set_margin_start(ref, 14);
    gtk_box_append(GTK_BOX(row), ref);

    act = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(act, COL_ACT_W, -1);
    gtk_widget_set_margin_start(act, 8);
    gtk_widget_set_valign(act, GTK_ALIGN_CENTER);
    bc = gtk_button_new();
    gtk_widget_add_css_class(bc, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(bc), hb_icon_new("copy", 13));
    gtk_widget_set_tooltip_text(bc, pub && *pub ? "复制公钥"
                                              : "公钥未知（加密导入）");
    gtk_widget_set_sensitive(bc, pub && *pub);
    g_object_set_data_full(G_OBJECT(bc), "pub",
                           g_strdup(pub ? pub : ""), g_free);
    g_object_set_data(G_OBJECT(bc), "kv", v);
    g_signal_connect(bc, "clicked", G_CALLBACK(on_row_copy), NULL);
    gtk_box_append(GTK_BOX(act), bc);
    bd = gtk_button_new();
    gtk_widget_add_css_class(bd, "hb-iconbtn");
    gtk_button_set_child(GTK_BUTTON(bd), hb_icon_new("trash", 13));
    gtk_widget_set_tooltip_text(bd, "删除密钥");
    g_object_set_data_full(G_OBJECT(bd), "kid",
                           g_strdup(hb_obj_id(k)), g_free);
    g_object_set_data(G_OBJECT(bd), "kv", v);
    g_signal_connect(bd, "clicked", G_CALLBACK(on_row_del), NULL);
    gtk_box_append(GTK_BOX(act), bd);
    gtk_box_append(GTK_BOX(row), act);

    /* 单击行 → 详情 */
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_PRIMARY);
    g_object_set_data_full(G_OBJECT(row), "kid",
                           g_strdup(hb_obj_id(k)), g_free);
    g_object_set_data(G_OBJECT(row), "kv", v);
    g_signal_connect(click, "released", G_CALLBACK(on_row_click), NULL);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    return row;
}

static void on_row_copy(GtkButton *b, gpointer ud)
{
    const char *pub = g_object_get_data(G_OBJECT(b), "pub");
    (void)ud;
    if (pub && *pub) {
        copy_text(GTK_WIDGET(b), pub);
        hb_toast(GTK_WIDGET(b), "公钥已复制");
    }
}

static void on_row_click(GtkGestureClick *g, int n, double x, double y,
                         gpointer ud)
{
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    const char *kid = g_object_get_data(G_OBJECT(row), "kid");
    HbKeychainView *v = g_object_get_data(G_OBJECT(row), "kv");
    (void)n; (void)x; (void)y; (void)ud;
    if (kid && v)
        open_detail(v, kid);
}

/* ------------------------------ reload ------------------------------ */

static gboolean key_matches(hb_obj *k, const char *needle)
{
    char *hay, *hit;
    gboolean ok;
    if (!needle || !*needle) return TRUE;
    hay = g_strjoin(" ",
        hb_obj_get_str(k, "name") ?: "",
        hb_obj_get_str(k, "kind") ?: "",
        hb_obj_get_str(k, "fingerprint") ?: "",
        hb_obj_get_str(k, "comment") ?: "", NULL);
    hit = g_utf8_casefold(hay, -1);
    ok = strstr(hit, needle) != NULL;
    g_free(hay); g_free(hit);
    return ok;
}

static void reload(HbKeychainView *v)
{
    hb_vault *vault = hb_store_vault(v->store);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search));
    char *needle = g_utf8_casefold(q ? q : "", -1);
    size_t i, shown = 0;
    GtkWidget *ch;

    while ((ch = gtk_widget_get_first_child(v->rows)))
        gtk_widget_unparent(ch);
    if (v->keys) {
        hb_obj_list_free(v->keys, v->nkeys);
        v->keys = NULL;
    }
    v->keys = hb_store_list(v->store, "key", &v->nkeys);

    for (i = 0; i < v->nkeys; i++) {
        if (!key_matches(v->keys[i], needle))
            continue;
        gtk_box_append(GTK_BOX(v->rows),
            key_row_new(v, v->keys[i],
                hb_vault_key_ref_count(vault, hb_obj_id(v->keys[i]))));
        shown++;
    }
    g_free(needle);

    if (v->nkeys == 0)
        gtk_stack_set_visible_child_name(GTK_STACK(v->body), "empty");
    else if (shown == 0)
        gtk_stack_set_visible_child_name(GTK_STACK(v->body), "nomatch");
    else
        gtk_stack_set_visible_child_name(GTK_STACK(v->body), "table");
}

static gboolean reload_idle(gpointer ud)
{
    HbKeychainView *v = ud;
    v->pending_reload = 0;
    reload(v);
    return G_SOURCE_REMOVE;
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbKeychainView *v = ud;
    (void)st; (void)id;
    if (strcmp(type, "key") && strcmp(type, "host")) return;
    if (!v->pending_reload)
        v->pending_reload = g_idle_add(reload_idle, v);
}

static void on_search(GtkSearchEntry *e, gpointer ud)
{
    (void)e;
    reload((HbKeychainView *)ud);
}

/* ------------------------------ 删除 + 撤销 ------------------------------ */

typedef struct {
    HbStore    *store;
    char       *key_id;
    hb_key_ref *refs;
    size_t      nrefs;
} KeyUndo;

static void key_undo_cb(gpointer ud)
{
    KeyUndo *u = ud;
    hb_vault_undo_key_cascade(hb_store_vault(u->store),
                              u->key_id, u->refs, u->nrefs);
}
static void key_undo_free(gpointer ud)
{
    KeyUndo *u = ud;
    hb_key_refs_free(u->refs, u->nrefs);
    g_free(u->key_id);
    g_free(u);
}

typedef struct {
    HbKeychainView *v;
    char *key_id;
    char *name;
    size_t refs;
} DelCtx;

static void del_done(gboolean ok, gpointer ud)
{
    DelCtx *d = ud;
    hb_vault *vault;
    KeyUndo *u;
    char *msg;

    if (!ok) {
        g_free(d->key_id); g_free(d->name); g_free(d);
        return;
    }
    vault = hb_store_vault(d->v->store);
    u = g_new0(KeyUndo, 1);
    u->store = d->v->store;
    u->key_id = g_strdup(d->key_id);
    if (hb_vault_delete_key_cascade(vault, d->key_id,
                                    &u->refs, &u->nrefs) != HB_OK) {
        hb_toast(GTK_WIDGET(d->v), "删除密钥失败");
        key_undo_free(u);
    } else {
        msg = g_strdup_printf("已删除密钥 %s", d->name);
        hb_toast_undo_destroy(GTK_WIDGET(d->v), msg,
                              key_undo_cb, key_undo_free, u);
        g_free(msg);
    }
    g_free(d->key_id); g_free(d->name); g_free(d);
}

static void delete_key(HbKeychainView *v, const char *key_id)
{
    hb_vault *vault = hb_store_vault(v->store);
    hb_obj *k = NULL;
    DelCtx *d;
    char *mk;
    const char *nm = "key";
    size_t refs = 0;

    if (hb_vault_get(vault, key_id, &k) == HB_OK && k) {
        const char *s = hb_obj_get_str(k, "name");
        if (s && *s) nm = s;
        refs = hb_vault_key_ref_count(vault, key_id);
    }
    {
        GString *g = g_string_new(NULL);
        char *esc = g_markup_escape_text(nm ? nm : "key", -1);
        g_string_append_printf(g, "确定删除 <b>%s</b>？", esc);
        g_free(esc);
        if (refs)
            g_string_append_printf(g,
                "有 %d 台主机引用此密钥，删除后这些主机将改为自动认证。",
                (int)refs);
        g_string_append(g, "删除后 9 秒内可撤销。");
        mk = g_strdup(g->str);
        g_string_free(g, TRUE);
    }
    d = g_new0(DelCtx, 1);
    d->v = v;
    d->key_id = g_strdup(key_id);
    d->name = g_strdup(nm);
    d->refs = refs;
    hb_dialog_confirm(GTK_WIDGET(v), "删除密钥", mk, "删除", TRUE,
                      del_done, d);
    g_free(mk);
    if (k) hb_obj_free(k);
}

static void on_row_del(GtkButton *b, gpointer ud)
{
    const char *kid = g_object_get_data(G_OBJECT(b), "kid");
    HbKeychainView *v = g_object_get_data(G_OBJECT(b), "kv");
    (void)ud;
    if (kid && v)
        delete_key(v, kid);
}

/* ------------------------------ 生成 ------------------------------ */

static const struct {
    const char *label, *defname;
    hb_key_type type; int rsa_bits;
} kTypes[] = {
    { "Ed25519",     "id_ed25519",    HB_KEY_ED25519,    0    },
    { "ECDSA P-256", "id_ecdsa_p256", HB_KEY_ECDSA_P256, 0    },
    { "ECDSA P-384", "id_ecdsa_p384", HB_KEY_ECDSA_P384, 0    },
    { "ECDSA P-521", "id_ecdsa_p521", HB_KEY_ECDSA_P521, 0    },
    { "RSA 3072",    "id_rsa_3072",   HB_KEY_RSA,        3072 },
    { "RSA 4096",    "id_rsa_4096",   HB_KEY_RSA,        4096 },
};
#define N_TYPES (sizeof(kTypes) / sizeof(kTypes[0]))

typedef struct {
    HbStore           *st;
    AdwDialog         *dlg;
    GtkWidget         *name_e, *type_dd, *comment_e, *confirm_sw;
    GtkWidget         *gen_btn, *spin, *err_l;
    hb_keychain_gen_cb cb;
    gpointer           ud;
    guint              prev_type;
    gboolean           busy, cancelled;
    char              *result_id;
} GenDlg;

typedef struct {
    hb_key_type  t;
    int          rsa_bits;
    char        *comment;
    hb_key_info  info;
    hb_err       err;
} GenWork;

static void gen_thread(GTask *task, gpointer src, gpointer data,
                       GCancellable *c)
{
    GenWork *w = data;
    (void)task; (void)src; (void)c;
    w->err = hb_key_generate(w->t, w->rsa_bits, w->comment, &w->info);
}

static void pub_copy_cb(gpointer ud)
{
    /* ud = strdup 的 public_line（toast dismiss 时 g_free） */
    GdkClipboard *cl = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(cl, ud);
}

static void gen_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    GenDlg *d = ud;
    GenWork *w = g_task_get_task_data(G_TASK(res));
    hb_vault *v;
    hb_obj *o;
    const char *nm;
    char *toast;
    (void)src;

    d->busy = FALSE;
    g_object_unref(d->dlg);         /* gen_start 里的保活 ref */
    if (d->cancelled) {             /* 对话框已关 → 丢弃结果 */
        if (w && w->err == HB_OK)
            hb_key_info_clear(&w->info);
        g_free(w ? w->comment : NULL);
        g_free(w);
        g_free(d->result_id);
        g_free(d);
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(d->dlg), TRUE);
    gtk_widget_set_visible(d->spin, FALSE);
    gtk_spinner_stop(GTK_SPINNER(d->spin));
    gtk_widget_set_sensitive(d->gen_btn, TRUE);

    if (!w || w->err != HB_OK) {
        gtk_label_set_text(GTK_LABEL(d->err_l),
            "生成失败，请重试");
        gtk_widget_set_visible(d->err_l, TRUE);
        if (w) { g_free(w->comment); g_free(w); }
        return;
    }

    v = hb_store_vault(d->st);
    o = hb_obj_new("key");
    nm = gtk_editable_get_text(GTK_EDITABLE(d->name_e));
    hb_obj_set_str(o, "name", nm && *nm ? nm : "key");
    hb_obj_set_str(o, "kind", w->info.kind);
    hb_obj_set_int(o, "bits", w->info.bits);
    hb_obj_set_str(o, "public", w->info.public_line);
    hb_obj_set_str(o, "fingerprint", w->info.fingerprint);
    hb_obj_set_str(o, "comment", w->info.comment);
    hb_obj_set_bool(o, "confirm",
        gtk_switch_get_active(GTK_SWITCH(d->confirm_sw)));
    hb_obj_set_str(o, "format", "openssh");
    hb_obj_set_int(o, "created", g_get_real_time() / 1000);
    hb_vault_put(v, o);
    hb_vault_set_secret(v, hb_obj_id(o), "private", w->info.private_text);
    d->result_id = g_strdup(hb_obj_id(o));
    hb_obj_free(o);

    toast = g_strdup_printf("已生成 %s",
                            nm && *nm ? nm : "key");
    {
        GtkWidget *any = GTK_WIDGET(d->dlg);
        /* toast action 带 destroy：pub strdup 在 dismiss 后释放 */
        GtkRoot *root = gtk_widget_get_root(any);
        HbWindow *w2 = HB_IS_WINDOW(root) ? HB_WINDOW(root) : NULL;
        if (w2)
            hb_window_toast_action2(w2, toast, "复制公钥", 6,
                (GCallback)pub_copy_cb, g_free,
                g_strdup(w->info.public_line));
        else
            hb_toast(any, toast);
    }
    g_free(toast);

    hb_key_info_clear(&w->info);    /* private_text 进锁页并擦除 */
    g_free(w->comment);
    g_free(w);
    adw_dialog_close(d->dlg);
}

static void gen_start(GtkButton *b, gpointer ud)
{
    GenDlg *d = ud;
    GTask *task;
    GenWork *w;
    guint ti = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->type_dd));
    (void)b;
    if (d->busy || ti >= N_TYPES) return;
    w = g_new0(GenWork, 1);
    w->t = kTypes[ti].type;
    w->rsa_bits = kTypes[ti].rsa_bits;
    w->comment = g_strdup(gtk_editable_get_text(
        GTK_EDITABLE(d->comment_e)));
    d->busy = TRUE;
    gtk_widget_set_visible(d->err_l, FALSE);
    gtk_widget_set_sensitive(d->gen_btn, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(d->dlg), FALSE);
    gtk_widget_set_visible(d->spin, TRUE);
    gtk_spinner_start(GTK_SPINNER(d->spin));
    /* RSA 4096 可能要几秒 → 线程里跑；dialog widget ref 保活到 done */
    g_object_ref(d->dlg);
    task = g_task_new(NULL, NULL, gen_done, d);
    g_task_set_task_data(task, w, NULL);
    g_task_run_in_thread(task, gen_thread);
    g_object_unref(task);
}

static void gen_closed(AdwDialog *dlg, gpointer ud)
{
    GenDlg *d = ud;
    (void)dlg;
    if (d->busy) {
        /* 生成中关闭：结果丢弃，cb 报 NULL；GenDlg 由 gen_done 释放 */
        d->cancelled = TRUE;
        if (d->cb) d->cb(NULL, d->ud);
        return;
    }
    if (d->cb) d->cb(d->result_id, d->ud);
    g_free(d->result_id);
    g_free(d);
}

static void gen_type_sel(GtkDropDown *dd, GParamSpec *ps, gpointer ud)
{
    GenDlg *d = ud;
    guint ti = gtk_drop_down_get_selected(dd);
    const char *cur;
    (void)ps;
    if (ti >= N_TYPES || d->prev_type >= N_TYPES) {
        d->prev_type = ti;
        return;
    }
    /* 名称还挂着上一类型的默认 → 跟随改名 */
    cur = gtk_editable_get_text(GTK_EDITABLE(d->name_e));
    if (!strcmp(cur, kTypes[d->prev_type].defname))
        gtk_editable_set_text(GTK_EDITABLE(d->name_e),
                              kTypes[ti].defname);
    d->prev_type = ti;
}

void hb_keychain_generate_dialog(GtkWidget *parent, HbStore *st,
                                 hb_keychain_gen_cb done_cb, gpointer ud)
{
    GenDlg *d = g_new0(GenDlg, 1);
    GtkWidget *body, *foot, *l, *row, *swr, *f;
    GtkRoot *root = gtk_widget_get_root(parent);
    char *defcomment;
    const char *labels[N_TYPES + 1];
    size_t i;

    d->st = st;
    d->cb = done_cb;
    d->ud = ud;
    d->dlg = hb_modal_new("New key", &body, &foot);

    /* 名称 */
    l = gtk_label_new("名称");
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    d->name_e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->name_e), kTypes[0].defname);
    gtk_box_append(GTK_BOX(body), d->name_e);

    /* 类型 */
    l = gtk_label_new("类型");
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    for (i = 0; i < N_TYPES; i++) labels[i] = kTypes[i].label;
    labels[N_TYPES] = NULL;
    d->type_dd = gtk_drop_down_new_from_strings(
        (const char *const *)labels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->type_dd), 0);
    g_signal_connect(d->type_dd, "notify::selected",
                     G_CALLBACK(gen_type_sel), d);
    gtk_box_append(GTK_BOX(body), d->type_dd);

    /* 注释 */
    l = gtk_label_new("注释");
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    d->comment_e = gtk_entry_new();
    defcomment = g_strdup_printf("%s@%s", g_get_user_name(),
                                 g_get_host_name());
    gtk_editable_set_text(GTK_EDITABLE(d->comment_e), defcomment);
    g_free(defcomment);
    gtk_box_append(GTK_BOX(body), d->comment_e);

    /* 使用前确认 */
    swr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *t = gtk_label_new("使用前确认");
        GtkWidget *s = gtk_label_new("每次用此密钥签名前先弹确认框");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(col), t);
        gtk_box_append(GTK_BOX(col), s);
        gtk_box_append(GTK_BOX(swr), col);
    }
    d->confirm_sw = gtk_switch_new();
    gtk_widget_set_valign(d->confirm_sw, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(swr), d->confirm_sw);
    gtk_widget_add_css_class(swr, "check-row");
    gtk_box_append(GTK_BOX(body), swr);

    /* 错误行 */
    d->err_l = gtk_label_new("");
    gtk_widget_add_css_class(d->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(d->err_l), 0);
    gtk_widget_set_visible(d->err_l, FALSE);
    gtk_box_append(GTK_BOX(body), d->err_l);

    /* footer：spinner + 取消 + 生成 */
    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    d->spin = gtk_spinner_new();
    gtk_widget_set_visible(d->spin, FALSE);
    gtk_box_append(GTK_BOX(f), d->spin);
    {
        GtkWidget *c = gtk_button_new_with_label("取消");
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect_swapped(c, "clicked",
            G_CALLBACK(adw_dialog_close), d->dlg);
        gtk_box_append(GTK_BOX(f), c);
    }
    d->gen_btn = gtk_button_new_with_label("生成");
    gtk_widget_add_css_class(d->gen_btn, "hb-btn");
    gtk_widget_add_css_class(d->gen_btn, "primary");
    g_signal_connect(d->gen_btn, "clicked", G_CALLBACK(gen_start), d);
    gtk_box_append(GTK_BOX(f), d->gen_btn);
    gtk_box_append(GTK_BOX(foot), f);
    row = NULL; (void)row;

    g_signal_connect(d->dlg, "closed", G_CALLBACK(gen_closed), d);
    adw_dialog_present(d->dlg,
                       GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : NULL);
}

/* ------------------------------ 导入 ------------------------------ */

static const char *fmt_str(hb_key_format f)
{
    switch (f) {
    case HB_KEYFMT_PEM:    return "pem";
    case HB_KEYFMT_PKCS8:  return "pkcs8";
    case HB_KEYFMT_PKCS12: return "pkcs12";
    default:               return "openssh";
    }
}

typedef struct {
    HbStore     *st;
    AdwDialog   *dlg;
    GtkWidget   *name_e;
    GtkTextBuffer *buf;
    GtkWidget   *info_l;       /* 识别到的信息 */
    GtkWidget   *pw_row;       /* 口令行（PasswordEntry+remember） */
    GtkWidget   *pw_e, *pw_rem;
    GtkWidget   *pw_note;
    GtkWidget   *pw_err;
    GtkWidget   *err_l;        /* 非私钥/重复 提示 */
    GtkWidget   *imp_btn;
    char        *src;          /* 当前源数据（拥有） */
    size_t       src_len;
    char        *src_path;
    hb_key_info  info;
    hb_err       info_err;
    gboolean     have_src;
    gboolean     inspecting;
    GCancellable *fcancel;    /* 文件选择器：关对话框时取消，防 use-after-free */
} ImpDlg;

static void imp_clear_src(ImpDlg *d)
{
    if (d->src) {
        OPENSSL_cleanse(d->src, d->src_len);
        g_free(d->src);
    }
    d->src = NULL;
    d->src_len = 0;
    d->have_src = FALSE;
    g_clear_pointer(&d->src_path, g_free);
    hb_key_info_clear(&d->info);
    d->info_err = HB_ERR_INVALID;
}

static void imp_refresh(ImpDlg *d)
{
    const char *pw;
    char *desc = NULL, *mk = NULL;
    gboolean need_pw = FALSE, dup = FALSE;
    hb_obj **ks = NULL;
    size_t n = 0, i;

    gtk_widget_set_visible(d->err_l, FALSE);
    gtk_widget_set_visible(d->pw_err, FALSE);
    gtk_label_set_text(GTK_LABEL(d->info_l), "");
    gtk_widget_set_sensitive(d->imp_btn, FALSE);
    d->info_err = HB_ERR_INVALID;
    if (!d->have_src) {
        gtk_widget_set_visible(d->pw_row, FALSE);
        return;
    }

    pw = gtk_editable_get_text(GTK_EDITABLE(d->pw_e));
    d->info_err = hb_key_inspect((const unsigned char *)d->src, d->src_len,
                                 pw && *pw ? pw : NULL, &d->info);

    if (d->info_err == HB_ERR_PASSPHRASE) {
        /* info 在 PASSPHRASE 下无有效 format；DER 总以 0x30 SEQUENCE 开头 */
        gboolean is_der = d->src_len > 0 &&
                          (unsigned char)d->src[0] == 0x30;
        if (pw && *pw) {
            gtk_label_set_text(GTK_LABEL(d->pw_err), "口令错误");
            gtk_widget_set_visible(d->pw_err, TRUE);
        }
        gtk_widget_set_visible(d->pw_row, TRUE);
        gtk_label_set_text(GTK_LABEL(d->pw_note),
            is_der ? "PKCS#12 密码（必需）" : "私钥口令");
        gtk_label_set_text(GTK_LABEL(d->info_l),
            "已识别为加密私钥 —— 请输入口令");
        gtk_widget_set_visible(d->pw_row, TRUE);
        return;
    }
    if (d->info_err != HB_OK) {
        gtk_label_set_text(GTK_LABEL(d->err_l),
            "不是受支持的私钥（OpenSSH / PEM / PKCS#8 / PKCS#12）");
        gtk_widget_set_visible(d->err_l, TRUE);
        gtk_widget_set_visible(d->pw_row, FALSE);
        return;
    }

    /* 识别成功 */
    {
        const char *kind = d->info.kind ? d->info.kind : "?";
        char tb[48];
        if (d->info.bits)
            g_snprintf(tb, sizeof tb, "%s %d%s · %s", kind, d->info.bits,
                       d->info.encrypted ? "（加密）" : "",
                       fmt_str(d->info.format));
        else
            g_snprintf(tb, sizeof tb, "%s%s · %s", kind,
                       d->info.encrypted ? "（加密）" : "",
                       fmt_str(d->info.format));
        desc = g_strdup(tb);
    }
    if (d->info.fingerprint) {
        mk = g_strdup_printf("%s\n指纹 %s", desc, d->info.fingerprint);
        g_free(desc);
        desc = mk;
    } else if (d->info.encrypted) {
        mk = g_strdup_printf("%s\n指纹首次使用后显示", desc);
        g_free(desc);
        desc = mk;
    }
    gtk_label_set_text(GTK_LABEL(d->info_l), desc);
    g_free(desc);

    need_pw = d->info.encrypted || d->info.format == HB_KEYFMT_PKCS12;
    gtk_widget_set_visible(d->pw_row, need_pw);
    if (need_pw) {
        gtk_label_set_text(GTK_LABEL(d->pw_note),
            d->info.format == HB_KEYFMT_PKCS12
                ? "PKCS#12 密码（必需）"
                : "可留空，首次连接时询问");
    }

    /* 重复检测：同指纹 */
    if (d->info.fingerprint) {
        ks = hb_store_list(d->st, "key", &n);
        for (i = 0; i < n; i++) {
            const char *fp = hb_obj_get_str(ks[i], "fingerprint");
            if (fp && *fp && !strcmp(fp, d->info.fingerprint)) {
                const char *onm = hb_obj_get_str(ks[i], "name");
                char *t = g_strdup_printf("该密钥已存在：%s",
                                          onm && *onm ? onm : "key");
                gtk_label_set_text(GTK_LABEL(d->err_l), t);
                g_free(t);
                gtk_widget_set_visible(d->err_l, TRUE);
                dup = TRUE;
                break;
            }
        }
        if (ks) hb_obj_list_free(ks, n);
    }
    /* PKCS12 必须口令正确后才到这里；encrypted 无 public 也允许导入 */
    gtk_widget_set_sensitive(d->imp_btn, !dup);
}

static void imp_text_changed(GtkTextBuffer *b, gpointer ud)
{
    ImpDlg *d = ud;
    GtkTextIter s, e;
    char *t;
    (void)b;
    if (d->inspecting) return;
    gtk_text_buffer_get_bounds(d->buf, &s, &e);
    t = gtk_text_buffer_get_text(d->buf, &s, &e, FALSE);
    imp_clear_src(d);
    if (t && *t) {
        d->src = t;
        d->src_len = strlen(t);
        d->have_src = TRUE;
        if (!*gtk_editable_get_text(GTK_EDITABLE(d->name_e)))
            gtk_editable_set_text(GTK_EDITABLE(d->name_e), "imported-key");
    } else {
        g_free(t);
    }
    imp_refresh(d);
}

static void imp_pw_changed(GtkEditable *e, gpointer ud)
{ (void)e; imp_refresh((ImpDlg *)ud); }

static void imp_file_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ImpDlg *d = ud;
    GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *data = NULL;
    gsize len = 0;
    GFileInfo *fi;
    gboolean ok;
    (void)src;
    if (!gf) return;
    /* 与 hb_key_inspect_file 同规则：仅普通文件且 ≤ 1 MiB */
    fi = g_file_query_info(gf,
                           G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                           G_FILE_ATTRIBUTE_STANDARD_SIZE,
                           G_FILE_QUERY_INFO_NONE, NULL, NULL);
    ok = fi && g_file_info_get_file_type(fi) == G_FILE_TYPE_REGULAR &&
         g_file_info_get_size(fi) > 0 &&
         g_file_info_get_size(fi) <= 1024 * 1024;
    g_clear_object(&fi);
    if (ok && g_file_load_contents(gf, NULL, &data, &len, NULL, NULL) &&
        len && len <= 1024 * 1024) {
        imp_clear_src(d);
        d->src = data;
        d->src_len = len;
        d->have_src = TRUE;
        d->src_path = g_file_get_path(gf);
        {
            char *base = g_path_get_basename(d->src_path);
            gtk_editable_set_text(GTK_EDITABLE(d->name_e), base);
            g_free(base);
        }
        /* 清掉粘贴区，避免双源 */
        d->inspecting = TRUE;
        gtk_text_buffer_set_text(d->buf, "", 0);
        d->inspecting = FALSE;
    } else {
        g_free(data);
        gtk_label_set_text(GTK_LABEL(d->err_l),
                           ok ? "无法读取该文件"
                              : "仅支持普通文件，且不超过 1 MiB");
        gtk_widget_set_visible(d->err_l, TRUE);
    }
    imp_refresh(d);
    g_object_unref(gf);
}

static void imp_pick(GtkButton *b, gpointer ud)
{
    ImpDlg *d = ud;
    GtkFileDialog *fd = gtk_file_dialog_new();
    GFile *home;
    char *sshdir;
    (void)b;
    sshdir = g_build_filename(g_get_home_dir(), ".ssh", NULL);
    if (g_file_test(sshdir, G_FILE_TEST_IS_DIR)) {
        home = g_file_new_for_path(sshdir);
        gtk_file_dialog_set_initial_folder(fd, home);
        g_object_unref(home);
    }
    g_free(sshdir);
    if (!d->fcancel)
        d->fcancel = g_cancellable_new();
    gtk_file_dialog_open(fd, NULL, d->fcancel, imp_file_done, d);
    g_object_unref(fd);
}

static void imp_save(GtkButton *b, gpointer ud)
{
    ImpDlg *d = ud;
    hb_vault *v;
    hb_obj *o;
    const char *nm, *pw;
    gboolean rem;
    char *t;
    (void)b;
    if (!d->have_src || d->info_err != HB_OK) return;

    v = hb_store_vault(d->st);
    nm = gtk_editable_get_text(GTK_EDITABLE(d->name_e));
    o = hb_obj_new("key");
    hb_obj_set_str(o, "name", nm && *nm ? nm : "imported-key");
    hb_obj_set_str(o, "kind", d->info.kind);
    if (d->info.bits) hb_obj_set_int(o, "bits", d->info.bits);
    if (d->info.public_line)
        hb_obj_set_str(o, "public", d->info.public_line);
    if (d->info.fingerprint)
        hb_obj_set_str(o, "fingerprint", d->info.fingerprint);
    if (d->info.comment)
        hb_obj_set_str(o, "comment", d->info.comment);
    hb_obj_set_str(o, "format", fmt_str(d->info.format));
    hb_obj_set_int(o, "created", g_get_real_time() / 1000);
    hb_vault_put(v, o);
    hb_vault_set_secret(v, hb_obj_id(o), "private", d->info.private_text);
    pw = gtk_editable_get_text(GTK_EDITABLE(d->pw_e));
    rem = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->pw_rem));
    if (pw && *pw && rem)
        hb_vault_set_secret(v, hb_obj_id(o), "passphrase", pw);
    t = g_strdup_printf("已导入 %s", hb_obj_get_str(o, "name"));
    hb_obj_free(o);
    hb_toast(GTK_WIDGET(d->dlg), t);
    g_free(t);
    adw_dialog_close(d->dlg);
}

static void imp_closed(AdwDialog *dlg, gpointer ud)
{
    ImpDlg *d = ud;
    (void)dlg;
    if (d->fcancel) {
        g_cancellable_cancel(d->fcancel);
        g_object_unref(d->fcancel);
        d->fcancel = NULL;
    }
    imp_clear_src(d);
    gtk_editable_set_text(GTK_EDITABLE(d->pw_e), "");
    /* 尽力清空粘贴区（GTK 不提供底层存储擦除） */
    if (d->buf)
        gtk_text_buffer_set_text(d->buf, "", 0);
    g_free(d);
}

static void open_import(HbKeychainView *v)
{
    ImpDlg *d = g_new0(ImpDlg, 1);
    GtkWidget *body, *foot, *l, *row, *f, *sw, *tv, *c;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(v));

    d->st = v->store;
    d->info_err = HB_ERR_INVALID;
    d->dlg = hb_modal_new("导入私钥", &body, &foot);

    /* 源 */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *pick = gtk_button_new_with_label("选择文件…");
        gtk_widget_add_css_class(pick, "hb-btn");
        g_signal_connect(pick, "clicked", G_CALLBACK(imp_pick), d);
        gtk_box_append(GTK_BOX(row), pick);
        GtkWidget *orl = gtk_label_new("或粘贴私钥内容（PEM / OpenSSH）：");
        gtk_widget_add_css_class(orl, "hb-dim");
        gtk_widget_set_valign(orl, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), orl);
    }
    gtk_box_append(GTK_BOX(body), row);

    d->buf = gtk_text_buffer_new(NULL);
    g_signal_connect(d->buf, "changed", G_CALLBACK(imp_text_changed), d);
    tv = gtk_text_view_new_with_buffer(d->buf);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_NONE);
    sw = gtk_scrolled_window_new();
    gtk_widget_add_css_class(sw, "codebox");
    gtk_widget_set_size_request(sw, -1, 110);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    gtk_box_append(GTK_BOX(body), sw);

    /* 名称 */
    l = gtk_label_new("名称");
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    d->name_e = gtk_entry_new();
    gtk_box_append(GTK_BOX(body), d->name_e);

    /* 识别结果 */
    d->info_l = gtk_label_new("");
    gtk_widget_add_css_class(d->info_l, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(d->info_l), 0);
    gtk_label_set_wrap(GTK_LABEL(d->info_l), TRUE);
    gtk_box_append(GTK_BOX(body), d->info_l);

    /* 口令行 */
    d->pw_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    {
        GtkWidget *pr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        d->pw_e = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(
            GTK_PASSWORD_ENTRY(d->pw_e), TRUE);
        gtk_widget_set_hexpand(d->pw_e, TRUE);
        g_signal_connect(d->pw_e, "changed",
                         G_CALLBACK(imp_pw_changed), d);
        gtk_box_append(GTK_BOX(pr), d->pw_e);
        d->pw_note = gtk_label_new("可留空，首次连接时询问");
        gtk_widget_add_css_class(d->pw_note, "hb-dim");
        gtk_widget_set_valign(d->pw_note, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(pr), d->pw_note);
        gtk_box_append(GTK_BOX(d->pw_row), pr);
        d->pw_rem = gtk_check_button_new_with_label(
            "记住口令（加密保存）");
        gtk_widget_add_css_class(d->pw_rem, "check-row");
        gtk_widget_set_halign(d->pw_rem, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(d->pw_row), d->pw_rem);
        d->pw_err = gtk_label_new("");
        gtk_widget_add_css_class(d->pw_err, "hb-red");
        gtk_label_set_xalign(GTK_LABEL(d->pw_err), 0);
        gtk_widget_set_visible(d->pw_err, FALSE);
        gtk_box_append(GTK_BOX(d->pw_row), d->pw_err);
    }
    gtk_widget_set_visible(d->pw_row, FALSE);
    gtk_box_append(GTK_BOX(body), d->pw_row);

    /* 错误 */
    d->err_l = gtk_label_new("");
    gtk_widget_add_css_class(d->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(d->err_l), 0);
    gtk_label_set_wrap(GTK_LABEL(d->err_l), TRUE);
    gtk_widget_set_visible(d->err_l, FALSE);
    gtk_box_append(GTK_BOX(body), d->err_l);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    c = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(c, "hb-btn");
    g_signal_connect_swapped(c, "clicked",
        G_CALLBACK(adw_dialog_close), d->dlg);
    gtk_box_append(GTK_BOX(f), c);
    d->imp_btn = gtk_button_new_with_label("导入");
    gtk_widget_add_css_class(d->imp_btn, "hb-btn");
    gtk_widget_add_css_class(d->imp_btn, "primary");
    gtk_widget_set_sensitive(d->imp_btn, FALSE);
    g_signal_connect(d->imp_btn, "clicked", G_CALLBACK(imp_save), d);
    gtk_box_append(GTK_BOX(f), d->imp_btn);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(d->dlg, "closed", G_CALLBACK(imp_closed), d);
    adw_dialog_present(d->dlg,
                       GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : NULL);
}

/* ------------------------------ 详情/编辑 ------------------------------ */

static char *randomart_for(hb_obj *k)
{
    const char *fp = hb_obj_get_str(k, "fingerprint");
    const char *b64;
    unsigned char dig[64];
    size_t dl = 0;
    char label[40];
    const char *kind;
    int bits;
    char *art;

    if (!fp || strncmp(fp, "SHA256:", 7)) return NULL;
    b64 = fp + 7;
    if (sodium_base642bin(dig, sizeof dig, b64, strlen(b64),
                          NULL, &dl, NULL,
                          sodium_base64_VARIANT_ORIGINAL_NO_PADDING) != 0)
        return NULL;
    kind = hb_obj_get_str(k, "kind");
    bits = (int)hb_obj_get_int(k, "bits", 0);
    if (!kind) kind = "SSH";
    if (!strcmp(kind, "ed25519"))
        g_snprintf(label, sizeof label, "ED25519 %d", bits ? bits : 256);
    else if (!strcmp(kind, "ecdsa"))
        g_snprintf(label, sizeof label, "ECDSA %d", bits);
    else if (!strcmp(kind, "rsa"))
        g_snprintf(label, sizeof label, "RSA %d", bits);
    else
        g_snprintf(label, sizeof label, "SSH %d", bits);
    art = hb_ssh_randomart(label, dig, dl);
    sodium_memzero(dig, sizeof dig);
    return art;
}

typedef struct {
    HbKeychainView *v;
    char           *key_id;
    hb_obj         *obj;      /* 已加载（编辑保存时 put 回去） */
    AdwDialog      *dlg;
    GtkWidget      *name_e, *comment_e, *confirm_sw;
    GtkWidget      *clear_pw;
} DetDlg;

static void det_closed(AdwDialog *dlg, gpointer ud)
{
    DetDlg *d = ud;
    (void)dlg;
    if (d->obj) hb_obj_free(d->obj);
    g_free(d->key_id);
    g_free(d);
}

static void det_clear_pw(GtkButton *b, gpointer ud)
{
    DetDlg *d = ud;
    (void)b;
    hb_vault_set_secret(hb_store_vault(d->v->store),
                        d->key_id, "passphrase", NULL);
    gtk_widget_set_visible(d->clear_pw, FALSE);
    hb_toast(GTK_WIDGET(d->dlg), "已清除保存的口令");
}

static void det_save(GtkButton *b, gpointer ud)
{
    DetDlg *d = ud;
    (void)b;
    if (d->obj) {
        hb_obj_set_str(d->obj, "name",
            gtk_editable_get_text(GTK_EDITABLE(d->name_e)));
        hb_obj_set_str(d->obj, "comment",
            gtk_editable_get_text(GTK_EDITABLE(d->comment_e)));
        hb_obj_set_bool(d->obj, "confirm",
            gtk_switch_get_active(GTK_SWITCH(d->confirm_sw)));
        hb_vault_put(hb_store_vault(d->v->store), d->obj);
    }
    adw_dialog_close(d->dlg);
    hb_toast(GTK_WIDGET(d->v), "密钥已保存");
}

static void det_del(GtkButton *b, gpointer ud)
{
    DetDlg *d = ud;
    HbKeychainView *v = d->v;
    char *kid = g_strdup(d->key_id);
    (void)b;
    adw_dialog_close(d->dlg);
    delete_key(v, kid);
    g_free(kid);
}

static void det_copy_pub(GtkButton *b, gpointer ud)
{
    DetDlg *d = ud;
    const char *pub;
    (void)b;
    pub = d->obj ? hb_obj_get_str(d->obj, "public") : NULL;
    if (pub && *pub) {
        copy_text(GTK_WIDGET(d->dlg), pub);
        hb_toast(GTK_WIDGET(d->dlg), "公钥已复制");
    }
}

static void det_export_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    char *pub = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    if (gf) {
        g_file_replace_contents(gf, pub, strlen(pub), NULL, FALSE,
            G_FILE_CREATE_NONE, NULL, NULL, NULL);
        g_object_unref(gf);
    }
    g_free(pub);
}

static void det_export_pub(GtkButton *b, gpointer ud)
{
    DetDlg *d = ud;
    const char *pub;
    char *name;
    GtkFileDialog *fd;
    (void)b;
    pub = d->obj ? hb_obj_get_str(d->obj, "public") : NULL;
    if (!pub || !*pub) return;
    fd = gtk_file_dialog_new();
    name = g_strdup_printf("%s.pub",
        hb_obj_get_str(d->obj, "name") ?: "key");
    gtk_file_dialog_set_initial_name(fd, name);
    g_free(name);
    gtk_file_dialog_save(fd, NULL, NULL, det_export_done,
                         g_strdup_printf("%s\n", pub));
    g_object_unref(fd);
}

static GtkWidget *det_row(const char *key, GtkWidget *val)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *k = gtk_label_new(key);
    gtk_widget_add_css_class(r, "hb-drawer-row");
    gtk_widget_add_css_class(k, "hb-drawer-key");
    gtk_label_set_xalign(GTK_LABEL(k), 0);
    gtk_widget_set_size_request(k, 88, -1);
    gtk_widget_set_valign(k, GTK_ALIGN_START);
    gtk_label_set_yalign(GTK_LABEL(k), 0.0);
    gtk_widget_set_hexpand(val, TRUE);
    gtk_box_append(GTK_BOX(r), k);
    gtk_box_append(GTK_BOX(r), val);
    return r;
}

static GtkWidget *det_val(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "hb-drawer-val");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    return l;
}

static void open_detail(HbKeychainView *v, const char *key_id)
{
    hb_vault *vault = hb_store_vault(v->store);
    DetDlg *d = g_new0(DetDlg, 1);
    GtkWidget *body, *foot, *l, *f, *c;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(v));
    const char *s;
    char tb[48];
    char *art;

    if (hb_vault_get(vault, key_id, &d->obj) != HB_OK || !d->obj)
        { g_free(d); return; }
    if (hb_obj_tombstone(d->obj))
        { hb_obj_free(d->obj); g_free(d); return; }

    d->v = v;
    d->key_id = g_strdup(key_id);
    d->dlg = hb_modal_new(hb_obj_get_str(d->obj, "name") ?: "密钥",
                          &body, &foot);

    /* 名称 / 注释 / 使用确认 —— 可编辑 */
    l = gtk_label_new("名称");
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    d->name_e = gtk_entry_new();
    s = hb_obj_get_str(d->obj, "name");
    gtk_editable_set_text(GTK_EDITABLE(d->name_e), s ? s : "");
    gtk_box_append(GTK_BOX(body), d->name_e);

    l = gtk_label_new("注释");
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    d->comment_e = gtk_entry_new();
    s = hb_obj_get_str(d->obj, "comment");
    gtk_editable_set_text(GTK_EDITABLE(d->comment_e), s ? s : "");
    gtk_box_append(GTK_BOX(body), d->comment_e);

    {
        GtkWidget *swr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *t = gtk_label_new("使用前确认");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_widget_set_hexpand(t, TRUE);
        gtk_box_append(GTK_BOX(swr), t);
        d->confirm_sw = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(d->confirm_sw),
            hb_obj_get_bool(d->obj, "confirm", false));
        gtk_widget_set_valign(d->confirm_sw, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(swr), d->confirm_sw);
        gtk_widget_add_css_class(swr, "check-row");
        gtk_box_append(GTK_BOX(body), swr);
    }

    /* 只读信息行 */
    g_snprintf(tb, sizeof tb, "%s · %s",
               key_type_label(d->obj, tb, sizeof tb),
               format_label(d->obj));
    {
        char buf[64];
        g_snprintf(buf, sizeof buf, "%s · %s",
                   key_type_label(d->obj, tb, sizeof tb),
                   format_label(d->obj));
        gtk_box_append(GTK_BOX(body), det_row("类型", det_val(buf)));
    }
    {
        int64_t cr = hb_obj_get_int(d->obj, "created", 0);
        char buf[64];
        if (cr > 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(cr / 1000);
            char *ds = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M")
                          : NULL;
            g_snprintf(buf, sizeof buf, "%s", ds ? ds : "—");
            g_free(ds);
            if (dt) g_date_time_unref(dt);
        } else g_strlcpy(buf, "—", sizeof buf);
        gtk_box_append(GTK_BOX(body), det_row("创建时间", det_val(buf)));
    }

    s = hb_obj_get_str(d->obj, "fingerprint");
    gtk_box_append(GTK_BOX(body),
        det_row("指纹", det_val(s && *s ? s : "（加密 · 首次使用后显示）")));
    art = randomart_for(d->obj);
    if (art) {
        GtkWidget *al = gtk_label_new(art);
        gtk_widget_add_css_class(al, "fingerprint-art");
        gtk_label_set_selectable(GTK_LABEL(al), TRUE);
        gtk_box_append(GTK_BOX(body), al);
        g_free(art);
    }

    /* 公钥 + 操作 */
    s = hb_obj_get_str(d->obj, "public");
    if (s && *s) {
        GtkWidget *tv, *sw, *br;
        GtkTextBuffer *tb2 = gtk_text_buffer_new(NULL);
        gtk_text_buffer_set_text(tb2, s, -1);
        tv = gtk_text_view_new_with_buffer(tb2);
        g_object_unref(tb2);
        gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
        sw = gtk_scrolled_window_new();
        gtk_widget_add_css_class(sw, "codebox");
        gtk_widget_set_size_request(sw, -1, 74);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
        gtk_box_append(GTK_BOX(body), det_row("公钥", sw));
        br = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(br, GTK_ALIGN_END);
        {
            GtkWidget *cb2 = gtk_button_new_with_label("复制公钥");
            gtk_widget_add_css_class(cb2, "hb-btn");
            gtk_widget_add_css_class(cb2, "sm");
            g_signal_connect(cb2, "clicked",
                G_CALLBACK(det_copy_pub), d);
            gtk_box_append(GTK_BOX(br), cb2);
            GtkWidget *eb = gtk_button_new_with_label("导出公钥…");
            gtk_widget_add_css_class(eb, "hb-btn");
            gtk_widget_add_css_class(eb, "sm");
            g_signal_connect(eb, "clicked",
                G_CALLBACK(det_export_pub), d);
            gtk_box_append(GTK_BOX(br), eb);
        }
        gtk_box_append(GTK_BOX(body), br);
    }

    /* 清除已保存口令 */
    {
        char *pw = hb_vault_get_secret(vault, key_id, "passphrase");
        d->clear_pw = gtk_button_new_with_label("清除已保存口令");
        gtk_widget_add_css_class(d->clear_pw, "hb-link");
        gtk_widget_set_halign(d->clear_pw, GTK_ALIGN_START);
        g_signal_connect(d->clear_pw, "clicked",
                         G_CALLBACK(det_clear_pw), d);
        gtk_widget_set_visible(d->clear_pw, pw != NULL);
        gtk_box_append(GTK_BOX(body), d->clear_pw);
        if (pw) hb_secret_free(pw);
    }

    /* 引用主机 */
    {
        hb_obj **hs = NULL;
        size_t n = 0, i;
        GString *names = g_string_new(NULL);
        hs = hb_store_list(v->store, "host", &n);
        for (i = 0; i < n; i++) {
            const char *kid = hb_obj_get_str(hs[i], "key_id");
            if (kid && !strcmp(kid, key_id)) {
                const char *hn = hb_obj_get_str(hs[i], "name");
                if (names->len) g_string_append(names, "、");
                g_string_append(names, hn && *hn ? hn : "?");
            }
        }
        if (hs) hb_obj_list_free(hs, n);
        gtk_box_append(GTK_BOX(body),
            det_row("被引用",
                det_val(names->len ? names->str : "（无主机使用）")));
        g_string_free(names, TRUE);
    }

    /* footer：删除(左) | 取消 保存(右) */
    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    {
        GtkWidget *del = gtk_button_new_with_label("删除");
        gtk_widget_add_css_class(del, "hb-btn");
        gtk_widget_add_css_class(del, "danger");
        g_signal_connect(del, "clicked", G_CALLBACK(det_del), d);
        gtk_box_append(GTK_BOX(f), del);
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(f), sp);
        c = gtk_button_new_with_label("取消");
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect_swapped(c, "clicked",
            G_CALLBACK(adw_dialog_close), d->dlg);
        gtk_box_append(GTK_BOX(f), c);
        GtkWidget *sv = gtk_button_new_with_label("保存");
        gtk_widget_add_css_class(sv, "hb-btn");
        gtk_widget_add_css_class(sv, "primary");
        g_signal_connect(sv, "clicked", G_CALLBACK(det_save), d);
        gtk_box_append(GTK_BOX(f), sv);
    }
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(d->dlg, "closed", G_CALLBACK(det_closed), d);
    adw_dialog_present(d->dlg,
                       GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : NULL);
}

/* ------------------------------ 导出全部公钥 ------------------------------ */

static void expall_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbKeychainView *v = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    GString *out;
    size_t i, n = 0;
    char *t;
    if (!gf) return;
    out = g_string_new(NULL);
    for (i = 0; i < v->nkeys; i++) {
        const char *pub = hb_obj_get_str(v->keys[i], "public");
        const char *nm = hb_obj_get_str(v->keys[i], "name");
        if (!pub || !*pub) continue;
        g_string_append_printf(out, "# %s\n%s\n",
                               nm && *nm ? nm : "key", pub);
        n++;
    }
    g_file_replace_contents(gf, out->str, out->len, NULL, FALSE,
        G_FILE_CREATE_NONE, NULL, NULL, NULL);
    g_object_unref(gf);
    g_string_free(out, TRUE);
    t = g_strdup_printf("已导出 %d 个公钥（私钥不会导出）", (int)n);
    hb_toast(GTK_WIDGET(v), t);
    g_free(t);
}

static void export_all_pubkeys(HbKeychainView *v)
{
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(fd, "harbor_pubkeys.txt");
    gtk_file_dialog_save(fd, NULL, NULL, expall_done, v);
    g_object_unref(fd);
}

/* ------------------------------ actions ------------------------------ */

static void act_newkey(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    hb_keychain_generate_dialog(GTK_WIDGET(ud),
        HB_KEYCHAIN_VIEW(ud)->store, NULL, NULL);
}
static void act_import(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    open_import((HbKeychainView *)ud);
}
static void act_export(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    export_all_pubkeys((HbKeychainView *)ud);
}

static void add_view_actions(HbKeychainView *v)
{
    GSimpleActionGroup *g = g_simple_action_group_new();
    GSimpleAction *a;
    a = g_simple_action_new("newkey", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_newkey), v);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("importkey", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_import), v);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    a = g_simple_action_new("exportpub", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act_export), v);
    g_action_map_add_action(G_ACTION_MAP(g), G_ACTION(a));
    gtk_widget_insert_action_group(GTK_WIDGET(v), "key",
                                   G_ACTION_GROUP(g));
    g_object_unref(g);
}

/* ------------------------------ 视图构建 ------------------------------ */

static void on_newkey_click(GtkButton *b, gpointer ud)
{
    (void)b;
    hb_keychain_generate_dialog(GTK_WIDGET(ud),
        HB_KEYCHAIN_VIEW(ud)->store, NULL, NULL);
}

static GtkWidget *build_toolbar(HbKeychainView *v)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *spacer;

    /* New key 分割按钮 */
    {
        GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *newb, *menub;
        GMenu *m = g_menu_new();
        gtk_widget_add_css_class(split, "linked");
        newb = gtk_button_new_with_label("New key");
        gtk_widget_add_css_class(newb, "hb-btn");
        gtk_widget_add_css_class(newb, "primary");
        g_signal_connect(newb, "clicked", G_CALLBACK(on_newkey_click), v);
        gtk_box_append(GTK_BOX(split), newb);
        menub = gtk_menu_button_new();
        gtk_menu_button_set_label(GTK_MENU_BUTTON(menub), "▾");
        gtk_widget_add_css_class(menub, "hb-btn");
        gtk_widget_add_css_class(menub, "primary");
        g_menu_append(m, "生成新密钥", "key.newkey");
        g_menu_append(m, "导入私钥…", "key.importkey");
        g_menu_append(m, "导出全部公钥…", "key.exportpub");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menub),
                                       G_MENU_MODEL(m));
        g_object_unref(m);
        gtk_box_append(GTK_BOX(split), menub);
        gtk_box_append(GTK_BOX(bar), split);
    }

    /* ghost：Certificate / FIDO2（后续版本） */
    {
        const char *txt[] = { "Certificate", "FIDO2" };
        const char *ico[] = { "cert", "fido" };
        int i;
        for (i = 0; i < 2; i++) {
            GtkWidget *b = gtk_button_new();
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            gtk_box_append(GTK_BOX(r), hb_icon_new(ico[i], 13));
            gtk_box_append(GTK_BOX(r), gtk_label_new(txt[i]));
            gtk_button_set_child(GTK_BUTTON(b), r);
            gtk_widget_add_css_class(b, "hb-btn");
            gtk_widget_set_sensitive(b, FALSE);
            gtk_widget_set_tooltip_text(b, "后续版本提供");
            gtk_box_append(GTK_BOX(bar), b);
        }
    }

    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    v->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
                                          "搜索密钥…");
    gtk_widget_set_size_request(v->search, 220, -1);
    gtk_widget_set_valign(v->search, GTK_ALIGN_CENTER);
    g_signal_connect(v->search, "search-changed",
                     G_CALLBACK(on_search), v);
    gtk_box_append(GTK_BOX(bar), v->search);

    gtk_widget_add_css_class(bar, "hb-toolbar");
    return bar;
}

static GtkWidget *build_table_head(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    struct { const char *txt; int w; gboolean grow; } cols[] = {
        { "名称",     160,        TRUE  },
        { "类型",     COL_TYPE_W, FALSE },
        { "指纹",     COL_FP_W,   FALSE },
        { "使用确认", COL_CONF_W, FALSE },
        { "被引用",   COL_REF_W,  FALSE },
        { "",         COL_ACT_W,  FALSE },
    };
    int i;
    gtk_widget_add_css_class(row, "hb-table-head");
    for (i = 0; i < 6; i++) {
        GtkWidget *l = gtk_label_new(cols[i].txt);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_margin_start(l, cols[i].grow ? 14 : 14);
        if (i == 3) gtk_widget_set_halign(l, GTK_ALIGN_START);
        if (cols[i].grow) gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_set_size_request(l, cols[i].w, -1);
        gtk_box_append(GTK_BOX(row), l);
    }
    return row;
}

static GtkWidget *build_empty(HbKeychainView *v, gboolean nomatch)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *ico = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ic = hb_icon_new("key", 24);
    GtkWidget *t, *s;

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
    gtk_box_append(GTK_BOX(box), ico);

    t = gtk_label_new(nomatch ? "无匹配密钥" : "Keychain 中还没有密钥");
    gtk_widget_add_css_class(t, "hb-e-title");
    gtk_box_append(GTK_BOX(box), t);
    s = gtk_label_new(nomatch ? "试试其他关键词，或清除筛选"
                              : "生成或导入一个私钥用于主机认证");
    gtk_widget_add_css_class(s, "hb-e-sub");
    gtk_box_append(GTK_BOX(box), s);

    if (!nomatch) {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *g, *im;
        gtk_widget_set_halign(r, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(r, 8);
        g = gtk_button_new_with_label("生成新密钥");
        gtk_widget_add_css_class(g, "hb-btn");
        gtk_widget_add_css_class(g, "primary");
        g_signal_connect(g, "clicked", G_CALLBACK(on_newkey_click), v);
        gtk_box_append(GTK_BOX(r), g);
        im = gtk_button_new_with_label("导入私钥");
        gtk_widget_add_css_class(im, "hb-btn");
        g_signal_connect_swapped(im, "clicked",
            G_CALLBACK(open_import), v);
        gtk_box_append(GTK_BOX(r), im);
        gtk_box_append(GTK_BOX(box), r);
    }
    return box;
}

/* ------------------------------ 生命周期 ------------------------------ */

static void hb_keychain_view_dispose(GObject *o)
{
    HbKeychainView *v = HB_KEYCHAIN_VIEW(o);
    if (v->store && v->changed_handler)
        g_signal_handler_disconnect(v->store, v->changed_handler);
    v->changed_handler = 0;
    if (v->pending_reload) {
        g_source_remove(v->pending_reload);
        v->pending_reload = 0;
    }
    if (v->keys) {
        hb_obj_list_free(v->keys, v->nkeys);
        v->keys = NULL;
    }
    G_OBJECT_CLASS(hb_keychain_view_parent_class)->dispose(o);
}

static void hb_keychain_view_class_init(HbKeychainViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_keychain_view_dispose;
}

static void hb_keychain_view_init(HbKeychainView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
}

GtkWidget *hb_keychain_view_new(HbStore *st)
{
    HbKeychainView *v = g_object_new(HB_TYPE_KEYCHAIN_VIEW, NULL);
    GtkWidget *toolbar, *table, *scrolled, *hint, *hl;

    v->store = st;

    toolbar = build_toolbar(v);
    gtk_widget_set_margin_start(toolbar, 16);
    gtk_widget_set_margin_end(toolbar, 16);
    gtk_widget_set_margin_top(toolbar, 14);
    gtk_box_append(GTK_BOX(v), toolbar);

    v->body = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(v->body),
                                  GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_vexpand(v->body, TRUE);
    gtk_widget_set_margin_start(v->body, 16);
    gtk_widget_set_margin_end(v->body, 16);
    gtk_widget_set_margin_top(v->body, 10);

    /* 表 */
    table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(table, "hb-table");
    gtk_box_append(GTK_BOX(table), build_table_head());
    v->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), v->rows);
    gtk_box_append(GTK_BOX(table), scrolled);
    gtk_stack_add_named(GTK_STACK(v->body), table, "table");

    gtk_stack_add_named(GTK_STACK(v->body), build_empty(v, FALSE), "empty");
    gtk_stack_add_named(GTK_STACK(v->body), build_empty(v, TRUE), "nomatch");
    gtk_box_append(GTK_BOX(v), v->body);

    /* hint */
    hint = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (hb_store_dek_fallback(st)) {
        gtk_widget_add_css_class(hint, "hintbox");
        gtk_widget_add_css_class(hint, "amber");
        hl = gtk_label_new(
            "未检测到系统密钥环，数据密钥保存在本机 0600 文件中");
    } else {
        gtk_widget_add_css_class(hint, "hintbox");
        gtk_widget_add_css_class(hint, "plain");
        hl = gtk_label_new(
            "私钥以 XChaCha20-Poly1305 加密存于本机 Vault，数据密钥由"
            "系统密钥环（libsecret）保护；明文仅在连接时进入锁定内存"
            "并随即擦除。");
    }
    gtk_box_append(GTK_BOX(hint), hb_icon_new("info", 15));
    gtk_label_set_wrap(GTK_LABEL(hl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_widget_set_hexpand(hl, TRUE);
    gtk_box_append(GTK_BOX(hint), hl);
    gtk_widget_set_margin_start(hint, 16);
    gtk_widget_set_margin_end(hint, 16);
    gtk_widget_set_margin_top(hint, 6);
    gtk_widget_set_margin_bottom(hint, 14);
    gtk_box_append(GTK_BOX(v), hint);

    add_view_actions(v);
    v->changed_handler = g_signal_connect(st, "changed",
        G_CALLBACK(on_store_changed), v);

    reload(v);
    return GTK_WIDGET(v);
}
