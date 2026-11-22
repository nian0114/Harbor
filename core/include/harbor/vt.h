#ifndef HB_VT_H
#define HB_VT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* hb_vt：libvterm 封装（屏幕 + scrollback + 事件 + 输入）。              */
/*                                                                     */
/* 线程模型：所有公开函数内部加锁（递归互斥），UI 可在渲染时用             */
/* hb_vt_lock/unlock 包住一段批量读取以获得一致快照。                      */
/* output_fn / event_fn 在持有 vt 锁的调用线程上同步触发。                */
/* ------------------------------------------------------------------ */

typedef struct hb_vt hb_vt;

enum { HB_COLOR_DEFAULT_FG, HB_COLOR_DEFAULT_BG, HB_COLOR_INDEXED, HB_COLOR_RGB };
typedef struct { uint8_t type, idx, r, g, b; } hb_color;

enum {
    HB_ATTR_BOLD       = 1 << 0,
    HB_ATTR_ITALIC     = 1 << 1,
    HB_ATTR_UNDERLINE  = 1 << 2,
    HB_ATTR_DUNDERLINE = 1 << 3,
    HB_ATTR_CURLY      = 1 << 4,
    HB_ATTR_REVERSE    = 1 << 5,
    HB_ATTR_STRIKE     = 1 << 6,
    HB_ATTR_BLINK      = 1 << 7,
    HB_ATTR_CONCEAL    = 1 << 8,
    HB_ATTR_DIM        = 1 << 9,
};

#define HB_VT_MAX_CHARS 6
/* chars[0]==0 → 空白格；width: 1 普通, 2 宽字符头, 0 宽字符尾 */
typedef struct {
    uint32_t chars[HB_VT_MAX_CHARS];
    uint8_t  width;
    uint16_t attrs;
    hb_color fg, bg;
} hb_cell;

typedef enum {
    HB_VT_EV_BELL,
    HB_VT_EV_TITLE,
    HB_VT_EV_OSC133,     /* mark='A'|'B'|'C'|'D'；code=D 的退出码或 -1 */
    HB_VT_EV_NOTIFY,     /* OSC 9/777；text=title 可 NULL，text2=body */
    HB_VT_EV_CLIPBOARD,  /* OSC 52 解码后的文本（需 osc52_allowed） */
    HB_VT_EV_ALTSCREEN,  /* code=1 进入 / 0 退出 */
    HB_VT_EV_CWD,        /* OSC 7；text=file://... URI */
} hb_vt_ev_type;

typedef struct {
    hb_vt_ev_type type;
    char  mark;
    int   code;
    const char *text;   /* 仅回调期间有效 */
    const char *text2;
    int   row, col;     /* OSC133：发出该标记瞬间的光标位置 */
    uint64_t pushed;    /* OSC133：当刻 hb_vt_total_pushed（供行号换算） */
} hb_vt_event;

/* 终端应发回 pty/channel 的字节（键盘、鼠标、DA 应答等） */
typedef void (*hb_vt_output_fn)(const char *bytes, size_t len, void *ud);
typedef void (*hb_vt_event_fn)(const hb_vt_event *ev, void *ud);

hb_vt *hb_vt_new(int rows, int cols, int scrollback_lines);
void   hb_vt_free(hb_vt *v);

void hb_vt_set_output_fn(hb_vt *v, hb_vt_output_fn fn, void *ud);
void hb_vt_set_event_fn(hb_vt *v, hb_vt_event_fn fn, void *ud);
void hb_vt_set_osc52_allowed(hb_vt *v, bool allowed);   /* default false */

void hb_vt_feed(hb_vt *v, const char *bytes, size_t len);
/* 回到出厂态：清两屏、scrollback、模式与光标（FR-12：重放快照前复位） */
void hb_vt_hard_reset(hb_vt *v);
void hb_vt_resize(hb_vt *v, int rows, int cols);
void hb_vt_size(hb_vt *v, int *rows, int *cols);

int  hb_vt_scrollback_count(hb_vt *v);
/* 推入 scrollback 的累计行数（resize 拉回会减）。配合 row 可得绝对行号：
 * scrollback 行 -k → total_pushed - k；屏幕行 r → total_pushed + r。
 * 同一行在 ring 内坐标稳定（满溢丢最旧行时其余行 abs 不变）。 */
uint64_t hb_vt_total_pushed(hb_vt *v);
/* row ∈ [-scrollback_count, rows-1]；负数 = scrollback（-1 为最新一行） */
bool hb_vt_get_cell(hb_vt *v, int row, int col, hb_cell *out);
/* 本行是否软换行续到下一行 */
bool hb_vt_line_wrapped(hb_vt *v, int row);
void hb_vt_cursor(hb_vt *v, int *row, int *col, bool *visible, bool *blink,
                  int *shape /* 1 block, 2 underline, 3 bar */);
bool hb_vt_altscreen(hb_vt *v);
/* 返回值在 vt 锁内有效 */
const char *hb_vt_title(hb_vt *v);

int  hb_vt_mouse_mode(hb_vt *v);        /* 0 none, 1 click, 2 drag, 3 move */
bool hb_vt_bracketed_paste(hb_vt *v);
bool hb_vt_app_cursor(hb_vt *v);        /* DECCKM ?1：application 光标键 */
bool hb_vt_app_keypad(hb_vt *v);        /* DECPAM ESC=：application keypad */

/* ANSI 重建流：在全新 rows×cols hb_vt 上回放即可重建终端（FR-12 §5）。
 * 内容：scrollback 尾部（≤ max_scrollback 行，带 SGR）、ESC[H ESC[2J、
 * 可见屏逐行（带 SGR、宽字符占位）、alt 屏（先主屏再 ?1049h 再备用屏）、
 * 光标位置/可见性/闪烁/形状、以及影响输入编码的模式（DECCKM、bracketed
 * paste、application keypad、mouse）。调用方必须已持有 vt 锁
 * （在 session tap / run_on_thread 回调里取，保证与 dseq 同点）。 */
void hb_vt_snapshot_ansi(hb_vt *v, int max_scrollback, char **out, size_t *len);

/* -------- 输入 -------- */

typedef enum {
    HB_KEY_NONE,
    HB_KEY_ENTER,
    HB_KEY_TAB,
    HB_KEY_BACKSPACE,
    HB_KEY_ESCAPE,
    HB_KEY_UP,
    HB_KEY_DOWN,
    HB_KEY_LEFT,
    HB_KEY_RIGHT,
    HB_KEY_INS,
    HB_KEY_DEL,
    HB_KEY_HOME,
    HB_KEY_END,
    HB_KEY_PAGEUP,
    HB_KEY_PAGEDOWN,
    HB_KEY_F1 = 100,          /* ..F12 连续编号 */
    HB_KEY_KP_0 = 200,        /* keypad（可选支持） */
} hb_key;

enum { HB_MOD_SHIFT = 1, HB_MOD_ALT = 2, HB_MOD_CTRL = 4 };

void hb_vt_key(hb_vt *v, hb_key key, int mods);
void hb_vt_unichar(hb_vt *v, uint32_t c, int mods);
void hb_vt_paste(hb_vt *v, const char *utf8);
void hb_vt_mouse(hb_vt *v, int row, int col, int button, bool pressed, int mods);
void hb_vt_focus(hb_vt *v, bool in);

/* -------- 文本与搜索 -------- */

/* malloc'd UTF-8。线性选取把软换行直接拼接（无 '\n'），硬行去尾部空白。 */
char *hb_vt_get_text(hb_vt *v, int r0, int c0, int r1, int c1, bool block);

typedef struct { int row, col, len; } hb_vt_match;
/* 覆盖 scrollback + 屏幕，最旧→最新；ASCII 大小写折叠。 */
size_t hb_vt_search(hb_vt *v, const char *needle, bool case_insensitive,
                    hb_vt_match **out);

void hb_vt_clear_scrollback(hb_vt *v);

/* 任何屏幕/scrollback 变化自增 */
uint64_t hb_vt_generation(hb_vt *v);

void hb_vt_lock(hb_vt *v);
void hb_vt_unlock(hb_vt *v);

#ifdef __cplusplus
}
#endif

#endif
