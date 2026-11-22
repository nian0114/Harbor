/* FR-11 账号与同步页：登录/注册/恢复 + 已登录账号面板（设备、范围、冲突、
 * TOTP、恢复码、改密、退出）。全部经 HbSyncService 异步操作。 */

#include "hb-account-view.h"
#include "hb-sync.h"
#include "hb-icons.h"
#include "hb-dialogs.h"

#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <glib/gi18n.h>

#define DEFAULT_SERVER "http://127.0.0.1:8787"

struct _HbAccountView {
    GtkBox     parent;
    HbStore   *store;
    HbSyncService *svc;
    GtkWidget *content;      /* scrolled 内的列容器 */
    HbSyncState built_for;   /* 已构建的形态 */
    gulong     svc_sig;
    /* 登录表单（signed_out 态保持） */
    GtkWidget *srv_e, *em_e, *pw_e, *totp_row, *totp_e, *err_l;
    GPtrArray *devices;      /* 最近一次拉到的设备列表 */
    guint      dev_src;
};

G_DEFINE_TYPE(HbAccountView, hb_account_view, GTK_TYPE_BOX)

static void rebuild_in(HbAccountView *v);
static void ask_merge(HbAccountView *v);
static void show_words_dialog(HbAccountView *v, const char *words);
static void refresh_devices(HbAccountView *v);
static gboolean refresh_devices_cb(gpointer p);
static void dev_free_pub(HbSyncDevice *d);

/* ============================ 小构件 ============================ */

static GtkWidget *panel(void)
{
    GtkWidget *p = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(p, "panel");
    return p;
}

static GtkWidget *h4(const char *txt)
{
    GtkWidget *l = gtk_label_new(txt);
    gtk_widget_add_css_class(l, "set-h4");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *psub(const char *txt)
{
    GtkWidget *l = gtk_label_new(txt);
    gtk_widget_add_css_class(l, "p-sub");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    return l;
}

static GtkWidget *hint(const char *txt, const char *cls)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l;
    gtk_widget_add_css_class(b, "hintbox");
    gtk_widget_add_css_class(b, cls);
    gtk_box_append(GTK_BOX(b), hb_icon_new("shield", 16));
    l = gtk_label_new(txt);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(b), l);
    return b;
}

static GtkWidget *field(const char *label, gboolean secret, GtkWidget **e_out)
{
    GtkWidget *f = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l = gtk_label_new(label);
    GtkWidget *e = gtk_entry_new();
    gtk_widget_add_css_class(f, "acct-field");
    gtk_widget_add_css_class(l, "acct-field-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(f), l);
    if (secret)
        gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_box_append(GTK_BOX(f), e);
    if (e_out) *e_out = e;
    return f;
}

static GtkWidget *btn(const char *txt, const char *css)
{
    GtkWidget *b = gtk_button_new_with_label(txt);
    gtk_widget_add_css_class(b, "hb-btn");
    if (css) gtk_widget_add_css_class(b, css);
    return b;
}

static void toastv(HbAccountView *v, const char *t)
{
    hb_toast(GTK_WIDGET(v), t);
}

/* ============================ words 网格 ============================ */

static char **split_words(const char *words, int *n_out)
{
    char **w = g_strsplit(words ? words : "", " ", -1);
    int n = 0;
    while (w && w[n] && *w[n]) n++;
    if (n_out) *n_out = n;
    return w;
}

/* 24 词编号网格（只读展示 + 复制按钮在 footer 由调用方加） */
static GtkWidget *words_grid(const char *words)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 4);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gtk_widget_add_css_class(g, "acct-words");
    {
        char **w = split_words(words, NULL);
        int i, n = 0;
        while (w && w[n]) n++;
        for (i = 0; i < n; i++) {
            char *txt = g_strdup_printf("%2d. %s", i + 1, w[i]);
            GtkWidget *l = gtk_label_new(txt);
            gtk_widget_add_css_class(l, "mono");
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_grid_attach(GTK_GRID(g), l, i % 4, i / 4, 1, 1);
            g_free(txt);
        }
        g_strfreev(w);
    }
    return g;
}

static void copy_words(GtkButton *b, gpointer ud)
{
    GdkClipboard *cl = gtk_widget_get_clipboard(GTK_WIDGET(b));
    const char *txt = ud ? ud : g_object_get_data(G_OBJECT(b), "txt");
    if (txt) gdk_clipboard_set_text(cl, txt);
}

/* ============================ 登录态（signed out） ============================ */

static void show_err(HbAccountView *v, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(v->err_l), msg ? msg : "");
    gtk_widget_set_visible(v->err_l, msg && *msg);
}

static void login_cb(HbSyncService *s, const HbSyncOpResult *r, gpointer ud)
{
    HbAccountView *v = ud;
    (void)s;
    if (r->ok) {
        if (r->need_merge)
            ask_merge(v);
        show_err(v, NULL);
        return;
    }
    if (r->code && !strcmp(r->code, "totp_required")) {
        gtk_widget_set_visible(v->totp_row, TRUE);
        show_err(v, _("需要两步验证码"));
        gtk_widget_grab_focus(v->totp_e);
        return;
    }
    if (r->code && !strcmp(r->code, "rate_limited") && r->retry_after > 0) {
        char *m = g_strdup_printf(ngettext("尝试次数过多，请 %d 秒后再试", "尝试次数过多，请 %d 秒后再试",
                                  r->retry_after), r->retry_after);
        show_err(v, m);
        g_free(m);
        return;
    }
    show_err(v, r->message ? r->message : _("登录失败"));
}

static void do_login(GtkButton *b, gpointer ud)
{
    HbAccountView *v = ud;
    const char *srv = gtk_editable_get_text(GTK_EDITABLE(v->srv_e));
    const char *em = gtk_editable_get_text(GTK_EDITABLE(v->em_e));
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(v->pw_e));
    const char *tp = gtk_editable_get_text(GTK_EDITABLE(v->totp_e));
    g_autofree char *nsrv = NULL;
    (void)b;
    nsrv = hb_sync_check_server(srv);
    if (!nsrv) {
        show_err(v, _("服务器地址仅支持 https:// 或回环 http://"));
        return;
    }
    if (!*em || !*pw) {
        show_err(v, _("请填写邮箱与密码"));
        return;
    }
    show_err(v, NULL);
    hb_sync_service_login(v->svc, nsrv, em, pw, tp, login_cb, v);
    gtk_editable_set_text(GTK_EDITABLE(v->pw_e), "");
    gtk_editable_set_text(GTK_EDITABLE(v->totp_e), "");
}

/* ============================ merge 询问 ============================ */

typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
} MergeCtx;

static void merge_pick(GtkButton *b, gpointer ud)
{
    MergeCtx *m = ud;
    gboolean merge = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(b), "merge"));
    hb_sync_service_confirm_merge(m->v->svc, merge);
    adw_dialog_close(m->dlg);
    g_free(m);
}

static void ask_merge(HbAccountView *v)
{
    GtkWidget *body, *foot;
    AdwDialog *d = hb_modal_new(_("首次登录"), &body, &foot);
    MergeCtx *m = g_new0(MergeCtx, 1);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mb, *db;
    m->v = v;
    m->dlg = d;
    gtk_box_append(GTK_BOX(body),
        psub(_("本机 Vault 中已有数据。合并后将上传加密副本并与云端保持一致；"
             "丢弃则以云端内容覆盖本机同步范围内的数据。")));
    mb = btn(_("合并本机数据到账号"), "primary");
    db = btn(_("丢弃本机数据，使用云端"), "danger");
    g_object_set_data(G_OBJECT(mb), "merge", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(db), "merge", GINT_TO_POINTER(0));
    g_signal_connect(mb, "clicked", G_CALLBACK(merge_pick), m);
    g_signal_connect(db, "clicked", G_CALLBACK(merge_pick), m);
    gtk_box_append(GTK_BOX(row), mb);
    gtk_box_append(GTK_BOX(row), db);
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(d, GTK_WIDGET(v));
}

/* ============================ 注册对话框 ============================ */

typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
    GtkWidget *em_e, *pw_e, *pw2_e, *err_l;
    char      *server;
} RegCtx;

static void reg_free(RegCtx *r)
{
    g_free(r->server);
    g_free(r);
}

static void reg_cb(HbSyncService *s, const HbSyncOpResult *r, gpointer ud)
{
    RegCtx *rc = ud;
    (void)s;
    if (!r->ok) {
        const char *msg = r->message ? r->message : _("注册失败");
        if (r->code && !strcmp(r->code, "rate_limited") &&
            r->retry_after > 0) {
            char *m = g_strdup_printf(ngettext("尝试次数过多，请 %d 秒后再试", "尝试次数过多，请 %d 秒后再试",
                                      r->retry_after), r->retry_after);
            gtk_label_set_text(GTK_LABEL(rc->err_l), m);
            g_free(m);
        } else {
            gtk_label_set_text(GTK_LABEL(rc->err_l), msg);
        }
        return;
    }
    adw_dialog_close(rc->dlg);
    show_words_dialog(rc->v, r->words);
    reg_free(rc);
}

static void reg_go(GtkButton *b, gpointer ud)
{
    RegCtx *r = ud;
    const char *em = gtk_editable_get_text(GTK_EDITABLE(r->em_e));
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(r->pw_e));
    const char *p2 = gtk_editable_get_text(GTK_EDITABLE(r->pw2_e));
    (void)b;
    if (!*em) { gtk_label_set_text(GTK_LABEL(r->err_l), _("请填写邮箱")); return; }
    if (strlen(pw) < 8) {
        gtk_label_set_text(GTK_LABEL(r->err_l), _("密码至少 8 位"));
        return;
    }
    if (strcmp(pw, p2)) {
        gtk_label_set_text(GTK_LABEL(r->err_l), _("两次密码不一致"));
        return;
    }
    gtk_label_set_text(GTK_LABEL(r->err_l), "");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    hb_sync_service_register(r->v->svc, r->server, em, pw, reg_cb, r);
    gtk_editable_set_text(GTK_EDITABLE(r->pw_e), "");
    gtk_editable_set_text(GTK_EDITABLE(r->pw2_e), "");
}

static void reg_cancel(GtkButton *b, gpointer ud)
{
    RegCtx *r = ud;
    (void)b;
    adw_dialog_close(r->dlg);
    reg_free(r);
}

static void open_register(HbAccountView *v)
{
    const char *srv = gtk_editable_get_text(GTK_EDITABLE(v->srv_e));
    g_autofree char *nsrv = hb_sync_check_server(srv);
    GtkWidget *body, *foot, *row;
    RegCtx *r;
    if (!nsrv) {
        show_err(v, _("服务器地址仅支持 https:// 或回环 http://"));
        return;
    }
    r = g_new0(RegCtx, 1);
    r->v = v;
    r->server = g_steal_pointer(&nsrv);
    r->dlg = hb_modal_new(_("注册新账号"), &body, &foot);
    gtk_box_append(GTK_BOX(body),
        hint(_("零知识：Harbor 服务器只存加密数据，无法帮你找回或重置密码。"
             "忘记密码且没有 24 词恢复码或其他已登录设备时，数据不可恢复。"),
             "amber"));
    gtk_box_append(GTK_BOX(body), field(_("邮箱"), FALSE, &r->em_e));
    gtk_box_append(GTK_BOX(body), field(_("主密码"), TRUE, &r->pw_e));
    gtk_box_append(GTK_BOX(body), field(_("确认密码"), TRUE, &r->pw2_e));
    r->err_l = gtk_label_new("");
    gtk_widget_add_css_class(r->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(r->err_l), 0);
    gtk_box_append(GTK_BOX(body), r->err_l);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *cb = btn(_("取消"), NULL);
        GtkWidget *ob = btn(_("注册"), "primary");
        g_signal_connect(cb, "clicked", G_CALLBACK(reg_cancel), r);
        g_signal_connect(ob, "clicked", G_CALLBACK(reg_go), r);
        gtk_box_append(GTK_BOX(row), cb);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(r->dlg, GTK_WIDGET(v));
}

/* ============================ 恢复码展示 + 确认 ============================ */

typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
    char      *words;     /* 持有，确认后擦除 */
    char     **warr;
    int        pos[3];
    GtkWidget *e[3];
    GtkWidget *err_l;
} WordsCtx;

static void words_ctx_free(WordsCtx *w)
{
    if (w->words) {
        memset(w->words, 0, strlen(w->words));
        g_free(w->words);
    }
    g_strfreev(w->warr);
    g_free(w);
}

static void words_done(GtkButton *b, gpointer ud);
static void words_check(GtkButton *b, gpointer ud)
{
    WordsCtx *w = ud;
    int i;
    (void)b;
    for (i = 0; i < 3; i++) {
        const char *t = gtk_editable_get_text(GTK_EDITABLE(w->e[i]));
        g_autofree char *tt = g_strdup(t ? t : "");
        g_strstrip(tt);
        if (!*tt || g_ascii_strcasecmp(tt, w->warr[w->pos[i]])) {
            gtk_label_set_text(GTK_LABEL(w->err_l),
                               _("词不匹配，请核对恢复码"));
            return;
        }
    }
    words_done(NULL, w);
}

static void words_done(GtkButton *b, gpointer ud)
{
    WordsCtx *w = ud;
    (void)b;
    adw_dialog_close(w->dlg);
    words_ctx_free(w);
}

/* 展示 24 词 →「我已保存」→ 3 个随机位置确认 → 完成 */
static void words_confirm_page(WordsCtx *w)
{
    GtkWidget *body, *foot;
    AdwDialog *d = hb_modal_new(_("确认恢复码"), &body, &foot);
    int i;
    char txt[64];
    GtkWidget *row;
    w->dlg = d;
    gtk_box_append(GTK_BOX(body),
        psub(_("请按编号填写对应的词，确认你已正确抄写。")));
    for (i = 0; i < 3; i++) {
        snprintf(txt, sizeof txt, _("第 %d 个词"), w->pos[i] + 1);
        gtk_box_append(GTK_BOX(body), field(txt, FALSE, &w->e[i]));
    }
    w->err_l = gtk_label_new("");
    gtk_widget_add_css_class(w->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(w->err_l), 0);
    gtk_box_append(GTK_BOX(body), w->err_l);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *ob = btn(_("确认"), "primary");
        g_signal_connect(ob, "clicked", G_CALLBACK(words_check), w);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(d, GTK_WIDGET(w->v));
}

static void words_next(GtkButton *b, gpointer ud)
{
    WordsCtx *w = ud;
    int i, j;
    (void)b;
    adw_dialog_close(w->dlg);
    /* 随机 3 个不同位置 */
    for (i = 0; i < 3; i++) {
        w->pos[i] = g_random_int_range(0, 24);
        for (j = 0; j < i; j++)
            if (w->pos[j] == w->pos[i]) { i--; break; }
    }
    words_confirm_page(w);
}

static void show_words_dialog(HbAccountView *v, const char *words)
{
    GtkWidget *body, *foot, *row;
    AdwDialog *d;
    WordsCtx *w = g_new0(WordsCtx, 1);
    w->v = v;
    w->words = g_strdup(words);
    w->warr = split_words(words, NULL);
    w->dlg = d = hb_modal_new(_("恢复码（请离线抄写）"), &body, &foot);
    gtk_box_append(GTK_BOX(body),
        hint(_("忘记密码时，24 词恢复码是找回 Vault 的唯一途径——"
             "服务端零知识，无法帮你重置。请离线抄写并妥善保存。"),
             "amber"));
    gtk_box_append(GTK_BOX(body), words_grid(words));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *cp = btn(_("复制"), NULL);
        GtkWidget *nb = btn(_("我已保存"), "primary");
        g_object_set_data_full(G_OBJECT(cp), "txt", g_strdup(words),
                               g_free);
        g_signal_connect(cp, "clicked", G_CALLBACK(copy_words), NULL);
        g_signal_connect(nb, "clicked", G_CALLBACK(words_next), w);
        gtk_box_append(GTK_BOX(row), cp);
        gtk_box_append(GTK_BOX(row), nb);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(d, GTK_WIDGET(v));
}

/* ============================ 恢复对话框 ============================ */

typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
    GtkWidget *em_e, *words_e, *pw_e, *pw2_e, *err_l;
    char      *server;
} RecCtx;

static void rec_free(RecCtx *r)
{
    g_free(r->server);
    g_free(r);
}

static void rec_cb(HbSyncService *s, const HbSyncOpResult *r, gpointer ud)
{
    RecCtx *rc = ud;
    (void)s;
    if (!r->ok) {
        gtk_label_set_text(GTK_LABEL(rc->err_l),
                           r->message ? r->message : _("恢复失败"));
        return;
    }
    adw_dialog_close(rc->dlg);
    if (r->need_merge)
        ask_merge(rc->v);
    rec_free(rc);
}

static void rec_go(GtkButton *b, gpointer ud)
{
    RecCtx *r = ud;
    const char *em = gtk_editable_get_text(GTK_EDITABLE(r->em_e));
    const char *wd = gtk_editable_get_text(GTK_EDITABLE(r->words_e));
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(r->pw_e));
    const char *p2 = gtk_editable_get_text(GTK_EDITABLE(r->pw2_e));
    int n = 0;
    char **w;
    (void)b;
    w = split_words(wd, &n);
    g_strfreev(w);
    if (n != 24) {
        gtk_label_set_text(GTK_LABEL(r->err_l), _("请输入全部 24 个词"));
        return;
    }
    if (strlen(pw) < 8) {
        gtk_label_set_text(GTK_LABEL(r->err_l), _("新密码至少 8 位"));
        return;
    }
    if (strcmp(pw, p2)) {
        gtk_label_set_text(GTK_LABEL(r->err_l), _("两次密码不一致"));
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    hb_sync_service_recover(r->v->svc, r->server, em, wd, pw, rec_cb, r);
    gtk_editable_set_text(GTK_EDITABLE(r->pw_e), "");
    gtk_editable_set_text(GTK_EDITABLE(r->pw2_e), "");
}

static void rec_cancel(GtkButton *b, gpointer ud)
{
    RecCtx *r = ud;
    (void)b;
    adw_dialog_close(r->dlg);
    rec_free(r);
}

static void open_recover(HbAccountView *v)
{
    const char *srv = gtk_editable_get_text(GTK_EDITABLE(v->srv_e));
    g_autofree char *nsrv = hb_sync_check_server(srv);
    GtkWidget *body, *foot, *row;
    RecCtx *r;
    if (!nsrv) {
        show_err(v, _("服务器地址仅支持 https:// 或回环 http://"));
        return;
    }
    r = g_new0(RecCtx, 1);
    r->v = v;
    r->server = g_steal_pointer(&nsrv);
    r->dlg = hb_modal_new(_("用恢复码恢复"), &body, &foot);
    gtk_box_append(GTK_BOX(body),
        hint(_("没有恢复码、也没有其他已登录设备时，数据无法恢复（零知识）。"),
             "red"));
    gtk_box_append(GTK_BOX(body), field(_("邮箱"), FALSE, &r->em_e));
    gtk_box_append(GTK_BOX(body), field(_("24 词恢复码（空格分隔）"), FALSE,
                                       &r->words_e));
    gtk_box_append(GTK_BOX(body), field(_("新密码"), TRUE, &r->pw_e));
    gtk_box_append(GTK_BOX(body), field(_("确认新密码"), TRUE, &r->pw2_e));
    r->err_l = gtk_label_new("");
    gtk_widget_add_css_class(r->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(r->err_l), 0);
    gtk_box_append(GTK_BOX(body), r->err_l);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *cb = btn(_("取消"), NULL);
        GtkWidget *ob = btn(_("恢复"), "primary");
        g_signal_connect(cb, "clicked", G_CALLBACK(rec_cancel), r);
        g_signal_connect(ob, "clicked", G_CALLBACK(rec_go), r);
        gtk_box_append(GTK_BOX(row), cb);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(r->dlg, GTK_WIDGET(v));
}

/* ============================ 已登录：各操作 ============================ */

/* 修改密码对话框 */
typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
    GtkWidget *o_e, *n_e, *n2_e, *err_l;
} PwCtx;

static void pw_cb(HbSyncService *s, const HbSyncOpResult *r, gpointer ud)
{
    PwCtx *c = ud;
    (void)s;
    if (!r->ok) {
        gtk_label_set_text(GTK_LABEL(c->err_l),
                           r->message ? r->message : _("修改失败"));
        return;
    }
    toastv(c->v, _("密码已修改"));
    adw_dialog_close(c->dlg);
    g_free(c);
}

static void pw_go(GtkButton *b, gpointer ud)
{
    PwCtx *c = ud;
    const char *o = gtk_editable_get_text(GTK_EDITABLE(c->o_e));
    const char *n = gtk_editable_get_text(GTK_EDITABLE(c->n_e));
    const char *n2 = gtk_editable_get_text(GTK_EDITABLE(c->n2_e));
    (void)b;
    if (strlen(n) < 8) {
        gtk_label_set_text(GTK_LABEL(c->err_l), _("新密码至少 8 位"));
        return;
    }
    if (strcmp(n, n2)) {
        gtk_label_set_text(GTK_LABEL(c->err_l), _("两次密码不一致"));
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    hb_sync_service_change_password(c->v->svc, o, n, pw_cb, c);
    gtk_editable_set_text(GTK_EDITABLE(c->o_e), "");
    gtk_editable_set_text(GTK_EDITABLE(c->n_e), "");
    gtk_editable_set_text(GTK_EDITABLE(c->n2_e), "");
}

static void open_change_pw(HbAccountView *v)
{
    GtkWidget *body, *foot, *row;
    PwCtx *c = g_new0(PwCtx, 1);
    c->v = v;
    c->dlg = hb_modal_new(_("修改密码"), &body, &foot);
    gtk_box_append(GTK_BOX(body), field(_("当前密码"), TRUE, &c->o_e));
    gtk_box_append(GTK_BOX(body), field(_("新密码"), TRUE, &c->n_e));
    gtk_box_append(GTK_BOX(body), field(_("确认新密码"), TRUE, &c->n2_e));
    c->err_l = gtk_label_new("");
    gtk_widget_add_css_class(c->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(c->err_l), 0);
    gtk_box_append(GTK_BOX(body), c->err_l);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *cb = btn(_("取消"), NULL);
        GtkWidget *ob = btn(_("修改"), "primary");
        g_signal_connect_swapped(cb, "clicked",
                                 G_CALLBACK(adw_dialog_close), c->dlg);
        g_signal_connect(ob, "clicked", G_CALLBACK(pw_go), c);
        g_signal_connect_swapped(cb, "clicked", G_CALLBACK(g_free), c);
        gtk_box_append(GTK_BOX(row), cb);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(c->dlg, GTK_WIDGET(v));
}

/* 恢复码轮换：密码确认 → rotate → 展示新词 */
static void rot_cb(HbSyncService *s, const HbSyncOpResult *r, gpointer ud)
{
    HbAccountView *v = ud;
    (void)s;
    if (!r->ok) {
        toastv(v, r->message ? r->message : _("恢复码轮换失败"));
        return;
    }
    show_words_dialog(v, r->words);
}

static void rot_pw_cb(const char *secret, gboolean remember, gpointer ud)
{
    HbAccountView *v = ud;
    (void)remember;
    if (!secret) return;
    hb_sync_service_rotate_recovery(v->svc, secret, rot_cb, v);
}

static void open_rotate(HbAccountView *v)
{
    hb_dialog_password(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(v))),
                       _("输入主密码以轮换恢复码"), FALSE, FALSE,
                       rot_pw_cb, v);
}

/* TOTP 管理 */
static void totp_setup_cb(HbSyncService *s, const HbSyncOpResult *r,
                          gpointer ud);
static void totp_enable_cb(HbSyncService *s, const HbSyncOpResult *r,
                           gpointer ud)
{
    HbAccountView *v = ud;
    (void)s;
    if (!r->ok) {
        toastv(v, r->message ? r->message : _("两步验证启用失败"));
        return;
    }
    toastv(v, _("两步验证已开启"));
}

typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
    GtkWidget *e;
    GtkWidget *err_l;
    gboolean   enable;
} TotpCtx;

static void totp_code_go(GtkButton *b, gpointer ud)
{
    TotpCtx *t = ud;
    const char *code = gtk_editable_get_text(GTK_EDITABLE(t->e));
    (void)b;
    if (t->enable)
        hb_sync_service_totp_enable(t->v->svc, code, totp_enable_cb,
                                    t->v);
    else
        hb_sync_service_totp_disable(t->v->svc, code, totp_enable_cb,
                                     t->v);
    adw_dialog_close(t->dlg);
    g_free(t);
}

static void totp_setup_cb(HbSyncService *s, const HbSyncOpResult *r,
                          gpointer ud)
{
    HbAccountView *v = ud;
    GtkWidget *body, *foot, *row;
    (void)s;
    TotpCtx *t;
    if (!r->ok) {
        toastv(v, r->message ? r->message : _("获取两步验证密钥失败"));
        return;
    }
    t = g_new0(TotpCtx, 1);
    t->v = v;
    t->enable = TRUE;
    t->dlg = hb_modal_new(_("开启两步验证"), &body, &foot);
    gtk_box_append(GTK_BOX(body),
        psub(_("在认证器中添加以下密钥（TOTP · SHA1 · 6 位 · 30 秒）。")));
    {
        GtkWidget *b1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l = gtk_label_new(r->totp_secret);
        GtkWidget *cp = btn(_("复制"), NULL);
        gtk_widget_add_css_class(l, "mono");
        gtk_label_set_selectable(GTK_LABEL(l), TRUE);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(b1), l);
        g_object_set_data_full(G_OBJECT(cp), "txt",
                               g_strdup(r->totp_secret), g_free);
        g_signal_connect(cp, "clicked", G_CALLBACK(copy_words), NULL);
        gtk_box_append(GTK_BOX(b1), cp);
        gtk_box_append(GTK_BOX(body), b1);
    }
    {
        GtkWidget *l = gtk_label_new(r->totp_uri);
        gtk_widget_add_css_class(l, "mono");
        gtk_widget_add_css_class(l, "hb-dim");
        gtk_label_set_selectable(GTK_LABEL(l), TRUE);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(body), l);
    }
    gtk_box_append(GTK_BOX(body), field(_("输入当前验证码以启用"), FALSE,
                                       &t->e));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *ob = btn(_("启用"), "primary");
        g_signal_connect(ob, "clicked", G_CALLBACK(totp_code_go), t);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(t->dlg, GTK_WIDGET(v));
}

static void open_totp(HbAccountView *v)
{
    if (hb_sync_service_totp_enabled(v->svc)) {
        /* disable：输入 code */
        GtkWidget *body, *foot, *row;
        TotpCtx *t = g_new0(TotpCtx, 1);
        t->v = v;
        t->dlg = hb_modal_new(_("关闭两步验证"), &body, &foot);
        gtk_box_append(GTK_BOX(body), field(_("验证码"), FALSE, &t->e));
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        {
            GtkWidget *ob = btn(_("关闭"), "danger");
            g_signal_connect(ob, "clicked", G_CALLBACK(totp_code_go), t);
            gtk_box_append(GTK_BOX(row), ob);
        }
        gtk_box_append(GTK_BOX(foot), row);
        adw_dialog_present(t->dlg, GTK_WIDGET(v));
    } else {
        hb_sync_service_totp_setup(v->svc, totp_setup_cb, v);
    }
}

/* 退出对话框：保留本地 / 擦除并退出 */
typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
} OutCtx;

static void out_pick(GtkButton *b, gpointer ud)
{
    OutCtx *o = ud;
    gboolean wipe = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "w"));
    hb_sync_service_logout(o->v->svc, wipe);
    adw_dialog_close(o->dlg);
    g_free(o);
}

static void open_logout(HbAccountView *v)
{
    GtkWidget *body, *foot, *row;
    OutCtx *o = g_new0(OutCtx, 1);
    o->v = v;
    o->dlg = hb_modal_new(_("退出账号"), &body, &foot);
    gtk_box_append(GTK_BOX(body),
        psub(_("退出后停止同步，云端密文保留。本地 Vault 可选择保留或擦除。")));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *kb = btn(_("保留本地退出"), NULL);
        GtkWidget *wb = btn(_("擦除并退出"), "danger");
        g_object_set_data(G_OBJECT(kb), "w", GINT_TO_POINTER(0));
        g_object_set_data(G_OBJECT(wb), "w", GINT_TO_POINTER(1));
        g_signal_connect(kb, "clicked", G_CALLBACK(out_pick), o);
        g_signal_connect(wb, "clicked", G_CALLBACK(out_pick), o);
        gtk_box_append(GTK_BOX(row), kb);
        gtk_box_append(GTK_BOX(row), wb);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(o->dlg, GTK_WIDGET(v));
}

/* 设备吊销：确认 + wipe 勾选 */
typedef struct {
    HbAccountView *v;
    AdwDialog *dlg;
    char      *dev_id;
    GtkWidget *wipe_cb;
    gboolean   wipe;
} RevCtx;

static void rev_go(GtkButton *b, gpointer ud)
{
    RevCtx *r = ud;
    (void)b;
    r->wipe = gtk_check_button_get_active(GTK_CHECK_BUTTON(r->wipe_cb));
    hb_sync_service_revoke(r->v->svc, r->dev_id, r->wipe, NULL, NULL);
    adw_dialog_close(r->dlg);
    g_free(r->dev_id);
    g_free(r);
    refresh_devices(r->v);
}

static void open_revoke(HbAccountView *v, const char *dev_id,
                        const char *name)
{
    GtkWidget *body, *foot, *row;
    RevCtx *r = g_new0(RevCtx, 1);
    char msg[256];
    r->v = v;
    r->dev_id = g_strdup(dev_id);
    r->dlg = hb_modal_new(_("吊销设备"), &body, &foot);
    snprintf(msg, sizeof msg, _("吊销设备「%s」？该设备将立即断开同步。"),
             name ? name : dev_id);
    gtk_box_append(GTK_BOX(body), psub(msg));
    r->wipe_cb = gtk_check_button_new_with_label(
        _("同时远程擦除该设备上的数据"));
    gtk_box_append(GTK_BOX(body), r->wipe_cb);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *cb = btn(_("取消"), NULL);
        GtkWidget *ob = btn(_("吊销"), "danger");
        g_signal_connect_swapped(cb, "clicked",
                                 G_CALLBACK(adw_dialog_close), r->dlg);
        g_signal_connect(ob, "clicked", G_CALLBACK(rev_go), r);
        gtk_box_append(GTK_BOX(row), cb);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(foot), row);
    adw_dialog_present(r->dlg, GTK_WIDGET(v));
}

static void on_revoke_btn(GtkButton *b, gpointer ud)
{
    (void)ud;
    HbAccountView *v = g_object_get_data(G_OBJECT(b), "v");
    const char *id = g_object_get_data(G_OBJECT(b), "id");
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    open_revoke(v, id, name);
}

/* ============================ 设备列表 ============================ */

static void devices_cb(HbSyncService *s, const HbSyncOpResult *r,
                       gpointer ud)
{
    HbAccountView *v = ud;
    (void)s;
    if (r->ok && r->devices) {
        guint i;
        if (v->devices) {
            for (i = 0; i < v->devices->len; i++)
                dev_free_pub(g_ptr_array_index(v->devices, i));
            g_ptr_array_free(v->devices, TRUE);
        }
        v->devices = g_ptr_array_new();
        for (i = 0; i < r->devices->len; i++) {
            HbSyncDevice *d = g_ptr_array_index(r->devices, i);
            HbSyncDevice *c = g_new0(HbSyncDevice, 1);
            c->id = g_strdup(d->id);
            c->name = g_strdup(d->name);
            c->platform = g_strdup(d->platform);
            c->created = d->created;
            c->last_seen = d->last_seen;
            c->current = d->current;
            g_ptr_array_add(v->devices, c);
        }
    }
    rebuild_in(v);
}

void refresh_devices(HbAccountView *v)
{
    hb_sync_service_devices(v->svc, devices_cb, v);
}

static void dev_free_pub(HbSyncDevice *d)
{
    g_free(d->id); g_free(d->name); g_free(d->platform); g_free(d);
}

static char *fmt_time(gint64 ms)
{
    GDateTime *t = g_date_time_new_from_unix_local(ms / 1000);
    char *s = t ? g_date_time_format(t, "%m-%d %H:%M") : g_strdup("—");
    if (t) g_date_time_unref(t);
    return s;
}

static GtkWidget *device_row(HbAccountView *v, HbSyncDevice *d)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *nm, *sub;
    g_autofree char *seen = NULL;
    gtk_widget_add_css_class(row, "acct-row");
    nm = gtk_label_new(d->name ? d->name : _("设备"));
    gtk_widget_add_css_class(nm, "acct-dname");
    gtk_label_set_xalign(GTK_LABEL(nm), 0);
    gtk_box_append(GTK_BOX(vb), nm);
    seen = fmt_time(d->last_seen);
    {
        g_autofree char *t = g_strdup_printf(
            _("%s · 上次活动 %s%s"),
            d->platform ? d->platform : "linux", seen,
            d->current ? "" : "");
        sub = gtk_label_new(t);
    }
    gtk_widget_add_css_class(sub, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_append(GTK_BOX(vb), sub);
    gtk_box_append(GTK_BOX(row), vb);
    if (d->current) {
        GtkWidget *tag = gtk_label_new(_("当前"));
        gtk_widget_add_css_class(tag, "hb-tag");
        gtk_widget_add_css_class(tag, "tun");
        gtk_box_append(GTK_BOX(row), tag);
    } else {
        GtkWidget *b = btn(_("吊销"), "danger");
        gtk_widget_add_css_class(b, "hb-btn-small");
        g_object_set_data_full(G_OBJECT(b), "id", g_strdup(d->id), g_free);
        g_object_set_data_full(G_OBJECT(b), "name",
                               g_strdup(d->name), g_free);
        g_object_set_data(G_OBJECT(b), "v", v);
        g_signal_connect(b, "clicked", G_CALLBACK(on_revoke_btn), NULL);
        gtk_widget_set_halign(b, GTK_ALIGN_END);
        gtk_widget_set_hexpand(b, TRUE);
        gtk_box_append(GTK_BOX(row), b);
    }
    return row;
}

/* ============================ 冲突面板 ============================ */

static void conflict_keep(GtkButton *b, gpointer ud)
{
    (void)ud;
    HbAccountView *v = g_object_get_data(G_OBJECT(b), "v");
    const char *id = g_object_get_data(G_OBJECT(b), "id");
    hb_sync_conflict_keep_local(hb_sync_service_sync(v->svc), id);
    rebuild_in(v);
}

static void conflict_take(GtkButton *b, gpointer ud)
{
    (void)ud;
    HbAccountView *v = g_object_get_data(G_OBJECT(b), "v");
    const char *id = g_object_get_data(G_OBJECT(b), "id");
    hb_sync_conflict_take_other(hb_sync_service_sync(v->svc), id);
    hb_sync_service_sync_now(v->svc);
    rebuild_in(v);
}

/* 冲突字段值：从 data JSON 中取任意类型成员的字符串表示。 */
static char *jmember_str(JsonObject *o, const char *field)
{
    JsonNode *n = o ? json_object_get_member(o, field) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE(n))
        return NULL;
    {
        GType t = json_node_get_value_type(n);
        if (t == G_TYPE_STRING)
            return g_strdup(json_node_get_string(n));
        if (t == G_TYPE_INT64)
            return g_strdup_printf("%" G_GINT64_FORMAT,
                                   json_node_get_int(n));
        if (t == G_TYPE_BOOLEAN)
            return g_strdup(json_node_get_boolean(n) ? "true" : "false");
        if (t == G_TYPE_DOUBLE)
            return g_strdup_printf("%g", json_node_get_double(n));
    }
    return NULL;
}

static JsonObject *parse_obj(const char *json)
{
    g_autoptr(JsonParser) p = json_parser_new();
    JsonNode *root;
    if (!json || !json_parser_load_from_data(p, json, -1, NULL))
        return NULL;
    root = json_parser_get_root(p);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    return json_object_ref(json_node_get_object(root));
}

static GtkWidget *conflict_card(HbAccountView *v, hb_sync_conflict *c)
{
    GtkWidget *card = panel();
    GtkWidget *hd;
    g_autofree char *title = NULL;
    g_autofree char *cur_json = NULL;
    g_autoptr(JsonObject) cur_o = NULL;
    g_autoptr(JsonObject) loser_o = NULL;
    g_autoptr(JsonObject) fields_o = NULL;
    hb_obj *cur = NULL;
    title = g_strdup_printf("%s · %.8s", c->type, c->id);
    hd = h4(title);
    gtk_box_append(GTK_BOX(card), hd);
    if (hb_vault_get(hb_store_vault(v->store), c->id, &cur) == HB_OK &&
        cur) {
        cur_json = hb_obj_to_json(cur);
        cur_o = parse_obj(cur_json);
        hb_obj_free(cur);
    }
    loser_o = parse_obj(c->loser_summary);
    {
        g_autoptr(JsonParser) p = json_parser_new();
        if (json_parser_load_from_data(p, c->fields_json, -1, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *fa = json_node_get_array(root);
                guint i, n = json_array_get_length(fa);
                for (i = 0; i < n; i++) {
                    const char *f = json_array_get_string_element(fa, i);
                    gboolean secret = f && (f[0] == '$' ||
                        strstr(f, "secret") || strstr(f, "key") ||
                        strstr(f, "password"));
                    GtkWidget *fr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
                    GtkWidget *fl, *hdr, *row2, *a, *b2, *cur_b, *oth_b;
                    g_autofree char *cv = NULL, *ov = NULL;
                    fl = gtk_label_new(f);
                    gtk_widget_add_css_class(fl, "hb-dim");
                    gtk_label_set_xalign(GTK_LABEL(fl), 0);
                    hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                    a = gtk_label_new(_("当前"));
                    b2 = gtk_label_new(_("另一版本"));
                    gtk_widget_add_css_class(a, "hb-dim");
                    gtk_widget_add_css_class(b2, "hb-dim");
                    gtk_widget_set_hexpand(a, TRUE);
                    gtk_widget_set_hexpand(b2, TRUE);
                    gtk_label_set_xalign(GTK_LABEL(a), 0);
                    gtk_label_set_xalign(GTK_LABEL(b2), 0);
                    gtk_box_append(GTK_BOX(hdr), a);
                    gtk_box_append(GTK_BOX(hdr), b2);
                    if (secret) {
                        cv = g_strdup(_("••• (已更改)"));
                        ov = g_strdup(_("••• (已更改)"));
                    } else {
                        cv = jmember_str(cur_o, f);
                        ov = jmember_str(loser_o, f);
                    }
                    row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                    cur_b = gtk_label_new(cv ? cv : "—");
                    oth_b = gtk_label_new(ov ? ov : "—");
                    gtk_widget_add_css_class(row2,
                                           "acct-conflict-vals");
                    gtk_widget_set_hexpand(cur_b, TRUE);
                    gtk_widget_set_hexpand(oth_b, TRUE);
                    gtk_label_set_xalign(GTK_LABEL(cur_b), 0);
                    gtk_label_set_xalign(GTK_LABEL(oth_b), 0);
                    gtk_label_set_wrap(GTK_LABEL(cur_b), TRUE);
                    gtk_label_set_wrap(GTK_LABEL(oth_b), TRUE);
                    gtk_box_append(GTK_BOX(row2), cur_b);
                    gtk_box_append(GTK_BOX(row2), oth_b);
                    gtk_box_append(GTK_BOX(fr), fl);
                    gtk_box_append(GTK_BOX(fr), hdr);
                    gtk_box_append(GTK_BOX(fr), row2);
                    gtk_box_append(GTK_BOX(card), fr);
                }
            }
        }
    }
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *kb = btn(_("保留当前"), NULL);
        GtkWidget *tb = btn(_("使用另一版本"), "primary");
        g_object_set_data(G_OBJECT(kb), "v", v);
        g_object_set_data(G_OBJECT(tb), "v", v);
        g_object_set_data_full(G_OBJECT(kb), "id", g_strdup(c->id),
                               g_free);
        g_object_set_data_full(G_OBJECT(tb), "id", g_strdup(c->id),
                               g_free);
        g_signal_connect(kb, "clicked", G_CALLBACK(conflict_keep),
                         NULL);
        g_signal_connect(tb, "clicked", G_CALLBACK(conflict_take),
                         NULL);
        gtk_box_append(GTK_BOX(row), kb);
        gtk_box_append(GTK_BOX(row), tb);
        gtk_box_append(GTK_BOX(card), row);
    }
    return card;
}

/* ============================ scope ============================ */

static const struct { const char *scope, *label; } SCOPES_UI[] = {
    { "hosts",    N_("主机与分组") },
    { "keys",     N_("密钥与凭据（E2EE）") },
    { "snippets", N_("Snippets") },
    { "forwards", N_("端口转发规则") },
    { "known",    N_("Known Hosts") },
    { "settings", N_("外观与设置") },
};

static void on_scope_switch(GObject *sw, GParamSpec *ps, gpointer ud)
{
    HbAccountView *v = ud;
    const char *scope = g_object_get_data(sw, "scope");
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    (void)ps;
    hb_sync_service_set_scope(v->svc, scope, on);
}

static GtkWidget *scope_panel(HbAccountView *v)
{
    GtkWidget *p = panel();
    size_t i;
    gtk_box_append(GTK_BOX(p), h4(_("同步范围")));
    gtk_box_append(GTK_BOX(p),
        psub(_("关闭的项目不参与同步。冲突采用版本向量 + LWW 合并。")));
    for (i = 0; i < G_N_ELEMENTS(SCOPES_UI); i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l = gtk_label_new(_(SCOPES_UI[i].label));
        GtkWidget *sw = gtk_switch_new();
        gtk_widget_add_css_class(row, "acct-row");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_switch_set_active(GTK_SWITCH(sw),
            hb_sync_scope_get(hb_sync_service_sync(v->svc),
                              SCOPES_UI[i].scope));
        g_object_set_data_full(G_OBJECT(sw), "scope",
                               g_strdup(SCOPES_UI[i].scope), g_free);
        g_signal_connect(sw, "notify::active",
                         G_CALLBACK(on_scope_switch), v);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), sw);
        gtk_box_append(GTK_BOX(p), row);
    }
    /* Shell History：暂不支持 */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l = gtk_label_new(_("Shell History（暂不支持）"));
        GtkWidget *sw = gtk_switch_new();
        gtk_widget_add_css_class(row, "acct-row");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_set_sensitive(sw, FALSE);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), sw);
        gtk_box_append(GTK_BOX(p), row);
    }
    return p;
}

/* ============================ 页面构建 ============================ */

static GtkWidget *build_signed_out(HbAccountView *v)
{
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *p = panel();
    GtkWidget *row;
    gtk_widget_set_halign(p, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(p, 460, -1);
    gtk_widget_set_margin_top(p, 26);
    gtk_box_append(GTK_BOX(p), h4(_("登录 Harbor 账号")));
    gtk_box_append(GTK_BOX(p),
        psub(_("登录后从云端拉取密文并在本地解密，恢复全部 Vault 与同步。")));
    gtk_box_append(GTK_BOX(p), field(_("服务器"), FALSE, &v->srv_e));
    gtk_editable_set_text(GTK_EDITABLE(v->srv_e), DEFAULT_SERVER);
    gtk_box_append(GTK_BOX(p), field(_("邮箱"), FALSE, &v->em_e));
    gtk_box_append(GTK_BOX(p), field(_("主密码"), TRUE, &v->pw_e));
    v->totp_row = field(_("TOTP 验证码"), FALSE, &v->totp_e);
    gtk_widget_set_visible(v->totp_row, FALSE);
    gtk_box_append(GTK_BOX(p), v->totp_row);
    v->err_l = gtk_label_new("");
    gtk_widget_add_css_class(v->err_l, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(v->err_l), 0);
    gtk_box_append(GTK_BOX(p), v->err_l);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    {
        GtkWidget *lb = btn(_("登录并同步"), "primary");
        GtkWidget *rb = btn(_("注册新账号"), NULL);
        GtkWidget *cb = btn(_("用恢复码恢复"), NULL);
        g_signal_connect(lb, "clicked", G_CALLBACK(do_login), v);
        g_signal_connect_swapped(rb, "clicked",
                                 G_CALLBACK(open_register), v);
        g_signal_connect_swapped(cb, "clicked",
                                 G_CALLBACK(open_recover), v);
        gtk_box_append(GTK_BOX(row), lb);
        gtk_box_append(GTK_BOX(row), rb);
        gtk_box_append(GTK_BOX(row), cb);
    }
    gtk_box_append(GTK_BOX(p), row);
    gtk_box_append(GTK_BOX(p),
        hint(_("零知识架构：密码错误则无法解密 Vault，服务端不能帮你重置。"),
             "plain"));
    gtk_box_append(GTK_BOX(wrap), p);
    return wrap;
}

static GtkWidget *e2ee_banner(HbAccountView *v)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t1, *t2, *rb;
    gtk_widget_add_css_class(b, "e2ee-banner");
    gtk_box_append(GTK_BOX(b), hb_icon_new("lock", 22));
    t1 = gtk_label_new(_("端到端加密同步已开启 — 服务端零知识"));
    gtk_widget_add_css_class(t1, "e2ee-t1");
    gtk_label_set_xalign(GTK_LABEL(t1), 0);
    gtk_box_append(GTK_BOX(vb), t1);
    t2 = gtk_label_new(_("主密钥由密码经 Argon2id 派生；Vault 数据在设备端用 "
                       "XChaCha20-Poly1305 加密后才上传。服务端仅存密文。"));
    gtk_widget_add_css_class(t2, "e2ee-t2");
    gtk_label_set_xalign(GTK_LABEL(t2), 0);
    gtk_label_set_wrap(GTK_LABEL(t2), TRUE);
    gtk_box_append(GTK_BOX(vb), t2);
    gtk_box_append(GTK_BOX(b), vb);
    rb = btn(_("恢复码"), NULL);
    gtk_widget_add_css_class(rb, "hb-btn-small");
    gtk_widget_set_halign(rb, GTK_ALIGN_END);
    gtk_widget_set_hexpand(rb, TRUE);
    g_signal_connect_swapped(rb, "clicked", G_CALLBACK(open_rotate), v);
    gtk_box_append(GTK_BOX(b), rb);
    return b;
}

static char *status_line(HbAccountView *v)
{
    HbSyncState st = hb_sync_service_state(v->svc);
    if (st == HB_SYNC_IDLE && hb_sync_service_last_sync(v->svc)) {
        GDateTime *t = g_date_time_new_from_unix_local(
            hb_sync_service_last_sync(v->svc));
        char *f = t ? g_date_time_format(t, _("已同步 · %H:%M")) : NULL;
        if (t) g_date_time_unref(t);
        return f ? f : g_strdup(_("已同步"));
    }
    return g_strdup(hb_sync_service_status(v->svc));
}

static GtkWidget *account_panel(HbAccountView *v)
{
    GtkWidget *p = panel();
    GtkWidget *row, *vb, *nm, *sub;
    g_autofree char *sl = NULL;
    const char *em = hb_sync_service_email(v->svc);
    gtk_box_append(GTK_BOX(p), h4(_("账号")));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 11);
    {
        GtkWidget *av = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *il;
        char ini[8] = "?";
        if (em && *em) g_snprintf(ini, sizeof ini, "%c", g_ascii_toupper(*em));
        gtk_widget_add_css_class(av, "hb-avatar");
        gtk_widget_set_size_request(av, 34, 34);
        il = gtk_label_new(ini);
        gtk_widget_set_halign(il, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(il, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(il, TRUE);
        gtk_box_append(GTK_BOX(av), il);
        gtk_box_append(GTK_BOX(row), av);
    }
    vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    nm = gtk_label_new(em ? em : "—");
    gtk_widget_add_css_class(nm, "acct-dname");
    gtk_label_set_xalign(GTK_LABEL(nm), 0);
    gtk_box_append(GTK_BOX(vb), nm);
    sl = status_line(v);
    {
        g_autofree char *t = g_strdup_printf(
            "%s · TOTP %s · %s",
            hb_sync_service_totp_enabled(v->svc) ? _("两步验证已开启") : _("两步验证关闭"),
            sl, "");
        (void)t;
    }
    {
        g_autofree char *t = g_strdup_printf(
            "TOTP %s · %s",
            hb_sync_service_totp_enabled(v->svc) ? _("已开启") : _("未开启"), sl);
        sub = gtk_label_new(t);
    }
    gtk_widget_add_css_class(sub, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_append(GTK_BOX(vb), sub);
    gtk_box_append(GTK_BOX(row), vb);
    {
        GtkWidget *ob = btn(_("退出"), NULL);
        gtk_widget_add_css_class(ob, "hb-btn-small");
        gtk_widget_set_halign(ob, GTK_ALIGN_END);
        gtk_widget_set_hexpand(ob, TRUE);
        g_signal_connect_swapped(ob, "clicked", G_CALLBACK(open_logout), v);
        gtk_box_append(GTK_BOX(row), ob);
    }
    gtk_box_append(GTK_BOX(p), row);
    /* 操作行 */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *sy = btn(_("立即同步"), NULL);
        GtkWidget *pw = btn(_("修改密码"), NULL);
        GtkWidget *tt = btn(_("两步验证"), NULL);
        gtk_widget_add_css_class(sy, "hb-btn-small");
        gtk_widget_add_css_class(pw, "hb-btn-small");
        gtk_widget_add_css_class(tt, "hb-btn-small");
        g_signal_connect_swapped(sy, "clicked",
            G_CALLBACK(hb_sync_service_sync_now), v->svc);
        g_signal_connect_swapped(pw, "clicked",
                                 G_CALLBACK(open_change_pw), v);
        g_signal_connect_swapped(tt, "clicked", G_CALLBACK(open_totp), v);
        gtk_box_append(GTK_BOX(row), sy);
        gtk_box_append(GTK_BOX(row), pw);
        gtk_box_append(GTK_BOX(row), tt);
    }
    gtk_box_append(GTK_BOX(p), row);
    return p;
}

static GtkWidget *devices_panel(HbAccountView *v)
{
    GtkWidget *p = panel();
    guint i;
    gtk_box_append(GTK_BOX(p), h4(_("已登录设备")));
    if (!v->devices || !v->devices->len) {
        gtk_box_append(GTK_BOX(p), psub(_("正在获取设备列表…")));
    } else {
        for (i = 0; i < v->devices->len; i++) {
            gtk_box_append(GTK_BOX(p),
                device_row(v, g_ptr_array_index(v->devices, i)));
        }
    }
    return p;
}

static GtkWidget *conflicts_panel(HbAccountView *v)
{
    GtkWidget *p = panel();
    hb_sync_conflict *cs = NULL;
    size_t n = 0, i;
    gtk_box_append(GTK_BOX(p), h4(_("同步冲突")));
    gtk_box_append(GTK_BOX(p),
        psub(_("以下字段在两个设备上被同时修改，请选择保留哪个版本。")));
    if (hb_sync_conflicts(hb_sync_service_sync(v->svc), &cs, &n) == HB_OK) {
        for (i = 0; i < n; i++)
            gtk_box_append(GTK_BOX(p), conflict_card(v, &cs[i]));
        if (cs) hb_sync_conflicts_free(cs, n);
    }
    if (!n)
        gtk_box_append(GTK_BOX(p), psub(_("暂无冲突")));
    return p;
}

static GtkWidget *build_signed_in(HbAccountView *v)
{
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *grid;
    gtk_widget_add_css_class(wrap, "acct-wrap");
    gtk_box_append(GTK_BOX(wrap), e2ee_banner(v));
    grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(grid, "acct-grid");
    {
        GtkWidget *col1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
        GtkWidget *col2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
        gtk_widget_set_hexpand(col1, TRUE);
        gtk_widget_set_hexpand(col2, TRUE);
        gtk_box_append(GTK_BOX(col1), account_panel(v));
        gtk_box_append(GTK_BOX(col1), devices_panel(v));
        gtk_box_append(GTK_BOX(col2), scope_panel(v));
        if (hb_sync_service_conflicts(v->svc))
            gtk_box_append(GTK_BOX(col2), conflicts_panel(v));
        gtk_box_append(GTK_BOX(grid), col1);
        gtk_box_append(GTK_BOX(grid), col2);
    }
    gtk_box_append(GTK_BOX(wrap), grid);
    return wrap;
}

static void rebuild_in(HbAccountView *v)
{
    GtkWidget *child;
    HbSyncState st = hb_sync_service_state(v->svc);
    gboolean in = st != HB_SYNC_SIGNED_OUT;
    if (v->built_for == st && !in)
        return;                     /* signed-out 表单保持输入 */
    if (!in) {
        if (v->built_for == st) return;
        v->built_for = st;
        while ((child = gtk_widget_get_first_child(v->content)))
            gtk_box_remove(GTK_BOX(v->content), child);
        gtk_box_append(GTK_BOX(v->content), build_signed_out(v));
        return;
    }
    v->built_for = st;
    while ((child = gtk_widget_get_first_child(v->content)))
        gtk_box_remove(GTK_BOX(v->content), child);
    gtk_box_append(GTK_BOX(v->content), build_signed_in(v));
    if (!v->dev_src)
        v->dev_src = g_timeout_add(300, refresh_devices_cb, v);
}

static gboolean refresh_devices_cb(gpointer p)
{
    HbAccountView *v = p;
    v->dev_src = 0;
    refresh_devices(v);
    return G_SOURCE_REMOVE;
}

static void on_svc_changed(HbSyncService *s, gpointer ud)
{
    HbAccountView *v = ud;
    (void)s;
    rebuild_in(v);
}

static void hb_account_view_dispose(GObject *o)
{
    HbAccountView *v = HB_ACCOUNT_VIEW(o);
    if (v->dev_src) g_source_remove(v->dev_src);
    g_signal_handlers_disconnect_by_data(v->svc, v);
    G_OBJECT_CLASS(hb_account_view_parent_class)->dispose(o);
}

static void hb_account_view_class_init(HbAccountViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = hb_account_view_dispose;
}

static void hb_account_view_init(HbAccountView *v)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);
}

#ifndef NDEBUG
/* dev：从任意 widget 找到首个 HbAccountView 并弹恢复码网格（dev action） */
static GtkWidget *dev_find_view(GtkWidget *w)
{
    GtkWidget *it;
    if (HB_IS_ACCOUNT_VIEW(w)) return w;
    for (it = gtk_widget_get_first_child(w); it;
         it = gtk_widget_get_next_sibling(it)) {
        GtkWidget *r = dev_find_view(it);
        if (r) return r;
    }
    return NULL;
}

void hb_account_view_dev_words(GtkWidget *any, const char *words)
{
    GtkWidget *v;
    if (!any || !words) return;
    v = dev_find_view(any);
    if (v) show_words_dialog(HB_ACCOUNT_VIEW(v), words);
}
#endif

GtkWidget *hb_account_view_new(HbStore *store)
{
    HbAccountView *v = g_object_new(HB_TYPE_ACCOUNT_VIEW, NULL);
    v->store = store;
    v->svc = hb_sync_service_default();
    {
        GtkWidget *sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(sw, TRUE);
        v->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), v->content);
        gtk_box_append(GTK_BOX(v), sw);
    }
    v->svc_sig = g_signal_connect(v->svc, "changed",
                                  G_CALLBACK(on_svc_changed), v);
    rebuild_in(v);
    return GTK_WIDGET(v);
}
