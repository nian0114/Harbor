#include "harbor/vt.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <vterm.h>

/* ------------------------- scrollback ring ------------------------- */

typedef struct {
    hb_cell *cells;
    int      cols;    /* cells 分配的列数（ring 满时复用缓冲） */
    bool     wrapped; /* 本行软续到下一行 */
} sb_line;

typedef enum { SCAN_TEXT, SCAN_ESC, SCAN_CSI, SCAN_PRIV } scan_state;

struct hb_vt {
    VTerm       *vt;
    VTermScreen *screen;
    VTermState  *state;
    int rows, cols;

    /* scrollback 环形缓冲：sb[sb_start] 最旧 */
    sb_line *sb;
    int      sb_cap, sb_len, sb_start;
    uint64_t total_pushed; /* 见 hb_vt_total_pushed */

    pthread_mutex_t lock; /* 递归 */

    hb_vt_output_fn out_fn;  void *out_ud;
    hb_vt_event_fn  ev_fn;   void *ev_ud;

    uint64_t gen;
    char    *title;
    bool     osc52_allowed;
    bool     alt;
    int      mouse_mode;
    int      cursor_shape;
    bool     cursor_visible, cursor_blink;
    bool     bracketed_paste;

    scan_state scan;   /* ?2004 侦测状态机 */
    long       scan_num;
    bool       scan_have;

    /* OSC 分片累积 */
    int    osc_cmd;
    char  *osc_buf;
    size_t osc_len, osc_cap;

    /* OSC52 selection 分片累积 */
    char  *sel_buf;
    size_t sel_len, sel_cap;
    char  *sel_vbuf;         /* libvterm 侧 selection 暂存区 */

    /* TITLE termprop 分片累积 */
    char  *tbuf;
    size_t tlen, tcap;
};

static void lock(hb_vt *v)   { pthread_mutex_lock(&v->lock); }
static void unlock(hb_vt *v) { pthread_mutex_unlock(&v->lock); }

void hb_vt_lock(hb_vt *v)   { lock(v); }
void hb_vt_unlock(hb_vt *v) { unlock(v); }

static void emit(hb_vt *v, hb_vt_ev_type type, char mark, int code,
                 const char *t1, const char *t2)
{
    hb_vt_event ev = { .type = type, .mark = mark, .code = code,
                       .text = t1, .text2 = t2 };
    if (v->ev_fn)
        v->ev_fn(&ev, v->ev_ud);
}

/* ------------------------- 颜色/属性转换 ------------------------- */

static hb_color conv_color(const VTermColor *c, bool is_fg)
{
    hb_color out = {0, 0, 0, 0, 0};
    if (is_fg && VTERM_COLOR_IS_DEFAULT_FG(c)) {
        out.type = HB_COLOR_DEFAULT_FG;
        return out;
    }
    if (!is_fg && VTERM_COLOR_IS_DEFAULT_BG(c)) {
        out.type = HB_COLOR_DEFAULT_BG;
        return out;
    }
    if (VTERM_COLOR_IS_INDEXED(c)) {
        out.type = HB_COLOR_INDEXED;
        out.idx = c->indexed.idx;
        return out;
    }
    out.type = HB_COLOR_RGB;
    out.r = c->rgb.red;
    out.g = c->rgb.green;
    out.b = c->rgb.blue;
    return out;
}

static uint16_t conv_attrs(const VTermScreenCellAttrs *a)
{
    uint16_t m = 0;
    if (a->bold)      m |= HB_ATTR_BOLD;
    if (a->italic)    m |= HB_ATTR_ITALIC;
    if (a->underline == VTERM_UNDERLINE_SINGLE) m |= HB_ATTR_UNDERLINE;
    if (a->underline == VTERM_UNDERLINE_DOUBLE) m |= HB_ATTR_DUNDERLINE;
    if (a->underline == VTERM_UNDERLINE_CURLY)  m |= HB_ATTR_CURLY;
    if (a->blink)     m |= HB_ATTR_BLINK;
    if (a->reverse)   m |= HB_ATTR_REVERSE;
    if (a->conceal)   m |= HB_ATTR_CONCEAL;
    if (a->strike)    m |= HB_ATTR_STRIKE;
    return m;
}

static void conv_cell(const VTermScreenCell *sc, hb_cell *hc)
{
    memset(hc, 0, sizeof *hc);
    memcpy(hc->chars, sc->chars, sizeof hc->chars);
    hc->width = (uint8_t)sc->width;
    hc->attrs = conv_attrs(&sc->attrs);
    hc->fg = conv_color(&sc->fg, true);
    hc->bg = conv_color(&sc->bg, false);
}

/* ------------------------- scrollback ------------------------- */

static void sb_push(hb_vt *v, const VTermScreenCell *cells, int cols)
{
    sb_line *l;
    const VTermLineInfo *li = NULL;
    int idx;

    if (v->sb_len == v->sb_cap) {
        /* 丢最旧行：其 cells 缓冲直接给新行复用（写 idx == 被丢槽位） */
        v->sb_start = (v->sb_start + 1) % v->sb_cap;
        v->sb_len--;
    }
    idx = (v->sb_start + v->sb_len) % v->sb_cap;
    l = &v->sb[idx];
    if (!l->cells || l->cols != cols) {
        free(l->cells);
        l->cells = malloc((size_t)cols * sizeof *l->cells);
        if (!l->cells) {
            l->cols = 0;
            return;
        }
        l->cols = cols;
    }
    for (int i = 0; i < cols; i++) {
        conv_cell(&cells[i], &l->cells[i]);
        if (i > 0 && cells[i - 1].width == 2)
            l->cells[i].width = 0;      /* 宽字符尾随格 */
    }
    /* 被推走的是旧行 0；其是否续到下一行 = 旧行 1 的 continuation 标记 */
    if (v->state)
        li = vterm_state_get_lineinfo(v->state, 1);
    l->wrapped = li ? li->continuation != 0 : false;
    v->sb_len++;
    v->total_pushed++;
}

static int sb_pop(hb_vt *v, VTermScreenCell *cells, int cols)
{
    sb_line *l;
    int idx, i;
    if (v->sb_len == 0)
        return 0;
    idx = (v->sb_start + v->sb_len - 1) % v->sb_cap;
    l = &v->sb[idx];
    for (i = 0; i < cols; i++) {
        memset(&cells[i], 0, sizeof cells[i]);
        if (i < v->cols)
            memcpy(cells[i].chars, l->cells[i].chars, sizeof cells[i].chars);
        /* 属性/颜色：近似还原文本即可（reflow 拉回的主要是字符） */
        cells[i].width = l->cells[i].width;
        cells[i].fg.type = VTERM_COLOR_DEFAULT_FG;
        cells[i].bg.type = VTERM_COLOR_DEFAULT_BG;
    }
    free(l->cells);
    l->cells = NULL;
    v->sb_len--;
    v->total_pushed--;
    return 1;
}

static void sb_clear(hb_vt *v)
{
    for (int i = 0; i < v->sb_len; i++) {
        int idx = (v->sb_start + i) % v->sb_cap;
        free(v->sb[idx].cells);
        v->sb[idx].cells = NULL;
    }
    v->sb_len = 0;
    v->sb_start = 0;
}

/* ------------------------- OSC 处理 ------------------------- */

static void osc_reset(hb_vt *v)
{
    v->osc_len = 0;
    v->osc_cmd = -1;
}

static void osc_dispatch(hb_vt *v, int cmd, const char *s, size_t len)
{
    char *str;
    switch (cmd) {
    case 133: {
        char mark = len ? s[0] : 0;
        int code = -1;
        if (mark == 'D') {
            /* D[;exit][;k=v ...] */
            const char *semi = memchr(s, ';', len);
            if (semi) {
                code = (int)strtol(semi + 1, NULL, 10);
            }
        }
        emit(v, HB_VT_EV_OSC133, mark, code, NULL, NULL);
        return;
    }
    case 9: {
        /* "9;text" → NOTIFY(text=NULL,text2=text)；忽略 ConEmu "9;4;..." 进度 */
        if (len > 2 && s[0] == '4' && s[1] == ';')
            return;
        str = strndup(s, len);
        if (str) {
            emit(v, HB_VT_EV_NOTIFY, 0, 0, NULL, str);
            free(str);
        }
        return;
    }
    case 777: {
        /* "777;notify;title;body" */
        str = strndup(s, len);
        if (str) {
            if (strncmp(str, "notify;", 7) == 0) {
                char *body = strchr(str + 7, ';');
                if (body) {
                    *body++ = 0;
                    emit(v, HB_VT_EV_NOTIFY, 0, 0, str + 7, body);
                } else {
                    emit(v, HB_VT_EV_NOTIFY, 0, 0, str + 7, NULL);
                }
            }
            free(str);
        }
        return;
    }
    case 7: {
        str = strndup(s, len);
        if (str) {
            emit(v, HB_VT_EV_CWD, 0, 0, str, NULL);
            free(str);
        }
        return;
    }
    default:
        return;
    }
}

static int fb_osc(int command, VTermStringFragment frag, void *user)
{
    hb_vt *v = user;
    if (frag.initial) {
        v->osc_cmd = command;
        v->osc_len = 0;
    }
    if (v->osc_len + frag.len + 1 > v->osc_cap) {
        size_t ncap = (v->osc_len + frag.len + 1) * 2 + 256;
        char *nb = realloc(v->osc_buf, ncap);
        if (!nb)
            return 0;
        v->osc_buf = nb;
        v->osc_cap = ncap;
    }
    if (frag.str && frag.len)
        memcpy(v->osc_buf + v->osc_len, frag.str, frag.len);
    v->osc_len += frag.len;
    v->osc_buf[v->osc_len] = 0;
    if (frag.final) {
        osc_dispatch(v, v->osc_cmd, v->osc_buf, v->osc_len);
        osc_reset(v);
    }
    return 1;
}

static const VTermStateFallbacks FB = {
    .osc = fb_osc,
};

/* OSC 52 在 libvterm 0.3.3 里走 selection 回调而非 OSC fallback：
 * "52;c;<b64>" → set(mask=CLIPBOARD, frag=已解码内容)，"52;c;?" → query()。 */
static int sel_set(VTermSelectionMask mask, VTermStringFragment frag, void *user)
{
    hb_vt *v = user;
    if (!(mask & VTERM_SELECTION_CLIPBOARD))
        return 1;
    if (frag.initial)
        v->sel_len = 0;
    if (v->sel_len + frag.len + 1 > v->sel_cap) {
        size_t ncap = (v->sel_len + frag.len + 1) * 2 + 256;
        char *nb = realloc(v->sel_buf, ncap);
        if (!nb)
            return 0;
        v->sel_buf = nb;
        v->sel_cap = ncap;
    }
    if (frag.str && frag.len)
        memcpy(v->sel_buf + v->sel_len, frag.str, frag.len);
    v->sel_len += frag.len;
    v->sel_buf[v->sel_len] = 0;
    if (frag.final) {
        if (v->osc52_allowed && v->sel_len)
            emit(v, HB_VT_EV_CLIPBOARD, 0, 0, v->sel_buf, NULL);
        v->sel_len = 0;
    }
    return 1;
}

static int sel_query(VTermSelectionMask mask, void *user)
{
    (void)mask;
    (void)user;
    return 1;   /* 不回显 query */
}

static const VTermSelectionCallbacks SEL = {
    .set = sel_set,
    .query = sel_query,
};

/* ------------------------- screen 回调 ------------------------- */

static int cb_damage(VTermRect rect, void *user)
{
    hb_vt *v = user;
    (void)rect;
    v->gen++;
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    hb_vt *v = user;
    (void)dest; (void)src;
    v->gen++;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    hb_vt *v = user;
    (void)pos; (void)oldpos; (void)visible;
    v->gen++;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    hb_vt *v = user;
    switch (prop) {
    case VTERM_PROP_TITLE:
        if (val->string.initial)
            v->tlen = 0;
        if (v->tlen + val->string.len + 1 > v->tcap) {
            size_t ncap = (v->tlen + val->string.len + 1) * 2 + 64;
            char *nb = realloc(v->tbuf, ncap);
            if (!nb)
                return 0;
            v->tbuf = nb;
            v->tcap = ncap;
        }
        if (val->string.str && val->string.len)
            memcpy(v->tbuf + v->tlen, val->string.str, val->string.len);
        v->tlen += val->string.len;
        v->tbuf[v->tlen] = 0;
        if (val->string.final) {
            free(v->title);
            v->title = strndup(v->tbuf, v->tlen);
            emit(v, HB_VT_EV_TITLE, 0, 0, v->title, NULL);
        }
        return 1;
    case VTERM_PROP_ALTSCREEN:
        v->alt = val->boolean != 0;
        emit(v, HB_VT_EV_ALTSCREEN, 0, v->alt, NULL, NULL);
        return 1;
    case VTERM_PROP_MOUSE:
        v->mouse_mode = val->number;
        return 1;
    case VTERM_PROP_CURSORVISIBLE:
        v->cursor_visible = val->boolean != 0;
        v->gen++;
        return 1;
    case VTERM_PROP_CURSORBLINK:
        v->cursor_blink = val->boolean != 0;
        return 1;
    case VTERM_PROP_CURSORSHAPE:
        v->cursor_shape = val->number;
        return 1;
    default:
        return 1;
    }
}

static int cb_bell(void *user)
{
    emit(user, HB_VT_EV_BELL, 0, 0, NULL, NULL);
    return 1;
}

static int cb_resize(int rows, int cols, void *user)
{
    hb_vt *v = user;
    v->rows = rows;
    v->cols = cols;
    v->gen++;
    return 1;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    hb_vt *v = user;
    sb_push(v, cells, cols);
    v->gen++;
    return 1;
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    hb_vt *v = user;
    int ok = sb_pop(v, cells, cols);
    v->gen++;
    return ok;
}

static int cb_sb_clear(void *user)
{
    hb_vt *v = user;
    sb_clear(v);
    v->gen++;
    return 1;
}

static const VTermScreenCallbacks SCR = {
    .damage      = cb_damage,
    .moverect    = cb_moverect,
    .movecursor  = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell        = cb_bell,
    .resize      = cb_resize,
    .sb_pushline = cb_sb_pushline,
    .sb_popline  = cb_sb_popline,
    .sb_clear    = cb_sb_clear,
};

static void out_cb(const char *s, size_t len, void *user)
{
    hb_vt *v = user;
    if (v->out_fn)
        v->out_fn(s, len, v->out_ud);
}

/* ------------------------- ?2004 扫描 ------------------------- */

/* 简单流式 CSI 扫描：识别 ESC[? 数字(;数字)* [hl]，只关心 2004。 */
static void scan_byte(hb_vt *v, unsigned char ch)
{
    switch (v->scan) {
    case SCAN_TEXT:
        if (ch == 0x1b)
            v->scan = SCAN_ESC;
        else if (ch == 0x9b)
            v->scan = SCAN_CSI;
        return;
    case SCAN_ESC:
        v->scan = (ch == '[') ? SCAN_CSI : SCAN_TEXT;
        return;
    case SCAN_CSI:
        if (ch == '?') {
            v->scan = SCAN_PRIV;
            v->scan_num = 0;
            v->scan_have = false;
        } else {
            v->scan = SCAN_TEXT;
        }
        return;
    case SCAN_PRIV:
        if (ch >= '0' && ch <= '9') {
            v->scan_num = v->scan_num * 10 + (ch - '0');
            v->scan_have = true;
            if (v->scan_num > 1000000)
                v->scan = SCAN_TEXT;
            return;
        }
        if (ch == ';') {
            v->scan_num = 0;
            v->scan_have = false;
            return;
        }
        if (ch == 'h' || ch == 'l') {
            if (v->scan_have && v->scan_num == 2004)
                v->bracketed_paste = (ch == 'h');
            v->scan = SCAN_TEXT;
            return;
        }
        v->scan = SCAN_TEXT;
        return;
    }
}

/* ------------------------- 构造/析构 ------------------------- */

hb_vt *hb_vt_new(int rows, int cols, int scrollback_lines)
{
    hb_vt *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    if (hb_core_init() != 0) {
        free(v);
        return NULL;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&v->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    v->rows = rows;
    v->cols = cols;
    v->sb_cap = scrollback_lines > 0 ? scrollback_lines : 1;
    v->sb = calloc((size_t)v->sb_cap, sizeof *v->sb);
    v->osc_cmd = -1;
    v->mouse_mode = 0;
    v->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
    v->cursor_visible = true;

    v->vt = vterm_new(rows, cols);
    if (!v->vt) {
        free(v->sb);
        pthread_mutex_destroy(&v->lock);
        free(v);
        return NULL;
    }
    vterm_set_utf8(v->vt, 1);
    vterm_output_set_callback(v->vt, out_cb, v);

    v->screen = vterm_obtain_screen(v->vt);
    vterm_screen_set_callbacks(v->screen, &SCR, v);
    vterm_screen_set_unrecognised_fallbacks(v->screen, &FB, v);
    vterm_screen_enable_altscreen(v->screen, 1);
    vterm_screen_enable_reflow(v->screen, true);
    vterm_screen_set_damage_merge(v->screen, VTERM_DAMAGE_SCROLL);

    v->state = vterm_obtain_state(v->vt);
    v->sel_vbuf = malloc(16 * 1024);
    vterm_state_set_selection_callbacks(v->state, &SEL, v,
                                        v->sel_vbuf,
                                        v->sel_vbuf ? 16 * 1024 : 0);
    vterm_screen_reset(v->screen, 1);
    return v;
}

void hb_vt_free(hb_vt *v)
{
    if (!v)
        return;
    sb_clear(v);
    free(v->sb);
    free(v->osc_buf);
    free(v->sel_buf);
    free(v->sel_vbuf);
    free(v->tbuf);
    free(v->title);
    if (v->vt)
        vterm_free(v->vt);
    pthread_mutex_destroy(&v->lock);
    free(v);
}

void hb_vt_set_output_fn(hb_vt *v, hb_vt_output_fn fn, void *ud)
{
    lock(v);
    v->out_fn = fn;
    v->out_ud = ud;
    unlock(v);
}

void hb_vt_set_event_fn(hb_vt *v, hb_vt_event_fn fn, void *ud)
{
    lock(v);
    v->ev_fn = fn;
    v->ev_ud = ud;
    unlock(v);
}

void hb_vt_set_osc52_allowed(hb_vt *v, bool allowed)
{
    lock(v);
    v->osc52_allowed = allowed;
    unlock(v);
}

/* ------------------------- feed/resize ------------------------- */

void hb_vt_feed(hb_vt *v, const char *bytes, size_t len)
{
    size_t i;
    lock(v);
    for (i = 0; i < len; i++)
        scan_byte(v, (unsigned char)bytes[i]);
    vterm_input_write(v->vt, bytes, len);
    unlock(v);
}

void hb_vt_resize(hb_vt *v, int rows, int cols)
{
    lock(v);
    vterm_set_size(v->vt, rows, cols);
    v->rows = rows;
    v->cols = cols;
    v->gen++;
    unlock(v);
}

void hb_vt_size(hb_vt *v, int *rows, int *cols)
{
    lock(v);
    if (rows) *rows = v->rows;
    if (cols) *cols = v->cols;
    unlock(v);
}

/* ------------------------- 读取 ------------------------- */

int hb_vt_scrollback_count(hb_vt *v)
{
    int n;
    lock(v);
    n = v->sb_len;
    unlock(v);
    return n;
}

uint64_t hb_vt_total_pushed(hb_vt *v)
{
    uint64_t n;
    lock(v);
    n = v->total_pushed;
    unlock(v);
    return n;
}

bool hb_vt_get_cell(hb_vt *v, int row, int col, hb_cell *out)
{
    bool ok = false;
    lock(v);
    if (col < 0 || col >= v->cols)
        goto out;
    if (row >= 0 && row < v->rows) {
        VTermScreenCell sc;
        VTermPos pos = { .row = row, .col = col };
        if (vterm_screen_get_cell(v->screen, pos, &sc)) {
            conv_cell(&sc, out);
            /* 宽字符尾随格：前一格 width==2 时本格报 width=0 */
            if (col > 0) {
                VTermScreenCell prev;
                VTermPos ppos = { .row = row, .col = col - 1 };
                if (vterm_screen_get_cell(v->screen, ppos, &prev) &&
                    prev.width == 2)
                    out->width = 0;
            }
            ok = true;
        }
    } else if (row < 0 && row >= -v->sb_len) {
        int idx = (v->sb_start + v->sb_len + row) % v->sb_cap;
        sb_line *l = &v->sb[idx];
        if (l->cells) {
            *out = l->cells[col];
            ok = true;
        }
    }
out:
    unlock(v);
    return ok;
}

bool hb_vt_line_wrapped(hb_vt *v, int row)
{
    bool w = false;
    lock(v);
    if (row >= 0) {
        const VTermLineInfo *li = NULL;
        if (row + 1 < v->rows)
            li = vterm_state_get_lineinfo(v->state, row + 1);
        w = li ? li->continuation != 0 : false;
    } else if (row >= -v->sb_len) {
        int idx = (v->sb_start + v->sb_len + row) % v->sb_cap;
        w = v->sb[idx].wrapped;
    }
    unlock(v);
    return w;
}

void hb_vt_cursor(hb_vt *v, int *row, int *col, bool *visible, bool *blink,
                  int *shape)
{
    VTermPos pos = {0, 0};
    lock(v);
    vterm_state_get_cursorpos(v->state, &pos);
    if (row) *row = pos.row;
    if (col) *col = pos.col;
    if (visible) *visible = v->cursor_visible;
    if (blink)   *blink = v->cursor_blink;
    if (shape)   *shape = v->cursor_shape;
    unlock(v);
}

bool hb_vt_altscreen(hb_vt *v)
{
    bool a;
    lock(v);
    a = v->alt;
    unlock(v);
    return a;
}

const char *hb_vt_title(hb_vt *v)
{
    return v ? v->title : NULL;
}

int hb_vt_mouse_mode(hb_vt *v)
{
    int m;
    lock(v);
    m = v->mouse_mode;
    unlock(v);
    return m;
}

bool hb_vt_bracketed_paste(hb_vt *v)
{
    bool b;
    lock(v);
    b = v->bracketed_paste;
    unlock(v);
    return b;
}

uint64_t hb_vt_generation(hb_vt *v)
{
    uint64_t g;
    lock(v);
    g = v->gen;
    unlock(v);
    return g;
}

void hb_vt_clear_scrollback(hb_vt *v)
{
    lock(v);
    sb_clear(v);
    v->gen++;
    unlock(v);
}

/* ------------------------- 输入 ------------------------- */

static VTermKey map_key(hb_key k)
{
    switch (k) {
    case HB_KEY_ENTER:     return VTERM_KEY_ENTER;
    case HB_KEY_TAB:       return VTERM_KEY_TAB;
    case HB_KEY_BACKSPACE: return VTERM_KEY_BACKSPACE;
    case HB_KEY_ESCAPE:    return VTERM_KEY_ESCAPE;
    case HB_KEY_UP:        return VTERM_KEY_UP;
    case HB_KEY_DOWN:      return VTERM_KEY_DOWN;
    case HB_KEY_LEFT:      return VTERM_KEY_LEFT;
    case HB_KEY_RIGHT:     return VTERM_KEY_RIGHT;
    case HB_KEY_INS:       return VTERM_KEY_INS;
    case HB_KEY_DEL:       return VTERM_KEY_DEL;
    case HB_KEY_HOME:      return VTERM_KEY_HOME;
    case HB_KEY_END:       return VTERM_KEY_END;
    case HB_KEY_PAGEUP:    return VTERM_KEY_PAGEUP;
    case HB_KEY_PAGEDOWN:  return VTERM_KEY_PAGEDOWN;
    default:
        if (k >= HB_KEY_F1 && k <= HB_KEY_F1 + 11)
            return VTERM_KEY_FUNCTION((int)(k - HB_KEY_F1) + 1);
        if (k >= HB_KEY_KP_0 && k <= HB_KEY_KP_0 + 9)
            return (VTermKey)(VTERM_KEY_KP_0 + (int)(k - HB_KEY_KP_0));
        return VTERM_KEY_NONE;
    }
}

void hb_vt_key(hb_vt *v, hb_key key, int mods)
{
    VTermKey vk = map_key(key);
    if (vk == VTERM_KEY_NONE)
        return;
    lock(v);
    vterm_keyboard_key(v->vt, vk, (VTermModifier)mods);
    unlock(v);
}

void hb_vt_unichar(hb_vt *v, uint32_t c, int mods)
{
    lock(v);
    vterm_keyboard_unichar(v->vt, c, (VTermModifier)mods);
    unlock(v);
}

void hb_vt_paste(hb_vt *v, const char *utf8)
{
    lock(v);
    if (v->bracketed_paste)
        vterm_keyboard_start_paste(v->vt);
    if (v->out_fn && utf8)
        v->out_fn(utf8, strlen(utf8), v->out_ud);
    if (v->bracketed_paste)
        vterm_keyboard_end_paste(v->vt);
    unlock(v);
}

void hb_vt_mouse(hb_vt *v, int row, int col, int button, bool pressed, int mods)
{
    lock(v);
    /* button 事件使用最近上报的坐标：先 move 再按 */
    vterm_mouse_move(v->vt, row, col, (VTermModifier)mods);
    if (button > 0)
        vterm_mouse_button(v->vt, button, pressed, (VTermModifier)mods);
    unlock(v);
}

void hb_vt_focus(hb_vt *v, bool in)
{
    lock(v);
    if (in)
        vterm_state_focus_in(v->state);
    else
        vterm_state_focus_out(v->state);
    unlock(v);
}

/* ------------------------- 文本提取 ------------------------- */

static size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static uint32_t cell_cp(const hb_cell *c)
{
    return c->chars[0] ? c->chars[0] : ' ';
}

/* 把一行 [c0,c1) 追加到 buf；hard 边界（非 wrap）先去尾部空白。返回新 len。 */
static size_t append_line(hb_vt *v, int row, int c0, int c1,
                          char *buf, size_t len, bool hard)
{
    int e = c1;
    hb_cell c;
    if (hard) {
        while (e > c0) {
            if (!hb_vt_get_cell(v, row, e - 1, &c))
                break;
            if (cell_cp(&c) != ' ')
                break;
            e--;
        }
    }
    for (int i = c0; i < e; i++) {
        uint32_t cp = ' ';
        char enc[4];
        size_t n;
        if (hb_vt_get_cell(v, row, i, &c) && c.width > 0)
            cp = cell_cp(&c);
        n = utf8_encode(cp, enc);
        memcpy(buf + len, enc, n);
        len += n;
    }
    return len;
}

char *hb_vt_get_text(hb_vt *v, int r0, int c0, int r1, int c1, bool block)
{
    char *buf;
    size_t len = 0;
    int r;

    lock(v);
    /* 上限估算：每 cell 4 字节 + 每行 1 换行 */
    {
        int lines = (r1 - r0) + 1;
        if (lines < 1)
            lines = 1;
        buf = malloc(((size_t)v->cols * 4 + 2) * (size_t)lines + 1);
    }
    if (!buf) {
        unlock(v);
        return NULL;
    }

    for (r = r0; r <= r1; r++) {
        bool wrapped;
        int sc, ec;
        if (block) {
            len = append_line(v, r, c0, c1, buf, len, true);
            if (r < r1)
                buf[len++] = '\n';
            continue;
        }
        wrapped = hb_vt_line_wrapped(v, r);
        sc = (r == r0) ? c0 : 0;
        ec = (r == r1) ? c1 : v->cols;
        len = append_line(v, r, sc, ec, buf, len, !wrapped);
        if (!wrapped && r < r1)
            buf[len++] = '\n';
    }
    buf[len] = 0;
    unlock(v);
    return buf;
}

/* ------------------------- 搜索 ------------------------- */

static uint32_t fold_cp(uint32_t cp, bool ci)
{
    if (ci && cp >= 'A' && cp <= 'Z')
        return cp + 32;
    return cp;
}

size_t hb_vt_search(hb_vt *v, const char *needle, bool case_insensitive,
                    hb_vt_match **out)
{
    uint32_t npat[256];
    size_t nlen = 0;
    hb_vt_match *res = NULL;
    size_t nres = 0, cap = 0;
    int r;

    *out = NULL;
    lock(v);
    /* 解码 needle → codepoints（只取前 256 个） */
    {
        const unsigned char *p = (const unsigned char *)needle;
        while (*p && nlen < 256) {
            uint32_t cp;
            if (*p < 0x80) cp = *p++;
            else if ((*p & 0xe0) == 0xc0 && p[1]) { cp = (*p & 0x1f) << 6 | (p[1] & 0x3f); p += 2; }
            else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) { cp = (*p & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f); p += 3; }
            else if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) { cp = (*p & 0x07) << 18 | (p[1] & 0x3f) << 12 | (p[2] & 0x3f) << 6 | (p[3] & 0x3f); p += 4; }
            else { p++; continue; }
            npat[nlen++] = fold_cp(cp, case_insensitive);
        }
    }
    if (nlen == 0) {
        unlock(v);
        return 0;
    }

    for (r = -v->sb_len; r < v->rows; r++) {
        int col = 0;
        while (col <= v->cols - (int)nlen) {
            size_t k;
            hb_cell c;
            bool match = true;
            for (k = 0; k < nlen; k++) {
                uint32_t cp = ' ';
                if (hb_vt_get_cell(v, r, col + (int)k, &c) && c.width > 0)
                    cp = cell_cp(&c);
                if (fold_cp(cp, case_insensitive) != npat[k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (nres == cap) {
                    size_t ncap = cap ? cap * 2 : 16;
                    hb_vt_match *na = realloc(res, ncap * sizeof *na);
                    if (!na) {
                        free(res);
                        unlock(v);
                        *out = NULL;
                        return 0;
                    }
                    res = na;
                    cap = ncap;
                }
                res[nres].row = r;
                res[nres].col = col;
                res[nres].len = (int)nlen;
                nres++;
                col += (int)nlen;
            } else {
                col++;
            }
        }
    }
    unlock(v);
    *out = res;
    return nres;
}
