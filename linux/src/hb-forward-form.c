/* hb-forward-form — 端口转发规则 新建/编辑对话框（FR-06）
 *
 * 类型（Local -L / Remote -R / Dynamic -D）、绑定地址:端口、
 * 目标地址:端口（Dynamic 隐藏）、经由 SSH 主机（proto=ssh）、
 * 随主机连接自动启动、等效 ssh 命令预览。
 * 保存前 local/dynamic 做绑定冲突预检（live 300ms 防抖 + 保存时复查），
 * 冲突行内红字并阻止保存；非回环绑定/低端口只给提示不阻止。
 * ================================================================== */

#include "hb-forward-form.h"
#include "hb-forward-mgr.h"
#include "hb-dialogs.h"
#include "hb-icons.h"

#include <harbor/pf.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gi18n.h>

typedef struct {
    GtkWidget *parent;
    HbStore   *st;
    hb_obj    *obj;            /* 编辑对象（owned；新建为 NULL） */
    AdwDialog *dlg;

    GtkWidget *kind_dd;
    GtkWidget *kind_sub;
    GtkWidget *bind_e, *port_sb;
    GtkWidget *dest_row, *dest_e, *dport_sb;
    GtkWidget *via_dd;
    GPtrArray *via_ids;        /* strdup'd host id，与 via_dd 对齐 */
    GtkWidget *auto_sw;
    GtkWidget *cmd_l;
    GtkWidget *port_err, *hint_l;
    GtkWidget *save_btn, *savestart_btn;

    guint      probe_src;      /* 300ms 防抖 */
    gboolean   conflict;       /* 当前存在确认冲突（保存阻止） */
    gboolean   dead;
} FwdForm;

static const char *kKindLbl[] = {
    "Local (-L)", "Remote (-R)", "Dynamic SOCKS5 (-D)"
};
static const char *kKindKey[] = { "local", "remote", "dynamic" };

static void schedule_probe(FwdForm *f);
static void refresh_cmd(FwdForm *f);
static void refresh_hints(FwdForm *f);

/* ------------------------------ 工具 ------------------------------ */

/* 数字 IP / localhost 校验（hb_pf_probe_bind 同规则） */
static gboolean addr_ok(const char *s)
{
    struct in_addr a4;
    struct in6_addr a6;
    if (!s || !*s)
        return FALSE;
    if (!strcmp(s, "localhost"))
        return TRUE;
    return inet_pton(AF_INET, s, &a4) == 1 ||
           inet_pton(AF_INET6, s, &a6) == 1;
}

static gboolean is_loopback(const char *s)
{
    if (!s || !*s || !strcmp(s, "localhost"))
        return TRUE;
    if (!strcmp(s, "::1"))
        return TRUE;
    return !strncmp(s, "127.", 4);
}

static gboolean is_wildcard(const char *s)
{
    return !s || !*s || !strcmp(s, "0.0.0.0") || !strcmp(s, "::");
}

static int kind_sel(FwdForm *f)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(f->kind_dd));
    return i < 3 ? (int)i : 0;
}

static const char *bind_addr(FwdForm *f)
{
    const char *s = gtk_editable_get_text(GTK_EDITABLE(f->bind_e));
    return s && *s ? s : "127.0.0.1";
}

static int bind_port(FwdForm *f)
{
    return (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(f->port_sb));
}

static const char *via_id(FwdForm *f)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(f->via_dd));
    return i < f->via_ids->len ? g_ptr_array_index(f->via_ids, i) : NULL;
}

/* 该规则的绑定端口是否正被自己占着（编辑运行中的规则 → 跳过自检） */
static gboolean port_held_by_self(FwdForm *f)
{
    hb_fwd_status st;
    if (!f->obj)
        return FALSE;
    if (!hb_forward_mgr_status(hb_forward_mgr_default(),
                               hb_obj_id(f->obj), &st))
        return FALSE;
    if (st.state != HB_FWD_LISTENING && st.state != HB_FWD_CONNECTING &&
        st.state != HB_FWD_RECONNECTING)
        return FALSE;
    /* 仍用保存时的绑定参数才算"自己占着" */
    {
        const char *sa = hb_obj_get_str(f->obj, "bind_addr");
        int sp = (int)hb_obj_get_int(f->obj, "bind_port", 0);
        return !g_strcmp0(sa && *sa ? sa : "127.0.0.1", bind_addr(f)) &&
               sp == bind_port(f);
    }
}

/* ------------------------------ 冲突预检 ------------------------------ */

static void set_port_err(FwdForm *f, const char *msg)
{
    if (msg && *msg) {
        gtk_label_set_text(GTK_LABEL(f->port_err), msg);
        gtk_widget_set_visible(f->port_err, TRUE);
        f->conflict = TRUE;
    } else {
        gtk_widget_set_visible(f->port_err, FALSE);
        f->conflict = FALSE;
    }
    gtk_widget_set_sensitive(f->save_btn, !f->conflict);
    gtk_widget_set_sensitive(f->savestart_btn, !f->conflict);
}

/* 其它已存 local/dynamic 规则是否同端口冲突（通配地址视为重叠） */
static const hb_obj *rule_conflict(FwdForm *f, hb_obj **objs, size_t n,
                                   const char *addr, int port)
{
    size_t i;
    for (i = 0; objs && i < n; i++) {
        const char *k = hb_obj_get_str(objs[i], "kind");
        const char *oa = hb_obj_get_str(objs[i], "bind_addr");
        int op;
        if (f->obj && !strcmp(hb_obj_id(objs[i]), hb_obj_id(f->obj)))
            continue;
        if (k && !strcmp(k, "remote"))
            continue;   /* remote 不占本机端口 */
        op = (int)hb_obj_get_int(objs[i], "bind_port", 0);
        if (op != port)
            continue;
        if (is_wildcard(addr) || is_wildcard(oa) ||
            !g_strcmp0(oa && *oa ? oa : "127.0.0.1", addr))
            return objs[i];
    }
    return NULL;
}

static void do_probe(FwdForm *f)
{
    const char *addr = bind_addr(f);
    int port = bind_port(f);
    int kind = kind_sel(f);
    hb_obj **objs = NULL;
    size_t n = 0;
    const hb_obj *hit;
    char *err = NULL;

    if (f->dead)
        return;
    if (!addr_ok(addr)) {
        set_port_err(f, _("绑定地址必须是数字 IP（IPv4/IPv6）或 localhost"));
        return;
    }
    if (port < 1 || port > 65535) {
        set_port_err(f, _("端口必须在 1–65535"));
        return;
    }
    if (kind == 1 || port_held_by_self(f)) {   /* remote 不占本机端口 */
        set_port_err(f, NULL);
        return;
    }
    /* 其它已存规则同端口 */
    objs = hb_store_list(f->st, "forward", &n);
    hit = rule_conflict(f, objs, n, addr, port);
    if (hit) {
        const char *ha = hb_obj_get_str(hit, "bind_addr");
        char *m = g_strdup_printf(
            _("端口 %d 已被另一条转发规则占用（%s:%d）"),
            port, ha && *ha ? ha : "127.0.0.1",
            (int)hb_obj_get_int(hit, "bind_port", 0));
        set_port_err(f, m);
        g_free(m);
        hb_obj_list_free(objs, n);
        return;
    }
    if (objs) hb_obj_list_free(objs, n);
    /* 真实 bind 预检 */
    if (hb_pf_probe_bind(addr, port, &err) != HB_OK) {
        set_port_err(f, err ? err : _("端口不可用"));
        free(err);
        return;
    }
    set_port_err(f, NULL);
}

static gboolean probe_cb(gpointer ud)
{
    FwdForm *f = ud;
    f->probe_src = 0;
    if (!f->dead)
        do_probe(f);
    return G_SOURCE_REMOVE;
}

static void schedule_probe(FwdForm *f)
{
    if (f->probe_src)
        g_source_remove(f->probe_src);
    f->probe_src = g_timeout_add(300, probe_cb, f);
}

static void on_probe_field(GtkEditable *e, gpointer ud)
{
    FwdForm *f = ud;
    (void)e;
    if (f->dead) return;
    schedule_probe(f);
    refresh_cmd(f);
    refresh_hints(f);
}

static void on_port_changed(GtkSpinButton *sb, gpointer ud)
{
    FwdForm *f = ud;
    (void)sb;
    if (f->dead) return;
    schedule_probe(f);
    refresh_cmd(f);
    refresh_hints(f);
}

/* ------------------------------ 提示 / 命令 ------------------------------ */

static void refresh_hints(FwdForm *f)
{
    const char *addr = bind_addr(f);
    int kind = kind_sel(f);
    const char *hint = NULL;
    if (kind == 1) {   /* remote：绑定在服务端 */
        if (!is_loopback(addr))
            hint = _("需要服务器 sshd_config 设置 "
                   "GatewayPorts yes/clientspecified，否则仅绑定回环");
    } else {           /* local/dynamic：绑定在本机 */
        if (!is_loopback(addr))
            hint = _("将对局域网开放此端口");
        else if (bind_port(f) > 0 && bind_port(f) < 1024)
            hint = _("绑定 1024 以下端口需要 root 权限");
    }
    gtk_label_set_text(GTK_LABEL(f->hint_l), hint ? hint : "");
    gtk_widget_set_visible(f->hint_l, hint != NULL);
}

/* 等效 ssh 命令：ssh -L/-R bind:dest [-p port] [-J jumps] user@host */
static void refresh_cmd(FwdForm *f)
{
    int kind = kind_sel(f);
    const char *vid = via_id(f);
    GString *s = g_string_new("ssh ");
    const char *ba = bind_addr(f);
    int bp = bind_port(f);
    hb_obj *h = NULL;

    if (kind == 0) {
        const char *da = gtk_editable_get_text(GTK_EDITABLE(f->dest_e));
        g_string_append_printf(s, "-L %s:%d:%s:%d", ba, bp,
            da && *da ? da : _("目标地址"),
            (int)gtk_spin_button_get_value(
                GTK_SPIN_BUTTON(f->dport_sb)));
    } else if (kind == 1) {
        const char *da = gtk_editable_get_text(GTK_EDITABLE(f->dest_e));
        g_string_append_printf(s, "-R %s:%d:%s:%d", ba, bp,
            da && *da ? da : "127.0.0.1",
            (int)gtk_spin_button_get_value(
                GTK_SPIN_BUTTON(f->dport_sb)));
    } else {
        g_string_append_printf(s, "-D %s:%d", ba, bp);
    }

    if (vid && hb_vault_get(hb_store_vault(f->st), vid,
                            &h) == HB_OK && h) {
        const char **jids = NULL;
        size_t nj = hb_obj_get_strv(h, "jump", &jids);
        if (nj) {
            size_t i;
            g_string_append(s, " -J ");
            for (i = 0; i < nj; i++) {
                hb_obj *jh = NULL;
                if (i) g_string_append_c(s, ',');
                if (hb_vault_get(hb_store_vault(f->st), jids[i],
                                 &jh) == HB_OK && jh) {
                    const char *jn = hb_obj_get_str(jh, "name");
                    const char *ja = hb_obj_get_str(jh, "address");
                    g_string_append(s, jn && *jn ? jn :
                                     (ja && *ja ? ja : jids[i]));
                    hb_obj_free(jh);
                } else {
                    g_string_append(s, jids[i]);
                }
            }
            g_string_append_c(s, ' ');
        }
        free(jids);
        {
            int hp = (int)hb_obj_get_int(h, "port", 22);
            const char *hu = hb_obj_get_str(h, "user");
            const char *hn = hb_obj_get_str(h, "address");
            if (!hn || !*hn)
                hn = hb_obj_get_str(h, "name");
            if (hp != 22)
                g_string_append_printf(s, "-p %d ", hp);
            g_string_append_printf(s, "%s@%s",
                hu && *hu ? hu : g_get_user_name(),
                hn && *hn ? hn : "host");
        }
        hb_obj_free(h);
    } else {
        g_string_append(s, "user@host");
    }
    gtk_label_set_text(GTK_LABEL(f->cmd_l), s->str);
    g_string_free(s, TRUE);
}

static void on_kind_sel(GObject *dd, GParamSpec *ps, gpointer ud)
{
    FwdForm *f = ud;
    static const char *subs[] = {
        N_("本机 绑定地址:端口 → 经 SSH 服务器访问 目标地址:端口"),
        N_("服务器 绑定地址:端口 → 经本机访问 目标地址:端口（需要 sshd 允许）"),
        N_("本机 SOCKS5 代理端口 → 目标由客户端逐连接指定"),
    };
    int k;
    (void)dd; (void)ps;
    if (f->dead) return;
    k = kind_sel(f);
    gtk_label_set_text(GTK_LABEL(f->kind_sub), _(subs[k]));
    gtk_widget_set_visible(f->dest_row, k != 2);
    schedule_probe(f);
    refresh_cmd(f);
    refresh_hints(f);
}

static void on_via_sel(GObject *dd, GParamSpec *ps, gpointer ud)
{
    FwdForm *f = ud;
    (void)dd; (void)ps;
    if (f->dead) return;
    refresh_cmd(f);
}

/* ------------------------------ 保存 ------------------------------ */

static gboolean validate(FwdForm *f, char **err_out, GtkWidget **focus)
{
    int kind = kind_sel(f);
    const char *da;
    *focus = NULL;
    if (!addr_ok(bind_addr(f))) {
        *err_out = g_strdup(_("绑定地址必须是数字 IP 或 localhost"));
        *focus = f->bind_e;
        return FALSE;
    }
    if (bind_port(f) < 1 || bind_port(f) > 65535) {
        *err_out = g_strdup(_("绑定端口必须在 1–65535"));
        *focus = f->port_sb;
        return FALSE;
    }
    if (kind != 2) {
        da = gtk_editable_get_text(GTK_EDITABLE(f->dest_e));
        if (!da || !*da) {
            *err_out = g_strdup(_("目标地址不能为空"));
            *focus = f->dest_e;
            return FALSE;
        }
        if ((int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(f->dport_sb))
                < 1) {
            *err_out = g_strdup(_("目标端口必须在 1–65535"));
            *focus = f->dport_sb;
            return FALSE;
        }
    }
    if (!via_id(f)) {
        *err_out = g_strdup(_("请选择经由 SSH 主机"));
        *focus = f->via_dd;
        return FALSE;
    }
    return TRUE;
}

static hb_obj *collect_obj(FwdForm *f)
{
    hb_obj *o = f->obj;
    int kind = kind_sel(f);
    if (!o) {
        o = hb_obj_new("forward");
        hb_obj_set_int(o, "order", g_get_real_time() / 1000000);
        hb_obj_set_bool(o, "enabled", FALSE);
    }
    hb_obj_set_str(o, "kind", kKindKey[kind]);
    hb_obj_set_str(o, "bind_addr", bind_addr(f));
    hb_obj_set_int(o, "bind_port", bind_port(f));
    if (kind == 2) {
        hb_obj_set_str(o, "dest_addr", NULL);
        hb_obj_set_int(o, "dest_port", 0);
    } else {
        hb_obj_set_str(o, "dest_addr",
            gtk_editable_get_text(GTK_EDITABLE(f->dest_e)));
        hb_obj_set_int(o, "dest_port",
            (int)gtk_spin_button_get_value(
                GTK_SPIN_BUTTON(f->dport_sb)));
    }
    hb_obj_set_str(o, "via", via_id(f));
    hb_obj_set_bool(o, "autostart",
        gtk_switch_get_active(GTK_SWITCH(f->auto_sw)));
    if (!f->obj)
        f->obj = o;    /* 接管：新建对象由 form 持有到关闭 */
    return o;
}

/* 非端口校验错误：显示但不冻结保存键（改正后即可重试） */
static void show_err(FwdForm *f, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(f->port_err), msg);
    gtk_widget_set_visible(f->port_err, TRUE);
}

static void do_save(FwdForm *f, gboolean start)
{
    char *err = NULL;
    GtkWidget *focus = NULL;
    hb_obj *o;

    do_probe(f);   /* 保存时复查（不等防抖） */
    if (f->probe_src) {
        g_source_remove(f->probe_src);
        f->probe_src = 0;
    }
    if (f->conflict)
        return;    /* 行内红字已给出 */
    if (!validate(f, &err, &focus)) {
        show_err(f, err);
        g_free(err);
        if (focus) gtk_widget_grab_focus(focus);
        return;
    }
    o = collect_obj(f);
    hb_vault_put(hb_store_vault(f->st), o);
    if (start)
        hb_forward_mgr_start(hb_forward_mgr_default(), hb_obj_id(o));
    adw_dialog_close(f->dlg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    FwdForm *f = ud;
    (void)b;
    if (!f->dead) do_save(f, FALSE);
}
static void on_save_start(GtkButton *b, gpointer ud)
{
    FwdForm *f = ud;
    (void)b;
    if (!f->dead) do_save(f, TRUE);
}

static void form_free(FwdForm *f)
{
    if (f->probe_src) {
        g_source_remove(f->probe_src);
        f->probe_src = 0;
    }
    if (f->obj) hb_obj_free(f->obj);
    if (f->via_ids) g_ptr_array_free(f->via_ids, TRUE);
    g_free(f);
}

static void on_closed(AdwDialog *d, gpointer ud)
{
    FwdForm *f = ud;
    (void)d;
    f->dead = TRUE;
    if (f->probe_src) {
        g_source_remove(f->probe_src);
        f->probe_src = 0;
    }
}

/* ------------------------------ 构建 ------------------------------ */

static GtkWidget *field_label(GtkWidget *box, const char *txt)
{
    GtkWidget *l = gtk_label_new(txt);
    gtk_widget_add_css_class(l, "hb-form-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), l);
    return l;
}

void hb_forward_form_open(GtkWidget *parent, HbStore *st, hb_obj *obj)
{
    FwdForm *f = g_new0(FwdForm, 1);
    GtkWidget *body, *foot, *row, *sw, *b;
    GtkStringList *sm;
    hb_obj **hosts;
    size_t nh = 0, i;
    const char *s;
    guint sel;

    f->parent = parent;
    f->st = st;
    if (obj)
        f->obj = hb_obj_copy(obj);   /* 深拷贝：编辑期间不受外部变更影响 */

    f->dlg = hb_modal_new(obj ? _("Edit forwarding") : _("New forwarding"),
                          &body, &foot);

    /* 类型 */
    field_label(body, _("类型"));
    sm = gtk_string_list_new(NULL);
    for (i = 0; i < 3; i++)
        gtk_string_list_append(sm, kKindLbl[i]);
    f->kind_dd = gtk_drop_down_new(G_LIST_MODEL(sm), NULL);
    gtk_box_append(GTK_BOX(body), f->kind_dd);
    f->kind_sub = gtk_label_new("");
    gtk_widget_add_css_class(f->kind_sub, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(f->kind_sub), 0);
    gtk_box_append(GTK_BOX(body), f->kind_sub);

    /* 绑定地址 + 端口 */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *c1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *c2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(c1, TRUE);
        field_label(c1, _("绑定地址"));
        f->bind_e = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(f->bind_e), "127.0.0.1");
        gtk_box_append(GTK_BOX(c1), f->bind_e);
        field_label(c2, _("端口"));
        f->port_sb = gtk_spin_button_new_with_range(1, 65535, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->port_sb), 13306);
        gtk_widget_set_size_request(f->port_sb, 110, -1);
        gtk_box_append(GTK_BOX(c2), f->port_sb);
        gtk_box_append(GTK_BOX(row), c1);
        gtk_box_append(GTK_BOX(row), c2);
    }
    gtk_box_append(GTK_BOX(body), row);
    f->port_err = gtk_label_new("");
    gtk_widget_add_css_class(f->port_err, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(f->port_err), 0);
    gtk_label_set_wrap(GTK_LABEL(f->port_err), TRUE);
    gtk_widget_set_visible(f->port_err, FALSE);
    gtk_box_append(GTK_BOX(body), f->port_err);
    f->hint_l = gtk_label_new("");
    gtk_widget_add_css_class(f->hint_l, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(f->hint_l), 0);
    gtk_label_set_wrap(GTK_LABEL(f->hint_l), TRUE);
    gtk_widget_set_visible(f->hint_l, FALSE);
    gtk_box_append(GTK_BOX(body), f->hint_l);

    /* 目标地址 + 端口（Dynamic 隐藏） */
    f->dest_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *c1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *c2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(c1, TRUE);
        field_label(c1, _("目标地址"));
        f->dest_e = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(f->dest_e),
                                       "db.internal");
        gtk_box_append(GTK_BOX(c1), f->dest_e);
        field_label(c2, _("目标端口"));
        f->dport_sb = gtk_spin_button_new_with_range(1, 65535, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->dport_sb), 3306);
        gtk_widget_set_size_request(f->dport_sb, 110, -1);
        gtk_box_append(GTK_BOX(c2), f->dport_sb);
        gtk_box_append(GTK_BOX(f->dest_row), c1);
        gtk_box_append(GTK_BOX(f->dest_row), c2);
    }
    gtk_box_append(GTK_BOX(body), f->dest_row);

    /* 经由 SSH 主机（proto=ssh） */
    field_label(body, _("经由 SSH 主机"));
    sm = gtk_string_list_new(NULL);
    f->via_ids = g_ptr_array_new_with_free_func(g_free);
    hosts = hb_store_list(st, "host", &nh);
    sel = 0;
    {
        const char *want = obj ? hb_obj_get_str(obj, "via") : NULL;
        for (i = 0; hosts && i < nh; i++) {
            const char *pr = hb_obj_get_str(hosts[i], "proto");
            const char *nm = hb_obj_get_str(hosts[i], "name");
            const char *un = hb_obj_get_str(hosts[i], "user");
            const char *id = hb_obj_id(hosts[i]);
            if (pr && strcmp(pr, "ssh"))
                continue;
            {
                char *disp = g_strdup_printf("%s@%s",
                    un && *un ? un : g_get_user_name(),
                    nm && *nm ? nm : "?");
                gtk_string_list_append(sm, disp);
                g_free(disp);
            }
            g_ptr_array_add(f->via_ids, g_strdup(id));
            if (want && !strcmp(want, id))
                sel = f->via_ids->len - 1;
        }
    }
    if (hosts) hb_obj_list_free(hosts, nh);
    f->via_dd = gtk_drop_down_new(G_LIST_MODEL(sm), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->via_dd), sel);
    gtk_box_append(GTK_BOX(body), f->via_dd);

    /* autostart */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *t = gtk_label_new(_("随主机连接自动启动"));
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_box_append(GTK_BOX(vb), t);
        {
            GtkWidget *sub = gtk_label_new(
                _("该 SSH 主机连上时自动拉起此规则"));
            gtk_widget_add_css_class(sub, "hb-dim");
            gtk_label_set_xalign(GTK_LABEL(sub), 0);
            gtk_box_append(GTK_BOX(vb), sub);
        }
        gtk_box_append(GTK_BOX(row), vb);
    }
    f->auto_sw = gtk_switch_new();
    gtk_widget_set_valign(f->auto_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(f->auto_sw, TRUE);
    gtk_widget_set_halign(f->auto_sw, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), f->auto_sw);
    gtk_box_append(GTK_BOX(body), row);

    /* 等效命令（只读可选中） */
    field_label(body, _("等效命令"));
    f->cmd_l = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(f->cmd_l), TRUE);
    gtk_widget_add_css_class(f->cmd_l, "mono");
    gtk_widget_add_css_class(f->cmd_l, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(f->cmd_l), 0);
    gtk_label_set_wrap(GTK_LABEL(f->cmd_l), TRUE);
    gtk_box_append(GTK_BOX(body), f->cmd_l);

    /* footer：取消 / 保存 / 保存并启动 */
    {
        GtkWidget *fl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(fl, "hb-modal-footer");
        gtk_widget_set_halign(fl, GTK_ALIGN_END);
        b = gtk_button_new_with_label(_("取消"));
        gtk_widget_add_css_class(b, "hb-btn");
        g_signal_connect_swapped(b, "clicked",
            G_CALLBACK(adw_dialog_close), f->dlg);
        gtk_box_append(GTK_BOX(fl), b);
        f->save_btn = gtk_button_new_with_label(_("保存"));
        gtk_widget_add_css_class(f->save_btn, "hb-btn");
        g_signal_connect(f->save_btn, "clicked",
            G_CALLBACK(on_save), f);
        gtk_box_append(GTK_BOX(fl), f->save_btn);
        f->savestart_btn = gtk_button_new_with_label(_("保存并启动"));
        gtk_widget_add_css_class(f->savestart_btn, "hb-btn");
        gtk_widget_add_css_class(f->savestart_btn, "primary");
        g_signal_connect(f->savestart_btn, "clicked",
            G_CALLBACK(on_save_start), f);
        gtk_box_append(GTK_BOX(fl), f->savestart_btn);
        gtk_widget_set_margin_start(fl, 20);
        gtk_widget_set_margin_end(fl, 20);
        gtk_widget_set_margin_top(fl, 4);
        gtk_widget_set_margin_bottom(fl, 16);
        gtk_box_append(GTK_BOX(foot), fl);
    }

    /* 编辑模式灌值 */
    if (obj) {
        const char *k = hb_obj_get_str(obj, "kind");
        guint ki = 0;
        if (k && !strcmp(k, "remote")) ki = 1;
        else if (k && !strcmp(k, "dynamic")) ki = 2;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->kind_dd), ki);
        s = hb_obj_get_str(obj, "bind_addr");
        gtk_editable_set_text(GTK_EDITABLE(f->bind_e),
                              s && *s ? s : "127.0.0.1");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->port_sb),
            (double)hb_obj_get_int(obj, "bind_port", 0));
        s = hb_obj_get_str(obj, "dest_addr");
        gtk_editable_set_text(GTK_EDITABLE(f->dest_e), s ? s : "");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(f->dport_sb),
            (double)hb_obj_get_int(obj, "dest_port", 22));
        gtk_switch_set_active(GTK_SWITCH(f->auto_sw),
            hb_obj_get_bool(obj, "autostart", FALSE));
    }

    /* 信号 */
    sw = f->kind_dd;
    g_signal_connect(sw, "notify::selected",
                     G_CALLBACK(on_kind_sel), f);
    g_signal_connect(f->via_dd, "notify::selected",
                     G_CALLBACK(on_via_sel), f);
    g_signal_connect(f->bind_e, "changed",
                     G_CALLBACK(on_probe_field), f);
    g_signal_connect(f->dest_e, "changed",
                     G_CALLBACK(refresh_cmd), f);
    g_signal_connect(f->port_sb, "value-changed",
                     G_CALLBACK(on_port_changed), f);
    g_signal_connect(f->dport_sb, "value-changed",
                     G_CALLBACK(refresh_cmd), f);
    g_object_set_data_full(G_OBJECT(f->dlg), "hb-form", f,
                           (GDestroyNotify)form_free);
    g_signal_connect(f->dlg, "closed", G_CALLBACK(on_closed), f);

    on_kind_sel(NULL, NULL, f);   /* 初态：副标题/目标行显隐/命令 */
    adw_dialog_present(f->dlg, parent);
}
