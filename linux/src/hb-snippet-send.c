/* 共享 Snippet 发送流（FR-08）：解析目标终端 / Broadcast workspace，
 * {{var}} 变量对话框（记住上次值、预填默认）、run 追加回车、广播确认。 */

#include "hb-snippet-send.h"
#include "hb-dialogs.h"
#include "hb-icons.h"
#include "hb-session-page.h"
#include "hb-terminal.h"
#include "hb-window.h"
#include "hb-workspace.h"

#include <harbor/snippet.h>
#include <harbor/keys.h>

gboolean hb_snippet_scope_match(const char *scope, const char *host_id,
                                const char *group)
{
    if (!scope || !*scope ||
        !strcmp(scope, "global") || !strcmp(scope, "全局"))
        return TRUE;
    if (!strncmp(scope, "host:", 5))
        return host_id && !strcmp(scope + 5, host_id);
    if (!strncmp(scope, "group:", 6))
        return group && *group && !strcmp(scope + 6, group);
    return TRUE;
}

/* ---------------------------- 记忆的上次变量值 ---------------------------- */

static GHashTable *last_vals;   /* snippet_id → GHashTable(name → value) */

static GHashTable *last_for(const char *snip_id, gboolean create)
{
    GHashTable *t;
    if (!last_vals)
        last_vals = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify)g_hash_table_unref);
    t = g_hash_table_lookup(last_vals, snip_id);
    if (!t && create) {
        t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(last_vals, g_strdup(snip_id), t);
    }
    return t;
}

/* -------------------------------- 作业 ---------------------------------- */

typedef struct {
    GtkWidget   *win;      /* 对话框 parent（借） */
    hb_obj      *snip;     /* owned */
    hb_session  *sess;     /* 单目标（ws NULL 时） */
    HbWorkspace *ws;       /* broadcast 目标 */
    guint        npanes;
    char        *text;     /* 渲染后的待发送文本 */
    gboolean     run;
} SendCtx;

static void send_ctx_free(SendCtx *c)
{
    if (c->snip) hb_obj_free(c->snip);
    g_free(c->text);
    g_free(c);
}

static void deliver(SendCtx *c)
{
    if (c->ws) {
        hb_workspace_paste_all(c->ws, c->text, c->run);
    } else if (c->sess) {
        hb_session_paste(c->sess, c->text);
        if (c->run)
            hb_session_key(c->sess, HB_KEY_ENTER, 0);
    }
    send_ctx_free(c);
}

static void bc_confirmed(gboolean ok, gpointer ud)
{
    SendCtx *c = ud;
    if (ok)
        deliver(c);
    else
        send_ctx_free(c);
}

/* 收尾：Broadcast workspace → 先确认；否则直接发。调用时 c->text 已就绪。 */
static void maybe_send(SendCtx *c)
{
    if (c->ws) {
        char *msg = g_strdup_printf("将发送到 %u 个分屏", c->npanes);
        hb_dialog_confirm(c->win, "Broadcast 已开启", msg, "发送", FALSE,
                          bc_confirmed, c);
        g_free(msg);
        return;
    }
    deliver(c);
}

/* ------------------------------ 变量对话框 ------------------------------ */

typedef struct {
    SendCtx    *c;
    char      **names;    /* owned */
    GtkWidget **entries;  /* owned array */
    size_t      n;
    AdwDialog  *dlg;
} VarCtx;

static void var_ctx_free(VarCtx *v)
{
    g_free(v->entries);
    if (v->names) hb_strfreev(v->names, v->n);
    g_free(v);
}

static void var_ok(GtkButton *b, gpointer ud)
{
    VarCtx *v = ud;
    const char **vals;
    GHashTable *memo;
    size_t i;
    (void)b;

    vals = g_new0(const char *, v->n);
    memo = last_for(hb_obj_id(v->c->snip), TRUE);
    for (i = 0; i < v->n; i++) {
        const char *t = gtk_editable_get_text(GTK_EDITABLE(v->entries[i]));
        vals[i] = t;
        g_hash_table_replace(memo, g_strdup(v->names[i]), g_strdup(t));
    }
    v->c->text = hb_snippet_render(hb_obj_get_str(v->c->snip, "body"),
                                   (const char *const *)v->names,
                                   vals, v->n);
    g_free(vals);
    adw_dialog_close(v->dlg);
}

static void var_closed(AdwDialog *d, gpointer ud)
{
    VarCtx *v = ud;
    (void)d;
    if (v->c->text)
        maybe_send(v->c);
    else
        send_ctx_free(v->c);   /* 取消 */
    var_ctx_free(v);
}

static void open_var_dialog(SendCtx *c, char **names, char **defs, size_t n)
{
    VarCtx *v = g_new0(VarCtx, 1);
    GtkWidget *body, *foot, *f, *cancel, *ok;
    GHashTable *memo;
    size_t i;

    v->c = c;
    v->names = names;
    v->n = n;
    v->entries = g_new0(GtkWidget *, n);
    v->dlg = hb_modal_new("填写变量", &body, &foot);
    memo = last_for(hb_obj_id(c->snip), FALSE);

    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *l = gtk_label_new(names[i]);
        const char *pre = NULL;
        gtk_widget_add_css_class(l, "hb-form-label");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(row), l);
        v->entries[i] = gtk_entry_new();
        if (memo)
            pre = g_hash_table_lookup(memo, names[i]);
        if (!pre)
            pre = defs ? defs[i] : NULL;
        if (pre)
            gtk_editable_set_text(GTK_EDITABLE(v->entries[i]), pre);
        gtk_box_append(GTK_BOX(row), v->entries[i]);
        gtk_box_append(GTK_BOX(body), row);
    }
    if (defs) hb_strfreev(defs, n);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(cancel, "hb-btn");
    g_signal_connect_swapped(cancel, "clicked",
                             G_CALLBACK(adw_dialog_close), v->dlg);
    gtk_box_append(GTK_BOX(f), cancel);
    ok = gtk_button_new_with_label("发送");
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(var_ok), v);
    gtk_box_append(GTK_BOX(f), ok);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(v->dlg, "closed", G_CALLBACK(var_closed), v);
    adw_dialog_present(v->dlg, c->win);
}

/* ---------------------------- 目标解析 + 入口 ---------------------------- */

static void send_with_ctx(SendCtx *c)
{
    const char *body = hb_obj_get_str(c->snip, "body");
    char **names = NULL, **defs = NULL;
    size_t nv;

    c->run = hb_obj_get_bool(c->snip, "run", FALSE);
    nv = hb_snippet_vars(body ? body : "", &names, &defs);
    if (!nv) {
        if (defs) hb_strfreev(defs, nv);
        c->text = g_strdup(body ? body : "");
        maybe_send(c);
        return;
    }
    open_var_dialog(c, names, defs, nv);
}

static void send_common(GtkWidget *win, hb_session *sess, HbWorkspace *ws,
                        HbStore *store, const char *snippet_id)
{
    SendCtx *c;
    hb_obj *snip = NULL;

    if (hb_vault_get(hb_store_vault(store), snippet_id, &snip) != HB_OK ||
        !snip) {
        hb_toast(win, "Snippet 不存在");
        if (snip) hb_obj_free(snip);
        return;
    }
    if (!sess && !ws) {
        hb_toast(win, "没有活动终端");
        hb_obj_free(snip);
        return;
    }
    c = g_new0(SendCtx, 1);
    c->win = win;
    c->snip = snip;
    c->sess = sess;
    c->ws = ws;
    if (ws)
        c->npanes = hb_workspace_pane_count(ws);
    send_with_ctx(c);
}

void hb_snippet_send(GtkWidget *win, HbStore *store, const char *snippet_id)
{
    GtkWidget *content, *pg;
    hb_session *sess = NULL;
    HbWorkspace *ws = NULL;

    if (!win || !store || !snippet_id)
        return;
    content = hb_window_active_page(HB_WINDOW(win));
    if (content && HB_IS_WORKSPACE(content) &&
        hb_workspace_is_broadcast(HB_WORKSPACE(content))) {
        ws = HB_WORKSPACE(content);
        pg = hb_workspace_focused_page(ws);
    } else {
        pg = hb_window_active_session_page(HB_WINDOW(win));
    }
    if (pg && HB_IS_SESSION_PAGE(pg)) {
        HbTerminal *t = hb_session_page_terminal(HB_SESSION_PAGE(pg));
        hb_session *s = t ? hb_terminal_get_session(t) : NULL;
        if (s && hb_session_alive(s) && !ws)
            sess = s;
    }
    send_common(win, sess, ws, store, snippet_id);
}

void hb_snippet_send_to_session(GtkWidget *parent, hb_session *sess,
                                HbStore *store, const char *snippet_id)
{
    if (!parent || !store || !snippet_id)
        return;
    if (!sess || !hb_session_alive(sess)) {
        hb_toast(parent, "没有活动终端");
        return;
    }
    send_common(parent, sess, NULL, store, snippet_id);
}
