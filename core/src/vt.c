#include "harbor/vt.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <vterm.h>

/* ------------------------------------------------------------------ */
/* 屏幕数据模型：不用 libvterm 的 VTermScreen（其整屏 memmove 是性能      */
/* 瓶颈）。改用 VTermState 回调 + 自建行指针屏：滚动只是指针旋转，          */
/* 顶部滚出行直接把行缓冲移入 scrollback ring（零 cell 拷贝）。            */
/* ------------------------------------------------------------------ */

typedef struct {
    hb_cell *cells;
    int      cap;     /* cells 容量（≥ cols） */
    int      used;    /* cells[0,used) 有效；[used,..) 视作 fill 空白 */
    hb_color fill_fg, fill_bg; /* 清空/滚动入空行的背景（BCE） */
    bool     wrapped; /* 本行是上一行的软续行（lineinfo.continuation） */
} row_t;

/* scrollback 描述符：内容在 sb_arena 里（追加式字节环，写点始终热） */
typedef struct {
    size_t   off;        /* arena 内 cell 偏移 */
    int      n;          /* 有效 cell 数（= 行的 used） */
    hb_color fg, bg;     /* [n,cols) 空白区颜色（入环时的 fill） */
    bool     wraps_next; /* 本行内容软续到其后一行（入环时记录） */
} sb_line;

typedef struct {
    uint16_t attrs;
    hb_color fg, bg;
} pen_t;

typedef enum { SCAN_TEXT, SCAN_ESC, SCAN_CSI, SCAN_PRIV } scan_state;

struct hb_vt {
    VTerm      *vt;
    VTermState *state;
    int rows, cols;

    row_t **prim;      /* 主屏 rows 个行指针 */
    row_t **alt;       /* alt 屏 */
    row_t **buf;       /* 当前活动屏 = prim 或 alt */
    row_t **tmp;       /* 滚动指针旋转暂存，容量 ≥ rows */
    int     tmp_cap;

    pen_t pen;         /* 当前 erase/putglyph 用笔（BCE） */
    int   pg_row;      /* 最近一次 putglyph 的行（wrapped 惰性同步） */

    /* scrollback 环形缓冲：sb[sb_start] 最旧；内容在 arena */
    sb_line *sb;
    int      sb_cap, sb_len, sb_start;
    hb_cell *sb_arena;   /* 追加式 cell 环；写点总在尾部（热） */
    size_t   sb_acap, sb_atail;
    uint64_t total_pushed; /* 见 hb_vt_total_pushed */

    pthread_mutex_t lock; /* 递归 */

    hb_vt_output_fn out_fn;  void *out_ud;
    hb_vt_event_fn  ev_fn;   void *ev_ud;

    uint64_t gen;
    char    *title;
    bool     osc52_allowed;
    bool     alt_active;
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

/* ------------------------- 颜色/空白格 ------------------------- */

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

/* 空白格：BCE —— 用当前 pen 的前/背景，attrs 清零 */
static hb_cell blank_cell(const hb_vt *v)
{
    hb_cell c;
    memset(&c, 0, sizeof c);
    c.width = 1;
    c.fg = v->pen.fg;
    c.bg = v->pen.bg;
    return c;
}

static row_t *row_new(int cols)
{
    row_t *r = calloc(1, sizeof *r);
    int cap = cols > 16 ? cols : 16;
    if (!r)
        return NULL;
    r->cells = calloc((size_t)cap, sizeof *r->cells);
    if (!r->cells) {
        free(r);
        return NULL;
    }
    r->cap = cap;
    r->fill_fg.type = HB_COLOR_DEFAULT_FG;
    r->fill_bg.type = HB_COLOR_DEFAULT_BG;
    return r;
}

static void row_free(row_t *r)
{
    if (!r)
        return;
    free(r->cells);
    free(r);
}

/* 保证容量 ≥ need；[cap,ncap) 无需初始化（used 之外不读） */
static void row_ensure(row_t *r, int need)
{
    int ncap;
    if (need <= r->cap)
        return;
    ncap = r->cap;
    while (ncap < need)
        ncap *= 2;
    {
        hb_cell *nc = realloc(r->cells, (size_t)ncap * sizeof *nc);
        if (!nc)
            return;
        r->cells = nc;
        r->cap = ncap;
    }
}

/* 逻辑清空：不写字格。used=0 且记录 fill 颜色（BCE）。
 * 之后 [0,used) 不变式保证读取安全：写入跨过 used 时先补 fill。 */
static void row_blank(hb_vt *v, row_t *r)
{
    r->used = 0;
    r->wrapped = false;
    r->fill_fg = v->pen.fg;
    r->fill_bg = v->pen.bg;
}

/* 写入到 col 之前若超过 used，先把 [used,col) 补成 fill 空白 */
static void row_gap(hb_vt *v, row_t *r, int upto)
{
    hb_cell e;
    int i;
    (void)v;
    if (upto <= r->used)
        return;
    if (upto > r->cap)
        upto = r->cap;
    memset(&e, 0, sizeof e);
    e.width = 1;
    e.fg = r->fill_fg;
    e.bg = r->fill_bg;
    for (i = r->used; i < upto; i++)
        r->cells[i] = e;
    r->used = upto;
}

static void fill_range(hb_vt *v, row_t *r, int c0, int c1)
{
    hb_cell e = blank_cell(v);
    int i;
    if (c0 < 0)
        c0 = 0;
    if (c1 > r->cap)
        c1 = r->cap;
    if (c0 > r->used)
        row_gap(v, r, c0);          /* [used,c0) 补成 fill 空白，保持前缀有效 */
    for (i = c0; i < c1; i++)
        r->cells[i] = e;
    if (c1 > r->used)
        r->used = c1;
}

/* ------------------------- scrollback ring ------------------------- */

/* 保证 arena 里有 ≥ n 个连续 cell；不够时回卷或逐出最旧描述符 */
static int sb_reserve(hb_vt *v, int n)
{
    size_t free;
    if (!v->sb_arena)
        return 0;
    if ((size_t)n > v->sb_acap)
        return 0;
    for (;;) {
        if (v->sb_len == 0) {
            free = v->sb_acap - v->sb_atail;
            if (n <= (int)free)
                return 1;
            v->sb_atail = 0;
            return 1;                  /* 全空：尾部死区后从头写 */
        }
        {
            size_t head = v->sb[v->sb_start].off;
            if (head > v->sb_atail)
                free = head - v->sb_atail;
            else
                free = v->sb_acap - v->sb_atail;
            if (n <= (int)free)
                return 1;
            if (head <= v->sb_atail) {   /* 尾部不够 → 回卷到 0 */
                v->sb_atail = 0;
                continue;
            }
            /* head > tail 仍不够 → 逐出最旧 */
            v->sb_start = (v->sb_start + 1) % v->sb_cap;
            v->sb_len--;
        }
    }
}

/* 把行内容 memcpy 进 arena 尾部（顺序写，热）。返回 ring 槽位或 NULL。 */
static void sb_push_row(hb_vt *v, const row_t *row, bool wraps_next)
{
    int idx;
    int n = row->used;

    if (v->sb_len == v->sb_cap) {
        v->sb_start = (v->sb_start + 1) % v->sb_cap;
        v->sb_len--;
    }
    if (!sb_reserve(v, n))
        n = 0;
    idx = (v->sb_start + v->sb_len) % v->sb_cap;
    v->sb[idx].off = v->sb_atail;
    v->sb[idx].n = n;
    v->sb[idx].fg = row->fill_fg;
    v->sb[idx].bg = row->fill_bg;
    v->sb[idx].wraps_next = wraps_next;
    if (n)
        memcpy(v->sb_arena + v->sb_atail, row->cells,
               (size_t)n * sizeof *row->cells);
    v->sb_atail += (size_t)n;
    v->sb_len++;
    v->total_pushed++;
}

/* 弹出最新一行到 *row（memcpy 回来）；total_pushed 递减。 */
static int sb_pop_row(hb_vt *v, row_t *row)
{
    int idx, n;
    sb_line *l;
    if (v->sb_len == 0)
        return 0;
    idx = (v->sb_start + v->sb_len - 1) % v->sb_cap;
    l = &v->sb[idx];
    n = l->n;
    row_ensure(row, n);
    if (n > row->cap)
        n = row->cap;
    if (n)
        memcpy(row->cells, v->sb_arena + l->off, (size_t)n * sizeof *row->cells);
    row->used = n;
    row->fill_fg = l->fg;
    row->fill_bg = l->bg;
    row->wrapped = false;
    v->sb_len--;
    v->total_pushed--;
    return 1;
}

static void sb_clear(hb_vt *v)
{
    v->sb_len = 0;
    v->sb_start = 0;
    v->sb_atail = 0;
}

static const sb_line *sb_desc_at(const hb_vt *v, int row) /* row < 0, -1 最新 */
{
    int idx = (v->sb_start + v->sb_len + row) % v->sb_cap;
    return &v->sb[idx];
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

/* ------------------------- VTermState 回调 ------------------------- */

static int cb_putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
    hb_vt *v = user;
    row_t *r;
    hb_cell *c;
    int col = pos.col, i, n;

    if (pos.row < 0 || pos.row >= v->rows || col < 0 || col >= v->cols)
        return 1;
    r = v->buf[pos.row];
    if (!r)
        return 1;
    if (col + (info->width > 1 ? info->width : 1) > r->cap) {
        row_ensure(r, col + (info->width > 1 ? info->width : 1));
        if (col >= r->cap)
            return 1;
    }

    /* 写入跨过 used → 先补 fill 空白保持 [0,used) 有效 */
    if (col > r->used)
        row_gap(v, r, col);

    /* 覆盖宽字符任一半 → 另一格变孤儿，清成空白 */
    if (col > 0 && col - 1 < r->used && r->cells[col - 1].width == 2)
        r->cells[col - 1] = blank_cell(v);
    if (col < r->used && r->cells[col].width == 2 && col + 1 < r->cap)
        r->cells[col + 1] = blank_cell(v);

    c = &r->cells[col];
    memset(c, 0, sizeof *c);
    n = 0;
    for (i = 0; info->chars[i] && n < HB_VT_MAX_CHARS; i++, n++)
        c->chars[n] = info->chars[i];
    c->width = (uint8_t)(info->width > 0 ? info->width : 1);
    c->attrs = v->pen.attrs;
    c->fg = v->pen.fg;
    c->bg = v->pen.bg;

    if (info->width == 2 && col + 1 < v->cols && col + 1 < r->cap) {
        /* 尾随格自身可能曾是宽字符头 → 其尾随格成孤儿 */
        if (col + 2 < r->cap && r->cells[col + 1].width == 2)
            r->cells[col + 2] = blank_cell(v);
        memset(&r->cells[col + 1], 0, sizeof r->cells[col + 1]);
        r->cells[col + 1].width = 0;
        r->cells[col + 1].fg = v->pen.fg;
        r->cells[col + 1].bg = v->pen.bg;
    }

    if (col + info->width > r->used)
        r->used = col + info->width;
    if (r->used > v->cols)
        r->used = v->cols;

    /* state 在 autowrap 时直接写 lineinfo.continuation（不经回调）。
     * 只有换行（wrap/linefeed 后写行变化）才可能动它，按行变化时同步一次。 */
    if (pos.row != v->pg_row) {
        const VTermLineInfo *li = vterm_state_get_lineinfo(v->state, pos.row);
        r->wrapped = li ? li->continuation != 0 : false;
        v->pg_row = pos.row;
    }
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    (void)user;
    (void)pos; (void)oldpos; (void)visible;
    return 1;
}

/* 全宽竖滚：行指针旋转；主屏 start_row==0 时顶行移入 scrollback。 */
static void scroll_full(hb_vt *v, VTermRect rect, int downward)
{
    int s = rect.start_row, e = rect.end_row;
    int h, k, i;
    bool push;

    if (e > v->rows)
        e = v->rows;
    h = e - s;
    if (h <= 0)
        return;

    if (v->tmp_cap < v->rows) {
        row_t **nt = realloc(v->tmp, (size_t)v->rows * sizeof *nt);
        if (nt) {
            v->tmp = nt;
            v->tmp_cap = v->rows;
        }
    }

    if (downward > 0) {
        k = downward < h ? downward : h;
        push = (s == 0 && v->buf == v->prim);
        for (i = 0; i < k; i++)
            v->tmp[i] = v->buf[s + i];
        memmove(&v->buf[s], &v->buf[s + k], (size_t)(h - k) * sizeof *v->buf);
        for (i = 0; i < k; i++) {
            row_t *leaving = v->tmp[i];
            if (leaving) {
                if (push) {
                    /* 被推行是否软续到下一行 = 后继行的 continuation 标记 */
                    bool wn = (i + 1 < k)
                        ? (v->tmp[i + 1] && v->tmp[i + 1]->wrapped)
                        : (k < h && v->buf[s] && v->buf[s]->wrapped);
                    sb_push_row(v, leaving, wn);
                }
                /* 直接复用离开行作新空行：cells 还是热的 */
                row_blank(v, leaving);
            }
            v->buf[e - k + i] = leaving;
        }
    } else {
        k = -downward < h ? -downward : h;
        /* 向下滚：底行丢弃并复用为新空行 */
        for (i = 0; i < k; i++)
            v->tmp[i] = v->buf[e - 1 - i];
        memmove(&v->buf[s + k], &v->buf[s], (size_t)(h - k) * sizeof *v->buf);
        for (i = 0; i < k; i++) {
            row_t *b = v->tmp[i];
            if (b) {
                if (b->cap < v->cols)
                    row_ensure(b, v->cols);
                row_blank(v, b);
            }
            v->buf[s + i] = b;
        }
    }
}

/* 部分宽竖滚 / 横滚 / moverect 的按行 cell 拷贝。 */
static void cells_move(hb_vt *v, int dst_row, int dst_c0,
                       int src_row, int src_c0, int n)
{
    row_t *dr = v->buf[dst_row];
    row_t *sr = v->buf[src_row];
    if (n <= 0 || !dr || !sr)
        return;
    row_ensure(dr, dst_c0 + n);
    row_ensure(sr, src_c0 + n);
    if (dst_c0 + n > dr->cap)
        n = dr->cap - dst_c0;
    if (src_c0 + n > sr->cap)
        n = sr->cap - src_c0;
    if (n <= 0)
        return;
    /* src 超 used 的尾部是 fill 空白，先物化再拷贝（fill 即语义值） */
    if (src_c0 + n > sr->used)
        row_gap(v, sr, src_c0 + n);
    if (dst_c0 > dr->used)
        row_gap(v, dr, dst_c0);
    memmove(&dr->cells[dst_c0], &sr->cells[src_c0],
            (size_t)n * sizeof *dr->cells);
    if (dst_c0 + n > dr->used)
        dr->used = dst_c0 + n;
}

static int cb_scrollrect(VTermRect rect, int downward, int rightward,
                         void *user)
{
    hb_vt *v = user;
    int r;

    if (rect.start_row < 0)
        rect.start_row = 0;
    if (rect.end_row > v->rows)
        rect.end_row = v->rows;
    if (rect.start_col < 0)
        rect.start_col = 0;
    if (rect.end_col > v->cols)
        rect.end_col = v->cols;

    if (rightward == 0 &&
        rect.start_col == 0 && rect.end_col >= v->cols) {
        scroll_full(v, rect, downward);
        return 1;
    }

    if (rightward != 0) {
        /* 水平：每行内 [c0,c1) 平移，空出端填空白 */
        int c0 = rect.start_col, c1 = rect.end_col;
        int w = c1 - c0;
        int k = rightward < 0 ? -rightward : rightward;
        if (k > w)
            k = w;
        for (r = rect.start_row; r < rect.end_row; r++) {
            row_t *row = v->buf[r];
            if (!row)
                continue;
            row_ensure(row, c1);
            if (c1 > row->used)
                row_gap(v, row, c1);   /* 先物化 fill 空白，再整体平移 */
            if (rightward < 0) {   /* libvterm: 负 = 内容右移（ICH） */
                memmove(&row->cells[c0 + k], &row->cells[c0],
                        (size_t)(w - k) * sizeof *row->cells);
                fill_range(v, row, c0, c0 + k);
            } else {               /* 正 = 内容左移（DCH） */
                memmove(&row->cells[c0], &row->cells[c0 + k],
                        (size_t)(w - k) * sizeof *row->cells);
                fill_range(v, row, c1 - k, c1);
            }
            /* used 上估保持正确（区域内容仍在行内） */
            if (row->used > c0 && row->used < c1)
                row->used = c1;
        }
        return 1;
    }

    /* 部分宽度竖滚：逐行拷贝 [c0,c1)，空出区填空白 */
    {
        int c0 = rect.start_col, c1 = rect.end_col;
        int n = c1 - c0;
        int s = rect.start_row, e = rect.end_row;
        int h = e - s;
        int k = downward < 0 ? -downward : downward;
        if (k > h)
            k = h;
        if (downward > 0) {
            for (r = s; r < e - k; r++)
                cells_move(v, r, c0, r + k, c0, n);
            for (r = e - k; r < e; r++) {
                if (v->buf[r]) {
                    row_ensure(v->buf[r], c1);
                    fill_range(v, v->buf[r], c0, c1);
                }
            }
        } else {
            for (r = e - 1; r >= s + k; r--)
                cells_move(v, r, c0, r - k, c0, n);
            for (r = s; r < s + k; r++) {
                if (v->buf[r]) {
                    row_ensure(v->buf[r], c1);
                    fill_range(v, v->buf[r], c0, c1);
                }
            }
        }
    }
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    hb_vt *v = user;
    int n = src.end_col - src.start_col;
    int h = src.end_row - src.start_row;
    int dr = dest.start_row - src.start_row;
    int dc = dest.start_col - src.start_col;
    int i;

    if (dr > 0) {
        for (i = h - 1; i >= 0; i--)
            cells_move(v, src.start_row + i + dr, src.start_col + dc,
                       src.start_row + i, src.start_col, n);
    } else {
        for (i = 0; i < h; i++)
            cells_move(v, src.start_row + i + dr, src.start_col + dc,
                       src.start_row + i, src.start_col, n);
    }
    return 1;
}

static int cb_erase(VTermRect rect, int selective, void *user)
{
    hb_vt *v = user;
    int r;
    (void)selective; /* 不跟踪 protected，全部擦除 */

    if (rect.start_row < 0)
        rect.start_row = 0;
    if (rect.end_row > v->rows)
        rect.end_row = v->rows;

    for (r = rect.start_row; r < rect.end_row; r++) {
        row_t *row = v->buf[r];
        if (!row)
            continue;
        row_ensure(row, rect.end_col);
        fill_range(v, row, rect.start_col, rect.end_col);
    }
    /* state 在 end_col==cols 时直接清后续行的 continuation，镜像之 */
    if (rect.end_col == v->cols)
        for (r = rect.start_row + 1; r < rect.end_row + 1 && r < v->rows; r++)
            if (v->buf[r])
                v->buf[r]->wrapped = false;
    return 1;
}

static int cb_initpen(void *user)
{
    hb_vt *v = user;
    v->pen.attrs = 0;
    v->pen.fg.type = HB_COLOR_DEFAULT_FG;
    v->pen.fg.idx = v->pen.fg.r = v->pen.fg.g = v->pen.fg.b = 0;
    v->pen.bg.type = HB_COLOR_DEFAULT_BG;
    v->pen.bg.idx = v->pen.bg.r = v->pen.bg.g = v->pen.bg.b = 0;
    return 1;
}

static int cb_setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
    hb_vt *v = user;
    switch (attr) {
    case VTERM_ATTR_BOLD:
        if (val->boolean) v->pen.attrs |= HB_ATTR_BOLD;
        else              v->pen.attrs &= (uint16_t)~HB_ATTR_BOLD;
        return 1;
    case VTERM_ATTR_UNDERLINE:
        v->pen.attrs &= (uint16_t)~(HB_ATTR_UNDERLINE | HB_ATTR_DUNDERLINE |
                                    HB_ATTR_CURLY);
        if (val->number == 1)      v->pen.attrs |= HB_ATTR_UNDERLINE;
        else if (val->number == 2) v->pen.attrs |= HB_ATTR_DUNDERLINE;
        else if (val->number >= 3) v->pen.attrs |= HB_ATTR_CURLY;
        return 1;
    case VTERM_ATTR_ITALIC:
        if (val->boolean) v->pen.attrs |= HB_ATTR_ITALIC;
        else              v->pen.attrs &= (uint16_t)~HB_ATTR_ITALIC;
        return 1;
    case VTERM_ATTR_BLINK:
        if (val->boolean) v->pen.attrs |= HB_ATTR_BLINK;
        else              v->pen.attrs &= (uint16_t)~HB_ATTR_BLINK;
        return 1;
    case VTERM_ATTR_REVERSE:
        if (val->boolean) v->pen.attrs |= HB_ATTR_REVERSE;
        else              v->pen.attrs &= (uint16_t)~HB_ATTR_REVERSE;
        return 1;
    case VTERM_ATTR_CONCEAL:
        if (val->boolean) v->pen.attrs |= HB_ATTR_CONCEAL;
        else              v->pen.attrs &= (uint16_t)~HB_ATTR_CONCEAL;
        return 1;
    case VTERM_ATTR_STRIKE:
        if (val->boolean) v->pen.attrs |= HB_ATTR_STRIKE;
        else              v->pen.attrs &= (uint16_t)~HB_ATTR_STRIKE;
        return 1;
    case VTERM_ATTR_FOREGROUND:
        v->pen.fg = conv_color(&val->color, true);
        return 1;
    case VTERM_ATTR_BACKGROUND:
        v->pen.bg = conv_color(&val->color, false);
        return 1;
    default:
        /* FONT/SMALL/BASELINE 忽略 */
        return 1;
    }
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
        v->alt_active = val->boolean != 0;
        v->buf = v->alt_active ? v->alt : v->prim;
        /* state 会在返回后对新 alt 屏发整屏 erase（清屏语义自带） */
        emit(v, HB_VT_EV_ALTSCREEN, 0, v->alt_active, NULL, NULL);
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

static int cb_setlineinfo(int row, const VTermLineInfo *newinfo,
                          const VTermLineInfo *oldinfo, void *user)
{
    hb_vt *v = user;
    (void)oldinfo;
    if (row >= 0 && row < v->rows && v->buf[row])
        v->buf[row]->wrapped = newinfo->continuation != 0;
    return 1;
}

static int cb_sb_clear(void *user)
{
    hb_vt *v = user;
    sb_clear(v);
    v->gen++;
    return 1;
}

/* 单缓冲行数变化（无 reflow，xterm 语义）。
 * can_sb：主屏缩行时顶行入 scrollback、增行时拉回；pos 仅活动屏传。 */
static void resize_rows(hb_vt *v, row_t ***pbuf, int orows, int nrows,
                        bool can_sb, VTermPos *pos)
{
    row_t **b = *pbuf;
    int i;

    if (nrows == orows)
        return;

    if (nrows < orows) {
        int drop = orows - nrows;
        for (i = 0; i < drop; i++) {
            row_t *leaving = b[i];
            bool wn = (i + 1 < orows && b[i + 1]) ? b[i + 1]->wrapped : false;
            if (leaving) {
                if (can_sb)
                    sb_push_row(v, leaving, wn);
                row_free(leaving);
            }
        }
        memmove(&b[0], &b[drop], (size_t)nrows * sizeof *b);
        {
            row_t **nb = realloc(b, (size_t)nrows * sizeof *b);
            if (nb)
                b = nb;
        }
        if (pos) {
            pos->row -= drop;
            if (pos->row < 0)
                pos->row = 0;
        }
    } else {
        int added = nrows - orows;
        int pulled = 0;
        row_t **nb;
        /* libvterm 语义：拉回 min(added, sb_len) 行填充顶部（不按光标限） */
        if (can_sb)
            while (pulled < added && v->sb_len > pulled)
                pulled++;
        nb = realloc(b, (size_t)nrows * sizeof *b);
        if (nb)
            b = nb;
        memmove(&b[pulled], &b[0], (size_t)orows * sizeof *b);
        for (i = pulled - 1; i >= 0; i--) {
            b[i] = row_new(v->cols);
            if (b[i])
                sb_pop_row(v, b[i]);   /* 最新行放最底 */
        }
        if (pos)
            pos->row += pulled;
        for (i = pulled + orows; i < nrows; i++)
            b[i] = row_new(v->cols);
    }
    *pbuf = b;
}

static int cb_resize(int rows, int cols, VTermStateFields *fields, void *user)
{
    hb_vt *v = user;
    int orows = v->rows;
    int bi;

    /* lineinfo 数组随行数变化（默认 allocator == malloc/free，realloc 兼容） */
    for (bi = 0; bi < 2; bi++) {
        VTermLineInfo *old = fields->lineinfos[bi];
        VTermLineInfo *nl;
        int r;
        if (!old)
            continue;
        nl = realloc(old, (size_t)rows * sizeof *nl);
        if (!nl)
            continue;
        for (r = orows; r < rows; r++)
            memset(&nl[r], 0, sizeof nl[r]);
        fields->lineinfos[bi] = nl;
    }

    if (cols > v->cols && v->sb_arena) {
        /* arena 保持 sb_cap × cols 的最坏容量 */
        size_t need = (size_t)v->sb_cap * (size_t)cols;
        if (need > v->sb_acap) {
            hb_cell *na = realloc(v->sb_arena, need * sizeof *na);
            if (na) {
                v->sb_arena = na;
                v->sb_acap = need;
            }
        }
    }
    v->cols = cols;

    resize_rows(v, &v->prim, orows, rows, true,
                v->alt_active ? NULL : &fields->pos);
    resize_rows(v, &v->alt, orows, rows, false,
                v->alt_active ? &fields->pos : NULL);

    v->rows = rows;
    v->buf = v->alt_active ? v->alt : v->prim;
    v->gen++;
    return 1;
}

static const VTermStateCallbacks ST = {
    .putglyph    = cb_putglyph,
    .movecursor  = cb_movecursor,
    .scrollrect  = cb_scrollrect,
    .moverect    = cb_moverect,
    .erase       = cb_erase,
    .initpen     = cb_initpen,
    .setpenattr  = cb_setpenattr,
    .settermprop = cb_settermprop,
    .bell        = cb_bell,
    .resize      = cb_resize,
    .setlineinfo = cb_setlineinfo,
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
    int i;
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
    v->pg_row = -1;

    v->prim = calloc((size_t)rows, sizeof *v->prim);
    v->alt = calloc((size_t)rows, sizeof *v->alt);
    if (!v->prim || !v->alt || !v->sb)
        goto fail;
    for (i = 0; i < rows; i++) {
        v->prim[i] = row_new(cols);
        v->alt[i] = row_new(cols);
        if (!v->prim[i] || !v->alt[i])
            goto fail;
    }
    v->buf = v->prim;

    /* scrollback 内容 arena：最坏 sb_cap × cols cells（追加式，热写）。
     * 分配失败仅降级为不存内容，不致命。 */
    v->sb_acap = (size_t)v->sb_cap * (size_t)(cols > 0 ? cols : 1);
    v->sb_arena = malloc(v->sb_acap * sizeof *v->sb_arena);
    if (!v->sb_arena)
        v->sb_acap = 0;

    v->vt = vterm_new(rows, cols);
    if (!v->vt)
        goto fail;
    vterm_set_utf8(v->vt, 1);
    vterm_output_set_callback(v->vt, out_cb, v);

    /* 只用 parser + state 层；绝不取 VTermScreen。 */
    v->state = vterm_obtain_state(v->vt);
    vterm_state_set_callbacks(v->state, &ST, v);
    vterm_state_set_unrecognised_fallbacks(v->state, &FB, v);
    v->sel_vbuf = malloc(16 * 1024);
    vterm_state_set_selection_callbacks(v->state, &SEL, v,
                                        v->sel_vbuf,
                                        v->sel_vbuf ? 16 * 1024 : 0);
    vterm_state_reset(v->state, 1);
    return v;

fail:
    if (v->prim)
        for (i = 0; i < rows; i++)
            row_free(v->prim[i]);
    if (v->alt)
        for (i = 0; i < rows; i++)
            row_free(v->alt[i]);
    free(v->prim);
    free(v->alt);
    free(v->sb);
    if (v->vt)
        vterm_free(v->vt);
    pthread_mutex_destroy(&v->lock);
    free(v);
    return NULL;
}

void hb_vt_free(hb_vt *v)
{
    int i;
    if (!v)
        return;
    sb_clear(v);
    free(v->sb);
    free(v->sb_arena);
    if (v->prim)
        for (i = 0; i < v->rows; i++)
            row_free(v->prim[i]);
    if (v->alt)
        for (i = 0; i < v->rows; i++)
            row_free(v->alt[i]);
    free(v->prim);
    free(v->alt);
    free(v->tmp);
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
    const unsigned char *p = (const unsigned char *)bytes;
    const unsigned char *end = p + len;
    lock(v);
    v->gen++;
    /* 只为 ?2004 侦测扫描转义序列：memchr 定位 ESC/C1，非私有 CSI 直接
     * 跳到终止字节，只有 "?" 开头的序列走状态机。 */
    while (v->scan != SCAN_TEXT && p < end)
        scan_byte(v, *p++);
    while (p < end) {
        const unsigned char *e = memchr(p, 0x1b, (size_t)(end - p));
        /* C1 CSI(0x9b) 只需在下个 ESC 之前找（这避免 memchr 扫到末尾形成 O(n^2)） */
        const unsigned char *lim = e ? e : end;
        const unsigned char *c = memchr(p, 0x9b, (size_t)(lim - p));
        const unsigned char *q = e ? e : c;
        const unsigned char *seq;
        int csi;
        if (!q)
            break;
        seq = q;
        if (*q == 0x1b) {
            q++;
            csi = (q < end && *q == '[');
            if (csi)
                q++;
        } else {
            q++;                    /* C1 CSI：参数直接跟在后面 */
            csi = 1;
        }
        if (!csi) {
            p = q;                  /* 非 CSI 的 ESC 序列：略过 ESC 一字节 */
            continue;
        }
        if (q < end && *q == '?') {
            /* 私有 CSI：交给状态机直到收尾 */
            do {
                scan_byte(v, *seq);
                seq++;
            } while (seq < end && v->scan != SCAN_TEXT);
            p = seq;
        } else {
            /* 非私有序列：跳到 CSI 终止字节（0x40-0x7E） */
            while (q < end && !(*q >= 0x40 && *q <= 0x7e))
                q++;
            p = q < end ? q + 1 : end;
        }
    }
    vterm_input_write(v->vt, bytes, len);
    unlock(v);
}

void hb_vt_resize(hb_vt *v, int rows, int cols)
{
    lock(v);
    vterm_set_size(v->vt, rows, cols);
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
    lock(v);
    if (col < 0 || col >= v->cols)
        goto fail;
    if (row >= 0 && row < v->rows) {
        row_t *r = v->buf[row];
        if (!r)
            goto fail;
        if (col < r->used && col < r->cap)
            *out = r->cells[col];
        else {
            memset(out, 0, sizeof *out);
            out->width = 1;
            out->fg = r->fill_fg;
            out->bg = r->fill_bg;
        }
    } else if (row < 0 && row >= -v->sb_len) {
        const sb_line *l = sb_desc_at(v, row);
        if (col < l->n && v->sb_arena)
            *out = v->sb_arena[l->off + (size_t)col];
        else {
            memset(out, 0, sizeof *out);
            out->width = 1;
            out->fg = l->fg;
            out->bg = l->bg;
        }
    } else {
        goto fail;
    }
    unlock(v);
    return true;
fail:
    unlock(v);
    return false;
}

bool hb_vt_line_wrapped(hb_vt *v, int row)
{
    bool w = false;
    lock(v);
    if (row >= 0) {
        /* 本行软续到下一行 = 下一行的 continuation 标记（state 的 lineinfo
         * 是最新真值，含 autowrap 直写） */
        if (row + 1 < v->rows) {
            const VTermLineInfo *li = vterm_state_get_lineinfo(v->state, row + 1);
            w = li ? li->continuation != 0 : false;
        }
    } else if (row >= -v->sb_len) {
        int idx = (v->sb_start + v->sb_len + row) % v->sb_cap;
        w = v->sb[idx].wraps_next;
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
    a = v->alt_active;
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
    out[2] = (char)(0x80 | (cp & 0x3f));
    out[3] = (char)(0x80 | ((cp >> 6) & 0x3f));
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
