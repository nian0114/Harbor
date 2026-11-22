/* FR-12 viewer tab：渲染 hb_share_peer 的 vt（外部 vt 模式），
 * banner / 只读提示 / ended overlay。 */

#include "hb-share-page.h"
#include "hb-terminal.h"
#include "hb-icons.h"
#include "hb-store.h"
#include "hb-window.h"
#include "hb-term-theme.h"

#include <string.h>

struct _HbSharePage {
    GtkBox             parent;

    hb_share_peer_ctx *ctx;          /* borrowed，svc->peers 拥有 */
    HbShareService    *svc;
    GtkWidget         *term;         /* HbTerminal ext_vt 模式 */
    GtkWidget         *clamp;        /* letterbox 包装 */
    GtkWidget         *banner;
    GtkWidget         *perm_lbl;
    GtkWidget         *notice;       /* 只读提示（rate-limit） */
    GtkWidget         *overlay_end;  /* ended overlay */
    GtkWidget         *end_reason;
    guint              notice_src;
    gint64             last_notice;
    gulong             sig_ev, sig_ended, sig_dead;
    gboolean           ended;
};

G_DEFINE_TYPE(HbSharePage, hb_share_page, GTK_TYPE_BOX)

/* vt rows×cols → letterbox 约束（GtkAspectFrame 太死：用尺寸约束+居中） */
static void fit_term(HbSharePage *p)
{
    hb_vt *v = hb_share_peer_ctx_vt(p->ctx);
    double cw = 0, ch = 0;
    int r = 24, c = 80;
    if (!v) return;
    hb_vt_size(v, &r, &c);
    hb_terminal_cell_size(HB_TERMINAL(p->term), &cw, &ch);
    if (cw > 0 && ch > 0)
        gtk_widget_set_size_request(p->term,
            (int)(c * cw + 32), (int)(r * ch + 28));
}

static void update_banner(HbSharePage *p)
{
    int perm = hb_share_peer_ctx_perm(p->ctx);
    gtk_label_set_label(GTK_LABEL(p->perm_lbl),
        perm == HB_SHARE_COLLAB ? "可协作" : "只读");
    gtk_widget_remove_css_class(p->perm_lbl, "collab");
    if (perm == HB_SHARE_COLLAB)
        gtk_widget_add_css_class(p->perm_lbl, "collab");
    hb_terminal_set_external_writable(HB_TERMINAL(p->term),
                                      perm == HB_SHARE_COLLAB);
}

static gboolean hide_notice(gpointer ud)
{
    HbSharePage *p = ud;
    gtk_widget_set_visible(p->notice, FALSE);
    p->notice_src = 0;
    return G_SOURCE_REMOVE;
}

static void show_notice(HbSharePage *p, const char *txt)
{
    gint64 now = g_get_monotonic_time();
    if (now - p->last_notice < 3 * G_USEC_PER_SEC)
        return;                     /* rate-limit 3s */
    p->last_notice = now;
    gtk_label_set_label(GTK_LABEL(p->notice), txt);
    gtk_widget_set_visible(p->notice, TRUE);
    if (p->notice_src)
        g_source_remove(p->notice_src);
    p->notice_src = g_timeout_add_seconds(3, hide_notice, p);
}

static void show_ended(HbSharePage *p, const char *reason)
{
    const char *txt;
    if (p->ended) return;
    p->ended = TRUE;
    if (!reason) reason = "stopped";
    if (!strcmp(reason, "stopped"))      txt = "所有者已停止共享";
    else if (!strcmp(reason, "kicked"))  txt = "已被移除";
    else if (!strcmp(reason, "owner_gone")) txt = "所有者已离线";
    else if (!strcmp(reason, "expired")) txt = "共享已过期";
    else                                 txt = "共享已结束";
    gtk_label_set_label(GTK_LABEL(p->end_reason), txt);
    gtk_widget_set_visible(p->overlay_end, TRUE);
}

static void on_dead_input(HbTerminal *t, const gchar *utf8,
                          HbSharePage *p)
{
    (void)t; (void)utf8;
    if (!p->ended)
        show_notice(p, "只读模式，输入未生效");
}

static void on_peer_event(HbShareService *s, gpointer ctx,
                          gpointer evp, HbSharePage *p)
{
    hb_share_event *ev = evp;
    (void)s;
    if (ctx != p->ctx) return;
    switch (ev->type) {
    case HB_SH_EV_SNAPSHOT:
    case HB_SH_EV_RESIZE:
        fit_term(p);
        hb_terminal_external_poke(HB_TERMINAL(p->term));
        break;
    case HB_SH_EV_PERM:
        update_banner(p);
        break;
    case HB_SH_EV_NOTICE:
        if (ev->text && *ev->text)
            show_notice(p, ev->text);
        break;
    case HB_SH_EV_ENDED:
        show_ended(p, ev->text);
        break;
    case HB_SH_EV_PAKE_FAIL:
        show_notice(p, "配对码错误");
        break;
    case HB_SH_EV_LOCKED:
        show_notice(p,
            "尝试次数过多，请让所有者重新生成配对码");
        show_ended(p, "expired");
        gtk_label_set_label(GTK_LABEL(p->end_reason),
            "尝试次数过多，请让所有者重新生成配对码");
        break;
    case HB_SH_EV_ERROR:
        show_notice(p, ev->text ? ev->text : "共享出错");
        break;
    default:
        hb_terminal_external_poke(HB_TERMINAL(p->term));
        break;
    }
    /* 其余事件（JOINED/PRESENCE 等）也可能改了 vt */
    hb_terminal_external_poke(HB_TERMINAL(p->term));
}

static void on_peer_ended(HbShareService *s, gpointer ctx,
                          const char *reason, HbSharePage *p)
{
    (void)s;
    if (ctx != p->ctx) return;
    show_ended(p, reason);
}

static void on_leave(GtkButton *b, HbSharePage *p)
{
    GtkRoot *r;
    (void)b;
    r = gtk_widget_get_root(GTK_WIDGET(p));
    if (HB_IS_WINDOW(r)) {
        const char *id = g_object_get_data(G_OBJECT(p), "tab-id");
        if (id)
            hb_window_close_tab(HB_WINDOW(r), id);
    }
}

static void on_close_end(GtkButton *b, HbSharePage *p)
{ on_leave(b, p); }

/* viewer tab 打开（视图卡片 + dev join 共用）。tab id = share-<id>。 */
void hb_share_open_viewer(GtkWidget *win, hb_share_peer_ctx *ctx,
                          const char *title)
{
    GtkWidget *pg;
    char id[64];
    if (!win) return;
    snprintf(id, sizeof id, "share-%s", hb_share_peer_ctx_id(ctx));
    /* 同 share 的 viewer 已存在：旧页面若已 ended 就换新，否则直接切换，
     * 并释放这次 join 产生的多余 ctx（add_tab 对重名 id 会销毁新页面，
     * 进而 release_peer 释放仍在使用的引擎 —— 会导致 UAF）。 */
    if (hb_window_has_tab(HB_WINDOW(win), id)) {
        GtkWidget *old = hb_window_tab_page(HB_WINDOW(win), id);
        if (HB_IS_SHARE_PAGE(old) && hb_share_page_ended(old)) {
            hb_window_close_tab(HB_WINDOW(win), id);
        } else {
            hb_share_service_release_peer(
                hb_share_service_default(), ctx);
            hb_window_switch_tab(HB_WINDOW(win), id);
            return;
        }
    }
    pg = hb_share_page_new(ctx);
    g_object_set_data_full(G_OBJECT(pg), "tab-id", g_strdup(id), g_free);
    hb_window_add_tab(HB_WINDOW(win), id,
                      (title && *title) ? title : "共享",
                      "broadcast", pg, TRUE, FALSE);
    hb_window_switch_tab(HB_WINDOW(win), id);
}

hb_share_peer_ctx *hb_share_page_ctx(GtkWidget *page)
{
    return HB_IS_SHARE_PAGE(page) ? HB_SHARE_PAGE(page)->ctx : NULL;
}

gboolean hb_share_page_ended(GtkWidget *page)
{
    return HB_IS_SHARE_PAGE(page) && HB_SHARE_PAGE(page)->ended;
}

const char *hb_share_page_end_label(GtkWidget *page)
{
    HbSharePage *p;
    if (!HB_IS_SHARE_PAGE(page)) return NULL;
    p = HB_SHARE_PAGE(page);
    return p->ended ? gtk_label_get_text(GTK_LABEL(p->end_reason))
                    : NULL;
}

/* dev：peer 屏幕文本快照（含 scrollback? 只取可见区）。 */
char *hb_share_page_screen(GtkWidget *page)
{
    HbSharePage *p = HB_SHARE_PAGE(page);
    hb_vt *v;
    int rows, cols;
    GString *out;
    int r;
    if (!p->ctx) return NULL;
    v = hb_share_peer_ctx_vt(p->ctx);
    if (!v) return NULL;
    hb_vt_size(v, &rows, &cols);
    out = g_string_new(NULL);
    hb_vt_lock(v);
    for (r = 0; r < rows; r++) {
        char *line = hb_vt_get_text(v, r, 0, r, cols - 1, false);
        if (line) {
            g_string_append(out, line);
            g_free(line);
        }
        g_string_append_c(out, '\n');
    }
    hb_vt_unlock(v);
    return g_string_free(out, FALSE);
}

GtkWidget *hb_share_page_new(hb_share_peer_ctx *ctx)
{
    HbSharePage *p = g_object_new(HB_TYPE_SHARE_PAGE, NULL);
    GtkWidget *title_l, *e2e, *leave, *center, *endbox;
    const char *title;
    char *btxt;

    p->ctx = ctx;
    p->svc = hb_share_service_default();

    /* banner：正在观看 <title> · [只读|可协作] · 端到端加密 · [离开] */
    p->banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(p->banner, "hb-share-banner");
    gtk_widget_set_margin_start(p->banner, 12);
    gtk_widget_set_margin_end(p->banner, 12);
    gtk_widget_set_margin_top(p->banner, 6);
    gtk_widget_set_margin_bottom(p->banner, 6);

    gtk_box_append(GTK_BOX(p->banner), hb_icon_new("broadcast", 14));
    title = hb_share_peer_ctx_title(ctx);
    btxt = g_strdup_printf("正在观看 %s",
                           (title && *title) ? title : "共享会话");
    title_l = gtk_label_new(btxt);
    g_free(btxt);
    gtk_widget_add_css_class(title_l, "hb-share-banner-title");
    gtk_box_append(GTK_BOX(p->banner), title_l);

    p->perm_lbl = gtk_label_new("只读");
    gtk_widget_add_css_class(p->perm_lbl, "hb-share-pill");
    gtk_box_append(GTK_BOX(p->banner), p->perm_lbl);

    e2e = gtk_label_new("端到端加密");
    gtk_widget_add_css_class(e2e, "hb-term-muted");
    gtk_widget_set_hexpand(e2e, TRUE);
    gtk_label_set_xalign(GTK_LABEL(e2e), 1);
    gtk_box_append(GTK_BOX(p->banner), e2e);

    leave = gtk_button_new_with_label("离开");
    gtk_widget_add_css_class(leave, "tbtn");
    g_signal_connect(leave, "clicked", G_CALLBACK(on_leave), p);
    gtk_box_append(GTK_BOX(p->banner), leave);
    gtk_box_append(GTK_BOX(p), p->banner);

    /* 只读提示（banner 下的小条，rate-limited） */
    p->notice = gtk_label_new("");
    gtk_widget_add_css_class(p->notice, "hb-share-notice");
    gtk_widget_set_visible(p->notice, FALSE);
    gtk_box_append(GTK_BOX(p), p->notice);

    /* 终端 letterbox：GtkOverlay 上放 ended overlay */
    {
        GtkWidget *ovl = gtk_overlay_new();
        center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        p->clamp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_halign(p->clamp, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(p->clamp, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(p->clamp, TRUE);
        gtk_widget_set_vexpand(p->clamp, TRUE);

        p->term = hb_terminal_new();
        gtk_widget_set_hexpand(p->term, FALSE);
        gtk_widget_set_vexpand(p->term, FALSE);
        gtk_widget_set_halign(p->term, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(p->term, GTK_ALIGN_CENTER);
        /* 主题/字体按 settings（与普通终端一致） */
        {
            hb_obj *o = NULL;
            const char *theme_id = "harbor";
            const char *family = "Monospace";
            double px = 13, lh = 1.5;
            HbStore *st = hb_store_default();
            if (hb_vault_settings(hb_store_vault(st), &o) == HB_OK &&
                o) {
                const char *t = hb_obj_get_str(o, "theme");
                const char *f = hb_obj_get_str(o, "font_family");
                if (t && *t) theme_id = t;
                if (f && *f) family = f;
                px = (double)hb_obj_get_int(o, "font_size", 13);
                lh = hb_obj_get_double(o, "line_height", 1.5);
            }
            {
                g_autofree char *tc =
                    g_strdup_printf("theme-%s", theme_id);
                gtk_widget_add_css_class(GTK_WIDGET(p), tc);
            }
            hb_terminal_set_theme(HB_TERMINAL(p->term),
                                  hb_term_theme_get(theme_id));
            hb_terminal_set_font(HB_TERMINAL(p->term), family, px,
                                 lh);
        }
        hb_terminal_set_external_vt(HB_TERMINAL(p->term),
                                    hb_share_peer_ctx_vt(ctx));
        gtk_box_append(GTK_BOX(p->clamp), p->term);
        gtk_box_append(GTK_BOX(center), p->clamp);
        gtk_overlay_set_child(GTK_OVERLAY(ovl), center);

        /* ended overlay：保留最后一帧 */
        p->overlay_end = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_add_css_class(p->overlay_end, "hb-share-ended");
        gtk_widget_set_halign(p->overlay_end, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(p->overlay_end, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(p->overlay_end, FALSE);
        {
            GtkWidget *ic = hb_icon_new("broadcast", 28);
            gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(p->overlay_end), ic);
            gtk_widget_add_css_class(ic, "hb-share-ended-ico");
        }
        p->end_reason = gtk_label_new("共享已结束");
        gtk_widget_add_css_class(p->end_reason, "hb-share-ended-txt");
        gtk_box_append(GTK_BOX(p->overlay_end), p->end_reason);
        endbox = gtk_button_new_with_label("关闭");
        gtk_widget_add_css_class(endbox, "pill");
        gtk_widget_set_halign(endbox, GTK_ALIGN_CENTER);
        g_signal_connect(endbox, "clicked",
                         G_CALLBACK(on_close_end), p);
        gtk_box_append(GTK_BOX(p->overlay_end), endbox);
        gtk_overlay_add_overlay(GTK_OVERLAY(ovl), p->overlay_end);

        gtk_box_append(GTK_BOX(p), ovl);
        gtk_widget_set_vexpand(ovl, TRUE);
    }

    update_banner(p);
    fit_term(p);
    p->sig_ev = g_signal_connect(p->svc, "peer-event",
                                 G_CALLBACK(on_peer_event), p);
    p->sig_ended = g_signal_connect(p->svc, "peer-ended",
                                    G_CALLBACK(on_peer_ended), p);
    p->sig_dead = g_signal_connect(p->term, "dead-input",
                                   G_CALLBACK(on_dead_input), p);
    return GTK_WIDGET(p);
}

static void hb_share_page_dispose(GObject *o)
{
    HbSharePage *p = HB_SHARE_PAGE(o);
    if (p->svc) {
        g_signal_handler_disconnect(p->svc, p->sig_ev);
        g_signal_handler_disconnect(p->svc, p->sig_ended);
    }
    if (p->notice_src)
        g_source_remove(p->notice_src);
    /* stack 的 crossfade 会让本页在 dispose 后才真正销毁子树：
     * 先摘掉外部 vt，防止滞留的 blink 定时器访问已释放的 vt。 */
    if (p->term)
        hb_terminal_set_external_vt(HB_TERMINAL(p->term), NULL);
    if (p->ctx)
        hb_share_service_release_peer(p->svc, p->ctx);
    p->ctx = NULL;
    G_OBJECT_CLASS(hb_share_page_parent_class)->dispose(o);
}

static void hb_share_page_class_init(HbSharePageClass *k)
{
    G_OBJECT_CLASS(k)->dispose = hb_share_page_dispose;
}

static void hb_share_page_init(HbSharePage *p)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(p),
                                   GTK_ORIENTATION_VERTICAL);
}
