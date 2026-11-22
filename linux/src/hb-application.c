#include "hb-application.h"
#include "hb-window.h"
#include "hb-store.h"
#include "hb-vault-page.h"
#include "hb-hosts-view.h"
#include "hb-term-page.h"
#include "hb-ssh-page.h"
#include "hb-sftp-view.h"
#include "hb-term-theme.h"
#include "hb-notify.h"
#include "hb-icons.h"
#include "hb-dialogs.h"
#include "hb-host-actions.h"
#include "hb-sync.h"
#include "hb-account-view.h"
#include "hb-tray.h"
#include "hb-forward-mgr.h"
#include "hb-share.h"
#include "hb-login.h"
#include "hb-palette.h"
#include "hb-host-form.h"
#include "hb-shortcuts.h"
#include "hb-settings-view.h"
#ifndef NDEBUG
#include "hb-share.h"
#include "hb-login.h"
#include "hb-palette.h"
#include "hb-host-form.h"
#include "hb-shortcuts.h"
#include "hb-settings-view.h"
#include "hb-share-page.h"
#include "hb-share-dialog.h"
#include <stdio.h>
#endif

#include <harbor/ssh.h>
#include <string.h>

struct _HbApplication {
    AdwApplication parent;
};

G_DEFINE_TYPE(HbApplication, hb_application, ADW_TYPE_APPLICATION)

/* dev 选项（main.c 用 --xxx 传入） */
char *hb_opt_screenshot;
int   hb_opt_screenshot_delay = 800;
char *hb_opt_section;
char *hb_opt_filter;
char *hb_opt_host_view;
char *hb_opt_theme;
char *hb_opt_open_local;    /* "" = 只开页；非空 = 启动后注入 CMD\n */
char *hb_opt_connect;       /* --connect=user@host[:port] 启动即快速连接 */
int   hb_opt_background;    /* --background：托盘可用时不开窗（自启动用） */

static HbTray *app_tray;
static gboolean tray_notify_shown;

/* ------------------------------ 截图 ------------------------------ */

static gboolean shot_window(GtkWidget *win, const char *path)
{
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(win));
    GdkPaintable *p = gtk_widget_paintable_new(win);
    GskRenderer *r = gsk_renderer_new_for_surface(surf);
    GtkSnapshot *snap = gtk_snapshot_new();
    GskRenderNode *node;
    GdkTexture *tex;
    graphene_rect_t bounds;
    int w = gtk_widget_get_width(win), h = gtk_widget_get_height(win);
    gboolean ok;

    graphene_rect_init(&bounds, 0, 0, (float)w, (float)h);
    gdk_paintable_snapshot(p, snap, w, h);
    node = gtk_snapshot_free_to_node(snap);
    tex = node ? gsk_renderer_render_texture(r, node, &bounds) : NULL;
    ok = tex && gdk_texture_save_to_png(tex, path);
    g_clear_object(&tex);
    if (node)
        gsk_render_node_unref(node);
    gsk_renderer_unrealize(r);
    g_clear_object(&r);
    g_clear_object(&p);
    g_printerr("harbor: screenshot %s -> %s\n", ok ? "saved" : "FAILED", path);
    return ok;
}

static gboolean take_shot(gpointer ud)
{
    GtkWidget *win = ud;
    gboolean ok = shot_window(win, hb_opt_screenshot);

    if (!ok) {
        static int retries;
        if (++retries < 10) {
            g_printerr("harbor: screenshot retry %d/10 -> %s\n",
                       retries, hb_opt_screenshot);
            g_timeout_add(500, take_shot, win);
            return G_SOURCE_REMOVE;
        }
    }
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

#ifndef NDEBUG
/* app.shot <png 路径>：dev 调试动作，运行中实例按控件树截图。 */
static void on_dev_shot(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    (void)a;
    if (!param) return;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (win)
        shot_window(GTK_WIDGET(win), g_variant_get_string(param, NULL));
}

/* app.feed <text>：dev 调试动作，把文本写进活动 tab 的会话。 */
static void on_dev_feed(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    GtkWidget *page;
    const char *text;
    (void)a;
    if (!param) return;
    text = g_variant_get_string(param, NULL);
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    /* workspace tab → 焦点 pane */
    page = hb_window_active_session_page(HB_WINDOW(win));
    if (HB_IS_TERM_PAGE(page))
        hb_term_page_feed_input(HB_TERM_PAGE(page), text);
    else if (HB_IS_SSH_PAGE(page))
        hb_ssh_page_feed_input(HB_SSH_PAGE(page), text);
}

/* ----------------- FR-12 share dev 动作（DEV_ACTIONS 门控） ----------------- */

static hb_session *dev_active_sess(GtkApplication *app)
{
    GtkWindow *win = gtk_application_get_active_window(app);
    GtkWidget *pg;
    if (!HB_IS_WINDOW(win)) return NULL;
    pg = hb_window_active_session_page(HB_WINDOW(win));
    if (HB_IS_TERM_PAGE(pg))
        return hb_term_page_session(HB_TERM_PAGE(pg));
    if (HB_IS_SSH_PAGE(pg))
        return hb_ssh_page_session(HB_SSH_PAGE(pg));
    return NULL;
}

/* app.share-start <mode>：active session → start share；stdout 回执。 */
static void dev_share_cb(HbShareService *s, const char *sid,
                         const char *err, gpointer ud)
{
    if (err)
        g_print("SHARE-START-FAIL %s\n", err);
    else
        g_print("SHARE-START-OK id=%s link=%s code=%s\n", sid,
                hb_share_service_link(s, sid),
                hb_share_service_code(s, sid));
    fflush(stdout);
    (void)ud;
}

static void on_dev_share_start(GSimpleAction *a, GVariant *param,
                               gpointer ud)
{
    hb_session *sess;
    int mode;
    (void)a;
    sess = dev_active_sess(GTK_APPLICATION(ud));
    if (!sess) { g_print("SHARE-START-FAIL no-session\n"); return; }
    mode = param ? atoi(g_variant_get_string(param, NULL)) : 0;
    hb_share_service_start(hb_share_service_default(), sess,
                           "dev-share", "dev-host",
                           mode ? HB_SHARE_COLLAB : HB_SHARE_VIEW,
                           dev_share_cb, NULL);
}

static void on_dev_share_stop(GSimpleAction *a, GVariant *p, gpointer ud)
{
    hb_session *sess = dev_active_sess(GTK_APPLICATION(ud));
    const char *sid;
    (void)a; (void)p;
    if (!sess) return;
    sid = hb_share_service_share_for_session(
        hb_share_service_default(), sess);
    if (sid) hb_share_service_stop(hb_share_service_default(), sid);
    g_print("SHARE-STOPPED\n"); fflush(stdout);
}

static void on_dev_share_mode(GSimpleAction *a, GVariant *p, gpointer ud)
{
    hb_session *sess = dev_active_sess(GTK_APPLICATION(ud));
    const char *sid;
    (void)a;
    if (!sess || !p) return;
    sid = hb_share_service_share_for_session(
        hb_share_service_default(), sess);
    if (sid)
        hb_share_service_set_mode(hb_share_service_default(), sid,
            atoi(g_variant_get_string(p, NULL))
                ? HB_SHARE_COLLAB : HB_SHARE_VIEW);
    g_print("SHARE-MODE-SET\n"); fflush(stdout);
}

static void on_dev_share_regen(GSimpleAction *a, GVariant *p, gpointer ud)
{
    hb_session *sess = dev_active_sess(GTK_APPLICATION(ud));
    const char *sid;
    (void)a; (void)p;
    if (!sess) return;
    sid = hb_share_service_share_for_session(
        hb_share_service_default(), sess);
    if (sid) {
        hb_share_service_regen(hb_share_service_default(), sid);
        g_print("SHARE-REGEN code=%s link=%s\n",
                hb_share_service_code(hb_share_service_default(), sid),
                hb_share_service_link(hb_share_service_default(), sid));
        fflush(stdout);
    }
}

static void on_dev_share_kick(GSimpleAction *a, GVariant *p, gpointer ud)
{
    hb_session *sess = dev_active_sess(GTK_APPLICATION(ud));
    const char *sid;
    (void)a;
    if (!sess || !p) return;
    sid = hb_share_service_share_for_session(
        hb_share_service_default(), sess);
    if (sid)
        hb_share_service_kick(hb_share_service_default(), sid,
            (uint32_t)atoi(g_variant_get_string(p, NULL)));
    g_print("SHARE-KICKED\n"); fflush(stdout);
}

/* app.share-info：active session 的 share 状态行。 */
static void on_dev_share_info(GSimpleAction *a, GVariant *p, gpointer ud)
{
    hb_session *sess = dev_active_sess(GTK_APPLICATION(ud));
    HbShareService *s = hb_share_service_default();
    const char *sid;
    (void)a; (void)p;
    sid = sess ? hb_share_service_share_for_session(s, sess) : NULL;
    if (!sid) {
        g_print("SHARE-INFO none\n"); fflush(stdout); return;
    }
    g_print("SHARE-INFO id=%s mode=%d count=%d link=%s code=%s\n",
            sid, hb_share_service_mode(s, sid),
            hb_share_service_count(s, sid),
            hb_share_service_link(s, sid),
            hb_share_service_code(s, sid));
    fflush(stdout);
}

void dev_share_member_list(GtkApplication *app, const char *sid);

/* app.share-join <"member <sid>"|"guest <link> <code>">：加入并开 viewer tab。 */
static void dev_join_cb(HbShareService *s, gpointer ctx, const char *err,
                        gpointer ud)
{
    GtkApplication *app = GTK_APPLICATION(ud);
    GtkWindow *win;
    (void)s;
    if (err) {
        g_print("SHARE-JOIN-FAIL %s\n", err); fflush(stdout);
        return;
    }
    win = gtk_application_get_active_window(app);
    if (HB_IS_WINDOW(win))
        hb_share_open_viewer(GTK_WIDGET(win), ctx,
                             hb_share_peer_ctx_title(ctx));
    g_print("SHARE-JOIN-OK\n"); fflush(stdout);
}

static void on_dev_share_join(GSimpleAction *a, GVariant *p, gpointer ud)
{
    const char *arg;
    char **f;
    (void)a;
    if (!p) return;
    arg = g_variant_get_string(p, NULL);
    f = g_strsplit(arg, " ", 4);
    if (f[0] && f[1] && !strcmp(f[0], "member")) {
        dev_share_member_list(GTK_APPLICATION(ud), f[1]);
    } else if (f[0] && f[1] && f[2] && !strcmp(f[0], "guest")) {
        hb_share_service_join_guest(hb_share_service_default(),
                                    f[1], f[2], dev_join_cb, ud);
    }
    g_strfreev(f);
}

/* app.share-input <text>：活动 tab 是 share viewer → hb_share_peer_input。 */
static void on_dev_share_input(GSimpleAction *a, GVariant *p,
                               gpointer ud)
{
    GtkWindow *win;
    GtkWidget *pg;
    hb_share_peer_ctx *ctx;
    const char *txt;
    (void)a;
    if (!p) return;
    txt = g_variant_get_string(p, NULL);
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    pg = hb_window_active_page(HB_WINDOW(win));
    ctx = hb_share_page_ctx(pg);
    if (!ctx || !txt) {
        g_print("SHARE-INPUT not-a-viewer\n"); fflush(stdout);
        return;
    }
    {
        hb_err e = hb_share_peer_input(hb_share_peer_ctx_engine(ctx),
                                       txt, strlen(txt));
        g_print("SHARE-INPUT %s\n", e == HB_OK ? "ok" : "rejected");
        fflush(stdout);
    }
}

/* app.term-screen <path>：活动 session tab 的可见区文本写文件（dev）。 */
static void on_dev_term_screen(GSimpleAction *a, GVariant *p,
                               gpointer ud)
{
    hb_session *sess = dev_active_sess(GTK_APPLICATION(ud));
    hb_vt *v;
    int rows, cols, r;
    GString *out;
    (void)a;
    if (!p || !sess) {
        g_print("TERM-SCREEN not-a-session\n"); fflush(stdout);
        return;
    }
    v = hb_session_vt(sess);
    if (!v) { g_print("TERM-SCREEN no-vt\n"); fflush(stdout); return; }
    hb_vt_size(v, &rows, &cols);
    out = g_string_new(NULL);
    hb_vt_lock(v);
    for (r = 0; r < rows; r++) {
        char *line = hb_vt_get_text(v, r, 0, r, cols - 1, false);
        if (line) { g_string_append(out, line); g_free(line); }
        g_string_append_c(out, '\n');
    }
    hb_vt_unlock(v);
    {
        g_autofree char *txt = g_string_free(out, FALSE);
        g_file_set_contents(g_variant_get_string(p, NULL), txt, -1,
                            NULL);
    }
    g_print("TERM-SCREEN-WROTE\n"); fflush(stdout);
}

/* app.share-screen <path>：活动 tab 是 share viewer → 文本写文件。 */
static void on_dev_share_screen(GSimpleAction *a, GVariant *p,
                                gpointer ud)
{
    GtkWindow *win;
    GtkWidget *pg;
    char *txt;
    (void)a;
    if (!p) return;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    pg = hb_window_active_page(HB_WINDOW(win));
    if (!HB_IS_SHARE_PAGE(pg)) {
        g_print("SHARE-SCREEN not-a-viewer\n"); fflush(stdout);
        return;
    }
    txt = hb_share_page_screen(pg);
    g_file_set_contents(g_variant_get_string(p, NULL),
                        txt ? txt : "", -1, NULL);
    g_free(txt);
    g_print("SHARE-SCREEN-WROTE\n"); fflush(stdout);
}

/* app.share-state：活动 tab 是 share viewer → 打印 ended 状态（dev）。 */
static void on_dev_share_state(GSimpleAction *a, GVariant *p,
                               gpointer ud)
{
    GtkWindow *win;
    GtkWidget *pg;
    const char *lbl;
    (void)a; (void)p;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    pg = hb_window_active_page(HB_WINDOW(win));
    if (!HB_IS_SHARE_PAGE(pg)) {
        g_print("SHARE-STATE not-a-viewer\n"); fflush(stdout); return;
    }
    lbl = hb_share_page_end_label(pg);
    g_print("SHARE-STATE ended=%d label=%s\n",
            hb_share_page_ended(pg), lbl ? lbl : "-");
    fflush(stdout);
}

/* app.share-list：GET /shares → stdout。 */
static void dev_list_cb(HbShareService *s, GList *items, const char *err,
                        gpointer ud)
{
    GList *l;
    (void)s; (void)ud;
    if (err) {
        g_print("SHARE-LIST-FAIL %s\n", err); fflush(stdout); return;
    }
    g_print("SHARE-LIST n=%d\n", g_list_length(items));
    for (l = items; l; l = l->next) {
        HbShareItem *i = l->data;
        g_print("SHARE-ITEM id=%s dev=%s\n", i->share_id,
                i->owner_device);
    }
    fflush(stdout);
}

static void on_dev_share_list(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p; (void)ud;
    hb_share_service_list(hb_share_service_default(), dev_list_cb, NULL);
}

/* member join：先 list 拿 meta，再 join。 */
typedef struct { GtkApplication *app; char *sid; } MemberJC;
static void member_list_cb(HbShareService *s, GList *items,
                           const char *err, gpointer ud)
{
    MemberJC *jc = ud;
    GList *l;
    (void)s;
    if (!err)
        for (l = items; l; l = l->next) {
            HbShareItem *i = l->data;
            if (!strcmp(i->share_id, jc->sid)) {
                hb_share_service_join_member(
                    hb_share_service_default(), i->share_id,
                    i->meta_b64, dev_join_cb, jc->app);
                g_free(jc->sid); g_free(jc);
                return;
            }
        }
    g_print("SHARE-JOIN-FAIL share-not-found\n"); fflush(stdout);
    g_free(jc->sid); g_free(jc);
}

void dev_share_member_list(GtkApplication *app, const char *sid)
{
    MemberJC *jc = g_new0(MemberJC, 1);
    jc->app = app;
    jc->sid = g_strdup(sid);
    hb_share_service_list(hb_share_service_default(),
                          member_list_cb, jc);
}

/* app.open-host <host_id>：dev 调试动作，按主机 id 打开对应 tab。 */
static void on_dev_open_host(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    const char *id;
    (void)a;
    if (!param) return;
    id = g_variant_get_string(param, NULL);
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (HB_IS_WINDOW(win))
        hb_host_action_connect(GTK_WIDGET(win), hb_store_default(), id);
}

/* app.close-host <host_id>：dev 调试动作，关掉该主机的所有 tab。 */
static void on_dev_close_host(GSimpleAction *a, GVariant *param,
                              gpointer ud)
{
    GtkWindow *win;
    (void)a;
    if (!param) return;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (HB_IS_WINDOW(win))
        hb_window_close_host_tabs(HB_WINDOW(win),
                                  g_variant_get_string(param, NULL));
}

/* ================== dev sync 驱动动作（测试用，仅 debug+DEV_ACTIONS） ================== */

static void dev_auth_cb(HbSyncService *s, const HbSyncOpResult *r,
                        gpointer ud)
{
    (void)s; (void)ud;
    g_printerr("SYNC-AUTH-RESULT ok=%d need_merge=%d code=%s msg=%s\n",
               r->ok, r->need_merge, r->code ? r->code : "-",
               r->message ? r->message : "-");
    if (r->words)
        g_printerr("SYNC-AUTH-WORDS %s\n", r->words);
    if (r->totp_secret)
        g_printerr("SYNC-AUTH-TOTP secret=%s uri=%s\n", r->totp_secret,
                   r->totp_uri ? r->totp_uri : "-");
    if (r->devices) {
        guint _i;
        for (_i = 0; _i < r->devices->len; _i++) {
            HbSyncDevice *_d = g_ptr_array_index(r->devices, _i);
            g_printerr("DEVICE id=%s name=%s platform=%s current=%d\n",
                       _d->id, _d->name ? _d->name : "-",
                       _d->platform ? _d->platform : "-", _d->current);
        }
    }
    if (r->need_merge)
        hb_sync_service_confirm_merge(s, TRUE);
    fflush(stderr);
}

/* app.sync-auth "op|server|email|password|totp|words|newpw"：
 * op = login|register|recover|rotate|password(old=pw,new=newpw) */
static void on_dev_sync_auth(GSimpleAction *a, GVariant *param, gpointer ud)
{
    gchar **f;
    const char *op;
    HbSyncService *s;
    (void)a; (void)ud;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", 8);
    if (!f || !f[0]) { g_strfreev(f); return; }
    op = f[0];
    s = hb_sync_service_default();
    {
        guint _n = g_strv_length(f);
#define DEV_F(i) ((i) < _n ? f[i] : NULL)
    if (!strcmp(op, "login") && DEV_F(3))
        hb_sync_service_login(s, f[1], f[2], f[3], DEV_F(4),
                              dev_auth_cb, NULL);
    else if (!strcmp(op, "register") && DEV_F(3))
        hb_sync_service_register(s, f[1], f[2], f[3], dev_auth_cb, NULL);
    else if (!strcmp(op, "recover") && DEV_F(5) && DEV_F(6))
        hb_sync_service_recover(s, f[1], f[2], f[5], f[6],
                                dev_auth_cb, NULL);
    else if (!strcmp(op, "rotate") && DEV_F(3))
        hb_sync_service_rotate_recovery(s, f[3], dev_auth_cb, NULL);
    else if (!strcmp(op, "password") && DEV_F(3) && DEV_F(6))
        hb_sync_service_change_password(s, f[3], f[6], dev_auth_cb, NULL);
    else if (!strcmp(op, "logout"))
        hb_sync_service_logout(s, g_strcmp0(DEV_F(7), "wipe") == 0);
    else if (!strcmp(op, "totp-setup"))
        hb_sync_service_totp_setup(s, dev_auth_cb, NULL);
    else if (!strcmp(op, "totp-enable") && DEV_F(7))
        hb_sync_service_totp_enable(s, DEV_F(7), dev_auth_cb, NULL);
    else if (!strcmp(op, "totp-disable") && DEV_F(7))
        hb_sync_service_totp_disable(s, DEV_F(7), dev_auth_cb, NULL);
    else if (!strcmp(op, "revoke") && DEV_F(7))
        hb_sync_service_revoke(s, DEV_F(7),
                               g_strcmp0(DEV_F(4), "wipe") == 0,
                               dev_auth_cb, NULL);
    else if (!strcmp(op, "devices"))
        hb_sync_service_devices(s, dev_auth_cb, NULL);
    else if (!strcmp(op, "kick"))
        hb_sync_service_sync_now(s);
    else
        g_printerr("SYNC-AUTH-RESULT bad-op\n");
#undef DEV_F
    }
    g_strfreev(f);
}

/* app.sync-state：打印状态行（dev） */
static void on_dev_sync_state(GSimpleAction *a, GVariant *p, gpointer ud)
{
    HbSyncService *s = hb_sync_service_default();
    (void)a; (void)p; (void)ud;
    g_printerr("SYNC-STATE state=%d conflicts=%zu last=%ld email=%s\n",
               hb_sync_service_state(s), hb_sync_service_conflicts(s),
               (long)hb_sync_service_last_sync(s),
               hb_sync_service_email(s) ? hb_sync_service_email(s) : "-");
    fflush(stderr);
}

/* app.mkobj "type|k=v|k=v…" → 建 vault 对象并打印 id（dev） */
static void on_dev_mkobj(GSimpleAction *a, GVariant *param, gpointer ud)
{
    gchar **f;
    hb_obj *o;
    int i;
    (void)a; (void)ud;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", -1);
    if (!f || !f[0]) { g_strfreev(f); return; }
    o = hb_obj_new(f[0]);
    for (i = 1; f[i]; i++) {
        gchar *eq = strchr(f[i], '=');
        if (!eq) continue;
        *eq = 0;
        if (g_str_has_suffix(eq + 1, "#i"))
            hb_obj_set_int(o, f[i], g_ascii_strtoll(eq + 1, NULL, 10));
        else
            hb_obj_set_str(o, f[i], eq + 1);
    }
    hb_vault_put(hb_store_vault(hb_store_default()), o);
    g_printerr("MKOBJ-RESULT id=%s type=%s\n", hb_obj_id(o), f[0]);
    fflush(stderr);
    hb_obj_free(o);
    g_strfreev(f);
}

/* app.setsecret "id|field|text" → 写 vault secret 字段（dev 导入密钥用） */
static void on_dev_setsecret(GSimpleAction *a, GVariant *param, gpointer ud)
{
    gchar **f;
    (void)a; (void)ud;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", 3);
    if (!f || !f[0] || !f[1] || !f[2]) { g_strfreev(f); return; }
    {
        gchar *text = f[2];
        g_autofree gchar *filetext = NULL;
        if (text[0] == '@' && g_file_get_contents(text + 1, &filetext,
                                                NULL, NULL))
            text = filetext;
        if (hb_vault_set_secret(hb_store_vault(hb_store_default()), f[0],
                                f[1], text) == HB_OK) {
            g_printerr("SETSECRET-RESULT id=%s field=%s ok=1\n", f[0],
                       f[1]);
            fflush(stderr);
        }
        if (filetext) memset(filetext, 0, strlen(filetext));
    }
    memset(f[2], 0, strlen(f[2]));
    g_strfreev(f);
}

/* app.chksecret "id|field" → 打印 secret 是否存在及长度（dev 验证） */
static void on_dev_chksecret(GSimpleAction *a, GVariant *param,
                             gpointer ud)
{
    gchar **f;
    char *sec;
    (void)a; (void)ud;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", 2);
    if (!f || !f[0] || !f[1]) { g_strfreev(f); return; }
    sec = hb_vault_get_secret(hb_store_vault(hb_store_default()),
                              f[0], f[1]);
    g_printerr("CHKSECRET id=%s field=%s len=%zu\n", f[0], f[1],
               sec ? strlen(sec) : (size_t)0);
    if (sec) hb_secret_free(sec);
    fflush(stderr);
    g_strfreev(f);
}

/* app.resolve "id|other" / "id|keep" → 调 UI 冲突按钮同一处理（dev） */
static void on_dev_resolve(GSimpleAction *a, GVariant *param, gpointer ud)
{
    gchar **f;
    hb_sync *sync;
    (void)a; (void)ud;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", 2);
    if (!f || !f[0] || !f[1]) { g_strfreev(f); return; }
    sync = hb_sync_service_sync(hb_sync_service_default());
    if (!strcmp(f[1], "other")) {
        hb_sync_conflict_take_other(sync, f[0]);
        hb_sync_service_sync_now(hb_sync_service_default());
    } else {
        hb_sync_conflict_keep_local(sync, f[0]);
    }
    g_printerr("RESOLVE-RESULT id=%s mode=%s\n", f[0], f[1]);
    fflush(stderr);
    g_strfreev(f);
}

/* app.dev-words <24 words> → 弹恢复码网格（dev 截图） */
static void on_dev_words(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    (void)a; (void)ud;
    if (!param) return;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (win)
        hb_account_view_dev_words(GTK_WIDGET(win),
                                  g_variant_get_string(param, NULL));
}

/* app.objcount <type|"all"> → 打印同步范围对象数量（dev） */
static void on_dev_objcount(GSimpleAction *a, GVariant *param, gpointer ud)
{
    static const char *types[] = {
        "host", "group", "key", "snippet", "forward", "known", "settings",
    };
    const char *want;
    size_t i;
    (void)a; (void)ud;
    want = param ? g_variant_get_string(param, NULL) : "all";
    for (i = 0; i < G_N_ELEMENTS(types); i++) {
        size_t n = 0;
        hb_obj **list;
        if (strcmp(want, "all") && strcmp(want, types[i])) continue;
        list = hb_store_list(hb_store_default(), types[i], &n);
        g_printerr("OBJCOUNT type=%s n=%zu\n", types[i], n);
        if (list) {
            size_t k;
            for (k = 0; k < n; k++) {
                const char *nm = hb_obj_get_str(list[k], "name");
                g_printerr("OBJ id=%s name=%s\n", hb_obj_id(list[k]),
                           nm ? nm : "-");
            }
            hb_obj_list_free(list, n);
        }
    }
    fflush(stderr);
}

/* app.setfield "id|k=v" → 改字段；v 后缀 #i 为 int（dev） */
static void on_dev_setfield(GSimpleAction *a, GVariant *param, gpointer ud)
{
    gchar **f;
    hb_obj *o = NULL;
    gchar *eq;
    (void)a; (void)ud;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", 3);
    if (!f || !f[0] || !f[1]) { g_strfreev(f); return; }
    eq = strchr(f[1], '=');
    if (!eq) { g_strfreev(f); return; }
    *eq = 0;
    if (hb_vault_get(hb_store_vault(hb_store_default()), f[0], &o) == HB_OK
        && o) {
        if (g_str_has_suffix(eq + 1, "#i"))
            hb_obj_set_int(o, f[1], g_ascii_strtoll(eq + 1, NULL, 10));
        else
            hb_obj_set_str(o, f[1], eq + 1);
        hb_vault_put(hb_store_vault(hb_store_default()), o);
        g_printerr("SETFIELD-RESULT id=%s ok=1\n", f[0]);
        fflush(stderr);
        hb_obj_free(o);
    }
    g_strfreev(f);
}

/* app.share-dialog：在活动 session tab 上弹 owner 共享对话框（dev）。 */
static void on_dev_share_dialog(GSimpleAction *a, GVariant *p,
                                gpointer ud)
{
    GtkWindow *win;
    GtkWidget *pg;
    hb_session *sess;
    (void)a; (void)p;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    pg = hb_window_active_page(HB_WINDOW(win));
    sess = dev_active_sess(GTK_APPLICATION(ud));
    if (!sess || !pg) return;
    hb_share_dialog_present(pg, sess, "dev-share", "localhost");
}

/* app.vault-open <section>：切到 vault tab 并选 section（dev）。 */
static void on_dev_vault_open(GSimpleAction *a, GVariant *p, gpointer ud)
{
    GtkWindow *win;
    GtkWidget *vp;
    (void)a;
    if (!p) return;
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) return;
    vp = hb_window_vault_page(HB_WINDOW(win));
    if (!vp) return;
    hb_window_switch_tab(HB_WINDOW(win), "vault");
    hb_vault_page_set_section(HB_VAULT_PAGE(vp),
                              g_variant_get_string(p, NULL));
}
#endif /* !NDEBUG */

/* ------------------------------ 生命周期 ------------------------------ */

#ifndef NDEBUG
/* app.window-close：dev 动作，触发 close-request（= WM X 按钮路径）。 */
static void on_dev_win_close(GSimpleAction *a, GVariant *p, gpointer ud)
{
    GtkWindow *w;
    (void)a; (void)p;
    w = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (w)
        gtk_window_close(w);
}
#endif

/* app.show-tab <tab_id>：监控通知的默认动作 —— 呈现窗口并切到该
 * tab；tab 已关则什么都不做（遍历所有窗口）。 */
static void on_show_tab(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GApplication *app = ud;
    GList *ws, *l;
    const char *id;
    (void)a;
    if (!param) return;
    id = g_variant_get_string(param, NULL);
    ws = gtk_application_get_windows(GTK_APPLICATION(app));
    for (l = ws; l; l = l->next) {
        if (HB_IS_WINDOW(l->data) &&
            hb_window_has_tab(HB_WINDOW(l->data), id)) {
            hb_window_switch_tab(HB_WINDOW(l->data), id);
            gtk_window_present(GTK_WINDOW(l->data));
            return;
        }
    }
}

/* ------------------------------ 托盘 ------------------------------ */

static int tray_count_online(void *ud)
{
    GList *ws, *l;
    int n = 0;
    (void)ud;
    ws = gtk_application_get_windows(
        GTK_APPLICATION(g_application_get_default()));
    for (l = ws; l; l = l->next)
        if (HB_IS_WINDOW(l->data))
            n += hb_window_online_count(HB_WINDOW(l->data));
    n += (int)hb_forward_mgr_running_count(hb_forward_mgr_default());
    return n;
}

static void tray_activate(void *ud)
{
    GList *ws, *l;
    (void)ud;
    ws = gtk_application_get_windows(
        GTK_APPLICATION(g_application_get_default()));
    for (l = ws; l; l = l->next)
        if (HB_IS_WINDOW(l->data)) {
            gtk_window_present(GTK_WINDOW(l->data));
            return;
        }
    g_application_activate(g_application_get_default());
}

static void tray_disconnect_all(void *ud)
{
    GList *ws, *l;
    (void)ud;
    ws = gtk_application_get_windows(
        GTK_APPLICATION(g_application_get_default()));
    for (l = ws; l; l = l->next)
        if (HB_IS_WINDOW(l->data))
            hb_window_disconnect_all(HB_WINDOW(l->data));
    hb_forward_mgr_stop_all(hb_forward_mgr_default());
    hb_tray_sessions_changed(app_tray);
}

static void tray_quit(void *ud)
{
    (void)ud;
    g_application_quit(g_application_get_default());
}

gboolean hb_application_tray_available(void)
{
    return hb_tray_available(app_tray);
}

/* 会话状态变化的统一上报点（窗口/页面在加删 tab、改 dot 时调）。 */
void hb_app_note_sessions(void)
{
    hb_tray_sessions_changed(app_tray);
}

/* 读 settings 的 close_to_tray（默认 TRUE）。 */
static gboolean want_close_to_tray(void)
{
    hb_obj *o = NULL;
    gboolean v = TRUE;
    if (hb_vault_settings(hb_store_vault(hb_store_default()),
                          &o) == HB_OK && o) {
        v = hb_obj_get_bool(o, "close_to_tray", TRUE);
        hb_obj_free(o);
    }
    return v;
}

/* 关窗 → 托盘可用时隐窗保活，一次性通知提示。返回 TRUE 吞掉关闭。 */
static gboolean on_close_request(GtkWindow *win, gpointer ud)
{
    GNotification *n;
    (void)ud;
    if (!want_close_to_tray() || !hb_tray_available(app_tray))
        return FALSE;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    g_application_hold(g_application_get_default());
    if (!tray_notify_shown) {
        tray_notify_shown = TRUE;
        n = g_notification_new("Harbor 仍在托盘中运行");
        g_notification_set_body(n, "会话保持连接；点托盘图标可重新打开窗口。");
        g_application_send_notification(
            g_application_get_default(), "tray-alive", n);
        g_object_unref(n);
    }
    return TRUE;
}

/* ------------------------- start_at_login ------------------------- */


/* --background：3s 内 watcher 出现 → 保持无窗；否则正常开窗。 */
static gboolean bg_check(gpointer ud)
{
    GtkWidget *win = ud;
    static int ticks;
    if (hb_tray_available(app_tray)) {
        g_application_hold(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    if (++ticks >= 12) {          /* ~3s */
        ticks = 0;
        gtk_window_present(GTK_WINDOW(win));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* 暗色 token provider：有效主题为暗时加载 theme-dark.css 覆盖同名
 * @define-color，style.css 里的全部颜色即随之翻转。 */
static GtkCssProvider *dark_css;

static void update_dark_provider(void)
{
    gboolean dark = adw_style_manager_get_dark(
        adw_style_manager_get_default());
    GdkDisplay *d = gdk_display_get_default();

    if (dark && !dark_css) {
        dark_css = gtk_css_provider_new();
        gtk_css_provider_load_from_resource(
            dark_css, "/dev/harbor/Harbor/theme-dark.css");
        gtk_style_context_add_provider_for_display(
            d, GTK_STYLE_PROVIDER(dark_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    } else if (!dark && dark_css) {
        gtk_style_context_remove_provider_for_display(
            d, GTK_STYLE_PROVIDER(dark_css));
        g_clear_object(&dark_css);
    }
}

static void on_dark_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    (void)o; (void)ps; (void)ud;
    update_dark_provider();
}

static void on_store_changed_theme(HbStore *st, const char *type,
                                   const char *id, gpointer ud)
{
    (void)st; (void)id; (void)ud;
    if (!g_strcmp0(type, "settings"))
        hb_application_apply_theme();
}

void hb_application_apply_theme(void)
{
    hb_obj *o = NULL;
    const char *t = "light";
    AdwStyleManager *m = adw_style_manager_get_default();

    if (hb_vault_settings(hb_store_vault(hb_store_default()), &o) == HB_OK
        && o) {
        const char *x = hb_obj_get_str(o, "ui_theme");
        if (x && *x)
            t = x;
    }
    if (!strcmp(t, "dark"))
        adw_style_manager_set_color_scheme(m, ADW_COLOR_SCHEME_FORCE_DARK);
    else if (!strcmp(t, "system"))
        adw_style_manager_set_color_scheme(m, ADW_COLOR_SCHEME_DEFAULT);
    else
        adw_style_manager_set_color_scheme(m, ADW_COLOR_SCHEME_FORCE_LIGHT);
    hb_obj_free(o);
    update_dark_provider();
}

#ifndef NDEBUG
/* app.dev-ui "cmd|arg"：调试构建的 UI 驱动钩子（截图/无头验证）。
 * cmd: section|name  drawer|hid  hostform[|hid]  palette  notif
 *      toast|text  sftp|hid  key|accel  scheme|light|dark|system
 *      listmode|0|1  bkexport  bkimport  drawerclose  closetop */
static void on_dev_ui(GSimpleAction *a, GVariant *param, gpointer ud)
{
    GtkWindow *win;
    gchar **f;
    (void)a;
    if (!param) return;
    f = g_strsplit(g_variant_get_string(param, NULL), "|", 4);
    if (!f || !f[0]) { g_strfreev(f); return; }
    win = gtk_application_get_active_window(GTK_APPLICATION(ud));
    if (!HB_IS_WINDOW(win)) { g_strfreev(f); return; }

    if (!strcmp(f[0], "section") && f[1]) {
        GtkWidget *vp = hb_window_vault_page(HB_WINDOW(win));
        if (vp) hb_vault_page_set_section(HB_VAULT_PAGE(vp), f[1]);
    } else if (!strcmp(f[0], "drawer") && f[1]) {
        GtkWidget *vp = hb_window_vault_page(HB_WINDOW(win));
        GtkWidget *hv = vp ? hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))
                           : NULL;
        if (hv) hb_hosts_view_show_drawer(HB_HOSTS_VIEW(hv), f[1]);
    } else if (!strcmp(f[0], "hostform")) {
        hb_host_form_open(GTK_WIDGET(win), hb_store_default(),
                          f[1] && *f[1] ? f[1] : NULL);
    } else if (!strcmp(f[0], "palette")) {
        hb_palette_open(HB_WINDOW(win));
    } else if (!strcmp(f[0], "notifpost") && f[1]) {
        hb_notify_post("info", f[1], f[2] ? f[2] : "", NULL);
    } else if (!strcmp(f[0], "notif")) {
        hb_window_notif_toggle(HB_WINDOW(win));
    } else if (!strcmp(f[0], "toast") && f[1]) {
        hb_toast(GTK_WIDGET(win), f[1]);
    } else if (!strcmp(f[0], "sftp") && f[1]) {
        hb_host_action_sftp(GTK_WIDGET(win), hb_store_default(), f[1]);
    } else if (!strcmp(f[0], "scheme") && f[1]) {
        AdwStyleManager *m = adw_style_manager_get_default();
        if (!strcmp(f[1], "dark"))
            adw_style_manager_set_color_scheme(m, ADW_COLOR_SCHEME_FORCE_DARK);
        else if (!strcmp(f[1], "system"))
            adw_style_manager_set_color_scheme(m, ADW_COLOR_SCHEME_DEFAULT);
        else
            adw_style_manager_set_color_scheme(m, ADW_COLOR_SCHEME_FORCE_LIGHT);
    } else if (!strcmp(f[0], "listmode") && f[1]) {
        GtkWidget *vp = hb_window_vault_page(HB_WINDOW(win));
        GtkWidget *hv = vp ? hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))
                           : NULL;
        if (hv)
            hb_hosts_view_set_list_mode(HB_HOSTS_VIEW(hv),
                                        !strcmp(f[1], "1"));
    } else if (!strcmp(f[0], "bkexport") || !strcmp(f[0], "bkimport")) {
        GtkWidget *vp = hb_window_vault_page(HB_WINDOW(win));
        GtkWidget *sv = vp ? hb_vault_page_settings_view(
                                HB_VAULT_PAGE(vp)) : NULL;
        if (sv && !strcmp(f[0], "bkexport"))
            hb_settings_view_export_backup(HB_SETTINGS_VIEW(sv));
        else if (sv && f[1] && *f[1])
            hb_settings_view_import_backup_dev(HB_SETTINGS_VIEW(sv),
                                               f[1]);
        else if (sv)
            hb_settings_view_import_backup(HB_SETTINGS_VIEW(sv));
    } else if (!strcmp(f[0], "bkout") && f[1]) {
        hb_settings_view_bk_dev_out(f[1]);
    } else if (!strcmp(f[0], "bkfill") && f[1]) {
        hb_settings_view_bk_dev_fill(f[1]);
    } else if (!strcmp(f[0], "bkpress")) {
        hb_settings_view_bk_dev_press();
    } else if (!strcmp(f[0], "bkconfirm")) {
        hb_settings_view_bk_dev_confirm(!f[1] || strcmp(f[1], "0"));
    } else if ((!strcmp(f[0], "scedit") || !strcmp(f[0], "sckey"))
               && f[1]) {
        GtkWidget *vp = hb_window_vault_page(HB_WINDOW(win));
        GtkWidget *sv = vp ? hb_vault_page_settings_view(
                                HB_VAULT_PAGE(vp)) : NULL;
        if (sv)
            g_printerr("DEVUI-SC %s\n",
                hb_settings_view_sc_dev(HB_SETTINGS_VIEW(sv),
                                        (size_t)atoi(f[1]),
                    !strcmp(f[0], "sckey") ? f[2] : NULL)
                    ? "ok" : "fail");
    } else if (!strcmp(f[0], "drawerclose")) {
        GtkWidget *vp = hb_window_vault_page(HB_WINDOW(win));
        GtkWidget *hv = vp ? hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))
                           : NULL;
        if (hv) hb_hosts_view_close_drawer(HB_HOSTS_VIEW(hv));
    } else if (!strcmp(f[0], "closetop")) {
        GtkWidget *pal = g_object_get_data(G_OBJECT(win), "hb-palette");
        GList *tl, *l;
        if (pal) gtk_widget_unparent(pal);
        tl = gtk_window_list_toplevels();
        for (l = tl; l; l = l->next) {
            GtkWidget *w = l->data;
            if (w == (GtkWidget *)win) continue;
            if (ADW_IS_DIALOG(w)) adw_dialog_close(ADW_DIALOG(w));
            else if (GTK_IS_POPOVER(w)) gtk_popover_popdown(GTK_POPOVER(w));
            else if (GTK_IS_WINDOW(w)) gtk_window_close(GTK_WINDOW(w));
        }
        g_list_free(tl);
    } else if (!strcmp(f[0], "key") && f[1]) {
        guint k = 0;
        GdkModifierType m = 0;
        if (gtk_accelerator_parse(f[1], &k, &m) && k)
            g_printerr("DEVUI-KEY %s %s\n", f[1],
                hb_shortcuts_inject(HB_WINDOW(win), k, m)
                    ? "handled" : "ignored");
    }
    fflush(stderr);
    g_strfreev(f);
}
#endif /* !NDEBUG */


static void hb_application_startup(GApplication *app)
{
    GtkCssProvider *css;
    GSimpleAction *sa;

    G_APPLICATION_CLASS(hb_application_parent_class)->startup(app);

    hb_store_default();          /* vault 打开 + 首次播种 */
    hb_notify_default();

    /* FR-10：启动时按 log_retention_days（默认 90；<=0 = 永久）清理日志 */
    {
        hb_obj *o = NULL;
        hb_vault *v = hb_store_vault(hb_store_default());
        if (hb_vault_settings(v, &o) == HB_OK && o) {
            hb_vault_log_purge(v, (int)hb_obj_get_int(
                                   o, "log_retention_days", 90));
            hb_obj_free(o);
        }
    }

    sa = g_simple_action_new("show-tab", G_VARIANT_TYPE_STRING);
    g_signal_connect(sa, "activate", G_CALLBACK(on_show_tab), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
    g_object_unref(sa);

    sa = g_simple_action_new("quit", NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(tray_quit), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
    g_object_unref(sa);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app),
        "app.quit", (const char *[]){ "<Ctrl>q", NULL });

    {
        HbTrayHooks hooks = { tray_count_online, tray_activate,
                              tray_disconnect_all, tray_quit, NULL };
        app_tray = hb_tray_new(&hooks);
    }

#ifndef NDEBUG

    /* shot/feed/open-host 会经 org.gtk.Actions 导出到 session bus，
     * 任何同 bus 进程都能驱动会话 —— 仅 debug build + 显式开关。 */
    if (!g_strcmp0(g_getenv("HARBOR_DEV_ACTIONS"), "1")) {
        sa = g_simple_action_new("dev-ui", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_ui), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("shot", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_shot), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("window-close", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_win_close), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("feed", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_feed), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("term-screen", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_term_screen), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("close-host", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_close_host),
                         app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("open-host", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_open_host), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("sync-auth", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_sync_auth), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("sync-state", NULL);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_sync_state), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("mkobj", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_mkobj), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("setfield", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_setfield), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("setsecret", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_setsecret), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("resolve", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_resolve), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("chksecret", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_chksecret),
                         app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-start", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_start), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-stop", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_stop), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-mode", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_mode), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-regen", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_regen), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-kick", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_kick), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-info", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_info), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-join", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_join), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-input", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_input), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-screen", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_screen), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-list", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_list), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-state", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_state), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("vault-open", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_vault_open), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("share-dialog", NULL);
        g_signal_connect(sa, "activate",
                         G_CALLBACK(on_dev_share_dialog), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("dev-words", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_words), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);

        sa = g_simple_action_new("objcount", G_VARIANT_TYPE_STRING);
        g_signal_connect(sa, "activate", G_CALLBACK(on_dev_objcount), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sa));
        g_object_unref(sa);
    }
#endif


    css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/dev/harbor/Harbor/style.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_signal_connect(adw_style_manager_get_default(), "notify::dark",
        G_CALLBACK(on_dark_changed), NULL);
    /* settings.shortcuts/ui_theme 被本机或 sync 改动时即时应用 */
    g_signal_connect(hb_store_default(), "changed",
        G_CALLBACK(on_store_changed_theme), NULL);
    hb_application_apply_theme();
    hb_term_theme_install_css();
}

/* Hosts 页 connect(id)：ssh → SSH 页；local → Local 页；rdp/vnc → 占位 toast */
static void on_host_connect(GtkWidget *hv, const char *hid, gpointer ud)
{
    HbWindow *win = ud;
    hb_obj *o = NULL;
    (void)hv;
    if (hb_vault_get(hb_store_vault(hb_store_default()), hid, &o) != HB_OK ||
        !o)
        return;
    hb_host_connect_obj(win, hb_store_default(), o);
    hb_obj_free(o);
}

/* 快速连接：已有主机精确命中（name == 文本，或解析出的
 * address+port+user 相等 —— 不做子串匹配，10.0.0.1 不会误连
 * 10.0.0.12）则直接 connect；否则解析文本建主机。 */
static void quick_connect(HbWindow *win, const char *text)
{
    char *user = NULL, *host = NULL;
    int port = 0;
    hb_obj *h, **objs = NULL;
    size_t n = 0, i;
    hb_err pe;
    hb_vault *v = hb_store_vault(hb_store_default());

    pe = hb_ssh_parse_target(text, &user, &host, &port);
    objs = hb_store_list(hb_store_default(), "host", &n);
    for (i = 0; i < n; i++) {
        const char *nm = hb_obj_get_str(objs[i], "name");
        const char *ad = hb_obj_get_str(objs[i], "address");
        const char *hu = hb_obj_get_str(objs[i], "user");
        int hp = (int)hb_obj_get_int(objs[i], "port", 22);
        gboolean hit = FALSE;
        if (nm && !g_ascii_strcasecmp(nm, text))
            hit = TRUE;
        else if (pe == HB_OK && host && ad &&
                 !g_ascii_strcasecmp(ad, host) &&
                 (hp > 0 ? hp : 22) == (port > 0 ? port : 22) &&
                 (!user || !*user || (hu && !strcmp(hu, user))))
            hit = TRUE;
        if (hit) {
            hb_host_connect_obj(win, hb_store_default(), objs[i]);
            hb_obj_list_free(objs, n);
            g_free(user); g_free(host);
            return;
        }
    }
    if (objs) hb_obj_list_free(objs, n);

    if (pe != HB_OK || !host || !*host) {
        hb_toast(GTK_WIDGET(win), "格式：user@hostname[:port]");
        g_free(user); g_free(host);
        return;
    }
    h = hb_obj_new("host");
    hb_obj_set_str(h, "name", host);
    hb_obj_set_str(h, "address", host);
    hb_obj_set_str(h, "user", user && *user ? user : g_get_user_name());
    hb_obj_set_int(h, "port", port > 0 ? port : 22);
    hb_obj_set_str(h, "proto", "ssh");
    hb_obj_set_str(h, "os", "auto");
    hb_obj_set_str(h, "auth", "auto");
    hb_vault_put(v, h);
    hb_host_connect_obj(win, hb_store_default(), h);
    hb_obj_free(h);
    g_free(user); g_free(host);
}

static void on_quick_connect(GtkWidget *hv, const char *text, gpointer ud)
{
    (void)hv;
    quick_connect((HbWindow *)ud, text);
}

static void hb_application_activate(GApplication *app)
{
    HbWindow *win;
    GtkWidget *vp;
    GtkWindow *cur = gtk_application_get_active_window(
        GTK_APPLICATION(app));

    if (cur) {
        gtk_window_present(cur);
        return;
    }

    win = hb_window_new(GTK_APPLICATION(app));

    vp = hb_vault_page_new();
    hb_window_add_tab(win, "vault", "Vault", "server", vp, FALSE, FALSE);
    hb_window_set_vault_page(win, vp);
    {
        GtkWidget *sv = hb_sftp_view_new(hb_store_default());
        hb_window_add_tab(win, "sftp", "SFTP", "folder",
                          sv, FALSE, FALSE);
        hb_sftp_view_attach_tab(HB_SFTP_VIEW(sv),
                                GTK_WIDGET(win), "sftp");
    }
    hb_window_switch_tab(win, "vault");

    /* dev 选项 */
    if (hb_opt_section)
        hb_vault_page_set_section(HB_VAULT_PAGE(vp), hb_opt_section);
    if (hb_opt_filter)
        hb_hosts_view_set_search(
            HB_HOSTS_VIEW(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))),
            hb_opt_filter);
    if (hb_opt_host_view)
        hb_hosts_view_set_list_mode(
            HB_HOSTS_VIEW(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp))),
            !strcmp(hb_opt_host_view, "list"));
    g_signal_connect(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp)),
                     "connect", G_CALLBACK(on_host_connect), win);
    g_signal_connect(hb_vault_page_hosts_view(HB_VAULT_PAGE(vp)),
                     "quick-connect", G_CALLBACK(on_quick_connect), win);
    if (hb_opt_connect)
        quick_connect(win, hb_opt_connect);
    if (hb_opt_open_local) {
        GtkWidget *page = NULL;
        hb_window_open_local(win, &page);
        if (page && *hb_opt_open_local) {
            char *cmd = g_strconcat(hb_opt_open_local, "\n", NULL);
            hb_term_page_feed_input(HB_TERM_PAGE(page), cmd);
            g_free(cmd);
        }
    }

    g_signal_connect(win, "close-request",
                     G_CALLBACK(on_close_request), NULL);

    if (hb_opt_background) {
        /* --background：等托盘 watcher ~3s；出现则不开窗 */
        g_timeout_add(250, bg_check, win);
    } else {
        gtk_window_present(GTK_WINDOW(win));
    }

    if (hb_opt_screenshot)
        g_timeout_add(hb_opt_screenshot_delay, take_shot, win);
}

static void hb_application_class_init(HbApplicationClass *klass)
{
    GApplicationClass *ac = G_APPLICATION_CLASS(klass);
    ac->startup = hb_application_startup;
    ac->activate = hb_application_activate;
}

static void hb_application_init(HbApplication *app)
{
    (void)app;
}

HbApplication *hb_application_new(void)
{
    return g_object_new(HB_TYPE_APPLICATION,
        "application-id", "dev.harbor.Harbor",
        "flags", G_APPLICATION_DEFAULT_FLAGS,
        NULL);
}
