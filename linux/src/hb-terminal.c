#include "hb-terminal.h"
#include "hb-icons.h"

#include <glib-unix.h>
#include <string.h>
#include <glib/gi18n.h>

static void send_input_scroll_bottom(HbTerminal *t);

#define PAD_X 16.0
#define PAD_Y 14.0
#define WHEEL_LINES 3
#define BLINK_MS 530

typedef struct { int64_t abs; int col, len; } HbTermFindM;

struct _HbTerminal {
    GtkWidget parent;

    hb_session *sess;            /* borrowed */
    hb_vt      *preview_vt;      /* 预览模式：无会话的内部 vt（设置页） */
    char       *preview_text;    /* 预览 ANSI 文本；尺寸变化时重喂 */
    hb_vt      *ext_vt;          /* 外部 vt（FR-12 viewer，borrowed） */
    gboolean    ext_writable;    /* ext 模式：false=只读，输入被拒 */
    guint       ev_source;

    const HbTermTheme *theme;
    char        *font_family;
    double       font_px, line_h;
    PangoFontDescription *fdesc;
    double       cw, ch;         /* cell 尺寸（device px） */
    double       font_h;         /* ascent+descent */
    int          cols, rows;     /* 上次 resize 上报的格数 */

    int64_t      scroll_off;     /* 0 = 在底部；>0 回滚行数 */
    double       wheel_acc;
    uint64_t     last_total;     /* 上次事件时的 total_pushed */
    GtkAdjustment *adj;

    /* 选择：绝对行坐标 abs = total_pushed + row（见 vt.h） */
    gboolean     sel;
    int64_t      sel_a_r, sel_b_r;
    int          sel_a_c, sel_b_c;
    int64_t      anchor_r;  int anchor_c;
    gboolean     dragging;

    GtkIMContext *im;
    char         *preedit;

    gboolean     focused;
    guint        blink_src;
    gboolean     blink_vis;
    gboolean     cursor_blink_set;
    gboolean     osc52;
    gboolean     dead;

    guint        flash_src;

    GRegex      *url_re;
    gboolean     url_hover;
    int64_t      url_r;          /* 命中 URL 的 vt 行 + 列区间 */
    int          url_c0, url_c1;

    /* 查找：find_ms 存绝对行（= search 时 total_pushed + vt row） */
    char        *find_needle;
    gboolean     find_ci;
    HbTermFindM *find_ms;
    size_t       find_n;
    int          find_cur;      /* 0-based；-1 无命中 */
    uint64_t     find_gen;      /* 上次搜索时的 vt generation */
    guint        find_timer;

    hb_terminal_event_cb ev_cb;  /* 转发终端不消费的 session 事件 */
    gpointer     ev_ud;

    hb_terminal_mirror_cb mirror_cb;  /* Workspace Broadcast 输入镜像 */
    gpointer     mirror_ud;
};

/* 输入写进本 session 之后回调镜像（workspace broadcast 转发给其他 pane） */
static void mirror_emit(HbTerminal *t, const hb_terminal_mirror_event *ev)
{
    if (t->mirror_cb)
        t->mirror_cb(t, ev, t->mirror_ud);
}

void hb_terminal_set_input_mirror(HbTerminal *t, hb_terminal_mirror_cb cb,
                                  gpointer ud)
{
    t->mirror_cb = cb;
    t->mirror_ud = ud;
}

G_DEFINE_TYPE(HbTerminal, hb_terminal, GTK_TYPE_WIDGET)

enum {
    SIG_TITLE, SIG_EXITED, SIG_BELL, SIG_OSC133, SIG_NOTIFY,
    SIG_DEAD_INPUT, SIG_FIND_CHANGED, N_SIGS
};
static guint sigs[N_SIGS];

/* ------------------------------------------------------------------ */
/* 颜色解析                                                            */
/* ------------------------------------------------------------------ */

static void resolve_color(HbTerminal *t, const hb_color *c, gboolean fg,
                          GdkRGBA *out)
{
    switch (c->type) {
    case HB_COLOR_DEFAULT_FG: *out = t->theme->fg; return;
    case HB_COLOR_DEFAULT_BG: *out = t->theme->bg; return;
    case HB_COLOR_INDEXED:
        hb_term_theme_indexed(t->theme, c->idx, out);
        return;
    default:
        out->red = c->r / 255.0; out->green = c->g / 255.0;
        out->blue = c->b / 255.0; out->alpha = 1.0;
        return;
    }
    (void)fg;
}

static gboolean flash_cb(gpointer ud);
static gboolean blink_cb(gpointer ud);

GtkAdjustment *hb_terminal_scroll_adjustment(HbTerminal *t)
{
    return t->adj;
}

static gboolean same_rgba(const GdkRGBA *a, const GdkRGBA *b)
{
    return fabs(a->red - b->red) < 0.004 && fabs(a->green - b->green) < 0.004
        && fabs(a->blue - b->blue) < 0.004 && fabs(a->alpha - b->alpha) < 0.004;
}

/* 渲染/滚动/查找走"当前 vt"：预览模式用内部 vt，否则用 session 的 */
static hb_vt *term_vt(HbTerminal *t)
{
    if (t->ext_vt)
        return t->ext_vt;
    if (t->preview_vt)
        return t->preview_vt;
    return t->sess ? hb_session_vt(t->sess) : NULL;
}

/* ext 模式下所有输入经 vt 的 output_fn（服务挂到 hb_share_peer_input）
 * 统一编码送出；ext_writable=false 走 dead-input 拒绝路径。 */
static gboolean ext_dead(HbTerminal *t)
{
    return t->ext_vt && !t->ext_writable;
}

/* ------------------------------------------------------------------ */
/* 字体度量                                                            */
/* ------------------------------------------------------------------ */

static void update_metrics(HbTerminal *t)
{
    PangoContext *ctx = gtk_widget_get_pango_context(GTK_WIDGET(t));
    PangoFontMetrics *m;
    int aw, ah;

    m = pango_context_get_metrics(ctx, t->fdesc, NULL);
    aw = pango_font_metrics_get_approximate_digit_width(m);
    ah = pango_font_metrics_get_ascent(m) + pango_font_metrics_get_descent(m);
    pango_font_metrics_unref(m);

    t->cw = MAX(1.0, floor(aw / (double)PANGO_SCALE + 0.5));
    t->font_h = ah / (double)PANGO_SCALE;
    t->ch = MAX(t->font_h, floor(t->font_h * t->line_h + 0.5));
}

void hb_terminal_set_font(HbTerminal *t, const char *family,
                          double px, double line_height)
{
    g_free(t->font_family);
    t->font_family = g_strdup(family && *family ? family : "Monospace");
    t->font_px = px > 0 ? px : 13;
    t->line_h = line_height > 0 ? line_height : 1.0;
    pango_font_description_free(t->fdesc);
    t->fdesc = pango_font_description_new();
    pango_font_description_set_family(t->fdesc, t->font_family);
    pango_font_description_set_size(t->fdesc,
        (int)(t->font_px * PANGO_SCALE));
    update_metrics(t);
    t->cols = t->rows = 0;     /* 下次 allocate 重新上报 */
    gtk_widget_queue_resize(GTK_WIDGET(t));
}

void hb_terminal_set_theme(HbTerminal *t, const HbTermTheme *theme)
{
    t->theme = theme ? theme : hb_term_theme_get(NULL);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void hb_terminal_set_osc52(HbTerminal *t, gboolean allowed)
{
    hb_vt *v;
    t->osc52 = allowed;
    if ((v = term_vt(t)))
        hb_vt_set_osc52_allowed(v, allowed);
}

void hb_terminal_set_cursor_blink(HbTerminal *t, gboolean enabled)
{
    t->cursor_blink_set = enabled;
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* ------------------------------------------------------------------ */
/* 滚动                                                                */
/* ------------------------------------------------------------------ */

static void sync_adj(HbTerminal *t)
{
    hb_vt *v = term_vt(t);
    int sb = v ? hb_vt_scrollback_count(v) : 0;
    gtk_adjustment_configure(t->adj, (double)t->scroll_off,
        0.0, (double)sb, 1.0, (double)t->rows, (double)t->rows);
}

void hb_terminal_scroll_to_bottom(HbTerminal *t)
{
    if (t->scroll_off != 0) {
        t->scroll_off = 0;
        sync_adj(t);
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

static void scroll_lines(HbTerminal *t, double nlines)
{
    hb_vt *v = term_vt(t);
    int sb = v ? hb_vt_scrollback_count(v) : 0;
    int64_t off;
    t->wheel_acc += nlines;
    off = t->scroll_off - (int64_t)t->wheel_acc;
    t->wheel_acc -= (int64_t)t->wheel_acc;
    if (off < 0) off = 0;
    if (off > sb) off = sb;
    if (off != t->scroll_off) {
        t->scroll_off = off;
        sync_adj(t);
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

static void on_adj_changed(GtkAdjustment *a, gpointer ud)
{
    HbTerminal *t = ud;
    int64_t off = (int64_t)(gtk_adjustment_get_value(a) + 0.5);
    if (off != t->scroll_off) {
        t->scroll_off = off;
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

/* ------------------------------------------------------------------ */
/* 查找（scrollback + 屏幕）                                            */
/*                                                                     */
/* hb_vt_search 返回的 row 是相对 vt 行（-sb_len..rows-1，负数=回滚区，  */
/* -1 最新）。这里统一转绝对行 abs = total_pushed + row 存储：新输出    */
/* 只增大 total_pushed，abs 不变 → 当前项跨 recompute 稳定；被淘汰出    */
/* scrollback 的行 abs 保持不变但不再出现在新搜索结果里，自然消失。      */
/* ------------------------------------------------------------------ */

static void find_emit(HbTerminal *t)
{
    g_signal_emit(t, sigs[SIG_FIND_CHANGED], 0);
}

/* 执行搜索，结果转绝对行；keep_abs >= 0 时把 cur 定到该绝对行的命中 */
static void find_run(HbTerminal *t, int64_t keep_abs)
{
    hb_vt *v = term_vt(t);
    hb_vt_match *ms = NULL;
    int64_t total;
    size_t n, i;

    g_clear_pointer(&t->find_ms, g_free);
    t->find_n = 0;
    t->find_cur = -1;
    if (!v || !t->find_needle || !*t->find_needle) {
        t->find_gen = v ? hb_vt_generation(v) : 0;
        return;
    }
    n = hb_vt_search(v, t->find_needle, t->find_ci, &ms);
    total = (int64_t)hb_vt_total_pushed(v);
    t->find_gen = hb_vt_generation(v);
    t->find_ms = g_new(HbTermFindM, n ? n : 1);
    for (i = 0; i < n; i++) {
        t->find_ms[i].abs = total + ms[i].row;   /* → 绝对行 */
        t->find_ms[i].col = ms[i].col;
        t->find_ms[i].len = ms[i].len;
    }
    g_free(ms);
    t->find_n = n;
    if (n) {
        t->find_cur = 0;
        if (keep_abs >= 0) {
            /* 同一绝对行取第一个命中；没有就用最近的不超过它的 */
            for (i = 0; i < n && t->find_ms[i].abs < keep_abs; i++)
                t->find_cur = (int)i;
            if (t->find_cur < (int)n - 1 &&
                t->find_ms[t->find_cur].abs != keep_abs &&
                t->find_ms[t->find_cur + 1].abs == keep_abs)
                t->find_cur++;
        }
    }
}

static gboolean find_recompute_cb(gpointer ud)
{
    HbTerminal *t = ud;
    hb_vt *v = term_vt(t);
    t->find_timer = 0;
    if (t->find_needle && v &&
        hb_vt_generation(v) != t->find_gen) {
        int64_t keep = t->find_cur >= 0 ? t->find_ms[t->find_cur].abs : -1;
        size_t old_n = t->find_n; int old_c = t->find_cur;
        find_run(t, keep);
        if (t->find_n != old_n || t->find_cur != old_c)
            find_emit(t);
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
    return G_SOURCE_REMOVE;
}

/* vt 有新输出：挂起（≤10Hz）一次重算 */
static void find_schedule(HbTerminal *t)
{
    if (!t->find_needle || t->find_timer)
        return;
    t->find_timer = g_timeout_add(100, find_recompute_cb, t);
}

/* 当前命中滚到视图垂直居中 */
static void find_scroll_cur(HbTerminal *t)
{
    hb_vt *v;
    int64_t total, vrow;
    int vrows = 0, sb = 0, off;
    if (t->find_cur < 0) return;
    v = term_vt(t);
    if (!v) return;
    total = (int64_t)hb_vt_total_pushed(v);
    sb = hb_vt_scrollback_count(v);
    hb_vt_size(v, &vrows, NULL);
    vrow = t->find_ms[t->find_cur].abs - total;
    off = vrows / 2 - (int)vrow;
    if (off < 0) off = 0;
    if (off > sb) off = sb;
    if (off != t->scroll_off) {
        t->scroll_off = off;
        sync_adj(t);
    }
}

guint hb_terminal_find_set(HbTerminal *t, const char *needle,
                           gboolean case_sensitive)
{
    gboolean empty = !needle || !*needle;
    g_free(t->find_needle);
    t->find_needle = empty ? NULL : g_strdup(needle);
    t->find_ci = !case_sensitive;
    if (t->find_timer) {
        g_source_remove(t->find_timer);
        t->find_timer = 0;
    }
    find_run(t, -1);
    if (!empty && t->find_n)
        find_scroll_cur(t);
    find_emit(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
    return (guint)t->find_n;
}

guint hb_terminal_find_step(HbTerminal *t, int dir)
{
    if (!t->find_n)
        return 0;
    t->find_cur = (t->find_cur + (dir >= 0 ? 1 : -1) + (int)t->find_n)
                  % (int)t->find_n;
    find_scroll_cur(t);
    find_emit(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
    return (guint)t->find_cur + 1;
}

void hb_terminal_find_counts(HbTerminal *t, guint *cur, guint *total)
{
    if (cur) *cur = t->find_cur >= 0 ? (guint)t->find_cur + 1 : 0;
    if (total) *total = (guint)t->find_n;
}

/* ------------------------------------------------------------------ */
/* 会话事件                                                            */
/* ------------------------------------------------------------------ */

static void on_output(HbTerminal *t)
{
    hb_vt *v = hb_session_vt(t->sess);
    uint64_t total = hb_vt_total_pushed(v);
    int64_t pushed = (int64_t)(total - t->last_total);
    int sb = hb_vt_scrollback_count(v);

    if (t->scroll_off > 0) {
        /* 回滚中：锚定同一批绝对行（新的推入把 scrollback 整体加高） */
        t->scroll_off += pushed;
        if (t->scroll_off > sb) t->scroll_off = sb;
    }
    /* 与屏幕区相交的选区随新输出作废 */
    if (t->sel && t->sel_b_r >= (int64_t)t->last_total)
        t->sel = FALSE;
    t->last_total = total;
    find_schedule(t);
    sync_adj(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static gboolean on_ev_fd(int fd, GIOCondition cond, gpointer ud)
{
    HbTerminal *t = ud;
    hb_sess_event ev;
    (void)fd;
    if (cond & (G_IO_HUP | G_IO_ERR) || !t->sess)
        return G_SOURCE_REMOVE;
    while (hb_session_next_event(t->sess, &ev)) {
        switch (ev.type) {
        case HB_SESS_EV_OUTPUT:
            on_output(t);
            break;
        case HB_SESS_EV_TITLE:
            g_signal_emit(t, sigs[SIG_TITLE], 0, ev.text);
            break;
        case HB_SESS_EV_BELL:
            g_signal_emit(t, sigs[SIG_BELL], 0);
            if (t->flash_src)
                g_source_remove(t->flash_src);
            t->flash_src = g_timeout_add(100, flash_cb, t);
            gtk_widget_queue_draw(GTK_WIDGET(t));
            break;
        case HB_SESS_EV_OSC133:
            g_signal_emit(t, sigs[SIG_OSC133], 0, ev.mark, ev.code);
            break;
        case HB_SESS_EV_NOTIFY:
            g_signal_emit(t, sigs[SIG_NOTIFY], 0, ev.text, ev.text2);
            break;
        case HB_SESS_EV_CLIPBOARD:
            if (t->osc52 && ev.text) {
                GdkClipboard *cb = gdk_display_get_clipboard(
                    gtk_widget_get_display(GTK_WIDGET(t)));
                gdk_clipboard_set_text(cb, ev.text);
            }
            break;
        case HB_SESS_EV_EXIT:
            t->dead = TRUE;
            g_signal_emit(t, sigs[SIG_EXITED], 0, ev.code);
            gtk_widget_queue_draw(GTK_WIDGET(t));
            break;
        default:
            /* STAGE/CONNECTED/ERROR/HOSTKEY/PROMPT/OSINFO 等 → 页层消费 */
            if (t->ev_cb)
                t->ev_cb(t, &ev, t->ev_ud);
            break;
        }
        hb_sess_event_clear(&ev);
    }
    return G_SOURCE_CONTINUE;
}

void hb_terminal_set_session(HbTerminal *t, hb_session *s)
{
    if (t->ev_source) {
        g_source_remove(t->ev_source);
        t->ev_source = 0;
    }
    t->sess = s;
    t->ext_vt = NULL;           /* session 模式与 ext 模式互斥 */
    t->dead = FALSE;
    t->scroll_off = 0;
    t->sel = FALSE;
    /* 旧 vt 的命中随 session 一起失效 */
    g_clear_pointer(&t->find_needle, g_free);
    g_clear_pointer(&t->find_ms, g_free);
    t->find_n = 0;
    t->find_cur = -1;
    if (t->find_timer) {
        g_source_remove(t->find_timer);
        t->find_timer = 0;
    }
    if (s) {
        hb_vt *v = hb_session_vt(s);
        t->last_total = hb_vt_total_pushed(v);
        hb_vt_set_osc52_allowed(v, t->osc52);
        t->ev_source = g_unix_fd_add(hb_session_event_fd(s),
            G_IO_IN | G_IO_ERR | G_IO_HUP, on_ev_fd, t);
        if (t->cols > 0)
            hb_session_resize(s, t->rows, t->cols);
    }
    sync_adj(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

hb_session *hb_terminal_get_session(HbTerminal *t)
{
    return t->sess;
}

/* ------------------------- 外部 vt（FR-12 viewer） ------------------------- */

void hb_terminal_set_external_vt(HbTerminal *t, hb_vt *v)
{
    hb_terminal_set_session(t, NULL);   /* 清 sess/ev_source + 复位命中 */
    t->ext_vt = v;
    t->ext_writable = FALSE;
    t->dead = FALSE;
    t->focused = TRUE;                  /* viewer 里实心块光标 */
    t->blink_vis = TRUE;
    if (v) {
        t->last_total = hb_vt_total_pushed(v);
        hb_vt_set_osc52_allowed(v, t->osc52);
        if (!t->blink_src)
            t->blink_src = g_timeout_add(BLINK_MS, blink_cb, t);
    }
    sync_adj(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void hb_terminal_set_external_writable(HbTerminal *t, gboolean writable)
{
    t->ext_writable = writable;
}

gboolean hb_terminal_external_writable(HbTerminal *t)
{
    return t->ext_writable;
}

void hb_terminal_external_poke(HbTerminal *t)
{
    if (!t->ext_vt)
        return;
    find_schedule(t);
    sync_adj(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void hb_terminal_cell_size(HbTerminal *t, double *cw, double *ch)
{
    if (cw) *cw = t->cw;
    if (ch) *ch = t->ch;
}

/* 预览 vt 重建：按当前尺寸新建再喂文本。resize 会把顶行推入 scrollback
 * （xterm 语义），预览内容会"消失"，所以尺寸变化时重建而非 resize。 */
static void preview_rebuild(HbTerminal *t)
{
    int rows = t->rows > 0 ? t->rows : 24;
    int cols = t->cols > 0 ? t->cols : 80;

    if (!t->preview_text)
        return;
    g_clear_pointer(&t->preview_vt, hb_vt_free);
    t->preview_vt = hb_vt_new(rows, cols, 256);
    if (t->preview_vt) {
        hb_vt_feed(t->preview_vt, t->preview_text,
                   strlen(t->preview_text));
        t->last_total = hb_vt_total_pushed(t->preview_vt);
    }
}

/* 预览模式（设置页）：无会话的内部 vt。feed 一段 ANSI 文本后静态渲染，
 * 输入天然被忽略（所有输入路径都走 t->sess）。实心块光标 + 闪烁。 */
void hb_terminal_set_preview(HbTerminal *t, const char *ansi_text)
{
    g_free(t->preview_text);
    t->preview_text = g_strdup(ansi_text);
    preview_rebuild(t);
    t->focused = TRUE;           /* 块光标（非空心） */
    t->blink_vis = TRUE;
    if (!t->blink_src)
        t->blink_src = g_timeout_add(BLINK_MS, blink_cb, t);
    sync_adj(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* ------------------------------------------------------------------ */
/* 选择                                                                */
/* ------------------------------------------------------------------ */

static void cell_from_xy(HbTerminal *t, double x, double y,
                         int64_t *abs_r, int *col, int *d_row)
{
    int c = (int)((x - PAD_X) / t->cw);
    int d = (int)((y - PAD_Y) / t->ch);
    if (d < 0) d = 0;
    if (c < 0) c = 0;
    if (d_row) *d_row = d;
    *col = c;
    *abs_r = (int64_t)t->last_total + (d - t->scroll_off);
}

static void sel_normalize(HbTerminal *t, int64_t *r0, int *c0,
                          int64_t *r1, int *c1)
{
    if (t->sel_a_r < t->sel_b_r ||
        (t->sel_a_r == t->sel_b_r && t->sel_a_c <= t->sel_b_c)) {
        *r0 = t->sel_a_r; *c0 = t->sel_a_c;
        *r1 = t->sel_b_r; *c1 = t->sel_b_c;
    } else {
        *r0 = t->sel_b_r; *c0 = t->sel_b_c;
        *r1 = t->sel_a_r; *c1 = t->sel_a_c;
    }
}

static gboolean sel_covers(HbTerminal *t, int64_t abs_r, int col)
{
    int64_t r0, r1; int c0, c1;
    if (!t->sel) return FALSE;
    sel_normalize(t, &r0, &c0, &r1, &c1);
    if (abs_r < r0 || abs_r > r1) return FALSE;
    if (abs_r == r0 && col < c0) return FALSE;
    if (abs_r == r1 && col > c1) return FALSE;
    return TRUE;
}

static char *sel_text(HbTerminal *t)
{
    hb_vt *v;
    int64_t r0, r1; int c0, c1;
    if (!t->sel) return NULL;
    v = term_vt(t);
    if (!v) return NULL;
    sel_normalize(t, &r0, &c0, &r1, &c1);
    return hb_vt_get_text(v, (int)(r0 - (int64_t)hb_vt_total_pushed(v)),
                          c0,
                          (int)(r1 - (int64_t)hb_vt_total_pushed(v)),
                          c1, false);
}

void hb_terminal_select_all(HbTerminal *t)
{
    hb_vt *v;
    int rows, cols;
    v = term_vt(t);
    if (!v) return;
    hb_vt_size(v, &rows, &cols);
    t->sel = TRUE;
    t->sel_a_r = (int64_t)hb_vt_total_pushed(v) - hb_vt_scrollback_count(v);
    t->sel_a_c = 0;
    t->sel_b_r = (int64_t)hb_vt_total_pushed(v) + rows - 1;
    t->sel_b_c = cols - 1;
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void set_primary_text(HbTerminal *t, const char *text)
{
    GdkClipboard *cb;
    if (!text) return;
    cb = gdk_display_get_primary_clipboard(
        gtk_widget_get_display(GTK_WIDGET(t)));
    gdk_clipboard_set_text(cb, text);
}

void hb_terminal_copy_clipboard(HbTerminal *t)
{
    char *txt = sel_text(t);
    GdkClipboard *cb;
    if (!txt) return;
    cb = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(t)));
    gdk_clipboard_set_text(cb, txt);
    g_free(txt);
}

static void on_paste_text(GObject *src, GAsyncResult *res, gpointer ud)
{
    HbTerminal *t = ud;
    char *txt = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (txt && t->ext_vt && t->ext_writable) {
        send_input_scroll_bottom(t);
        hb_vt_paste(t->ext_vt, txt);
    } else if (txt && t->ext_vt) {
        g_signal_emit(t, sigs[SIG_DEAD_INPUT], 0, txt);
    } else if (txt && t->sess && hb_session_alive(t->sess)) {
        hb_terminal_mirror_event me = { HB_MIRROR_PASTE, 0, 0, 0,
                                        txt, strlen(txt) };
        hb_session_paste(t->sess, txt);
        mirror_emit(t, &me);
    }
    g_free(txt);
}

void hb_terminal_paste_clipboard(HbTerminal *t)
{
    GdkClipboard *cb = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(t)));
    gdk_clipboard_read_text_async(cb, NULL, on_paste_text, t);
}

static void paste_primary(HbTerminal *t)
{
    GdkClipboard *cb = gdk_display_get_primary_clipboard(
        gtk_widget_get_display(GTK_WIDGET(t)));
    gdk_clipboard_read_text_async(cb, NULL, on_paste_text, t);
}

void hb_terminal_clear_scrollback(HbTerminal *t)
{
    if (!t->sess) return;
    hb_vt_clear_scrollback(hb_session_vt(t->sess));
    t->scroll_off = 0;
    sync_adj(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* ------------------------------------------------------------------ */
/* 键盘 / IME                                                          */
/* ------------------------------------------------------------------ */

static int ev_mods(GdkModifierType st)
{
    int m = 0;
    if (st & GDK_SHIFT_MASK)   m |= HB_MOD_SHIFT;
    if (st & GDK_ALT_MASK)     m |= HB_MOD_ALT;
    if (st & GDK_CONTROL_MASK) m |= HB_MOD_CTRL;
    return m;
}

static hb_key map_keyval(guint kv)
{
    switch (kv) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return HB_KEY_ENTER;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return HB_KEY_TAB;
    case GDK_KEY_BackSpace: return HB_KEY_BACKSPACE;
    case GDK_KEY_Escape: return HB_KEY_ESCAPE;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: return HB_KEY_UP;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: return HB_KEY_DOWN;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: return HB_KEY_LEFT;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: return HB_KEY_RIGHT;
    case GDK_KEY_Insert: case GDK_KEY_KP_Insert: return HB_KEY_INS;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return HB_KEY_DEL;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: return HB_KEY_HOME;
    case GDK_KEY_End: case GDK_KEY_KP_End: return HB_KEY_END;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return HB_KEY_PAGEUP;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return HB_KEY_PAGEDOWN;
    case GDK_KEY_F1: return HB_KEY_F1;
    case GDK_KEY_F2: return HB_KEY_F1 + 1;
    case GDK_KEY_F3: return HB_KEY_F1 + 2;
    case GDK_KEY_F4: return HB_KEY_F1 + 3;
    case GDK_KEY_F5: return HB_KEY_F1 + 4;
    case GDK_KEY_F6: return HB_KEY_F1 + 5;
    case GDK_KEY_F7: return HB_KEY_F1 + 6;
    case GDK_KEY_F8: return HB_KEY_F1 + 7;
    case GDK_KEY_F9: return HB_KEY_F1 + 8;
    case GDK_KEY_F10: return HB_KEY_F1 + 9;
    case GDK_KEY_F11: return HB_KEY_F1 + 10;
    case GDK_KEY_F12: return HB_KEY_F1 + 11;
    case GDK_KEY_KP_0: return HB_KEY_KP_0;
    case GDK_KEY_KP_1: return HB_KEY_KP_0 + 1;
    case GDK_KEY_KP_2: return HB_KEY_KP_0 + 2;
    case GDK_KEY_KP_3: return HB_KEY_KP_0 + 3;
    case GDK_KEY_KP_4: return HB_KEY_KP_0 + 4;
    case GDK_KEY_KP_5: return HB_KEY_KP_0 + 5;
    case GDK_KEY_KP_6: return HB_KEY_KP_0 + 6;
    case GDK_KEY_KP_7: return HB_KEY_KP_0 + 7;
    case GDK_KEY_KP_8: return HB_KEY_KP_0 + 8;
    case GDK_KEY_KP_9: return HB_KEY_KP_0 + 9;
    default: return HB_KEY_NONE;
    }
}

static void send_input_scroll_bottom(HbTerminal *t)
{
    hb_terminal_scroll_to_bottom(t);
    /* 输入时让光标立即可见并重置闪烁节拍 */
    t->blink_vis = TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *kc, guint keyval,
                               guint code, GdkModifierType state, gpointer ud)
{
    HbTerminal *t = ud;
    GdkEvent *ev;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    hb_key k;
    (void)kc; (void)code;

    /* 终端保留快捷键 */
    if (ctrl && shift) {
        switch (keyval) {
        case GDK_KEY_C: case GDK_KEY_c:
            hb_terminal_copy_clipboard(t); return TRUE;
        case GDK_KEY_V: case GDK_KEY_v:
            hb_terminal_paste_clipboard(t); return TRUE;
        case GDK_KEY_A: case GDK_KEY_a:
            hb_terminal_select_all(t); return TRUE;
        default: break;
        }
    }
    if (shift && !ctrl) {
        switch (keyval) {
        case GDK_KEY_Insert:
            paste_primary(t); return TRUE;
        case GDK_KEY_Page_Up:
            scroll_lines(t, t->rows > 0 ? t->rows : 10); return TRUE;
        case GDK_KEY_Page_Down:
            scroll_lines(t, -(t->rows > 0 ? t->rows : 10)); return TRUE;
        case GDK_KEY_Home:
            scroll_lines(t, 1 << 20); return TRUE;
        case GDK_KEY_End:
            scroll_lines(t, -(1 << 20)); return TRUE;
        default: break;
        }
    }

    if (t->ext_vt ? !t->ext_writable
                  : (!t->sess || !hb_session_alive(t->sess))) {
        /* 断线/只读时：可打印字符上报 dead-input，由页层缓冲/提示 */
        guint32 uc = gdk_keyval_to_unicode(keyval);
        if (uc && uc >= 0x20 && uc != 0x7f && !ctrl &&
            !(state & GDK_ALT_MASK)) {
            char b[8];
            int n = g_unichar_to_utf8(uc, b);
            char *s = g_strndup(b, n);
            g_signal_emit(t, sigs[SIG_DEAD_INPUT], 0, s);
            g_free(s);
            return TRUE;
        }
        return FALSE;
    }

    /* IME 先过滤（组字、输入法切换等） */
    ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(kc));
    if (ev && gtk_im_context_filter_keypress(t->im, ev))
        return TRUE;

    k = map_keyval(keyval);
    if (k != HB_KEY_NONE) {
        int mods = ev_mods(state);
        hb_terminal_mirror_event me;
        if (keyval == GDK_KEY_ISO_Left_Tab)
            mods |= HB_MOD_SHIFT;
        send_input_scroll_bottom(t);
        if (t->ext_vt)
            hb_vt_key(t->ext_vt, k, mods);
        else
            hb_session_key(t->sess, k, mods);
        me.kind = HB_MIRROR_KEY; me.key = k; me.mods = mods;
        me.unichar = 0; me.text = NULL; me.len = 0;
        mirror_emit(t, &me);
        return TRUE;
    }
    /* Ctrl/Alt + 可打印字符（Ctrl+C、Alt+f…） */
    if (ctrl || (state & GDK_ALT_MASK)) {
        guint32 uc = gdk_keyval_to_unicode(keyval);
        if (uc) {
            int mods = ev_mods(state);
            hb_terminal_mirror_event me = { HB_MIRROR_UNICHAR, 0,
                                            mods, uc, NULL, 0 };
            send_input_scroll_bottom(t);
            if (t->ext_vt)
                hb_vt_unichar(t->ext_vt, uc, mods);
            else
                hb_session_unichar(t->sess, uc, mods);
            mirror_emit(t, &me);
            return TRUE;
        }
        return FALSE;
    }
    /* 普通可打印：直接走 unichar（IME commit 已处理复杂输入） */
    {
        guint32 uc = gdk_keyval_to_unicode(keyval);
        if (uc && uc >= 0x20 && uc != 0x7f) {
            int mods = ev_mods(state);
            hb_terminal_mirror_event me = { HB_MIRROR_UNICHAR, 0,
                                            mods, uc, NULL, 0 };
            send_input_scroll_bottom(t);
            if (t->ext_vt)
                hb_vt_unichar(t->ext_vt, uc, mods);
            else
                hb_session_unichar(t->sess, uc, mods);
            mirror_emit(t, &me);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_im_commit(GtkIMContext *im, const gchar *str, gpointer ud)
{
    HbTerminal *t = ud;
    (void)im;
    if (t->ext_vt && t->ext_writable) {
        const char *p = str;
        send_input_scroll_bottom(t);
        while (*p) {
            gunichar uc = g_utf8_get_char(p);
            hb_vt_unichar(t->ext_vt, uc, 0);
            p = g_utf8_next_char(p);
        }
    } else if (t->ext_vt) {
        g_signal_emit(t, sigs[SIG_DEAD_INPUT], 0, str);
    } else if (t->sess && hb_session_alive(t->sess)) {
        hb_terminal_mirror_event me = { HB_MIRROR_RAW, 0, 0, 0,
                                        str, strlen(str) };
        send_input_scroll_bottom(t);
        hb_session_write(t->sess, str, strlen(str));
        mirror_emit(t, &me);
    } else {
        g_signal_emit(t, sigs[SIG_DEAD_INPUT], 0, str);
    }
}

static void on_im_preedit(GtkIMContext *im, gpointer ud)
{
    HbTerminal *t = ud;
    gchar *str = NULL;
    PangoAttrList *attrs = NULL;
    gint pos = 0;
    gtk_im_context_get_preedit_string(im, &str, &attrs, &pos);
    g_free(t->preedit);
    t->preedit = (str && *str) ? str : NULL;
    if (str && !*str)
        g_free(str);
    if (attrs) pango_attr_list_unref(attrs);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static gboolean blink_cb(gpointer ud)
{
    HbTerminal *t = ud;
    hb_vt *v;
    int r, c; bool vis, blink; int shape;
    v = term_vt(t);
    if (!v) return G_SOURCE_REMOVE;
    hb_vt_cursor(v, &r, &c, &vis, &blink, &shape);
    if (!blink || !t->cursor_blink_set)
        return G_SOURCE_CONTINUE;
    t->blink_vis = !t->blink_vis;
    gtk_widget_queue_draw(GTK_WIDGET(t));
    return G_SOURCE_CONTINUE;
}

static void on_focus_enter(GtkEventControllerFocus *f, gpointer ud)
{
    HbTerminal *t = ud;
    (void)f;
    t->focused = TRUE;
    gtk_im_context_focus_in(t->im);
    if (t->ext_vt && t->ext_writable)
        hb_vt_focus(t->ext_vt, TRUE);
    else if (t->sess)
        hb_vt_focus(hb_session_vt(t->sess), TRUE);
    t->blink_vis = TRUE;
    if (!t->blink_src)
        t->blink_src = g_timeout_add(BLINK_MS, blink_cb, t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_focus_leave(GtkEventControllerFocus *f, gpointer ud)
{
    HbTerminal *t = ud;
    (void)f;
    t->focused = FALSE;
    gtk_im_context_focus_out(t->im);
    if (t->ext_vt && t->ext_writable)
        hb_vt_focus(t->ext_vt, FALSE);
    else if (t->sess)
        hb_vt_focus(hb_session_vt(t->sess), FALSE);
    if (t->blink_src) {
        g_source_remove(t->blink_src);
        t->blink_src = 0;
    }
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* ------------------------------------------------------------------ */
/* 鼠标                                                                */
/* ------------------------------------------------------------------ */

static gboolean is_word_char(uint32_t c)
{
    if (c >= 0x80) return TRUE;        /* CJK 等非 ASCII 计入词 */
    if (g_unichar_isalnum(c)) return TRUE;
    return c == '-' || c == '_' || c == '.' || c == '/' || c == '~' ||
           c == ':' || c == '@' || c == '%' || c == '+';
}

static char *row_text(hb_vt *v, int row, int *ncols)
{
    /* malloc'd UTF-8 整行 + 每列码点起点表不需要：逐列取码点拼 */
    int cols;
    GString *s;
    int i;
    hb_vt_size(v, NULL, &cols);
    s = g_string_new(NULL);
    for (i = 0; i < cols; i++) {
        hb_cell c;
        char tmp[8];
        int n;
        if (!hb_vt_get_cell(v, row, i, &c) || c.width == 0 || !c.chars[0])
            continue;
        n = g_unichar_to_utf8(c.chars[0], tmp);
        g_string_append_len(s, tmp, n);
    }
    if (ncols) *ncols = cols;
    return g_string_free(s, FALSE);
}

static int col_from_byte(hb_vt *v, int row, int byte_off)
{
    /* 把 row_text 的字节偏移映射回列号 */
    int cols, i, off = 0;
    hb_vt_size(v, NULL, &cols);
    for (i = 0; i < cols; i++) {
        hb_cell c;
        char tmp[8];
        if (off >= byte_off)
            return i;
        if (hb_vt_get_cell(v, row, i, &c) && c.width != 0 && c.chars[0])
            off += g_unichar_to_utf8(c.chars[0], tmp);
    }
    return cols - 1;
}

static void sel_word(HbTerminal *t, int64_t abs_r, int col)
{
    hb_vt *v = term_vt(t);
    int row = (int)(abs_r - (int64_t)hb_vt_total_pushed(v));
    int cols, lo = col, hi = col, i;
    hb_vt_size(v, NULL, &cols);
    while (lo > 0) {
        hb_cell c;
        if (!hb_vt_get_cell(v, row, lo - 1, &c) ||
            !c.chars[0] || !is_word_char(c.chars[0]))
            break;
        lo--;
    }
    for (i = col; i < cols; i++) {
        hb_cell c;
        if (!hb_vt_get_cell(v, row, i, &c) ||
            !c.chars[0] || !is_word_char(c.chars[0]))
            break;
        hi = i;
    }
    t->sel = TRUE;
    t->sel_a_r = t->sel_b_r = abs_r;
    t->sel_a_c = lo;
    t->sel_b_c = hi + 1;
}

static gboolean url_at(HbTerminal *t, int row, int col,
                       int *c0, int *c1, char **url)
{
    hb_vt *v = term_vt(t);
    char *line;
    GMatchInfo *mi;
    int byte_col;
    gboolean found = FALSE;

    line = row_text(v, row, NULL);
    /* col → byte offset：逐列数 UTF-8 长度 */
    {
        int cols, i;
        byte_col = 0;
        hb_vt_size(v, NULL, &cols);
        for (i = 0; i < col && i < cols; i++) {
            hb_cell c;
            char tmp[8];
            if (hb_vt_get_cell(v, row, i, &c) && c.width != 0 && c.chars[0])
                byte_col += g_unichar_to_utf8(c.chars[0], tmp);
        }
    }
    if (g_regex_match(t->url_re, line, 0, &mi)) {
        do {
            int s0, e0;
            g_match_info_fetch_pos(mi, 0, &s0, &e0);
            if (byte_col >= s0 && byte_col < e0) {
                *c0 = col_from_byte(v, row, s0);
                *c1 = col_from_byte(v, row, e0);
                if (url) *url = g_match_info_fetch(mi, 0);
                found = TRUE;
                break;
            }
        } while (g_match_info_next(mi, NULL));
    }
    g_match_info_free(mi);
    g_free(line);
    return found;
}

static void on_drag_begin(GtkGestureDrag *g, double sx, double sy, gpointer ud)
{
    HbTerminal *t = ud;
    hb_vt *v;
    v = term_vt(t);
    if (!v) return;
    if (hb_vt_mouse_mode(v) > 0 && !ext_dead(t) &&
        !(gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(g)) & GDK_SHIFT_MASK)) {
        {
            int64_t r; int c, d;
            cell_from_xy(t, sx, sy, &r, &c, &d);
            hb_vt_mouse(v, d, c, 1, TRUE,
                ev_mods(gtk_event_controller_get_current_event_state(
                    GTK_EVENT_CONTROLLER(g))));
        }
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    {
        int64_t r; int c;
        cell_from_xy(t, sx, sy, &r, &c, NULL);
        t->anchor_r = r; t->anchor_c = c;
        t->sel = FALSE;
        t->dragging = TRUE;
    }
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    HbTerminal *t = ud;
    double sx, sy;
    int64_t r; int c;
    if (!t->dragging) return;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    cell_from_xy(t, sx + dx, sy + dy, &r, &c, NULL);
    t->sel = TRUE;
    t->sel_a_r = t->anchor_r; t->sel_a_c = t->anchor_c;
    t->sel_b_r = r; t->sel_b_c = c;
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    HbTerminal *t = ud;
    char *txt;
    (void)g; (void)dx; (void)dy;
    t->dragging = FALSE;
    txt = sel_text(t);
    if (txt) {
        set_primary_text(t, txt);
        g_free(txt);
    }
}

static void on_click_pressed(GtkGestureClick *g, int n_press,
                             double x, double y, gpointer ud)
{
    HbTerminal *t = ud;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    GdkModifierType st =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    hb_vt *v;
    int64_t r; int c, d;

    v = term_vt(t);
    if (!v) return;

    if (btn == GDK_BUTTON_SECONDARY)
        return;                 /* 右键菜单在 popover 控制器里处理 */

    if (hb_vt_mouse_mode(v) > 0 && !ext_dead(t) &&
        !(st & GDK_SHIFT_MASK) &&
        (btn == GDK_BUTTON_PRIMARY || btn == GDK_BUTTON_MIDDLE)) {
        cell_from_xy(t, x, y, &r, &c, &d);
        hb_vt_mouse(v, d, c, (int)btn, TRUE, ev_mods(st));
        return;
    }
    if (btn == GDK_BUTTON_MIDDLE) {
        paste_primary(t);
        return;
    }
    if (btn != GDK_BUTTON_PRIMARY)
        return;

    if (n_press == 1 && (st & GDK_CONTROL_MASK)) {
        cell_from_xy(t, x, y, &r, &c, &d);
        {
            int vtrow = (int)(r - (int64_t)hb_vt_total_pushed(v));
            char *url = NULL;
            int c0, c1;
            if (url_at(t, vtrow, c, &c0, &c1, &url)) {
                GtkUriLauncher *l = gtk_uri_launcher_new(url);
                gtk_uri_launcher_launch(l,
                    GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(t))),
                    NULL, NULL, NULL);
                g_object_unref(l);
                g_free(url);
            }
        }
        return;
    }
    if (n_press == 1 && (st & GDK_SHIFT_MASK)) {
        int64_t rr; int cc;
        cell_from_xy(t, x, y, &rr, &cc, NULL);
        t->sel = TRUE;
        t->sel_a_r = t->anchor_r; t->sel_a_c = t->anchor_c;
        t->sel_b_r = rr; t->sel_b_c = cc;
        gtk_widget_queue_draw(GTK_WIDGET(t));
        return;
    }
    cell_from_xy(t, x, y, &r, &c, &d);
    if (n_press == 2) {
        sel_word(t, r, c);
        set_primary_text(t, sel_text(t));
        gtk_widget_queue_draw(GTK_WIDGET(t));
    } else if (n_press == 3) {
        int cols;
        hb_vt_size(v, NULL, &cols);
        t->sel = TRUE;
        t->sel_a_r = t->sel_b_r = r;
        t->sel_a_c = 0;
        t->sel_b_c = cols;
        set_primary_text(t, sel_text(t));
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

static void on_click_released(GtkGestureClick *g, int n_press,
                              double x, double y, gpointer ud)
{
    HbTerminal *t = ud;
    guint btn;
    GdkModifierType st;
    hb_vt *v;
    int64_t r; int c, d;
    (void)n_press;
    v = term_vt(t);
    if (!v || ext_dead(t)) return;
    if (hb_vt_mouse_mode(v) <= 0) return;
    btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (st & GDK_SHIFT_MASK) return;
    cell_from_xy(t, x, y, &r, &c, &d);
    hb_vt_mouse(v, d, c, (int)btn, FALSE, ev_mods(st));
}

static void on_motion(GtkEventControllerMotion *m, double x, double y,
                      gpointer ud)
{
    HbTerminal *t = ud;
    GdkModifierType st;
    gboolean hover = FALSE;
    (void)m;
    if (!term_vt(t)) return;
    st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(m));
    if ((st & GDK_CONTROL_MASK) && term_vt(t)) {
        hb_vt *v = term_vt(t);
        int64_t r; int c, d;
        cell_from_xy(t, x, y, &r, &c, &d);
        hover = url_at(t, (int)(r - (int64_t)hb_vt_total_pushed(v)),
                       c, &t->url_c0, &t->url_c1, NULL);
        if (hover)
            t->url_r = r;
    }
    /* vt 移动上报（mode 2/3） */
    if (!ext_dead(t) && hb_vt_mouse_mode(term_vt(t)) >= 2 &&
        !(st & GDK_SHIFT_MASK)) {
        int64_t r; int c, d;
        cell_from_xy(t, x, y, &r, &c, &d);
        hb_vt_mouse(term_vt(t), d, c, 0,
                    t->dragging, ev_mods(st));
    }
    if (hover != t->url_hover) {
        t->url_hover = hover;
        gtk_widget_set_cursor_from_name(GTK_WIDGET(t),
            hover ? "pointer" : "text");
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

static gboolean on_scroll(GtkEventControllerScroll *sc, double dx, double dy,
                          gpointer ud)
{
    HbTerminal *t = ud;
    hb_vt *v;
    (void)sc; (void)dx;
    v = term_vt(t);
    if (!v) return FALSE;
    if (hb_vt_mouse_mode(v) > 0 && !ext_dead(t))
        return TRUE;            /* 应用自己处理滚轮（mouse 模式下不滚） */
    if (hb_vt_altscreen(v)) {
        /* alt screen 无滚轮回滚：转成 3 次方向键 */
        hb_terminal_mirror_event me = { HB_MIRROR_KEY,
            dy < 0 ? HB_KEY_UP : HB_KEY_DOWN, 0, 0, NULL, 0 };
        int i;
        for (i = 0; i < WHEEL_LINES; i++) {
            if (t->ext_vt)
                hb_vt_key(t->ext_vt, me.key, 0);
            else
                hb_session_key(t->sess, me.key, 0);
            mirror_emit(t, &me);
        }
        return TRUE;
    }
    scroll_lines(t, -dy * WHEEL_LINES);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 右键菜单                                                            */
/* ------------------------------------------------------------------ */

static void act_copy(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; hb_terminal_copy_clipboard(ud); }
static void act_paste(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; hb_terminal_paste_clipboard(ud); }
static void act_selall(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; hb_terminal_select_all(ud); }
static void act_clrsb(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; hb_terminal_clear_scrollback(ud); }

static void on_right_pressed(GtkGestureClick *g, int n_press,
                             double x, double y, gpointer ud)
{
    HbTerminal *t = ud;
    GtkWidget *pop;
    GdkRectangle r;
    (void)g; (void)n_press;
    pop = g_object_get_data(G_OBJECT(t), "ctxpop");
    r.x = (int)x; r.y = (int)y; r.width = r.height = 1;
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_popover_popup(GTK_POPOVER(pop));
}

/* ------------------------------------------------------------------ */
/* 尺寸 → resize                                                       */
/* ------------------------------------------------------------------ */

static void hb_terminal_size_allocate(GtkWidget *w, int width, int height,
                                      int baseline)
{
    HbTerminal *t = HB_TERMINAL(w);
    int cols, rows;
    (void)baseline;
    GTK_WIDGET_CLASS(hb_terminal_parent_class)->size_allocate(
        w, width, height, baseline);
    cols = (int)((width - 2 * PAD_X) / t->cw);
    rows = (int)((height - 2 * PAD_Y) / t->ch);
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols != t->cols || rows != t->rows) {
        t->cols = cols; t->rows = rows;
        if (t->sess)
            hb_session_resize(t->sess, rows, cols);
        else if (t->preview_text)
            preview_rebuild(t);
        else if (t->preview_vt)
            hb_vt_resize(t->preview_vt, rows, cols);
        sync_adj(t);
    }
    /* IM 候选窗位置 */
    if (t->sess) {
        hb_vt *v = hb_session_vt(t->sess);
        int cr, cc; bool vis, bl; int sh;
        GdkRectangle r;
        hb_vt_cursor(v, &cr, &cc, &vis, &bl, &sh);
        r.x = (int)(PAD_X + cc * t->cw);
        r.y = (int)(PAD_Y + cr * t->ch);
        r.width = (int)t->cw; r.height = (int)t->ch;
        gtk_im_context_set_cursor_location(t->im, &r);
    }
}

/* ------------------------------------------------------------------ */
/* 渲染                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    hb_cell cell;
    int64_t abs_r;
    int     col;
    gboolean selected;
    int     find;            /* 0 无 / 1 命中 / 2 当前命中 */
} DrawCell;

static void append_run(GtkSnapshot *snap, PangoContext *pctx,
                       PangoFontDescription *base, const char *text,
                       double x, double y, const GdkRGBA *fg, uint16_t attrs)
{
    PangoLayout *lay = pango_layout_new(pctx);
    PangoAttrList *al = pango_attr_list_new();
    PangoFontDescription *d = pango_font_description_copy(base);

    if (attrs & HB_ATTR_BOLD)
        pango_font_description_set_weight(d, PANGO_WEIGHT_BOLD);
    if (attrs & HB_ATTR_ITALIC)
        pango_font_description_set_style(d, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(lay, d);
    pango_font_description_free(d);

    pango_attr_list_insert(al, pango_attr_foreground_new(
        (guint16)(fg->red * 65535), (guint16)(fg->green * 65535),
        (guint16)(fg->blue * 65535)));
    if (fg->alpha < 0.999)
        pango_attr_list_insert(al,
            pango_attr_foreground_alpha_new((guint16)(fg->alpha * 65535)));
    if (attrs & HB_ATTR_UNDERLINE)
        pango_attr_list_insert(al,
            pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    if (attrs & HB_ATTR_DUNDERLINE)
        pango_attr_list_insert(al,
            pango_attr_underline_new(PANGO_UNDERLINE_DOUBLE));
    if (attrs & HB_ATTR_CURLY)
        pango_attr_list_insert(al,
            pango_attr_underline_new(PANGO_UNDERLINE_ERROR));
    if (attrs & HB_ATTR_STRIKE)
        pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
    pango_layout_set_attributes(lay, al);
    pango_attr_list_unref(al);

    pango_layout_set_text(lay, text, -1);
    gtk_snapshot_save(snap);
    {
        graphene_point_t pt = GRAPHENE_POINT_INIT((float)x, (float)y);
        gtk_snapshot_translate(snap, &pt);
    }
    gtk_snapshot_append_layout(snap, lay, &(GdkRGBA){1,1,1,1});
    gtk_snapshot_restore(snap);
    g_object_unref(lay);
}

static void cell_colors(HbTerminal *t, const DrawCell *dc,
                        GdkRGBA *fg, GdkRGBA *bg, uint16_t *attrs)
{
    uint16_t a = dc->cell.attrs;
    resolve_color(t, &dc->cell.fg, TRUE, fg);
    resolve_color(t, &dc->cell.bg, FALSE, bg);
    if (a & HB_ATTR_REVERSE) {
        GdkRGBA tmp = *fg;
        gboolean fgdef = (dc->cell.fg.type == HB_COLOR_DEFAULT_FG ||
                          dc->cell.fg.type == HB_COLOR_DEFAULT_BG);
        gboolean bgdef = (dc->cell.bg.type == HB_COLOR_DEFAULT_FG ||
                          dc->cell.bg.type == HB_COLOR_DEFAULT_BG);
        *fg = *bg; *bg = tmp;
        if (fgdef) *bg = t->theme->fg;
        if (bgdef) *fg = t->theme->bg;
    }
    if (a & HB_ATTR_DIM) {
        fg->alpha = 0.6;
        a &= (uint16_t)~HB_ATTR_DIM;
    }
    if (dc->selected)
        *bg = t->theme->selection;
    *attrs = a;
}

static void draw_cursor(HbTerminal *t, GtkSnapshot *snap,
                        const hb_cell *cell, int d_row, int col,
                        int shape, gboolean focused_win)
{
    double x = PAD_X + col * t->cw, y = PAD_Y + d_row * t->ch;
    GdkRGBA cur = t->theme->cursor;
    int vis_w = (cell && cell->width == 2) ? 2 : 1;

    if (!focused_win || shape == 2 || shape == 3) {
        /* 非聚焦空心框 / underline / bar */
        if (shape == 2 && focused_win) {
            graphene_rect_t r = GRAPHENE_RECT_INIT(
                (float)x, (float)(y + t->ch - 2), (float)(t->cw * vis_w), 2.f);
            gtk_snapshot_append_color(snap, &cur, &r);
        } else if (shape == 3 && focused_win) {
            graphene_rect_t r = GRAPHENE_RECT_INIT(
                (float)x, (float)y, 2.f, (float)t->ch);
            gtk_snapshot_append_color(snap, &cur, &r);
        } else {
            /* hollow */
            graphene_rect_t r;
            r = GRAPHENE_RECT_INIT((float)x, (float)y,
                (float)(t->cw * vis_w), 1.5f);
            gtk_snapshot_append_color(snap, &cur, &r);
            r = GRAPHENE_RECT_INIT((float)x, (float)(y + t->ch - 1.5),
                (float)(t->cw * vis_w), 1.5f);
            gtk_snapshot_append_color(snap, &cur, &r);
            r = GRAPHENE_RECT_INIT((float)x, (float)y, 1.5f, (float)t->ch);
            gtk_snapshot_append_color(snap, &cur, &r);
            r = GRAPHENE_RECT_INIT((float)(x + t->cw * vis_w - 1.5),
                (float)y, 1.5f, (float)t->ch);
            gtk_snapshot_append_color(snap, &cur, &r);
        }
        return;
    }
    /* block */
    {
        graphene_rect_t r = GRAPHENE_RECT_INIT((float)x, (float)y,
            (float)(t->cw * vis_w), (float)t->ch);
        gtk_snapshot_append_color(snap, &cur, &r);
    }
}

static void hb_terminal_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    HbTerminal *t = HB_TERMINAL(w);
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    hb_vt *v;
    DrawCell *buf;
    hb_cell cur_cell;
    int vrows, vcols, sb, crow, ccol, cshape;
    bool cvis, cblink, alt;
    uint64_t total;
    PangoContext *pctx;
    int d, i;
    double base_y;

    v = term_vt(t);
    if (!v)
        return;

    /* 锁内只拷数据 */
    hb_vt_lock(v);
    hb_vt_size(v, &vrows, &vcols);
    sb = hb_vt_scrollback_count(v);
    total = hb_vt_total_pushed(v);
    hb_vt_cursor(v, &crow, &ccol, &cvis, &cblink, &cshape);
    alt = hb_vt_altscreen(v);
    t->last_total = total;
    if (t->scroll_off > sb) t->scroll_off = sb;
    if (alt) t->scroll_off = 0;
    memset(&cur_cell, 0, sizeof cur_cell);
    if (crow >= 0 && crow < vrows)
        hb_vt_get_cell(v, crow, ccol, &cur_cell);

    buf = g_new(DrawCell, (gsize)vrows * (gsize)vcols);
    for (d = 0; d < vrows; d++) {
        int vrow = d - (int)t->scroll_off;
        for (i = 0; i < vcols; i++) {
            DrawCell *dc = &buf[d * vcols + i];
            hb_vt_get_cell(v, vrow, i, &dc->cell);
            dc->abs_r = (int64_t)total + vrow;
            dc->col = i;
            dc->selected = sel_covers(t, dc->abs_r, i);
            dc->find = 0;
        }
    }
    /* 查找高亮：只标可见区内的命中 */
    if (t->find_n) {
        int64_t top = (int64_t)total - t->scroll_off;
        int64_t bot = top + vrows - 1;
        size_t lo = 0, hi = t->find_n, mi;
        /* 二分第一个 abs >= top 的命中 */
        while (lo < hi) {
            mi = (lo + hi) / 2;
            if (t->find_ms[mi].abs < top) lo = mi + 1; else hi = mi;
        }
        for (mi = lo; mi < t->find_n && t->find_ms[mi].abs <= bot; mi++) {
            int dr = (int)(t->find_ms[mi].abs - top);
            int c0 = t->find_ms[mi].col, k;
            if (dr < 0 || dr >= vrows) continue;
            for (k = 0; k < t->find_ms[mi].len && c0 + k < vcols; k++)
                buf[dr * vcols + c0 + k].find =
                    (int)mi == t->find_cur ? 2 : 1;
        }
    }
    hb_vt_unlock(v);

    pctx = gtk_widget_get_pango_context(w);
    base_y = PAD_Y + (t->ch - t->font_h) / 2.0;

    /* 背景 */
    {
        graphene_rect_t r = GRAPHENE_RECT_INIT(
            0, 0, (float)width, (float)height);
        gtk_snapshot_append_color(snap, &t->theme->bg, &r);
    }

    for (d = 0; d < vrows; d++) {
        GString *run = NULL;
        double run_x = 0;
        GdkRGBA run_fg = {0}, run_bg = {0};
        uint16_t run_attrs = 0;
        gboolean run_bgset = FALSE;

        for (i = 0; i < vcols; i++) {
            DrawCell *dc = &buf[d * vcols + i];
            GdkRGBA fg, bg;
            uint16_t attrs;
            gboolean hasbg;

            cell_colors(t, dc, &fg, &bg, &attrs);
            if (dc->find == 2)
                fg = t->theme->bg;   /* 当前命中：glyph 反色保证可读 */
            hasbg = dc->selected || dc->find ||
                    !same_rgba(&bg, &t->theme->bg);

            /* 格背景（含选中 / 查找命中） */
            if (hasbg) {
                graphene_rect_t r = GRAPHENE_RECT_INIT(
                    (float)(PAD_X + i * t->cw),
                    (float)(PAD_Y + d * t->ch),
                    (float)(t->cw * (dc->cell.width == 2 ? 2 : 1)),
                    (float)t->ch);
                gtk_snapshot_append_color(snap,
                    dc->find == 2 ? &t->theme->find_cur :
                    dc->find == 1 ? &t->theme->find :
                    dc->selected  ? &t->theme->selection : &bg, &r);
            }
            if (dc->cell.width == 0 || (attrs & HB_ATTR_CONCEAL) ||
                !dc->cell.chars[0]) {
                if (run && run->len) {
                    append_run(snap, pctx, t->fdesc, run->str,
                        run_x, base_y + d * t->ch, &run_fg, run_attrs);
                    g_string_set_size(run, 0);
                }
                continue;
            }

            if (dc->cell.width == 2) {
                /* 宽字符：独立绘制，防止回退字体破坏网格 */
                char tmp[8 * HB_VT_MAX_CHARS];
                int k, off = 0;
                if (run && run->len) {
                    append_run(snap, pctx, t->fdesc, run->str,
                        run_x, base_y + d * t->ch, &run_fg, run_attrs);
                    g_string_set_size(run, 0);
                }
                for (k = 0; k < HB_VT_MAX_CHARS && dc->cell.chars[k]; k++)
                    off += g_unichar_to_utf8(dc->cell.chars[k], tmp + off);
                tmp[off] = 0;
                append_run(snap, pctx, t->fdesc, tmp,
                    PAD_X + i * t->cw, base_y + d * t->ch, &fg, attrs);
                i++;                    /* 跳尾随格 */
                continue;
            }

            /* 普通格：按 fg/bg/attrs 归并 run */
            if (!run)
                run = g_string_new(NULL);
            if (run->len &&
                (!same_rgba(&fg, &run_fg) || attrs != run_attrs ||
                 hasbg != run_bgset ||
                 (hasbg && !same_rgba(&bg, &run_bg)))) {
                append_run(snap, pctx, t->fdesc, run->str,
                    run_x, base_y + d * t->ch, &run_fg, run_attrs);
                g_string_set_size(run, 0);
            }
            if (!run->len) {
                run_x = PAD_X + i * t->cw;
                run_fg = fg; run_bg = bg;
                run_attrs = attrs; run_bgset = hasbg;
            }
            {
                int k;
                char tmp[8];
                for (k = 0; k < HB_VT_MAX_CHARS && dc->cell.chars[k]; k++) {
                    int n = g_unichar_to_utf8(dc->cell.chars[k], tmp);
                    g_string_append_len(run, tmp, n);
                }
            }
        }
        if (run && run->len)
            append_run(snap, pctx, t->fdesc, run->str,
                run_x, base_y + d * t->ch, &run_fg, run_attrs);
        if (run) g_string_free(run, TRUE);
    }

    /* URL hover 下划线 */
    if (t->url_hover) {
        int vr = (int)(t->url_r - (int64_t)total);
        int dr = vr + (int)t->scroll_off;
        if (dr >= 0 && dr < vrows) {
            graphene_rect_t r = GRAPHENE_RECT_INIT(
                (float)(PAD_X + t->url_c0 * t->cw),
                (float)(PAD_Y + dr * t->ch + t->font_h - 1),
                (float)((t->url_c1 - t->url_c0) * t->cw), 1.5f);
            gtk_snapshot_append_color(snap, &t->theme->fg, &r);
        }
    }

    /* preedit（输入法未提交文本）：画在光标处带下划线 */
    if (t->preedit && t->scroll_off == 0) {
        append_run(snap, pctx, t->fdesc, t->preedit,
            PAD_X + ccol * t->cw, base_y + crow * t->ch,
            &t->theme->fg, HB_ATTR_UNDERLINE);
    }

    /* 光标（回滚时隐藏；blink 关闭期不画） */
    if (cvis && t->scroll_off == 0 &&
        (!cblink || !t->cursor_blink_set || t->blink_vis)) {
        draw_cursor(t, snap, &cur_cell, crow, ccol,
            cshape ? cshape : 1, t->focused);
        if (t->focused && (cshape == 0 || cshape == 1) &&
            cur_cell.chars[0] && cur_cell.width == 1 &&
            !(cur_cell.attrs & HB_ATTR_CONCEAL)) {
            char tmp[8 * HB_VT_MAX_CHARS];
            int k, off = 0;
            for (k = 0; k < HB_VT_MAX_CHARS && cur_cell.chars[k]; k++)
                off += g_unichar_to_utf8(cur_cell.chars[k], tmp + off);
            tmp[off] = 0;
            append_run(snap, pctx, t->fdesc, tmp,
                PAD_X + ccol * t->cw, base_y + crow * t->ch,
                &t->theme->bg, cur_cell.attrs);
        }
    }

    /* bell flash：flash_src 存活的 100ms 内蒙一层亮色 */
    if (t->flash_src) {
        GdkRGBA fl = { 1, 1, 1, 0.06 };
        graphene_rect_t r = GRAPHENE_RECT_INIT(
            0, 0, (float)width, (float)height);
        gtk_snapshot_append_color(snap, &fl, &r);
    }

    g_free(buf);
}

/* ------------------------------------------------------------------ */
/* bell flash 定时器                                                    */
/* ------------------------------------------------------------------ */

static gboolean flash_cb(gpointer ud)
{
    HbTerminal *t = ud;
    t->flash_src = 0;
    gtk_widget_queue_draw(GTK_WIDGET(t));
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

static void hb_terminal_dispose(GObject *o)
{
    HbTerminal *t = HB_TERMINAL(o);
    GtkWidget *pop;
    if (t->ev_source) {
        g_source_remove(t->ev_source);
        t->ev_source = 0;
    }
    if (t->blink_src) {
        g_source_remove(t->blink_src);
        t->blink_src = 0;
    }
    if (t->flash_src) {
        g_source_remove(t->flash_src);
        t->flash_src = 0;
    }
    if (t->find_timer) {
        g_source_remove(t->find_timer);
        t->find_timer = 0;
    }
    pop = g_object_get_data(G_OBJECT(t), "ctxpop");
    if (pop)
        gtk_widget_unparent(pop);
    g_clear_object(&t->im);
    g_clear_object(&t->adj);
    g_clear_pointer(&t->preview_vt, hb_vt_free);
    G_OBJECT_CLASS(hb_terminal_parent_class)->dispose(o);
}

static void hb_terminal_finalize(GObject *o)
{
    HbTerminal *t = HB_TERMINAL(o);
    g_free(t->font_family);
    g_free(t->preedit);
    g_free(t->find_needle);
    g_free(t->preview_text);
    g_free(t->find_ms);
    if (t->fdesc) pango_font_description_free(t->fdesc);
    if (t->url_re) g_regex_unref(t->url_re);
    G_OBJECT_CLASS(hb_terminal_parent_class)->finalize(o);
}

static void hb_terminal_class_init(HbTerminalClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    oc->dispose = hb_terminal_dispose;
    oc->finalize = hb_terminal_finalize;
    wc->size_allocate = hb_terminal_size_allocate;
    wc->snapshot = hb_terminal_snapshot;

    sigs[SIG_TITLE] = g_signal_new("title-changed", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    sigs[SIG_EXITED] = g_signal_new("exited", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
    sigs[SIG_BELL] = g_signal_new("bell", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    sigs[SIG_OSC133] = g_signal_new("osc133", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_CHAR, G_TYPE_INT);
    sigs[SIG_NOTIFY] = g_signal_new("notify-event", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
    sigs[SIG_DEAD_INPUT] = g_signal_new("dead-input", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    sigs[SIG_FIND_CHANGED] = g_signal_new("find-changed", HB_TYPE_TERMINAL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

void hb_terminal_set_event_cb(HbTerminal *t, hb_terminal_event_cb cb,
                              gpointer ud)
{
    t->ev_cb = cb;
    t->ev_ud = ud;
}

static void hb_terminal_init(HbTerminal *t)
{
    GtkEventController *c;
    GtkGesture *g;
    GtkWidget *pop, *menu;
    GMenu *mm;
    GSimpleActionGroup *ag;

    gtk_widget_set_focusable(GTK_WIDGET(t), TRUE);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(t), "text");

    t->theme = hb_term_theme_get(NULL);
    t->fdesc = pango_font_description_from_string("Monospace 13");
    t->font_family = g_strdup("Monospace");
    t->font_px = 13;
    t->line_h = 1.5;
    t->cursor_blink_set = TRUE;
    t->blink_vis = TRUE;
    update_metrics(t);

    t->adj = gtk_adjustment_new(0, 0, 0, 1, 10, 10);
    g_signal_connect(t->adj, "value-changed",
                     G_CALLBACK(on_adj_changed), t);

    t->im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(t->im, GTK_WIDGET(t));
    g_signal_connect(t->im, "commit", G_CALLBACK(on_im_commit), t);
    g_signal_connect(t->im, "preedit-changed",
                     G_CALLBACK(on_im_preedit), t);

    t->url_re = g_regex_new("(https?|ftp|file)://[^\\s<>\"']+",
        G_REGEX_OPTIMIZE, 0, NULL);

    /* 键盘 */
    c = gtk_event_controller_key_new();
    g_signal_connect(c, "key-pressed", G_CALLBACK(on_key_pressed), t);
    gtk_widget_add_controller(GTK_WIDGET(t), c);

    /* 焦点 */
    c = gtk_event_controller_focus_new();
    g_signal_connect(c, "enter", G_CALLBACK(on_focus_enter), t);
    g_signal_connect(c, "leave", G_CALLBACK(on_focus_leave), t);
    gtk_widget_add_controller(GTK_WIDGET(t), c);

    /* 左键：单击/双击/三击 */
    g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
    g_signal_connect(g, "pressed", G_CALLBACK(on_click_pressed), t);
    g_signal_connect(g, "released", G_CALLBACK(on_click_released), t);
    gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(g));

    /* 拖动选择 */
    g = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
    g_signal_connect(g, "drag-begin", G_CALLBACK(on_drag_begin), t);
    g_signal_connect(g, "drag-update", G_CALLBACK(on_drag_update), t);
    g_signal_connect(g, "drag-end", G_CALLBACK(on_drag_end), t);
    gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(g));

    /* 移动（URL hover / vt move 上报） */
    c = gtk_event_controller_motion_new();
    g_signal_connect(c, "motion", G_CALLBACK(on_motion), t);
    gtk_widget_add_controller(GTK_WIDGET(t), c);

    /* 滚轮 */
    c = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
        GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(c, "scroll", G_CALLBACK(on_scroll), t);
    gtk_widget_add_controller(GTK_WIDGET(t), c);

    /* 右键菜单 */
    mm = g_menu_new();
    g_menu_append(mm, _("复制"), "term.copy");
    g_menu_append(mm, _("粘贴"), "term.paste");
    g_menu_append(mm, _("全选"), "term.selall");
    g_menu_append(mm, _("清除回滚"), "term.clrsb");
    menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(mm));
    g_object_unref(mm);
    pop = menu;
    gtk_widget_set_parent(pop, GTK_WIDGET(t));
    g_object_set_data(G_OBJECT(t), "ctxpop", pop);

    ag = g_simple_action_group_new();
    {
        GActionEntry entries[] = {
            { "copy", act_copy, NULL, NULL, NULL, {0} },
            { "paste", act_paste, NULL, NULL, NULL, {0} },
            { "selall", act_selall, NULL, NULL, NULL, {0} },
            { "clrsb", act_clrsb, NULL, NULL, NULL, {0} },
        };
        g_action_map_add_action_entries(G_ACTION_MAP(ag), entries, 4, t);
    }
    gtk_widget_insert_action_group(GTK_WIDGET(t), "term",
                                   G_ACTION_GROUP(ag));
    g_object_unref(ag);

    g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect(g, "pressed", G_CALLBACK(on_right_pressed), t);
    gtk_widget_add_controller(GTK_WIDGET(t), GTK_EVENT_CONTROLLER(g));
}

GtkWidget *hb_terminal_new(void)
{
    return g_object_new(HB_TYPE_TERMINAL, NULL);
}
