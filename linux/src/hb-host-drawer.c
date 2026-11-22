#include "hb-host-drawer.h"
#include "hb-host-actions.h"
#include "hb-icons.h"

#include <adwaita.h>
#include <harbor/harbor_core.h>
#include <string.h>
#include <glib/gi18n.h>

struct _HbHostDrawer {
    HbStore   *st;
    GtkWidget *root;       /* 横向：scrim + revealer */
    GtkWidget *scrim;
    GtkWidget *rev;
    GtkWidget *panel;      /* 400px 内容列 */
    GtkWidget *scroll_box; /* 详情内容容器 */
    GtkWidget *foot;
    char      *host_id;
    gulong     changed_id;
};

/* ------------------------------ 小工具 ------------------------------ */

static GtkWidget *drow(const char *key, const char *val, gboolean mono)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *k = gtk_label_new(key);
    GtkWidget *v = gtk_label_new(val && *val ? val : "—");
    gtk_widget_add_css_class(r, "hb-drawer-row");
    gtk_widget_add_css_class(k, "hb-drawer-key");
    gtk_widget_add_css_class(v, "hb-drawer-val");
    if (mono) gtk_widget_add_css_class(v, "hb-mono");
    gtk_label_set_xalign(GTK_LABEL(k), 0);
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_widget_set_halign(v, GTK_ALIGN_END);
    gtk_label_set_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_box_append(GTK_BOX(r), k);
    gtk_box_append(GTK_BOX(r), v);
    return r;
}

static void drow_append(GtkWidget *box, const char *key, const char *val)
{
    gtk_box_append(GTK_BOX(box), drow(key, val, FALSE));
}

static GtkWidget *chip(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "hb-drawer-chip");
    return l;
}

/* ------------------------------ 数据拼装 ------------------------------ */

typedef struct {
    const char *host_id;
    int64_t     ts;
    char        text[96];
    gboolean    found;
} LastLog;

static bool last_log_cb(const hb_log *l, void *ud)
{
    LastLog *ll = ud;
    GDateTime *dt;
    if (!l->host_id || strcmp(l->host_id, ll->host_id)) return true;
    dt = g_date_time_new_from_unix_local(l->ts / 1000);
    {
        const char *res = l->result ? l->result : "";
        if (dt) {
            char *s = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            snprintf(ll->text, sizeof ll->text, "%s%s%s", s,
                     *res ? " · " : "", res);
            g_free(s);
            g_date_time_unref(dt);
        } else {
            snprintf(ll->text, sizeof ll->text, "%s", res);
        }
    }
    ll->found = TRUE;
    return false;   /* 第一条即最新（desc） */
}

/* 跳链文字："a → b → 目标"；隧道："经 X 隧道 → 127.0.0.1:port" */
static char *chain_text(HbStore *st, hb_obj *h)
{
    hb_vault *v = hb_store_vault(st);
    const char *proto = hb_obj_get_str(h, "proto");
    GString *s = g_string_new(NULL);
    const char **jump = NULL;
    size_t nj, i;

    nj = hb_obj_get_strv(h, "jump", &jump);
    for (i = 0; i < nj; i++) {
        hb_obj *jh = NULL;
        const char *nm = "?";
        if (hb_vault_get(v, jump[i], &jh) == HB_OK && jh) {
            nm = hb_obj_get_str(jh, "name");
            if (!nm || !*nm) nm = hb_obj_get_str(jh, "address");
        }
        if (s->len) g_string_append(s, " → ");
        g_string_append(s, nm ? nm : "?");
        if (jh) hb_obj_free(jh);
    }
    free(jump);

    if (proto && (!strcmp(proto, "rdp") || !strcmp(proto, "vnc"))) {
        const char *via = hb_obj_get_str(h, "via");
        const char *tun = hb_obj_get_str(h, "tunnel");
        if (via || (tun && *tun)) {
            hb_obj *vh = NULL;
            const char *vn = "?";
            const char *ad = hb_obj_get_str(h, "address");
            if (via && hb_vault_get(v, via, &vh) == HB_OK && vh) {
                vn = hb_obj_get_str(vh, "name");
                if (!vn || !*vn) vn = hb_obj_get_str(vh, "address");
            }
            g_string_printf(s, _("经 %s 隧道 → %s:%d"),
                            vn ? vn : "?",
                            ad ? ad : "?",
                            (int)hb_obj_get_int(h, "port",
                                                !strcmp(proto, "rdp")
                                                ? 3389 : 5901));
            if (vh) hb_obj_free(vh);
        } else {
            g_string_assign(s, _("直连"));
        }
    } else {
        if (s->len) g_string_append(s, _(" → 目标"));
        else g_string_assign(s, _("直连"));
    }
    return g_string_free(s, FALSE);
}

static char *auth_text(HbStore *st, hb_obj *h)
{
    hb_vault *v = hb_store_vault(st);
    const char *a = hb_obj_get_str(h, "auth");
    const char *kid;
    if (!a || !strcmp(a, "auto")) return g_strdup(_("自动"));
    if (!strcmp(a, "password")) {
        char *sec = hb_vault_get_secret(v, hb_obj_id(h), "password");
        gboolean has = sec != NULL;
        if (sec) hb_secret_free(sec);
        return g_strdup(has ? _("密码（已保存）") : _("密码"));
    }
    if (!strcmp(a, "key")) {
        kid = hb_obj_get_str(h, "key_id");
        if (kid && *kid) {
            hb_obj *k = NULL;
            const char *kn = NULL;
            char *out;
            if (hb_vault_get(v, kid, &k) == HB_OK && k)
                kn = hb_obj_get_str(k, "name");
            out = g_strdup_printf(_("密钥 · %s"), kn ? kn : kid);
            if (k) hb_obj_free(k);
            return out;
        }
        {
            const char *idf = hb_obj_get_str(h, "identity_file");
            if (idf && *idf) {
                const char *base = strrchr(idf, '/');
                return g_strdup_printf(_("私钥文件 · %s"),
                                       base ? base + 1 : idf);
            }
        }
        return g_strdup(_("密钥"));
    }
    if (!strcmp(a, "agent")) return g_strdup(_("Agent"));
    if (!strcmp(a, "none")) return g_strdup(_("无"));
    return g_strdup(a);
}

static char *fp_text(HbStore *st, hb_obj *h)
{
    hb_obj **ks = NULL;
    size_t n = 0, i;
    const char *ad = hb_obj_get_str(h, "address");
    int port = (int)hb_obj_get_int(h, "port", 22);
    char *out = NULL;
    ks = hb_store_list(st, "known", &n);
    for (i = 0; i < n; i++) {
        const char *oh = hb_obj_get_str(ks[i], "host");
        int op = (int)hb_obj_get_int(ks[i], "port", 22);
        if (oh && ad && !strcmp(oh, ad) && op == port) {
            const char *fp = hb_obj_get_str(ks[i], "fingerprint");
            const char *kt = hb_obj_get_str(ks[i], "keytype");
            out = g_strdup_printf("%s%s%s", kt ? kt : "",
                                  kt && fp ? "  " : "",
                                  fp ? fp : "");
            break;
        }
    }
    if (ks) hb_obj_list_free(ks, n);
    if (!out) out = g_strdup(_("尚未信任主机指纹"));
    return out;
}

static const char *proto_text(const char *p)
{
    if (!p) return "SSH";
    if (!strcmp(p, "rdp")) return "RDP";
    if (!strcmp(p, "vnc")) return "VNC";
    if (!strcmp(p, "local")) return _("Local");
    return "SSH";
}

/* ------------------------------ 刷新 ------------------------------ */

static void refresh(HbHostDrawer *d)
{
    hb_obj *h = NULL;
    GtkWidget *c;
    const char *proto;
    gboolean local;

    if (!d->host_id) return;
    if (hb_vault_get(hb_store_vault(d->st), d->host_id, &h) != HB_OK ||
        !h || hb_obj_tombstone(h)) {
        if (h) hb_obj_free(h);
        hb_host_drawer_close(d);   /* 已删除 */
        return;
    }
    proto = hb_obj_get_str(h, "proto");
    local = proto && !strcmp(proto, "local");

    /* 清空旧内容（header+tags+rows 全重建，footer 保留） */
    while ((c = gtk_widget_get_first_child(d->scroll_box)))
        gtk_box_remove(GTK_BOX(d->scroll_box), c);

    /* header */
    {
        GtkWidget *hd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *av, *nb, *nm, *sub, *x;
        const char *name = hb_obj_get_str(h, "name");
        const char *ad = hb_obj_get_str(h, "address");
        const char *u = hb_obj_get_str(h, "user");
        int port = (int)hb_obj_get_int(h, "port", 22);
        char *conn;

        av = hb_os_avatar_new(
            hb_obj_get_str(h, "os") && *hb_obj_get_str(h, "os")
                ? hb_obj_get_str(h, "os") : proto, 34);
        gtk_box_append(GTK_BOX(hd), av);
        nb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(nb, TRUE);
        gtk_widget_set_valign(nb, GTK_ALIGN_CENTER);
        nm = gtk_label_new(name && *name ? name : (ad ? ad : _("host")));
        gtk_widget_add_css_class(nm, "hb-drawer-name");
        gtk_label_set_xalign(GTK_LABEL(nm), 0);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(nb), nm);
        if (local) {
            conn = g_strdup(_("本地终端"));
        } else {
            conn = g_strdup_printf("%s%s%s:%d",
                                   u && *u ? u : "", u && *u ? "@" : "",
                                   ad ? ad : "?", port);
        }
        sub = gtk_label_new(conn);
        g_free(conn);
        gtk_widget_add_css_class(sub, "hb-dim");
        gtk_label_set_xalign(GTK_LABEL(sub), 0);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(nb), sub);
        gtk_box_append(GTK_BOX(hd), nb);
        x = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(x), hb_icon_new("x", 14));
        gtk_widget_add_css_class(x, "hb-btn");
        gtk_widget_add_css_class(x, "hb-iconbtn");
        g_signal_connect_swapped(x, "clicked",
                                 G_CALLBACK(hb_host_drawer_close), d);
        gtk_box_append(GTK_BOX(hd), x);
        gtk_box_append(GTK_BOX(d->scroll_box), hd);
    }

    /* tag chips */
    {
        GtkWidget *chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gboolean any = FALSE;
        const char *grp = hb_obj_get_str(h, "group");
        const char *via = hb_obj_get_str(h, "via");
        const char **tags = NULL;
        size_t nt, i;
        if (local) { gtk_box_append(GTK_BOX(chips), chip(_("本地")));
                     any = TRUE; }
        if (via)   { gtk_box_append(GTK_BOX(chips), chip(_("SSH 隧道")));
                     any = TRUE; }
        if (grp && *grp) {
            char *g = g_strdup_printf(_("分组 · %s"), grp);
            gtk_box_append(GTK_BOX(chips), chip(g));
            g_free(g);
            any = TRUE;
        }
        nt = hb_obj_get_strv(h, "tags", &tags);
        for (i = 0; i < nt; i++) {
            gtk_box_append(GTK_BOX(chips), chip(tags[i]));
            any = TRUE;
        }
        free(tags);
        if (any)
            gtk_box_append(GTK_BOX(d->scroll_box), chips);
        else
            g_object_ref_sink(chips), g_object_unref(chips);
    }

    /* 详情行 */
    {
        GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(rows, "hb-drawer-rows");
        drow_append(rows, _("协议"), proto_text(proto));
        if (!local) {
            char buf[64];
            const char *u = hb_obj_get_str(h, "user");
            const char *grp = hb_obj_get_str(h, "group");
            const char *os = hb_obj_get_str(h, "os");
            char *t;
            drow_append(rows, _("地址"),
                        hb_obj_get_str(h, "address"));
            snprintf(buf, sizeof buf, "%d",
                     (int)hb_obj_get_int(h, "port", 22));
            drow_append(rows, _("端口"), buf);
            drow_append(rows, _("用户名"), u && *u ? u : "—");
            if (!proto || !strcmp(proto, "ssh")) {
                t = auth_text(d->st, h);
                drow_append(rows, _("认证"), t);
                g_free(t);
                snprintf(buf, sizeof buf, _("%d 秒%s"),
                         (int)hb_obj_get_int(h, "keepalive", 30),
                         hb_obj_has(h, "keepalive") &&
                         hb_obj_get_int(h, "keepalive", 30) == 0
                         ? _("（关闭）") : "");
                drow_append(rows, _("保活间隔"), buf);
            }
            {
                static const char *osmap[] = {
                    N_("自动检测"), "ubuntu", "debian", "generic", "windows" };
                const char *ost = _("自动检测");
                if (os && !strcmp(os, "ubuntu")) ost = "Ubuntu";
                else if (os && !strcmp(os, "debian")) ost = "Debian";
                else if (os && !strcmp(os, "generic")) ost = _("通用");
                else if (os && !strcmp(os, "windows")) ost = "Windows";
                (void)osmap;
                drow_append(rows, _("操作系统"), ost);
            }
            drow_append(rows, _("分组"), grp && *grp ? grp : "—");
            t = chain_text(d->st, h);
            drow_append(rows, _("跳板 / 隧道"), t);
            g_free(t);
            /* 最近连接 */
            {
                LastLog ll = { .host_id = d->host_id };
                hb_vault_log_foreach(hb_store_vault(d->st), NULL, true,
                                     last_log_cb, &ll);
                if (!ll.found) {
                    int64_t lc = hb_obj_get_int(h, "last_connected", 0);
                    if (lc > 0) {
                        GDateTime *dt =
                            g_date_time_new_from_unix_local(lc / 1000);
                        if (dt) {
                            char *s = g_date_time_format(
                                dt, "%Y-%m-%d %H:%M");
                            snprintf(ll.text, sizeof ll.text, "%s", s);
                            g_free(s);
                            g_date_time_unref(dt);
                            ll.found = TRUE;
                        }
                    }
                }
                drow_append(rows, _("最近连接"),
                            ll.found ? ll.text : _("尚未连接"));
            }
            if (!proto || !strcmp(proto, "ssh")) {
                t = fp_text(d->st, h);
                gtk_box_append(GTK_BOX(rows),
                               drow(_("主机指纹"), t, TRUE));
                g_free(t);
            }
        }
        gtk_box_append(GTK_BOX(d->scroll_box), rows);
    }
    hb_obj_free(h);
}

/* ------------------------------ 对外 ------------------------------ */

static void on_store_changed(HbStore *st, char *type, char *id,
                             gpointer ud)
{
    HbHostDrawer *d = ud;
    (void)st;
    if (!d->host_id) return;
    if (type && strcmp(type, "host")) return;
    if (id && strcmp(id, d->host_id)) return;
    refresh(d);
}

static void on_action_btn(GtkButton *b, gpointer ud)
{
    const char *what = g_object_get_data(G_OBJECT(b), "what");
    HbHostDrawer *d = g_object_get_data(G_OBJECT(b), "drawer");
    char *id = g_strdup(d->host_id);
    (void)ud;
    if (!id) return;
    if (!strcmp(what, "connect"))
        hb_host_action_connect(GTK_WIDGET(b), d->st, id);
    else if (!strcmp(what, "sftp"))
        hb_host_action_sftp(GTK_WIDGET(b), d->st, id);
    else if (!strcmp(what, "edit"))
        hb_host_action_edit(GTK_WIDGET(b), d->st, id);
    else if (!strcmp(what, "dup"))
        hb_host_action_duplicate(GTK_WIDGET(b), d->st, id);
    else if (!strcmp(what, "del"))
        hb_host_action_delete(GTK_WIDGET(b), d->st, id);
    g_free(id);
}

static GtkWidget *foot_btn(HbHostDrawer *d, const char *label,
                           const char *icon, const char *what,
                           const char *css)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    if (icon) gtk_box_append(GTK_BOX(inner), hb_icon_new(icon, 13));
    if (label) gtk_box_append(GTK_BOX(inner), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), inner);
    gtk_widget_add_css_class(b, "hb-btn");
    if (css) gtk_widget_add_css_class(b, css);
    g_object_set_data(G_OBJECT(b), "what", (gpointer)what);
    g_object_set_data(G_OBJECT(b), "drawer", d);
    g_signal_connect(b, "clicked", G_CALLBACK(on_action_btn), NULL);
    return b;
}

static gboolean on_key(GtkEventControllerKey *kc, guint keyval,
                       guint code, GdkModifierType st, gpointer ud)
{
    HbHostDrawer *d = ud;
    (void)kc; (void)code; (void)st;
    if (keyval == GDK_KEY_Escape) {
        hb_host_drawer_close(d);
        return TRUE;
    }
    return FALSE;
}

HbHostDrawer *hb_host_drawer_new(HbStore *st)
{
    HbHostDrawer *d = g_new0(HbHostDrawer, 1);
    GtkWidget *scroll, *foot, *sp;
    GtkEventController *kc;

    d->st = st;
    /* root 是 overlay：scrim 全尺寸，revealer 贴右 */
    d->root = gtk_overlay_new();
    gtk_widget_set_hexpand(d->root, TRUE);
    gtk_widget_set_vexpand(d->root, TRUE);

    d->scrim = gtk_button_new();
    gtk_widget_add_css_class(d->scrim, "hb-drawer-scrim");
    gtk_widget_set_hexpand(d->scrim, TRUE);
    gtk_widget_set_vexpand(d->scrim, TRUE);
    g_signal_connect_swapped(d->scrim, "clicked",
                             G_CALLBACK(hb_host_drawer_close), d);
    gtk_overlay_set_child(GTK_OVERLAY(d->root), d->scrim);

    d->rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(d->rev),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(d->rev), 200);
    gtk_widget_set_halign(d->rev, GTK_ALIGN_END);
    gtk_widget_set_valign(d->rev, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(d->root), d->rev);

    d->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(d->panel, "hb-drawer");
    gtk_widget_set_size_request(d->panel, 400, -1);
    gtk_revealer_set_child(GTK_REVEALER(d->rev), d->panel);

    scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    d->scroll_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(d->scroll_box, 18);
    gtk_widget_set_margin_end(d->scroll_box, 18);
    gtk_widget_set_margin_top(d->scroll_box, 16);
    gtk_widget_set_margin_bottom(d->scroll_box, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  d->scroll_box);
    gtk_box_append(GTK_BOX(d->panel), scroll);

    /* footer：连接 / 编辑 / 复制 / … / 删除 */
    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(foot, "hb-drawer-foot");
    gtk_box_append(GTK_BOX(foot), foot_btn(d, _("连接"), NULL, "connect", "primary"));
    gtk_box_append(GTK_BOX(foot), foot_btn(d, "SFTP", NULL, "sftp", NULL));
    gtk_box_append(GTK_BOX(foot), foot_btn(d, _("编辑"), NULL, "edit", NULL));
    gtk_box_append(GTK_BOX(foot), foot_btn(d, _("复制"), NULL, "dup", NULL));
    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(foot), sp);
    gtk_box_append(GTK_BOX(foot), foot_btn(d, NULL, "trash", "del", "danger"));
    gtk_box_append(GTK_BOX(d->panel), foot);
    d->foot = foot;

    gtk_widget_set_visible(d->root, FALSE);

    kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), d);
    gtk_widget_add_controller(d->panel, kc);

    d->changed_id = g_signal_connect(st, "changed",
                                     G_CALLBACK(on_store_changed), d);
    return d;
}

GtkWidget *hb_host_drawer_widget(HbHostDrawer *d) { return d->root; }

void hb_host_drawer_show(HbHostDrawer *d, const char *host_id)
{
    g_free(d->host_id);
    d->host_id = g_strdup(host_id);
    refresh(d);
    if (!d->host_id) return;   /* refresh 发现已删 → host_id 仍在但 drawer 已关 */
    gtk_widget_set_visible(d->scrim, TRUE);
    gtk_widget_set_visible(d->root, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(d->rev), TRUE);
}


void hb_host_drawer_close(HbHostDrawer *d)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(d->rev), FALSE);
    /* 立即隐藏 root（含 scrim，不阻塞点击） */
    gtk_widget_set_visible(d->root, FALSE);
    g_free(d->host_id);
    d->host_id = NULL;
}

gboolean hb_host_drawer_is_open(HbHostDrawer *d)
{
    return d->host_id != NULL;
}
