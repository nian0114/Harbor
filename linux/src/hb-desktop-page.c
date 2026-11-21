/* hb-desktop-page.c — FR-07 VNC/RDP 远程桌面页。
 *
 * 生命周期：连接视图（SSH 隧道阶段经 HbConnCtl FORWARD，tab 私有规则，
 * 不落 vault）→ rd 事件流驱动（STAGE/PROMPT/CERT/CONNECTED）→
 * 画布（FRAME→snapshot 拷贝帧）→ 断线退避重连 / 错误卡。
 *
 * 关闭路径绝不阻塞 UI：hb_rd_close / hb_session_close 与 *_free 全部
 * 放到工作线程（hb_conn_ctl_steal_session 先收走会话所有权）。 */

#include "hb-desktop-page.h"
#include "hb-conn.h"
#include "hb-connect-view.h"
#include "hb-dialogs.h"
#include "hb-icons.h"
#include "hb-window.h"

#include <harbor/harbor_core.h>
#include <adwaita.h>
#include <glib-unix.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================ 画布 ============================ */
/* 只负责渲染：FRAME 事件标脏 + queue_draw；snapshot 时（每个 frame
 * clock 至多一次）锁 fb 拷一份 BGRX → GdkMemoryTexture → scaled 绘制。 */

#define RD_TYPE_CANVAS (rd_canvas_get_type())
G_DECLARE_FINAL_TYPE(RdCanvas, rd_canvas, RD, CANVAS, GtkWidget)

struct _RdCanvas {
    GtkWidget parent_instance;
    hb_rd    *rd;          /* borrowed（page 拥有；detach 后 NULL） */
    GdkTexture *tex;       /* 最近一次 snapshot 的纹理 */
    int       fb_w, fb_h;
    gboolean  dirty;
    gboolean  orig;        /* TRUE=原始大小（1:1 + 滚动）；FALSE=适应窗口 */
};

G_DEFINE_FINAL_TYPE(RdCanvas, rd_canvas, GTK_TYPE_WIDGET)

static void rd_canvas_init(RdCanvas *c)
{
    gtk_widget_set_focusable(GTK_WIDGET(c), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(c), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(c), TRUE);
}

/* fit 模式下图像在控件内的目标矩形（原始模式 = 0,0 + 原尺寸） */
static void canvas_draw_rect(RdCanvas *c, float *x, float *y,
                             float *w, float *h)
{
    int aw = gtk_widget_get_width(GTK_WIDGET(c));
    int ah = gtk_widget_get_height(GTK_WIDGET(c));
    if (c->orig || c->fb_w <= 0 || c->fb_h <= 0) {
        *x = *y = 0;
        *w = c->fb_w;
        *h = c->fb_h;
        return;
    }
    float sx = (float)aw / c->fb_w, sy = (float)ah / c->fb_h;
    float s = sx < sy ? sx : sy;
    if (s <= 0) s = 1;
    *w = c->fb_w * s;
    *h = c->fb_h * s;
    *x = (aw - *w) / 2.f;
    *y = (ah - *h) / 2.f;
}

/* 控件坐标 → 帧缓冲坐标；图外（letterbox）返回 FALSE */
static gboolean rd_canvas_map(RdCanvas *c, double x, double y,
                              double *fx, double *fy)
{
    float rx, ry, rw, rh;
    canvas_draw_rect(c, &rx, &ry, &rw, &rh);
    if (rw <= 0 || rh <= 0 || x < rx || y < ry ||
        x >= rx + rw || y >= ry + rh)
        return FALSE;
    *fx = (x - rx) / rw * c->fb_w;
    *fy = (y - ry) / rh * c->fb_h;
    return TRUE;
}

static void rd_canvas_snapshot(GtkWidget *w, GtkSnapshot *sn)
{
    RdCanvas *c = RD_CANVAS(w);
    if (!c->rd)
        return;
    if (c->dirty || !c->tex) {
        const uint8_t *px;
        int fw, fh, stride;
        if (hb_rd_fb_lock(c->rd, &px, &fw, &fh, &stride)) {
            GBytes *gb;
            c->fb_w = fw;
            c->fb_h = fh;
            gb = g_bytes_new(px, (gsize)fh * stride); /* 拷贝；锁只持 memcpy */
            hb_rd_fb_unlock(c->rd);
            g_clear_object(&c->tex);
            c->tex = gdk_memory_texture_new(fw, fh,
                                            GDK_MEMORY_B8G8R8X8,
                                            gb, stride);
            g_bytes_unref(gb);
        }
        c->dirty = FALSE;
    }
    if (!c->tex)
        return;
    float rx, ry, rw, rh;
    canvas_draw_rect(c, &rx, &ry, &rw, &rh);
    if (rw > 0 && rh > 0)
        gtk_snapshot_append_scaled_texture(
            sn, c->tex, GSK_SCALING_FILTER_LINEAR,
            &GRAPHENE_RECT_INIT(rx, ry, rw, rh));
}

static void rd_canvas_dispose(GObject *o)
{
    RdCanvas *c = RD_CANVAS(o);
    g_clear_object(&c->tex);
    G_OBJECT_CLASS(rd_canvas_parent_class)->dispose(o);
}

static void rd_canvas_class_init(RdCanvasClass *cl)
{
    GTK_WIDGET_CLASS(cl)->snapshot = rd_canvas_snapshot;
    G_OBJECT_CLASS(cl)->dispose = rd_canvas_dispose;
    gtk_widget_class_set_layout_manager_type(
        GTK_WIDGET_CLASS(cl), GTK_TYPE_BIN_LAYOUT);
}

static GtkWidget *rd_canvas_new(void)
{
    return g_object_new(RD_TYPE_CANVAS, NULL);
}

static void rd_canvas_attach(RdCanvas *c, hb_rd *rd)
{
    c->rd = rd;
    c->fb_w = c->fb_h = 0;
    c->dirty = TRUE;
    g_clear_object(&c->tex);
    gtk_widget_queue_draw(GTK_WIDGET(c));
}

static void rd_canvas_frame(RdCanvas *c)
{
    c->dirty = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(c));
}

/* 远端尺寸变化：原始模式改变自然尺寸 → 滚动区跟随 */
static void rd_canvas_set_size(RdCanvas *c, int w, int h)
{
    c->fb_w = w;
    c->fb_h = h;
    if (c->orig && w > 0 && h > 0)
        gtk_widget_set_size_request(GTK_WIDGET(c), w, h);
    else
        gtk_widget_set_size_request(GTK_WIDGET(c), -1, -1);
}

/* ============================ 页面 ============================ */

struct _HbDesktopPage {
    GtkBox parent;

    HbStore  *store;
    HbWindow *win;
    char     *tab_id;

    /* 主机快照（每次连接前重读） */
    char    *host_id;
    char    *hname, *haddr, *huser, *hdomain;
    char     proto[8];        /* "rdp" | "vnc" */
    int      hport;
    gboolean use_tunnel;
    char    *via_id, *via_name, *via_user;
    gboolean via_same;        /* dest = 127.0.0.1（经主机同址/回环） */
    hb_rd_quality quality;

    /* 连接流程 */
    GtkWidget *stack, *conn_view;
    HbConnCtl *ctl;           /* tunnel 模式 */
    hb_session *tun_sess;     /* borrowed */
    guint     tun_ev_src;
    int       bound_port;

    hb_rd    *rd;
    guint     rd_ev_src;
    gboolean  rd_live;        /* rd 对象存在 */
    gboolean  connected;
    gboolean  ever_connected;

    /* 异步 teardown / 陈旧回调防护 */
    guint     gen;
    gboolean  disposed;      /* dispose 已跑；比 gen=0 更直接的临终标记 */

    /* 重连 */
    guint     bo_src;
    int       bo_k;           /* 退避第 k 次 */
    int       countdown;
    gboolean  connecting_now; /* 正在重连（覆盖层显示「正在重连…」） */

    /* 桌面视图 */
    GtkWidget *banner_lbl, *sec_lbl, *pass_hint, *ro_pill;
    GtkWidget *q_mb;
    GtkWidget *scrolled, *canvas;
    GtkWidget *disc_ov, *disc_lbl; /* 断线覆盖层 */
    GtkWidget *dim;
    gboolean  view_only, passthrough, orig_size;

    /* 对话框跟踪（一次至多一个；gen 防陈旧应答） */
    AdwDialog *open_dlg;

    /* 剪贴板 */
    char        *last_remote_clip;
    GCancellable *clip_cancel;
    gboolean     clip_busy;
    gulong       clip_handler;

    /* RDP 动态分辨率 */
    guint rsz_src;
    int   alloc_w, alloc_h;

    /* vault 日志 */
    int64_t log_id;
    gint64  log_t0;
};

G_DEFINE_FINAL_TYPE(HbDesktopPage, hb_desktop_page, GTK_TYPE_BOX)

enum { SIG_EDIT_HOST, N_SIGS };
static guint sigs[N_SIGS];

static void start_attempt(HbDesktopPage *p, gboolean is_reconnect);
static void banner_refresh(HbDesktopPage *p);
static void start_rd(HbDesktopPage *p, const char *host, int port);
static void kill_session_async(HbDesktopPage *p, hb_rd *rd,
                               hb_session *sess, gboolean restart);

/* ------------------------- 工具 ------------------------- */

static HbWindow *page_win(HbDesktopPage *p)
{
    if (p->win) return p->win;
    GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(p));
    return HB_IS_WINDOW(r) ? HB_WINDOW(r) : NULL;
}

static void tab_dot(HbDesktopPage *p, const char *d)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id)
        hb_window_set_tab_dot(w, p->tab_id, d);
}

static gboolean is_active(HbDesktopPage *p)
{
    HbWindow *w = page_win(p);
    const char *a;
    if (!w || !p->tab_id)
        return FALSE;
    a = hb_window_active_tab_id(w);
    return a && !strcmp(a, p->tab_id);
}

static gboolean loopback_addr(const char *a)
{
    return !a || !*a || !g_ascii_strcasecmp(a, "localhost") ||
           !strcmp(a, "127.0.0.1") || !strcmp(a, "::1");
}

static const char *quality_str(hb_rd_quality q)
{
    return q == HB_RD_Q_FAST ? "fast" :
           q == HB_RD_Q_HIGH ? "high" : "balanced";
}

static const char *quality_label(hb_rd_quality q)
{
    return q == HB_RD_Q_FAST ? "流畅" :
           q == HB_RD_Q_HIGH ? "高清" : "均衡";
}

static hb_rd_quality quality_of(const char *s)
{
    if (!g_strcmp0(s, "fast")) return HB_RD_Q_FAST;
    if (!g_strcmp0(s, "high")) return HB_RD_Q_HIGH;
    return HB_RD_Q_BALANCED;
}

/* ------------------------- 日志 ------------------------- */

static void dlog_start(HbDesktopPage *p)
{
    hb_log l = {0};
    const char *pr;
    if (p->log_id > 0)
        return;
    l.user = p->huser;
    l.host = p->hname;
    l.host_id = p->host_id;
    pr = !strcmp(p->proto, "rdp") ? "RDP" : "VNC";
    l.proto = p->use_tunnel ? (pr[0] == 'R' ? "RDP/Tunnel" : "VNC/Tunnel")
                            : pr;
    l.result = "connecting";
    l.duration_s = -1;
    l.saved = false;
    hb_vault_log_add(hb_store_vault(p->store), &l, &p->log_id);
    p->log_t0 = g_get_monotonic_time();
}

static void dlog_finish(HbDesktopPage *p, const char *result,
                        const char *reason)
{
    gint64 dur;
    if (p->log_id <= 0)
        return;
    dur = (g_get_monotonic_time() - p->log_t0) / G_USEC_PER_SEC;
    hb_vault_log_finish(hb_store_vault(p->store), p->log_id,
                        result, reason, dur);
    p->log_id = 0;
}

/* ------------------------- 关闭/清理 ------------------------- */

typedef struct {
    hb_rd      *rd;
    hb_session *sess;
    HbDesktopPage *p;
    guint       gen;
    gboolean    restart;
} KillSet;

static gboolean kill_done(gpointer ud);

static gpointer kill_thread(gpointer ud)
{
    KillSet *k = ud;
    if (k->rd) {
        hb_rd_close(k->rd);
        hb_rd_free(k->rd);
    }
    if (k->sess) {
        hb_session_close(k->sess);
        hb_session_free(k->sess);
    }
    /* 旧会话真正收完再回主线程：restart 之前的 detach 才算干净，
     * 也避免 idle 先于线程跑、在已销毁页面上读 gen。 */
    g_main_context_invoke(NULL, kill_done, k);
    return NULL;
}

static gboolean kill_done(gpointer ud)
{
    KillSet *k = ud;
    HbDesktopPage *p = k->p;
    gboolean restart = k->restart;
    guint gen = k->gen;
    g_free(k);
    /* p 由 KillSet 强引用保活到此处；页面若已销毁，disposed 先拦住。 */
    if (!p->disposed && p->gen == gen && restart)
        start_attempt(p, TRUE);
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

/* 摘走 rd/tun 的 UI 侧引用，worker 线程内做阻塞收尾。 */
static void kill_session_async(HbDesktopPage *p, hb_rd *rd,
                               hb_session *sess, gboolean restart)
{
    KillSet *k = g_new0(KillSet, 1);
    k->rd = rd;
    k->sess = sess;
    k->p = g_object_ref(p);    /* kill_done 可能晚于页面 dispose/finalize */
    k->gen = p->gen;
    k->restart = restart;
    if (rd && p->rd == rd) {
        if (p->rd_ev_src) {
            g_source_remove(p->rd_ev_src);
            p->rd_ev_src = 0;
        }
        p->rd = NULL;
        p->rd_live = FALSE;
    }
    if (rd && RD_CANVAS(p->canvas)->rd == rd)
        rd_canvas_attach(RD_CANVAS(p->canvas), NULL);
    g_thread_unref(g_thread_new("rd-kill", kill_thread, k));
}

/* 主线程：摘走当前 rd + tunnel session（含对话框/应答），转交线程。 */
static void detach_session(HbDesktopPage *p)
{
    hb_session *sess = NULL;
    hb_rd *rd = p->rd;
    if (p->rd_ev_src) {
        g_source_remove(p->rd_ev_src);
        p->rd_ev_src = 0;
    }
    p->rd = NULL;
    p->rd_live = FALSE;
    rd_canvas_attach(RD_CANVAS(p->canvas), NULL);
    if (p->tun_ev_src) {
        g_source_remove(p->tun_ev_src);
        p->tun_ev_src = 0;
    }
    if (p->ctl)
        sess = hb_conn_ctl_steal_session(p->ctl);
    p->tun_sess = NULL;
    if (rd || sess)
        kill_session_async(p, rd, sess, FALSE);
}

static void force_close_own_dialog(HbDesktopPage *p)
{
    if (p->open_dlg) {
        adw_dialog_force_close(p->open_dlg);
        p->open_dlg = NULL;
    }
}

/* ------------------------- 重连 ------------------------- */

static const int bo_tab[] = { 1, 2, 5, 10, 30 };

static void stop_backoff(HbDesktopPage *p)
{
    if (p->bo_src) {
        g_source_remove(p->bo_src);
        p->bo_src = 0;
    }
}

static void show_disc(HbDesktopPage *p, const char *text)
{
    gtk_label_set_label(GTK_LABEL(p->disc_lbl), text);
    gtk_widget_set_visible(p->disc_ov, TRUE);
    gtk_widget_set_visible(p->dim, TRUE);
}

static void hide_disc(HbDesktopPage *p)
{
    gtk_widget_set_visible(p->disc_ov, FALSE);
    gtk_widget_set_visible(p->dim, FALSE);
}

static void reconnect_now(HbDesktopPage *p);

static gboolean bo_tick(gpointer ud)
{
    HbDesktopPage *p = ud;
    if (--p->countdown <= 0) {
        p->bo_src = 0;
        reconnect_now(p);
        return G_SOURCE_REMOVE;
    }
    char *t = g_strdup_printf("连接中断 · %ds 后重连", p->countdown);
    show_disc(p, t);
    g_free(t);
    return G_SOURCE_CONTINUE;
}

static void start_backoff(HbDesktopPage *p)
{
    int idx = p->bo_k;
    int d = idx < (int)G_N_ELEMENTS(bo_tab) ? bo_tab[idx] : 30;
    char *t;
    p->bo_k++;
    p->countdown = d;
    t = g_strdup_printf("连接中断 · %ds 后重连", d);
    show_disc(p, t);
    g_free(t);
    stop_backoff(p);
    p->bo_src = g_timeout_add_seconds(1, bo_tick, p);
}

/* 断线/重连发起：摘走会话，重启一轮（ever_connected 时跳过连接视图，
 * 直接在旧帧上盖「正在重连…」）。 */
static void reconnect_now(HbDesktopPage *p)
{
    stop_backoff(p);
    p->gen++;
    force_close_own_dialog(p);
    p->connecting_now = TRUE;
    show_disc(p, "正在重连…");
    detach_session(p);
    start_attempt(p, TRUE);
}

static void on_reconn_btn(GtkButton *b, gpointer ud)
{
    (void)b;
    reconnect_now((HbDesktopPage *)ud);
}

static void on_disc_close_btn(GtkButton *b, gpointer ud)
{
    HbDesktopPage *p = ud;
    HbWindow *w = page_win(p);
    (void)b;
    if (w && p->tab_id)
        hb_window_close_tab(w, p->tab_id);
}

/* ------------------------- 错误卡 ------------------------- */

static void show_error(HbDesktopPage *p, const char *title,
                       const char *detail)
{
    stop_backoff(p);
    dlog_finish(p, "error", detail);
    hide_disc(p);
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");
    hb_connect_view_error(HB_CONNECT_VIEW(p->conn_view), title,
                          detail, 0);
    tab_dot(p, "err");
}

static void on_cv_close(HbConnectView *v, gpointer ud)
{ (void)v; on_disc_close_btn(NULL, ud); }
static void on_cv_retry(HbConnectView *v, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)v;
    start_attempt(p, FALSE);
}
static void on_cv_edit(HbConnectView *v, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)v;
    g_signal_emit(p, sigs[SIG_EDIT_HOST], 0, p->host_id);
}

/* ------------------------- 主机快照 ------------------------- */

/* 读主机字段到 p->h*；经由主机名/地址用于横幅与 dest 判定。 */
static gboolean load_host(HbDesktopPage *p, GError **err_out)
{
    hb_vault *v = hb_store_vault(p->store);
    hb_obj *h = NULL, *via = NULL;
    const char *s;
    (void)err_out;

    if (hb_vault_get(v, p->host_id, &h) != HB_OK || !h) {
        g_set_error(err_out, g_quark_from_static_string("rd"), 1,
                    "主机对象不存在或已删除");
        return FALSE;
    }
    g_free(p->hname);
    g_free(p->haddr);
    g_free(p->huser);
    g_free(p->hdomain);
    g_free(p->via_id);
    g_free(p->via_name);
    g_free(p->via_user);
    s = hb_obj_get_str(h, "name");
    p->hname = g_strdup(s && *s ? s : hb_obj_get_str(h, "address"));
    p->haddr = g_strdup(hb_obj_get_str(h, "address"));
    p->huser = g_strdup(hb_obj_get_str(h, "user"));
    p->hdomain = g_strdup(hb_obj_get_str(h, "domain"));
    p->hport = (int)hb_obj_get_int(h, "port",
                                 !strcmp(p->proto, "rdp") ? 3389 : 5900);
    p->quality = quality_of(hb_obj_get_str(h, "rd_quality"));

    s = hb_obj_get_str(h, "via");
    p->use_tunnel = s && *s &&
        !g_strcmp0(hb_obj_get_str(h, "tunnel"), "ssh");
    p->via_id = g_strdup(p->use_tunnel ? s : NULL);

    if (p->use_tunnel) {
        if (hb_vault_get(v, p->via_id, &via) != HB_OK || !via) {
            hb_obj_free(h);
            g_set_error(err_out, g_quark_from_static_string("rd"), 2,
                        "经由 SSH 主机不存在或已删除");
            return FALSE;
        }
        const char *vn = hb_obj_get_str(via, "name");
        const char *va = hb_obj_get_str(via, "address");
        p->via_name = g_strdup(vn && *vn ? vn : va);
        p->via_user = g_strdup(hb_obj_get_str(via, "user"));
        /* 目标 dest：经主机与桌面主机同址，或桌面地址为空/回环 →
         * 服务在 SSH 服务器本机，dest=127.0.0.1 */
        p->via_same = loopback_addr(p->haddr) ||
            (va && p->haddr && !g_ascii_strcasecmp(va, p->haddr));
        hb_obj_free(via);
    }
    hb_obj_free(h);
    return TRUE;
}

/* ------------------------- PROMPT / CERT 对话框 ------------------------- */

typedef struct {
    HbDesktopPage *p;
    guint   gen;
    AdwDialog *dlg;
    GtkWidget *user_e, *pw_e, *dom_e, *save_ck;
    gboolean done;
} RdPrompt;

static void rp_clear(RdPrompt *d)
{
    /* 用过的凭据立刻从 entry 抹掉 */
    if (d->pw_e)
        gtk_editable_set_text(GTK_EDITABLE(d->pw_e), "");
}

static void rp_deliver(RdPrompt *d, const char *user,
                       const char *pw, const char *dom)
{
    HbDesktopPage *p = d->p;
    gboolean save;
    if (d->done)
        return;
    d->done = TRUE;
    save = pw && d->save_ck &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(d->save_ck));
    rp_clear(d);
    if (p->gen == d->gen)
        p->open_dlg = NULL;
    if (p->gen != d->gen || !p->rd_live)
        return;
    hb_rd_prompt_reply(p->rd, user, pw, dom);
    if (pw && save) {
        /* 保存到 Vault：password secret + user/domain 字段 */
        hb_vault *v = hb_store_vault(p->store);
        hb_obj *h = NULL;
        hb_vault_set_secret(v, p->host_id, "password", pw);
        if (hb_vault_get(v, p->host_id, &h) == HB_OK && h) {
            if (user && *user)
                hb_obj_set_str(h, "user", user);
            if (dom)
                hb_obj_set_str(h, "domain",
                               *dom ? dom : NULL);
            hb_vault_put(v, h);
            hb_obj_free(h);
        }
    }
}

static void rp_ok(GtkButton *b, RdPrompt *d)
{
    const char *user = d->user_e
        ? gtk_editable_get_text(GTK_EDITABLE(d->user_e)) : NULL;
    const char *dom = d->dom_e
        ? gtk_editable_get_text(GTK_EDITABLE(d->dom_e)) : NULL;
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(d->pw_e));
    char *uc = g_strdup(user), *pc = g_strdup(pw), *dc = g_strdup(dom);
    (void)b;
    /* 先应答（标 done，wipe entry），再关 —— closed 里的 NULL 应答
     * 会被 done 拦住，不会误发 CANCEL */
    rp_deliver(d, uc, pc, dc);
    adw_dialog_close(d->dlg);
    g_free(uc);
    g_free(pc);
    g_free(dc);
}

static void rp_activate(GtkEditable *e, RdPrompt *d)
{ (void)e; rp_ok(NULL, d); }

static void rp_cancel(GtkButton *b, RdPrompt *d)
{
    (void)b;
    adw_dialog_close(d->dlg);   /* → closed → rp_deliver(NULL) */
}

static void rp_closed(AdwDialog *dlg, RdPrompt *d)
{
    (void)dlg;
    rp_deliver(d, NULL, NULL, NULL);
    g_free(d);
}

/* VNC code0：只问密码；RDP code1：user+password+domain。 */
static void show_prompt(HbDesktopPage *p, int code, const char *text)
{
    RdPrompt *d = g_new0(RdPrompt, 1);
    GtkWidget *body, *foot, *f, *ok, *cancel, *l;
    gboolean rdp = !strcmp(p->proto, "rdp");

    force_close_own_dialog(p);
    d->p = p;
    d->gen = p->gen;
    d->dlg = hb_modal_new(rdp ? "RDP 凭据" : "VNC 密码", &body, &foot);
    p->open_dlg = d->dlg;

    l = gtk_label_new(text && *text ? text :
                      (rdp ? "远程桌面需要登录凭据"
                           : "服务器要求 VNC 密码"));
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);

    if (code == 1) {
        gtk_box_append(GTK_BOX(body), gtk_label_new("用户名"));
        d->user_e = gtk_entry_new();
        if (p->huser)
            gtk_editable_set_text(GTK_EDITABLE(d->user_e), p->huser);
        gtk_box_append(GTK_BOX(body), d->user_e);
    }
    gtk_box_append(GTK_BOX(body), gtk_label_new("密码"));
    d->pw_e = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(
        GTK_PASSWORD_ENTRY(d->pw_e), TRUE);
    g_signal_connect(d->pw_e, "activate",
                     G_CALLBACK(rp_activate), d);
    gtk_box_append(GTK_BOX(body), d->pw_e);
    if (code == 1) {
        gtk_box_append(GTK_BOX(body), gtk_label_new("域（可空）"));
        d->dom_e = gtk_entry_new();
        if (p->hdomain)
            gtk_editable_set_text(GTK_EDITABLE(d->dom_e), p->hdomain);
        gtk_box_append(GTK_BOX(body), d->dom_e);
    }
    d->save_ck = gtk_check_button_new_with_label("保存到 Vault");
    gtk_widget_add_css_class(d->save_ck, "check-row");
    gtk_widget_set_halign(d->save_ck, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(body), d->save_ck);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(cancel, "hb-btn");
    g_signal_connect(cancel, "clicked", G_CALLBACK(rp_cancel), d);
    gtk_box_append(GTK_BOX(f), cancel);
    ok = gtk_button_new_with_label("连接");
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(rp_ok), d);
    gtk_box_append(GTK_BOX(f), ok);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(d->dlg, "closed", G_CALLBACK(rp_closed), d);
    adw_dialog_present(d->dlg, GTK_WIDGET(p));
    gtk_widget_grab_focus(d->pw_e);
}

typedef struct {
    HbDesktopPage *p;
    guint   gen;
    AdwDialog *dlg;
    gboolean accepted;
    gboolean done;
    char    *new_fp;
} RdCert;

static void rc_deliver(RdCert *d)
{
    HbDesktopPage *p = d->p;
    if (d->done)
        return;
    d->done = TRUE;
    if (p->gen == d->gen)
        p->open_dlg = NULL;
    if (p->gen != d->gen || !p->rd_live) {
        g_free(d->new_fp);
        g_free(d);
        return;
    }
    hb_rd_cert_reply(p->rd, d->accepted);
    if (d->accepted && d->new_fp && *d->new_fp) {
        /* 信任此证书 → 持久化指纹 */
        hb_obj *h = NULL;
        if (hb_vault_get(hb_store_vault(p->store), p->host_id,
                         &h) == HB_OK && h) {
            hb_obj_set_str(h, "rdp_cert_fp", d->new_fp);
            hb_vault_put(hb_store_vault(p->store), h);
            hb_obj_free(h);
        }
    }
    g_free(d->new_fp);
    g_free(d);
}

static void rc_cancel(GtkButton *b, RdCert *d)
{ (void)b; d->accepted = FALSE; adw_dialog_close(d->dlg); }
static void rc_ok(GtkButton *b, RdCert *d)
{ (void)b; d->accepted = TRUE;  adw_dialog_close(d->dlg); }
static void rc_closed(AdwDialog *dlg, RdCert *d)
{ (void)dlg; rc_deliver(d); }

static GtkWidget *code_lbl(const char *s)
{
    GtkWidget *l = gtk_label_new(s ? s : "");
    gtk_widget_add_css_class(l, "hb-code");
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

/* code 0 未知 / 1 已变更（危险框，默认焦点「断开」） */
static void show_cert(HbDesktopPage *p, int code, const char *fp,
                      const char *summary)
{
    RdCert *d = g_new0(RdCert, 1);
    GtkWidget *body, *foot, *f, *ok, *cancel, *l, *hb;
    gboolean changed = code == 1;

    force_close_own_dialog(p);
    d->p = p;
    d->gen = p->gen;
    d->new_fp = g_strdup(fp);
    d->dlg = hb_modal_new(changed ? "警告：远程桌面证书已变更"
                                  : "验证远程桌面证书",
                          &body, &foot);
    p->open_dlg = d->dlg;

    hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hb, "hintbox");
    gtk_widget_add_css_class(hb, changed ? "red" : "amber");
    gtk_box_append(GTK_BOX(hb), hb_icon_new("shield", 16));
    l = gtk_label_new(changed
        ? "远程主机的证书与本地记录不一致。这可能是重装或配置变更，"
          "也可能是中间人攻击——除非已带外核实，否则应断开。"
        : "该远程桌面证书未受信任。请通过带外渠道核对指纹后再信任。");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(hb), l);
    gtk_box_append(GTK_BOX(body), hb);

    if (changed) {
        hb_obj *h = NULL;
        const char *oldfp = NULL;
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
        if (hb_vault_get(hb_store_vault(p->store), p->host_id,
                         &h) == HB_OK && h)
            oldfp = hb_obj_get_str(h, "rdp_cert_fp");
        l = gtk_label_new("旧指纹");
        gtk_widget_add_css_class(l, "hb-dim");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), code_lbl(oldfp), 1, 0, 1, 1);
        l = gtk_label_new("新指纹");
        gtk_widget_add_css_class(l, "hb-dim");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(grid), l, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), code_lbl(fp), 1, 1, 1, 1);
        gtk_box_append(GTK_BOX(body), grid);
        if (h) hb_obj_free(h);
    } else {
        gtk_box_append(GTK_BOX(body), code_lbl(fp));
    }
    if (summary && *summary) {
        GtkWidget *s = gtk_label_new(summary);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_selectable(GTK_LABEL(s), TRUE);
        gtk_box_append(GTK_BOX(body), s);
    }

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label(changed ? "断开" : "取消");
    gtk_widget_add_css_class(cancel, "hb-btn");
    if (changed)
        gtk_widget_add_css_class(cancel, "primary");
    g_signal_connect(cancel, "clicked", G_CALLBACK(rc_cancel), d);
    gtk_box_append(GTK_BOX(f), cancel);
    ok = gtk_button_new_with_label(changed ? "我已核实，更新指纹并继续"
                                           : "信任此证书");
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, changed ? "danger" : "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(rc_ok), d);
    gtk_box_append(GTK_BOX(f), ok);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(d->dlg, "closed", G_CALLBACK(rc_closed), d);
    adw_dialog_present(d->dlg, GTK_WIDGET(p));
    /* 变更警告：焦点落在「断开」（同 hostkey_changed） */
    if (changed)
        gtk_widget_grab_focus(cancel);
}

/* ------------------------- rd 事件 ------------------------- */

static void rd_connected(HbDesktopPage *p, const hb_rd_event *ev)
{
    p->connected = TRUE;
    p->ever_connected = TRUE;
    p->connecting_now = FALSE;
    p->bo_k = 0;
    stop_backoff(p);
    hide_disc(p);
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "desktop");
    if (ev->text2)
        gtk_label_set_label(GTK_LABEL(p->sec_lbl), ev->text2);
    rd_canvas_attach(RD_CANVAS(p->canvas), p->rd);
    rd_canvas_set_size(RD_CANVAS(p->canvas), ev->w, ev->h);
    dlog_start(p);
    tab_dot(p, "ok");
    gtk_widget_grab_focus(p->canvas);
}

static void rd_disconnect(HbDesktopPage *p)
{
    p->connected = FALSE;
    dlog_finish(p, "error", "disconnected");
    p->log_id = 0;
    tab_dot(p, "err");
    if (p->ever_connected)
        start_backoff(p);
    else
        show_error(p, "连接中断", NULL);
}

static void rd_error(HbDesktopPage *p, const hb_rd_event *ev)
{
    p->connected = FALSE;
    tab_dot(p, "err");
    switch (ev->code) {
    case HB_RD_ERR_DISCONNECTED:
        rd_disconnect(p);
        break;
    case HB_RD_ERR_AUTH:
        show_error(p, "认证失败：用户名或密码错误", ev->text2);
        break;
    case HB_RD_ERR_CERT_REJECTED:
        show_error(p, "证书被拒绝", ev->text2);
        break;
    case HB_RD_ERR_CANCELLED:
        show_error(p, "已取消", ev->text2);
        break;
    default:
        /* REFUSED/TIMEOUT/TLS/PROTOCOL：错误卡（可手动重试） */
        show_error(p, ev->text ? ev->text : "连接失败", ev->text2);
        break;
    }
}

static void rd_ev_one(HbDesktopPage *p, hb_rd_event *ev)
{
    switch (ev->type) {
    case HB_RD_EV_STAGE:
        hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view),
                              4, ev->text, ' ');
        break;
    case HB_RD_EV_CONNECTED:
        rd_connected(p, ev);
        break;
    case HB_RD_EV_RESIZE:
        rd_canvas_set_size(RD_CANVAS(p->canvas), ev->w, ev->h);
        break;
    case HB_RD_EV_FRAME:
        rd_canvas_frame(RD_CANVAS(p->canvas));
        break;
    case HB_RD_EV_CURSOR:
        if (ev->px && ev->w > 0 && ev->h > 0) {
            GBytes *gb = g_bytes_new(ev->px,
                                     (gsize)ev->w * ev->h * 4);
            GdkTexture *t = gdk_memory_texture_new(
                ev->w, ev->h, GDK_MEMORY_B8G8R8A8, gb,
                                     (gsize)ev->w * 4);
            GdkCursor *cur = gdk_cursor_new_from_texture(
                t, ev->hx, ev->hy, NULL);
            gtk_widget_set_cursor(p->canvas, cur);
            g_object_unref(cur);
            g_object_unref(t);
            g_bytes_unref(gb);
        }
        break;
    case HB_RD_EV_CLIPBOARD:
        if (ev->text) {
            GdkDisplay *d = gtk_widget_get_display(GTK_WIDGET(p));
            GdkClipboard *cb = gdk_display_get_clipboard(d);
            g_free(p->last_remote_clip);
            p->last_remote_clip = g_strdup(ev->text);
            gdk_clipboard_set_text(cb, ev->text);
        }
        break;
    case HB_RD_EV_CERT:
        show_cert(p, ev->code, ev->text, ev->text2);
        break;
    case HB_RD_EV_PROMPT:
        show_prompt(p, ev->code, ev->text);
        break;
    case HB_RD_EV_ERROR:
        rd_error(p, ev);
        break;
    case HB_RD_EV_EXIT:
        /* worker 退出；断线场景 ERROR 已先行，这里仅兜底 */
        if (p->connected) {
            p->connected = FALSE;
            if (p->ever_connected)
                start_backoff(p);
        }
        break;
    }
}

static gboolean rd_ev_cb(int fd, GIOCondition cond, gpointer ud)
{
    HbDesktopPage *p = ud;
    hb_rd_event ev;
    (void)fd;
    if (!p->rd)
        return G_SOURCE_REMOVE;
    while (p->rd && hb_rd_next_event(p->rd, &ev)) {
        rd_ev_one(p, &ev);
        hb_rd_event_clear(&ev);
    }
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
        return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

/* ------------------------- rd 启动 ------------------------- */

static char *freerdp_cfg_dir(void)
{
    const char *xdg = g_get_user_data_dir();
    char *d = g_build_filename(xdg, "harbor", "freerdp", NULL);
    g_mkdir_with_parents(d, 0700);
    chmod(d, 0700);
    return d;
}

static void start_rd(HbDesktopPage *p, const char *host, int port)
{
    hb_rd_opts o = {0};
    hb_obj *h = NULL;
    char *cfg = NULL;
    char *sec = NULL;

    o.proto = !strcmp(p->proto, "rdp") ? HB_RD_RDP : HB_RD_VNC;
    o.host = host;
    o.port = port;
    o.user = p->huser;
    o.domain = p->hdomain;
    o.quality = p->quality;
    o.connect_timeout_s = 15;
    sec = hb_vault_get_secret(hb_store_vault(p->store),
                              p->host_id, "password");
    if (sec && *sec)
        o.password = sec;
    if (o.proto == HB_RD_RDP) {
        o.tls_hostname = p->haddr;
        cfg = freerdp_cfg_dir();
        o.freerdp_config_dir = cfg;
        if (p->alloc_w > 0 && p->alloc_h > 0) {
            o.width = p->alloc_w & ~1;
            o.height = p->alloc_h & ~1;
        }
        if (hb_vault_get(hb_store_vault(p->store), p->host_id,
                         &h) == HB_OK && h) {
            const char *fp = hb_obj_get_str(h, "rdp_cert_fp");
            if (fp && *fp)
                o.cert_fp = fp;
        }
    }

    p->rd = hb_rd_new(&o);          /* 深拷贝全部字段 */
    if (h) hb_obj_free(h);
    if (sec) hb_secret_free(sec);
    g_free(cfg);
    if (!p->rd) {
        show_error(p, "无法创建远程桌面会话", NULL);
        return;
    }
    p->rd_live = TRUE;
    p->rd_ev_src = g_unix_fd_add(hb_rd_event_fd(p->rd),
                                 G_IO_IN | G_IO_ERR | G_IO_HUP,
                                 rd_ev_cb, p);
    rd_canvas_attach(RD_CANVAS(p->canvas), p->rd);
}

/* ------------------------- 隧道（HbConnCtl FORWARD） ------------------------- */

static gboolean tun_ev_cb(int fd, GIOCondition cond, gpointer ud)
{
    HbDesktopPage *p = ud;
    hb_sess_event ev;
    (void)fd;
    if (!p->tun_sess)
        return G_SOURCE_REMOVE;
    while (p->tun_sess &&
           hb_session_next_event(p->tun_sess, &ev)) {
        if (hb_conn_ctl_handle_event(p->ctl, &ev))
            ;
        else if (ev.type == HB_SESS_EV_FORWARD && ev.pf) {
            if (ev.pf->kind == HB_PF_EV_LISTENING) {
                char *line, *dest = p->via_same ? "127.0.0.1"
                                                : p->haddr;
                p->bound_port = ev.pf->bound_port;
                line = g_strdup_printf(
                    "开通 Local 转发 127.0.0.1:%d → %s:%d",
                    p->bound_port, dest, p->hport);
                hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view),
                                      3, line, ' ');
                g_free(line);
                banner_refresh(p);
                line = g_strdup_printf("%s 握手中…",
                    !strcmp(p->proto, "rdp") ? "RDP" : "VNC");
                hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view),
                                      4, line, ' ');
                g_free(line);
                start_rd(p, "127.0.0.1", p->bound_port);
            } else if (ev.pf->kind == HB_PF_EV_WARN) {
                hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view),
                                      -1, ev.pf->message, 'e');
            }
        }
        hb_sess_event_clear(&ev);
    }
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
        return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

static void on_tun_session(HbConnCtl *c, gpointer sp, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)c; (void)sp;
    p->tun_sess = hb_conn_ctl_session(p->ctl);
    if (!p->tun_sess)
        return;
    if (p->tun_ev_src)
        g_source_remove(p->tun_ev_src);
    p->tun_ev_src = g_unix_fd_add(
        hb_session_event_fd(p->tun_sess),
        G_IO_IN | G_IO_ERR | G_IO_HUP, tun_ev_cb, p);
}

static void on_tun_stage(HbConnCtl *c, int idx, const char *text,
                         gchar mark, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)c;
    hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view),
                          idx, text, mark);
}

static void on_tun_failed(HbConnCtl *c, int code, const char *title,
                          const char *details, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)c; (void)code;
    /* 隧道侧失败一律不重试：错误卡 + 重试/编辑主机 */
    show_error(p, title,
               details && *details ? details : "连接失败");
}

static void on_tun_disconn(HbConnCtl *c, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)c;
    if (p->ever_connected)
        start_backoff(p);
    else
        show_error(p, "SSH 隧道断开", NULL);
}

static void on_tun_connected(HbConnCtl *c, gpointer ud)
{
    (void)c; (void)ud;
}

static void ctl_connect_signals(HbDesktopPage *p, HbConnCtl *ctl)
{
    g_signal_connect(ctl, "session", G_CALLBACK(on_tun_session), p);
    g_signal_connect(ctl, "stage", G_CALLBACK(on_tun_stage), p);
    g_signal_connect(ctl, "failed", G_CALLBACK(on_tun_failed), p);
    g_signal_connect(ctl, "disconnected",
                     G_CALLBACK(on_tun_disconn), p);
    g_signal_connect(ctl, "connected",
                     G_CALLBACK(on_tun_connected), p);
}

/* ------------------------- 一轮连接 ------------------------- */

static void start_attempt(HbDesktopPage *p, gboolean is_reconnect)
{
    GError *err = NULL;
    hb_pf_rule rule;

    p->gen++;
    force_close_own_dialog(p);
    if (!is_reconnect)
        hb_connect_view_reset(HB_CONNECT_VIEW(p->conn_view));
    hb_connect_view_clear_error(HB_CONNECT_VIEW(p->conn_view));
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack),
        p->ever_connected && is_reconnect ? "desktop" : "connect");
    tab_dot(p, NULL);

    if (!load_host(p, &err)) {
        show_error(p, err ? err->message : "主机配置错误", NULL);
        g_clear_error(&err);
        return;
    }
    hb_connect_view_set_head(HB_CONNECT_VIEW(p->conn_view), "monitor",
        p->hname,
        p->use_tunnel ? (strcmp(p->proto, "rdp")
            ? "VNC · 经由 SSH 隧道" : "RDP · 经由 SSH 隧道")
                      : (strcmp(p->proto, "rdp") ? "VNC" : "RDP"));

    if (!p->use_tunnel) {
        char *line = g_strdup_printf("正在连接 %s:%d…",
                                     p->haddr, p->hport);
        hb_connect_view_stage(HB_CONNECT_VIEW(p->conn_view),
                              0, line, ' ');
        g_free(line);
        start_rd(p, p->haddr, p->hport);
        return;
    }

    /* 隧道：tab 私有 local 转发 127.0.0.1:0 → dest:port */
    if (!p->ctl) {
        p->ctl = hb_conn_ctl_new(p->store, GTK_WIDGET(p),
                                 HB_CONN_FORWARD);
        ctl_connect_signals(p, p->ctl);
    }
    memset(&rule, 0, sizeof rule);
    rule.kind = HB_PF_LOCAL;
    rule.bind_addr = "127.0.0.1";
    rule.bind_port = 0;
    rule.dest_addr = p->via_same ? "127.0.0.1" : p->haddr;
    rule.dest_port = p->hport;
    hb_conn_ctl_set_forward(p->ctl, &rule);
    hb_conn_ctl_start(p->ctl, p->via_id, NULL, 0, 0);
}

/* ------------------------- 画质/菜单动作 ------------------------- */

static void save_quality(HbDesktopPage *p)
{
    hb_obj *h = NULL;
    if (hb_vault_get(hb_store_vault(p->store), p->host_id,
                     &h) == HB_OK && h) {
        hb_obj_set_str(h, "rd_quality", quality_str(p->quality));
        hb_vault_put(hb_store_vault(p->store), h);
        hb_obj_free(h);
    }
}

static void rd_reconnect_toast_cb(gpointer ud)
{
    HbDesktopPage *p = ud;
    if (!p->disposed)
        reconnect_now(p);
}

/* 画质单选：MenuButton + 单选钮组（标签随当前档更新），
 * 比 DropDown 少一层 model/selection 包装且更易读。 */
static void on_quality_check(GtkCheckButton *b, gpointer ud)
{
    HbDesktopPage *p = ud;
    hb_rd_quality q;
    HbWindow *w;
    if (!gtk_check_button_get_active(b))
        return;
    q = (hb_rd_quality)GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(b), "q"));
    if (q == p->quality)
        return;
    p->quality = q;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(p->q_mb),
                              quality_label(q));
    save_quality(p);
    if (p->rd_live) {
        hb_rd_set_quality(p->rd, q);
        if (!strcmp(p->proto, "rdp")) {
            w = page_win(p);
            if (w)
                hb_window_toast_action2(
                    w, "RDP 画质将在重连后生效",
                    "立即重连", 9,
                    (GCallback)rd_reconnect_toast_cb,
                    (GDestroyNotify)g_object_unref,
                    g_object_ref(p));   /* toast 可能比页面活得久 */
        }
    }
}

static void send_cad(HbDesktopPage *p)
{
    if (!p->rd_live)
        return;
    /* Ctrl+Alt+Del：keycode 37/64/119（X11），keysym 仅 VNC 用 */
    hb_rd_key(p->rd, 0xFFE3, 37, TRUE);   /* Control_L */
    hb_rd_key(p->rd, 0xFFE9, 64, TRUE);   /* Alt_L */
    hb_rd_key(p->rd, 0xFFFF, 119, TRUE);  /* Delete */
    hb_rd_key(p->rd, 0xFFFF, 119, FALSE);
    hb_rd_key(p->rd, 0xFFE9, 64, FALSE);
    hb_rd_key(p->rd, 0xFFE3, 37, FALSE);
}

static void send_win(HbDesktopPage *p)
{
    if (!p->rd_live)
        return;
    hb_rd_key(p->rd, 0xFFEB, 133, TRUE);  /* Super_L */
    hb_rd_key(p->rd, 0xFFEB, 133, FALSE);
}

/* ------------------------- 输入 ------------------------- */

typedef struct {
    unsigned mask;         /* 当前按下的按钮位 */
    double sx, sy;         /* 平滑滚动累积 */
    double px, py;         /* 最近一次 motion 的控件坐标（scroll 无 x/y） */
    gboolean have_pos;
    GHashTable *keys;      /* 按下中的 hardware keycode → keysym */
} InState;

static void in_state_free(gpointer ud)
{
    InState *st = ud;
    g_hash_table_destroy(st->keys);
    g_free(st);
}

static InState *in_of(HbDesktopPage *p)
{
    InState *st = g_object_get_data(G_OBJECT(p->canvas), "hb-in");
    if (!st) {
        st = g_new0(InState, 1);
        st->keys = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_object_set_data_full(G_OBJECT(p->canvas), "hb-in", st,
                               in_state_free);
    }
    return st;
}

static void in_motion(GtkEventControllerMotion *m, double x, double y,
                      gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    double fx, fy;
    (void)m;
    st->px = x;
    st->py = y;
    st->have_pos = TRUE;
    if (!p->rd_live || p->view_only)
        return;
    if (rd_canvas_map(RD_CANVAS(p->canvas), x, y, &fx, &fy))
        hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask);
}

static unsigned btn_bit(guint b)
{
    return b == GDK_BUTTON_PRIMARY   ? 1u :
           b == GDK_BUTTON_MIDDLE    ? 2u :
           b == GDK_BUTTON_SECONDARY ? 4u : 0u;
}

static void in_press(GtkGestureClick *g, int n, double x, double y,
                     gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    double fx, fy;
    unsigned b = btn_bit(gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(g)));
    (void)n;
    gtk_widget_grab_focus(p->canvas);
    if (!p->rd_live || p->view_only || !b)
        return;
    if (!rd_canvas_map(RD_CANVAS(p->canvas), x, y, &fx, &fy))
        return;
    st->mask |= b;
    hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask);
}

static void in_release(GtkGestureClick *g, int n, double x, double y,
                       gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    double fx, fy;
    unsigned b = btn_bit(gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(g)));
    (void)n;
    if (!b)
        return;
    st->mask &= ~b;
    if (!p->rd_live || p->view_only)
        return;
    if (rd_canvas_map(RD_CANVAS(p->canvas), x, y, &fx, &fy))
        hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask);
}

static gboolean in_scroll(GtkEventControllerScroll *s, double dx,
                          double dy, gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    double fx, fy;
    int sy, sx, i, n;
    unsigned m;
    (void)s;
    if (!p->rd_live || p->view_only)
        return TRUE;
    if (!st->have_pos)
        return TRUE;
    if (!rd_canvas_map(RD_CANVAS(p->canvas), st->px, st->py,
                       &fx, &fy))
        return TRUE;
    /* 平滑增量累积成整格滚轮（bit3/4 上下，bit5/6 左右；
     * 每格发送 press + release —— RFB 轮键是瞬时语义） */
    st->sy += dy;
    sy = (int)st->sy;
    if (sy) {
        st->sy -= sy;
        m = sy < 0 ? 1u << 3 : 1u << 4;
        n = sy < 0 ? -sy : sy;
        for (i = 0; i < n; i++) {
            hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask | m);
            hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask);
        }
    }
    st->sx += dx;
    sx = (int)st->sx;
    if (sx) {
        st->sx -= sx;
        m = sx < 0 ? 1u << 5 : 1u << 6;
        n = sx < 0 ? -sx : sx;
        for (i = 0; i < n; i++) {
            hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask | m);
            hb_rd_pointer(p->rd, (int)fx, (int)fy, st->mask);
        }
    }
    return TRUE;
}

static void send_key(HbDesktopPage *p, guint keyval, guint code,
                     gboolean down)
{
    if (!p->rd_live || p->view_only)
        return;
    hb_rd_key(p->rd, keyval, code, down);
}

static gboolean in_key_press(GtkEventControllerKey *kc, guint keyval,
                             guint code, GdkModifierType state,
                             gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    (void)kc; (void)state;
    g_hash_table_insert(st->keys, GUINT_TO_POINTER(code),
                        GUINT_TO_POINTER(keyval));
    send_key(p, keyval, code, TRUE);
    return TRUE;
}

static gboolean in_key_release(GtkEventControllerKey *kc, guint keyval,
                               guint code, GdkModifierType state,
                               gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    (void)kc; (void)state;
    g_hash_table_remove(st->keys, GUINT_TO_POINTER(code));
    send_key(p, keyval, code, FALSE);
    return TRUE;
}

static void in_release_all(HbDesktopPage *p, InState *st)
{
    GHashTableIter it;
    gpointer kc, kv;
    g_hash_table_iter_init(&it, st->keys);
    while (g_hash_table_iter_next(&it, &kc, &kv))
        send_key(p, GPOINTER_TO_UINT(kv), GPOINTER_TO_UINT(kc), FALSE);
    g_hash_table_remove_all(st->keys);
    if (st->mask && p->rd_live && !p->view_only && st->have_pos) {
        /* 释放全部按钮（在上次位置）——按掩码发一次 0 */
        double fx, fy;
        if (rd_canvas_map(RD_CANVAS(p->canvas), st->px, st->py,
                          &fx, &fy))
            hb_rd_pointer(p->rd, (int)fx, (int)fy, 0);
    }
    st->mask = 0;
}

static void in_focus_out(GtkEventControllerFocus *f, gpointer ud)
{
    HbDesktopPage *p = ud;
    InState *st = in_of(p);
    (void)f;
    in_release_all(p, st);
}

/* ------------------------- 剪贴板 ------------------------- */

static void clip_read_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbDesktopPage *p = ud;
    g_autoptr(GError) err = NULL;
    char *txt = gdk_clipboard_read_text_finish(
        GDK_CLIPBOARD(src), res, &err);
    /* p 持有调用期间强引用， disposed 与否内存都有效；
     * dispose 已取消/拆会话时仅清标记不再下发。 */
    p->clip_busy = FALSE;
    if (!txt || err || p->disposed || !p->rd_live) {
        g_free(txt);
        g_object_unref(p);
        return;
    }
    if (g_strcmp0(txt, p->last_remote_clip)) {
        g_free(p->last_remote_clip);
        p->last_remote_clip = g_strdup(txt);
        hb_rd_clipboard(p->rd, txt);
    }
    g_free(txt);
    g_object_unref(p);
}

static void maybe_send_clipboard(HbDesktopPage *p)
{
    GdkDisplay *d;
    GdkClipboard *cb;
    if (!p->rd_live || !p->connected || p->clip_busy || !is_active(p))
        return;
    d = gtk_widget_get_display(GTK_WIDGET(p));
    cb = gdk_display_get_clipboard(d);
    p->clip_busy = TRUE;
    if (!p->clip_cancel)
        p->clip_cancel = g_cancellable_new();
    /* 持页强引用直到回调：取消竞态（读已完成但回调未派发）下也安全。 */
    gdk_clipboard_read_text_async(cb, p->clip_cancel,
                                  clip_read_done, g_object_ref(p));
}

static void on_clip_changed(GdkClipboard *cb, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)cb;
    maybe_send_clipboard(p);
}

/* tab 激活/画布聚焦时补一次（离开期间剪贴板可能变过） */
static void on_canvas_enter(GtkEventControllerMotion *m, double x,
                            double y, gpointer ud)
{
    HbDesktopPage *p = ud;
    (void)m; (void)x; (void)y;
    maybe_send_clipboard(p);
    in_motion(m, x, y, ud);
}

/* ------------------------- RDP 动态分辨率 ------------------------- */

static gboolean rsz_send(gpointer ud)
{
    HbDesktopPage *p = ud;
    p->rsz_src = 0;
    if (p->rd_live && !strcmp(p->proto, "rdp"))
        hb_rd_request_size(p->rd, p->alloc_w & ~1, p->alloc_h & ~1);
    return G_SOURCE_REMOVE;
}

static void on_canvas_resize(RdCanvas *c, GParamSpec *ps, gpointer ud)
{
    HbDesktopPage *p = ud;
    int aw = gtk_widget_get_width(GTK_WIDGET(c));
    int ah = gtk_widget_get_height(GTK_WIDGET(c));
    (void)ps;
    if (aw <= 0 || ah <= 0)
        return;
    if (aw == p->alloc_w && ah == p->alloc_h)
        return;
    p->alloc_w = aw;
    p->alloc_h = ah;
    if (!p->connected || strcmp(p->proto, "rdp"))
        return;
    if (p->rsz_src)
        g_source_remove(p->rsz_src);
    p->rsz_src = g_timeout_add(500, rsz_send, p);
}

/* ------------------------- 菜单 ------------------------- */

static void on_menu_cad(GtkButton *b, gpointer ud)
{ (void)b; send_cad((HbDesktopPage *)ud); }
static void on_menu_win(GtkButton *b, gpointer ud)
{ (void)b; send_win((HbDesktopPage *)ud); }

static void on_ro_toggle(GtkCheckButton *ck, gpointer ud)
{
    HbDesktopPage *p = ud;
    p->view_only = gtk_check_button_get_active(ck);
    gtk_widget_set_visible(p->ro_pill, p->view_only);
}

static void on_fit_toggle(GtkCheckButton *ck, gpointer ud)
{
    HbDesktopPage *p = ud;
    RdCanvas *c = RD_CANVAS(p->canvas);
    p->orig_size = !gtk_check_button_get_active(ck);
    c->orig = p->orig_size;
    if (p->orig_size)
        gtk_widget_set_size_request(p->canvas,
                                    c->fb_w > 0 ? c->fb_w : -1,
                                    c->fb_h > 0 ? c->fb_h : -1);
    else
        gtk_widget_set_size_request(p->canvas, -1, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->scrolled),
        GTK_POLICY_AUTOMATIC,
        p->orig_size ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER);
    gtk_widget_queue_draw(p->canvas);
}

static void on_pass_toggle(GtkCheckButton *ck, gpointer ud)
{
    HbDesktopPage *p = ud;
    p->passthrough = gtk_check_button_get_active(ck);
    g_object_set_data(G_OBJECT(p->canvas), "hb-rd-passthrough",
                      GINT_TO_POINTER(p->passthrough ? 1 : 0));
    gtk_widget_set_visible(p->pass_hint, p->passthrough);
}

static void on_disconnect(GtkButton *b, gpointer ud)
{ on_disc_close_btn(b, ud); }

static GtkWidget *build_menu(HbDesktopPage *p)
{
    GtkWidget *pop, *vb, *b;

    pop = gtk_popover_new();
    vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(vb, 8);
    gtk_widget_set_margin_end(vb, 8);
    gtk_widget_set_margin_top(vb, 6);
    gtk_widget_set_margin_bottom(vb, 6);

    b = gtk_button_new_with_label("发送 Ctrl+Alt+Del");
    gtk_widget_add_css_class(b, "hb-pop-btn");
    g_signal_connect(b, "clicked", G_CALLBACK(on_menu_cad), p);
    gtk_box_append(GTK_BOX(vb), b);

    b = gtk_button_new_with_label("发送 Win");
    gtk_widget_add_css_class(b, "hb-pop-btn");
    g_signal_connect(b, "clicked", G_CALLBACK(on_menu_win), p);
    gtk_box_append(GTK_BOX(vb), b);

    b = gtk_check_button_new_with_label("只读观看");
    gtk_widget_add_css_class(b, "check-row");
    g_signal_connect(b, "toggled", G_CALLBACK(on_ro_toggle), p);
    gtk_box_append(GTK_BOX(vb), b);

    b = gtk_check_button_new_with_label("适应窗口（取消=原始大小）");
    gtk_widget_add_css_class(b, "check-row");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(b), TRUE);
    g_signal_connect(b, "toggled", G_CALLBACK(on_fit_toggle), p);
    gtk_box_append(GTK_BOX(vb), b);

    b = gtk_check_button_new_with_label("快捷键透传");
    gtk_widget_add_css_class(b, "check-row");
    g_signal_connect(b, "toggled", G_CALLBACK(on_pass_toggle), p);
    gtk_box_append(GTK_BOX(vb), b);

    gtk_popover_set_child(GTK_POPOVER(pop), vb);
    return pop;
}

/* ------------------------- 视图构建 ------------------------- */

static GtkWidget *build_banner(HbDesktopPage *p)
{
    GtkWidget *bar, *b;

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "hb-rd-banner");

    gtk_box_append(GTK_BOX(bar), hb_icon_new("lock", 13));
    p->banner_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->banner_lbl), 0);
    gtk_widget_set_hexpand(p->banner_lbl, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(p->banner_lbl),
                          PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(bar), p->banner_lbl);

    p->pass_hint = gtk_label_new("快捷键透传中");
    gtk_widget_add_css_class(p->pass_hint, "hb-rd-hint");
    gtk_widget_set_visible(p->pass_hint, FALSE);
    gtk_box_append(GTK_BOX(bar), p->pass_hint);

    p->ro_pill = gtk_label_new("只读");
    gtk_widget_add_css_class(p->ro_pill, "hb-tag");
    gtk_widget_set_visible(p->ro_pill, FALSE);
    gtk_box_append(GTK_BOX(bar), p->ro_pill);

    p->sec_lbl = gtk_label_new("");
    gtk_widget_add_css_class(p->sec_lbl, "hb-dim");
    gtk_box_append(GTK_BOX(bar), p->sec_lbl);

    /* 画质（MenuButton + 单选钮组；GtkDropDown 见 on_quality_check 注释） */
    {
        GtkWidget *pop, *vb;
        hb_rd_quality qs[3] = { HB_RD_Q_FAST, HB_RD_Q_BALANCED,
                                HB_RD_Q_HIGH };
        GtkWidget *first = NULL;
        int i;
        p->q_mb = gtk_menu_button_new();
        gtk_widget_add_css_class(p->q_mb, "tbtn");
        gtk_menu_button_set_label(GTK_MENU_BUTTON(p->q_mb),
                                  quality_label(p->quality));
        gtk_widget_set_tooltip_text(p->q_mb, "画质档位");
        pop = gtk_popover_new();
        vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(vb, 8);
        gtk_widget_set_margin_end(vb, 8);
        gtk_widget_set_margin_top(vb, 6);
        gtk_widget_set_margin_bottom(vb, 6);
        for (i = 0; i < 3; i++) {
            GtkWidget *c = gtk_check_button_new_with_label(
                quality_label(qs[i]));
            gtk_widget_add_css_class(c, "check-row");
            g_object_set_data(G_OBJECT(c), "q", GUINT_TO_POINTER(qs[i]));
            if (first)
                gtk_check_button_set_group(GTK_CHECK_BUTTON(c),
                                           GTK_CHECK_BUTTON(first));
            else
                first = c;
            gtk_check_button_set_active(GTK_CHECK_BUTTON(c),
                                        qs[i] == p->quality);
            g_signal_connect(c, "toggled",
                             G_CALLBACK(on_quality_check), p);
            gtk_box_append(GTK_BOX(vb), c);
        }
        gtk_popover_set_child(GTK_POPOVER(pop), vb);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(p->q_mb), pop);
        gtk_box_append(GTK_BOX(bar), p->q_mb);
    }

    /* ⋯ 菜单 */
    {
        GtkWidget *mb = gtk_menu_button_new();
        gtk_widget_add_css_class(mb, "tbtn");
        gtk_menu_button_set_child(GTK_MENU_BUTTON(mb),
                                  hb_icon_new("more", 13));
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), build_menu(p));
        gtk_box_append(GTK_BOX(bar), mb);
    }

    b = gtk_button_new_with_label("断开");
    gtk_widget_add_css_class(b, "hb-btn");
    gtk_widget_add_css_class(b, "sm");
    g_signal_connect(b, "clicked", G_CALLBACK(on_disconnect), p);
    gtk_box_append(GTK_BOX(bar), b);
    return bar;
}

static void banner_refresh(HbDesktopPage *p)
{
    char *t;
    if (p->use_tunnel) {
        const char *dest = p->via_same ? "127.0.0.1" : p->haddr;
        t = g_strdup_printf(
            "经由 %s@%s SSH 隧道 · 127.0.0.1:%d → 目标 %s:%d · "
            "目标服务未暴露公网",
            p->via_user && *p->via_user ? p->via_user : "?",
            p->via_name ? p->via_name : "?",
            p->bound_port, dest, p->hport);
    } else {
        t = g_strdup_printf("直连 %s:%d", p->haddr, p->hport);
    }
    gtk_label_set_label(GTK_LABEL(p->banner_lbl), t);
    g_free(t);
}

static void build_desktop_view(HbDesktopPage *p, GtkWidget *box)
{
    GtkWidget *ov, *cvb;

    gtk_box_append(GTK_BOX(box), build_banner(p));

    ov = gtk_overlay_new();
    gtk_widget_set_vexpand(ov, TRUE);
    gtk_widget_add_css_class(ov, "hb-rd-wrap");

    /* 画布：滚动窗口内（原始模式滚出） */
    p->scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(p->scrolled, TRUE);
    gtk_widget_set_hexpand(p->scrolled, TRUE);
    p->canvas = rd_canvas_new();
    gtk_widget_set_can_target(p->canvas, TRUE);
    {
        /* fit 模式用 GtkCenterBox 居中（letterbox） */
        cvb = gtk_center_box_new();
        gtk_widget_set_hexpand(cvb, TRUE);
        gtk_widget_set_vexpand(cvb, TRUE);
        gtk_center_box_set_center_widget(GTK_CENTER_BOX(cvb),
                                         p->canvas);
        gtk_scrolled_window_set_child(
            GTK_SCROLLED_WINDOW(p->scrolled), cvb);
    }
    gtk_overlay_set_child(GTK_OVERLAY(ov), p->scrolled);

    /* 断线变暗层 */
    p->dim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(p->dim, "hb-rd-dim");
    gtk_widget_set_visible(p->dim, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), p->dim);

    /* 断线覆盖层：文字 + 立即重连/关闭 */
    {
        GtkWidget *db = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        GtkWidget *br = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *bn;
        p->disc_lbl = gtk_label_new("连接中断");
        gtk_widget_add_css_class(p->disc_lbl, "hb-rd-disc");
        gtk_box_append(GTK_BOX(db), p->disc_lbl);
        bn = gtk_button_new_with_label("立即重连");
        gtk_widget_add_css_class(bn, "hb-btn");
        gtk_widget_add_css_class(bn, "primary");
        g_signal_connect(bn, "clicked",
                         G_CALLBACK(on_reconn_btn), p);
        gtk_box_append(GTK_BOX(br), bn);
        bn = gtk_button_new_with_label("关闭");
        gtk_widget_add_css_class(bn, "hb-btn");
        g_signal_connect(bn, "clicked",
                         G_CALLBACK(on_disc_close_btn), p);
        gtk_box_append(GTK_BOX(br), bn);
        gtk_widget_set_halign(br, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(db), br);
        gtk_widget_set_halign(db, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(db, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(db, "hb-rd-discbox");
        gtk_widget_set_visible(db, FALSE);
        p->disc_ov = db;
    }
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), p->disc_ov);

    gtk_box_append(GTK_BOX(box), ov);

    /* 输入控制器（装画布上） */
    {
        GtkEventController *m, *sc, *kc, *fc;
        GtkGesture *cl;
        m = gtk_event_controller_motion_new();
        g_signal_connect(m, "motion", G_CALLBACK(on_canvas_enter), p);
        g_signal_connect(m, "enter", G_CALLBACK(on_canvas_enter), p);
        gtk_widget_add_controller(p->canvas, m);

        cl = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cl), 0);
        g_signal_connect(cl, "pressed", G_CALLBACK(in_press), p);
        g_signal_connect(cl, "released", G_CALLBACK(in_release), p);
        gtk_widget_add_controller(p->canvas,
                                  GTK_EVENT_CONTROLLER(cl));

        sc = gtk_event_controller_scroll_new(
            GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
        g_signal_connect(sc, "scroll", G_CALLBACK(in_scroll), p);
        gtk_widget_add_controller(p->canvas, sc);

        kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed",
                         G_CALLBACK(in_key_press), p);
        g_signal_connect(kc, "key-released",
                         G_CALLBACK(in_key_release), p);
        gtk_widget_add_controller(p->canvas, kc);

        fc = gtk_event_controller_focus_new();
        g_signal_connect(fc, "leave", G_CALLBACK(in_focus_out), p);
        gtk_widget_add_controller(p->canvas, fc);
    }
    g_signal_connect(p->canvas, "notify::width",
                     G_CALLBACK(on_canvas_resize), p);
    g_signal_connect(p->canvas, "notify::height",
                     G_CALLBACK(on_canvas_resize), p);
}

/* ------------------------- 生命周期 ------------------------- */

static void hb_desktop_page_dispose(GObject *o)
{
    HbDesktopPage *p = HB_DESKTOP_PAGE(o);
    GdkClipboard *cb;
    GdkDisplay *d;

    /* GTK 可多次跑 dispose：第二遍子控件（canvas 等）已销毁，
     * detach/句柄摘除只做一次，余下交给父类链。 */
    if (p->disposed) {
        G_OBJECT_CLASS(hb_desktop_page_parent_class)->dispose(o);
        return;
    }
    p->disposed = TRUE;
    p->gen = 0;                /* 所有 gen 比较的挂起回调失效 */
    stop_backoff(p);
    if (p->rsz_src) {
        g_source_remove(p->rsz_src);
        p->rsz_src = 0;
    }
    if (p->clip_cancel)
        g_cancellable_cancel(p->clip_cancel);
    d = gtk_widget_get_display(GTK_WIDGET(p));
    cb = gdk_display_get_clipboard(d);
    if (p->clip_handler) {
        g_signal_handler_disconnect(cb, p->clip_handler);
        p->clip_handler = 0;
    }
    force_close_own_dialog(p);
    /* 挂起应答全部回绝，保证 worker 不被 reply 卡住 */
    if (p->rd) {
        hb_rd_prompt_reply(p->rd, NULL, NULL, NULL);
        hb_rd_cert_reply(p->rd, FALSE);
    }
    detach_session(p);          /* rd/session 转工作线程 close+free */
    if (p->ctl) {
        g_signal_handlers_disconnect_by_data(p->ctl, p);
        g_object_unref(p->ctl);
        p->ctl = NULL;
    }
    dlog_finish(p, p->ever_connected ? "ok" : "cancelled", NULL);
    G_OBJECT_CLASS(hb_desktop_page_parent_class)->dispose(o);
}

static void hb_desktop_page_finalize(GObject *o)
{
    HbDesktopPage *p = HB_DESKTOP_PAGE(o);
    g_free(p->host_id);
    g_free(p->hname);
    g_free(p->haddr);
    g_free(p->huser);
    g_free(p->hdomain);
    g_free(p->via_id);
    g_free(p->via_name);
    g_free(p->via_user);
    g_free(p->tab_id);
    g_free(p->last_remote_clip);
    g_clear_object(&p->clip_cancel);
    G_OBJECT_CLASS(hb_desktop_page_parent_class)->finalize(o);
}

static void hb_desktop_page_class_init(HbDesktopPageClass *cl)
{
    G_OBJECT_CLASS(cl)->dispose = hb_desktop_page_dispose;
    G_OBJECT_CLASS(cl)->finalize = hb_desktop_page_finalize;
    sigs[SIG_EDIT_HOST] = g_signal_new("edit-host",
        G_TYPE_FROM_CLASS(cl), G_SIGNAL_RUN_FIRST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void hb_desktop_page_init(HbDesktopPage *p)
{
    (void)p;
}

void hb_desktop_page_attach_tab(HbDesktopPage *p, GtkWidget *win,
                                const char *tab_id)
{
    p->win = HB_IS_WINDOW(win) ? HB_WINDOW(win) : NULL;
    g_free(p->tab_id);
    p->tab_id = g_strdup(tab_id);
}

GtkWidget *hb_desktop_page_new(HbStore *st, hb_obj *host)
{
    HbDesktopPage *p;
    GdkDisplay *d;
    GdkClipboard *cb;
    const char *proto;

    p = g_object_new(HB_TYPE_DESKTOP_PAGE, NULL);
    p->store = st;
    p->host_id = g_strdup(hb_obj_id(host));
    proto = hb_obj_get_str(host, "proto");
    g_strlcpy(p->proto, proto ? proto : "vnc", sizeof p->proto);
    p->gen = 1;

    /* stack: connect 视图 / desktop 视图 */
    p->stack = gtk_stack_new();
    gtk_widget_set_vexpand(p->stack, TRUE);
    gtk_box_append(GTK_BOX(p), p->stack);

    p->conn_view = hb_connect_view_new();
    hb_connect_view_set_edit_btn(HB_CONNECT_VIEW(p->conn_view), TRUE);
    g_signal_connect(p->conn_view, "close",
                     G_CALLBACK(on_cv_close), p);
    g_signal_connect(p->conn_view, "start-over",
                     G_CALLBACK(on_cv_retry), p);
    g_signal_connect(p->conn_view, "edit-host",
                     G_CALLBACK(on_cv_edit), p);
    gtk_stack_add_named(GTK_STACK(p->stack), p->conn_view, "connect");

    {
        GtkWidget *dv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(dv, "hb-rd-view");
        build_desktop_view(p, dv);
        gtk_stack_add_named(GTK_STACK(p->stack), dv, "desktop");
    }
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");

    /* 本地剪贴板 → 远端（活动 tab 时） */
    d = gtk_widget_get_display(GTK_WIDGET(p));
    cb = gdk_display_get_clipboard(d);
    p->clip_handler = g_signal_connect(cb, "changed",
                                       G_CALLBACK(on_clip_changed), p);

    banner_refresh(p);          /* bound_port=0 占位，LISTENING 后更新 */
    start_attempt(p, FALSE);
    return GTK_WIDGET(p);
}
