#include "hb-host-actions.h"
#include "hb-window.h"
#include "hb-dialogs.h"
#include "hb-host-form.h"
#include "hb-ssh-page.h"
#include "hb-desktop-page.h"
#include "hb-sftp-view.h"
#include "hb-icons.h"

#include <harbor/harbor_core.h>
#include <string.h>
#include <glib/gi18n.h>

static HbWindow *win_of(GtkWidget *w)
{
    GtkRoot *r = gtk_widget_get_root(w);
    return HB_IS_WINDOW(r) ? HB_WINDOW(r) : NULL;
}

/* ------------------------------ 连接 ------------------------------ */

/* SSH 页错误卡"编辑主机" → 表单 */
static void on_page_edit_host(HbStore *st, const char *hid, gpointer page);

/* 打开一个 SSH tab（host 对象已加载）。 */
static void open_ssh_tab(HbWindow *win, HbStore *st, hb_obj *host)
{
    GtkWidget *page;
    char *tab_id;
    const char *name = hb_obj_get_str(host, "name");
    const char *os = hb_obj_get_str(host, "os");

    tab_id = g_strdup_printf("ssh-%s", hb_obj_id(host));
    if (hb_window_has_tab(win, tab_id)) {
        hb_window_switch_tab(win, tab_id);
        g_free(tab_id);
        return;
    }
    page = hb_ssh_page_new(st, host);
    g_signal_connect_swapped(page, "edit-host",
        G_CALLBACK(on_page_edit_host), st);
    hb_window_add_tab(win, tab_id,
                      name && *name ? name : hb_obj_get_str(host, "address"),
                      hb_os_avatar_key(os && *os ? os : "generic"),
                      page, TRUE, FALSE);
    hb_ssh_page_attach_tab(HB_SSH_PAGE(page), GTK_WIDGET(win), tab_id);
    hb_window_switch_tab(win, tab_id);
    g_free(tab_id);
}

/* SSH 页错误卡"编辑主机" → 表单 */
static void on_page_edit_host(HbStore *st, const char *hid, gpointer page)
{
    hb_host_action_edit(GTK_WIDGET(page), st, hid);
}

/* 打开一个远程桌面 tab（rdp/vnc；tab 绑定 host，删主机即关页） */
static void open_desktop_tab(HbWindow *win, HbStore *st, hb_obj *host)
{
    GtkWidget *page;
    char *tab_id;
    const char *name = hb_obj_get_str(host, "name");
    const char *proto = hb_obj_get_str(host, "proto");
    gboolean rdp = !g_strcmp0(proto, "rdp");

    tab_id = g_strdup_printf("rd-%s", hb_obj_id(host));
    if (hb_window_has_tab(win, tab_id)) {
        hb_window_switch_tab(win, tab_id);
        g_free(tab_id);
        return;
    }
    page = hb_desktop_page_new(st, host);
    g_signal_connect_swapped(page, "edit-host",
        G_CALLBACK(on_page_edit_host), st);
    hb_window_add_tab(win, tab_id,
                      name && *name ? name : hb_obj_get_str(host, "address"),
                      rdp ? "win" : "monitor",
                      page, TRUE, FALSE);
    hb_window_set_tab_host(win, tab_id, hb_obj_id(host));
    hb_desktop_page_attach_tab(HB_DESKTOP_PAGE(page), GTK_WIDGET(win),
                               tab_id);
    hb_window_switch_tab(win, tab_id);
    g_free(tab_id);
}

/* 按 proto 分派：ssh → SSH 页；local → Local 页；rdp/vnc → 桌面页 */
void hb_host_connect_obj(HbWindow *win, HbStore *st, hb_obj *o)
{
    const char *proto = hb_obj_get_str(o, "proto");
    if (!proto) proto = "";
    if (!strcmp(proto, "local")) {
        hb_window_open_local(win, NULL);
    } else if (!strcmp(proto, "ssh")) {
        open_ssh_tab(win, st, o);
    } else if (!strcmp(proto, "rdp") || !strcmp(proto, "vnc")) {
        open_desktop_tab(win, st, o);
    }
}

void hb_host_action_connect(GtkWidget *any, HbStore *st, const char *id)
{
    HbWindow *win = win_of(any);
    hb_obj *o = NULL;
    if (!win) return;
    if (hb_vault_get(hb_store_vault(st), id, &o) != HB_OK || !o)
        return;
    hb_host_connect_obj(win, st, o);
    hb_obj_free(o);
}

/* 打开 SFTP tab 并预置两端 */
void hb_host_action_sftp(GtkWidget *any, HbStore *st, const char *id)
{
    HbWindow *win = win_of(any);
    GtkWidget *page;
    (void)st;
    if (!win) return;
    page = hb_window_tab_page(win, "sftp");
    if (page && HB_IS_SFTP_VIEW(page))
        hb_sftp_view_open_host(HB_SFTP_VIEW(page), id);
    hb_window_switch_tab(win, "sftp");
}

/* ------------------------------ 编辑 ------------------------------ */

void hb_host_action_edit(GtkWidget *any, HbStore *st, const char *id)
{
    GtkRoot *r = gtk_widget_get_root(any);
    hb_host_form_open(GTK_IS_WIDGET(r) ? GTK_WIDGET(r) : any, st, id);
}

/* ------------------------------ 复制 ------------------------------ */

/* 生成未占用副本名："X (副本)"、"X (副本 2)"… */
static char *dup_name(HbStore *st, const char *name)
{
    hb_obj **objs;
    size_t n = 0, j;
    char *cand;
    gboolean taken;
    int i;

    objs = hb_store_list(st, "host", &n);
    cand = g_strdup_printf(_("%s (副本)"), name && *name ? name : "host");
    for (i = 2; ; i++) {
        taken = FALSE;
        for (j = 0; j < n; j++) {
            const char *nm = hb_obj_get_str(objs[j], "name");
            if (nm && !strcmp(nm, cand)) { taken = TRUE; break; }
        }
        if (!taken) break;
        g_free(cand);
        cand = g_strdup_printf(_("%s (副本 %d)"),
                               name && *name ? name : "host", i);
    }
    if (objs) hb_obj_list_free(objs, n);
    return cand;
}

typedef struct {
    HbStore *st;
    char    *id;        /* 副本 host id */
    char    *win_name;
    HbWindow *win;
} DupUd;

static void dup_edit_toast(gpointer ud)
{
    DupUd *d = ud;
    hb_host_form_open(GTK_WIDGET(d->win), d->st, d->id);
    g_free(d->id);
    g_free(d);
}

void hb_host_action_duplicate(GtkWidget *any, HbStore *st, const char *id)
{
    HbWindow *win = win_of(any);
    hb_vault *v = hb_store_vault(st);
    hb_obj *src = NULL, *copy;
    char *name, *sec;
    DupUd *d;
    char *toast;

    if (!win) return;
    if (hb_vault_get(v, id, &src) != HB_OK || !src)
        return;

    copy = hb_obj_clone_new(src);
    name = dup_name(st, hb_obj_get_str(src, "name"));
    hb_obj_set_str(copy, "name", name);
    hb_obj_set_str(copy, "last_connected", NULL);
    hb_vault_put(v, copy);

    /* 已保存的密码随副本复制 */
    sec = hb_vault_get_secret(v, id, "password");
    if (sec) {
        hb_vault_set_secret(v, hb_obj_id(copy), "password", sec);
        if (sec) hb_secret_free(sec);
    }

    d = g_new0(DupUd, 1);
    d->st = st;
    d->id = g_strdup(hb_obj_id(copy));
    d->win = win;
    toast = g_strdup_printf(_("已复制为「%s」"), name);
    hb_window_toast_action(win, toast, _("编辑"), 9,
                           G_CALLBACK(dup_edit_toast), d);
    g_free(toast);
    g_free(name);
    hb_obj_free(copy);
    hb_obj_free(src);
}

/* ------------------------------ 删除 ------------------------------ */

typedef struct {
    HbStore *st;
    HbWindow *win;
    char    *id;
    char    *msg;
} DelUd;

/* 统计引用 id 的其他主机（jump 链 / via 隧道）。 */
static int count_references(HbStore *st, const char *id)
{
    hb_obj **objs = NULL;
    size_t n = 0, i;
    int cnt = 0;
    objs = hb_store_list(st, "host", &n);
    for (i = 0; i < n; i++) {
        const char **jump = NULL;
        const char *via;
        size_t nj = hb_obj_get_strv(objs[i], "jump", &jump), j;
        if (!strcmp(hb_obj_id(objs[i]), id)) { free(jump); continue; }
        via = hb_obj_get_str(objs[i], "via");
        if (via && !strcmp(via, id)) { cnt++; free(jump); continue; }
        for (j = 0; j < nj; j++)
            if (jump[j] && !strcmp(jump[j], id)) { cnt++; break; }
        free(jump);
    }
    if (objs) hb_obj_list_free(objs, n);
    return cnt;
}

static void del_undo(gpointer ud)
{
    DelUd *d = ud;
    hb_vault_undelete(hb_store_vault(d->st), d->id);
    g_free(d->id);
    g_free(d);
}

static void del_done(gboolean ok, gpointer ud)
{
    DelUd *d = ud;
    char *toast;

    if (!ok) {
        g_free(d->id);
        g_free(d->msg);
        g_free(d);
        return;
    }
    hb_vault_delete(hb_store_vault(d->st), d->id);
    hb_window_close_host_tabs(d->win, d->id);
    toast = g_strdup_printf(_("已删除主机「%s」"), d->msg);
    hb_window_toast_undo(d->win, toast, G_CALLBACK(del_undo), d);
    g_free(toast);
    /* d 移交 undo 回调 */
}

void hb_host_action_delete(GtkWidget *any, HbStore *st, const char *id)
{
    HbWindow *win = win_of(any);
    hb_obj *o = NULL;
    const char *nm;
    char *esc, *markup;
    int refs;
    DelUd *d;

    if (!win) return;
    if (hb_vault_get(hb_store_vault(st), id, &o) != HB_OK || !o)
        return;
    nm = hb_obj_get_str(o, "name");
    esc = g_markup_escape_text(nm && *nm ? nm :
                               (hb_obj_get_str(o, "address") ?: _("主机")), -1);
    markup = g_strdup_printf(
        _("确定删除 <b>%s</b>？已保存的密码会随主机一起删除。删除后 9 秒内可撤销。"),
        esc);
    refs = count_references(st, id);
    if (refs > 0) {
        char *m2 = g_strdup_printf(ngettext("%s\n\n有 %d 台主机以它为跳板/隧道", "%s\n\n有 %d 台主机以它为跳板/隧道",
                                   refs), markup, refs);
        g_free(markup);
        markup = m2;
    }
    g_free(esc);

    d = g_new0(DelUd, 1);
    d->st = st;
    d->win = win;
    d->id = g_strdup(id);
    d->msg = g_strdup(nm && *nm ? nm : _("主机"));
    hb_dialog_confirm(GTK_WIDGET(win), _("删除主机"), markup, _("删除"), TRUE,
                      del_done, d);
    g_free(markup);
    hb_obj_free(o);
}
