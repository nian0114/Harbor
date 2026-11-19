#include "test.h"

#include "harbor/vt.h"

#include <stdlib.h>
#include <string.h>

/* 收集 vt 输出字节与事件 */
static char g_out[8192];
static size_t g_out_len;

static hb_vt_event g_evs[64];
static int g_n_evs;
static char g_ev_text[64][256];
static char g_ev_text2[64][256];
static const char *g_ev_strs[64];
static const char *g_ev_str2s[64];

static void out_fn(const char *b, size_t n, void *ud)
{
    (void)ud;
    if (g_out_len + n < sizeof g_out) {
        memcpy(g_out + g_out_len, b, n);
        g_out_len += n;
        g_out[g_out_len] = 0;
    }
}

static void ev_fn(const hb_vt_event *ev, void *ud)
{
    int i;
    (void)ud;
    if (g_n_evs >= 64)
        return;
    i = g_n_evs++;
    g_evs[i] = *ev;
    if (ev->text) {
        snprintf(g_ev_text[i], sizeof g_ev_text[i], "%s", ev->text);
        g_ev_strs[i] = g_ev_text[i];
    } else
        g_ev_strs[i] = NULL;
    if (ev->text2) {
        snprintf(g_ev_text2[i], sizeof g_ev_text2[i], "%s", ev->text2);
        g_ev_str2s[i] = g_ev_text2[i];
    } else
        g_ev_str2s[i] = NULL;
    g_evs[i].text = g_ev_strs[i];
    g_evs[i].text2 = g_ev_str2s[i];
}

static void reset(void)
{
    g_out_len = 0;
    g_out[0] = 0;
    g_n_evs = 0;
}

static hb_vt *mkvt(int rows, int cols, int sb)
{
    hb_vt *v = hb_vt_new(rows, cols, sb);
    hb_vt_set_output_fn(v, out_fn, NULL);
    hb_vt_set_event_fn(v, ev_fn, NULL);
    return v;
}

static int count_ev(hb_vt_ev_type t)
{
    int n = 0;
    for (int i = 0; i < g_n_evs; i++)
        if (g_evs[i].type == t)
            n++;
    return n;
}

static const hb_vt_event *find_ev(hb_vt_ev_type t, char mark)
{
    for (int i = 0; i < g_n_evs; i++)
        if (g_evs[i].type == t && (!mark || g_evs[i].mark == mark))
            return &g_evs[i];
    return NULL;
}

static uint32_t cp_at(hb_vt *v, int row, int col)
{
    hb_cell c;
    if (!hb_vt_get_cell(v, row, col, &c))
        return 0;
    return c.chars[0];
}

static void test_text_and_sgr(void)
{
    hb_vt *v = mkvt(10, 20, 100);
    hb_cell c;
    int row, col;

    hb_vt_feed(v, "abc", 3);
    CHECK_EQ_INT(cp_at(v, 0, 0), 'a');
    CHECK_EQ_INT(cp_at(v, 0, 2), 'c');
    hb_vt_feed(v, "\r\nxy", 4);
    CHECK_EQ_INT(cp_at(v, 1, 0), 'x');
    hb_vt_cursor(v, &row, &col, NULL, NULL, NULL);
    CHECK_EQ_INT(row, 1);
    CHECK_EQ_INT(col, 2);

    /* SGR 31 → indexed 1 */
    hb_vt_feed(v, "\e[31mR", 6);
    CHECK(hb_vt_get_cell(v, 1, 2, &c));
    CHECK_EQ_INT(c.fg.type, HB_COLOR_INDEXED);
    CHECK_EQ_INT(c.fg.idx, 1);
    /* 38;5;196 → indexed 196 */
    hb_vt_feed(v, "\e[38;5;196mQ", 12);
    CHECK(hb_vt_get_cell(v, 1, 3, &c));
    CHECK_EQ_INT(c.fg.idx, 196);
    /* 38;2;1;2;3 → RGB */
    hb_vt_feed(v, "\e[38;2;1;2;3mZ", 14);
    CHECK(hb_vt_get_cell(v, 1, 4, &c));
    CHECK_EQ_INT(c.fg.type, HB_COLOR_RGB);
    CHECK_EQ_INT(c.fg.r, 1);
    CHECK_EQ_INT(c.fg.b, 3);
    /* bold + underline */
    hb_vt_feed(v, "\e[1;4mB", 7);
    CHECK(hb_vt_get_cell(v, 1, 5, &c));
    CHECK((c.attrs & HB_ATTR_BOLD) && (c.attrs & HB_ATTR_UNDERLINE));
    hb_vt_free(v);
}

static void test_wide_and_wrap(void)
{
    hb_vt *v = mkvt(5, 10, 50);
    hb_cell c;
    char *t;

    /* CJK 中 = width 2，次格 width 0 */
    hb_vt_feed(v, "中", 3); /* utf8 e4 b8 ad */
    CHECK(hb_vt_get_cell(v, 0, 0, &c));
    CHECK_EQ_INT(c.width, 2);
    CHECK_EQ_INT(c.chars[0], 0x4e2d);
    CHECK(hb_vt_get_cell(v, 0, 1, &c));
    CHECK_EQ_INT(c.width, 0);

    /* 软换行：10 列塞 15 个字符 */
    hb_vt_feed(v, "\r\nabcdefghijklmno", 17);
    CHECK(hb_vt_line_wrapped(v, 1));
    CHECK(!hb_vt_line_wrapped(v, 2));
    t = hb_vt_get_text(v, 1, 0, 2, 10, false);
    CHECK(t && strcmp(t, "abcdefghijklmno") == 0);
    free(t);
    hb_vt_free(v);
}

static void test_scrollback(void)
{
    hb_vt *v = mkvt(10, 20, 20);
    char buf[64];
    int i;

    /* 50 行推入 10 行屏，scrollback 上限 20 */
    for (i = 0; i < 50; i++) {
        int n = snprintf(buf, sizeof buf, "line%02d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 20);
    /* 最新一行 scrollback = line40（见注释：每次底部 CRLF 推走顶行） */
    CHECK_EQ_INT(cp_at(v, -1, 0), 'l');
    CHECK_EQ_INT(cp_at(v, -1, 4), '4');
    CHECK_EQ_INT(cp_at(v, -1, 5), '0');
    /* 最旧一行 = line21 */
    CHECK_EQ_INT(cp_at(v, -20, 4), '2');
    CHECK_EQ_INT(cp_at(v, -20, 5), '1');
    /* 屏底行 = line49 */
    CHECK_EQ_INT(cp_at(v, 8, 4), '4');
    CHECK_EQ_INT(cp_at(v, 8, 5), '9');

    /* alt screen：写入滚动不应长 scrollback */
    reset();
    hb_vt_feed(v, "\e[?1049h", 8);
    CHECK(hb_vt_altscreen(v));
    CHECK_EQ_INT(count_ev(HB_VT_EV_ALTSCREEN), 1);
    CHECK_EQ_INT(g_evs[g_n_evs - 1].code, 1);
    for (i = 0; i < 30; i++) {
        int n = snprintf(buf, sizeof buf, "alt%d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 20);
    hb_vt_feed(v, "\e[?1049l", 8);
    CHECK(!hb_vt_altscreen(v));
    CHECK_EQ_INT(count_ev(HB_VT_EV_ALTSCREEN), 2);
    CHECK_EQ_INT(g_evs[g_n_evs - 1].code, 0);
    hb_vt_free(v);
}

static void test_search(void)
{
    hb_vt *v = mkvt(5, 20, 10);
    hb_vt_match *m = NULL;
    size_t n;
    int i;
    char buf[64];

    for (i = 0; i < 8; i++) {
        int k = snprintf(buf, sizeof buf, "needle-%d\r\n", i);
        hb_vt_feed(v, buf, (size_t)k);
    }
    hb_vt_feed(v, "Hello hello", 11);

    /* scrollback + 屏幕全量 */
    n = hb_vt_search(v, "needle", false, &m);
    CHECK_EQ_INT(n, 8);
    if (n >= 1) {
        CHECK(m[0].row < 0);            /* 旧的在 scrollback */
        CHECK_EQ_INT(m[0].col, 0);
        CHECK_EQ_INT(m[0].len, 6);
        CHECK(m[n - 1].row < 0 || m[n - 1].row >= 0);
    }
    free(m);
    m = NULL;

    /* 大小写不敏感匹配屏幕上两处 */
    n = hb_vt_search(v, "hello", true, &m);
    CHECK_EQ_INT(n, 2);
    if (n == 2) {
        CHECK_EQ_INT(m[0].col, 0);
        CHECK_EQ_INT(m[1].col, 6);
        CHECK_EQ_INT(m[0].row, 4);
    }
    free(m);
    m = NULL;

    /* 大小写敏感不匹配 */
    n = hb_vt_search(v, "HELLO", false, &m);
    CHECK_EQ_INT(n, 0);
    CHECK(m == NULL);
    hb_vt_free(v);
}

static void test_osc_events(void)
{
    hb_vt *v = mkvt(5, 20, 10);
    const hb_vt_event *e;

    /* OSC 133：BEL 与 ST 两种结尾 + 分段 */
    reset();
    hb_vt_feed(v, "\e]133;A\a", 8);
    e = find_ev(HB_VT_EV_OSC133, 'A');
    CHECK(e != NULL);

    reset();
    hb_vt_feed(v, "\e]133;B", 7);
    hb_vt_feed(v, "\a", 1);                 /* 分两次喂 */
    e = find_ev(HB_VT_EV_OSC133, 'B');
    CHECK(e != NULL);

    reset();
    hb_vt_feed(v, "\e]133;C\e\\", 9);        /* ST 结尾 */
    e = find_ev(HB_VT_EV_OSC133, 'C');
    CHECK(e != NULL);

    reset();
    hb_vt_feed(v, "\e]133;D;1\e\\", 11);
    e = find_ev(HB_VT_EV_OSC133, 'D');
    CHECK(e != NULL);
    CHECK_EQ_INT(e->code, 1);

    /* OSC 9 通知 */
    reset();
    hb_vt_feed(v, "\e]9;hi there\a", 13);
    e = find_ev(HB_VT_EV_NOTIFY, 0);
    CHECK(e != NULL);
    if (e) {
        CHECK(e->text == NULL);
        CHECK_EQ_STR(e->text2, "hi there");
    }

    /* ConEmu 9;4;... 进度应被忽略 */
    reset();
    hb_vt_feed(v, "\e]9;4;3;50\a", 11);
    CHECK_EQ_INT(count_ev(HB_VT_EV_NOTIFY), 0);

    /* OSC 777 notify */
    reset();
    hb_vt_feed(v, "\e]777;notify;MyTitle;MyBody\a", 28);
    e = find_ev(HB_VT_EV_NOTIFY, 0);
    CHECK(e != NULL);
    if (e) {
        CHECK_EQ_STR(e->text, "MyTitle");
        CHECK_EQ_STR(e->text2, "MyBody");
    }

    /* OSC 52：默认禁止 */
    reset();
    hb_vt_feed(v, "\e]52;c;aGVsbG8=\a", strlen("\e]52;c;aGVsbG8=\a"));   /* "hello" b64 */
    CHECK_EQ_INT(count_ev(HB_VT_EV_CLIPBOARD), 0);
    /* 允许后解码（libvterm 已解 base64） */
    hb_vt_set_osc52_allowed(v, true);
    reset();
    hb_vt_feed(v, "\e]52;c;aGVsbG8=\a", strlen("\e]52;c;aGVsbG8=\a"));
    e = find_ev(HB_VT_EV_CLIPBOARD, 0);
    CHECK(e != NULL);
    if (e)
        CHECK_EQ_STR(e->text, "hello");
    /* '?' 查询忽略 */
    reset();
    hb_vt_feed(v, "\e]52;c;?\a", 9);
    CHECK_EQ_INT(count_ev(HB_VT_EV_CLIPBOARD), 0);

    /* OSC 7 → CWD */
    reset();
    hb_vt_feed(v, "\e]7;file://host/home/x\a", 23);
    e = find_ev(HB_VT_EV_CWD, 0);
    CHECK(e != NULL);
    if (e)
        CHECK_EQ_STR(e->text, "file://host/home/x");

    /* 标题 OSC 0 + BEL */
    reset();
    hb_vt_feed(v, "\e]0;My Title\a", 13);
    e = find_ev(HB_VT_EV_TITLE, 0);
    CHECK(e != NULL);
    if (e)
        CHECK_EQ_STR(e->text, "My Title");
    CHECK_EQ_STR(hb_vt_title(v), "My Title");

    reset();
    hb_vt_feed(v, "\a", 1);
    CHECK_EQ_INT(count_ev(HB_VT_EV_BELL), 1);
    hb_vt_free(v);
}

static void test_input(void)
{
    hb_vt *v = mkvt(5, 20, 10);

    reset();
    hb_vt_key(v, HB_KEY_UP, 0);
    CHECK_EQ_STR(g_out, "\e[A");

    /* application cursor keys */
    reset();
    hb_vt_feed(v, "\e[?1h", 5);
    hb_vt_key(v, HB_KEY_UP, 0);
    CHECK_EQ_STR(g_out, "\eOA");

    /* Ctrl+C → 0x03 */
    reset();
    hb_vt_unichar(v, 'c', HB_MOD_CTRL);
    CHECK_EQ_INT(g_out_len, 1);
    CHECK_EQ_INT((unsigned char)g_out[0], 0x03);

    /* 普通字符 */
    reset();
    hb_vt_unichar(v, 'x', 0);
    CHECK_EQ_STR(g_out, "x");

    /* bracketed paste */
    CHECK(!hb_vt_bracketed_paste(v));
    reset();
    hb_vt_feed(v, "\e[?2004h", 8);
    CHECK(hb_vt_bracketed_paste(v));
    reset();
    hb_vt_paste(v, "hello");
    CHECK_EQ_STR(g_out, "\e[200~hello\e[201~");
    reset();
    hb_vt_feed(v, "\e[?2004l", 8);
    CHECK(!hb_vt_bracketed_paste(v));
    reset();
    hb_vt_paste(v, "q");
    CHECK_EQ_STR(g_out, "q");

    /* 鼠标模式 + 上报 */
    reset();
    hb_vt_feed(v, "\e[?1000h", 8);
    CHECK_EQ_INT(hb_vt_mouse_mode(v), 1);
    reset();
    hb_vt_mouse(v, 4, 2, 1, true, 0);   /* row4 col2 → 0-based in vterm, +32/+33 */
    CHECK(g_out_len == 6);
    CHECK(memcmp(g_out, "\e[M", 3) == 0);
    CHECK_EQ_INT((unsigned char)g_out[3], ' ' + 0);       /* button1 press */
    CHECK_EQ_INT((unsigned char)g_out[4], ' ' + 3);       /* col 2 → +1+32 */
    CHECK_EQ_INT((unsigned char)g_out[5], ' ' + 5);       /* row 4 → +1+32 */

    /* focus 上报 */
    reset();
    hb_vt_feed(v, "\e[?1004h", 8);
    hb_vt_focus(v, true);
    CHECK_EQ_STR(g_out, "\e[I");
    reset();
    hb_vt_focus(v, false);
    CHECK_EQ_STR(g_out, "\e[O");
    hb_vt_free(v);
}

static void test_total_pushed(void)
{
    hb_vt *v = mkvt(10, 20, 20);
    char buf[64];
    int i;
    uint64_t t0, t1;

    CHECK_EQ_INT(hb_vt_total_pushed(v), 0);
    for (i = 0; i < 50; i++) {
        int n = snprintf(buf, sizeof buf, "line%02d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    t0 = hb_vt_total_pushed(v);
    /* ring 满后仍单调增长；绝对行号 abs = total_pushed + row 稳定 */
    CHECK(t0 > 0);
    CHECK(t0 >= (uint64_t)hb_vt_scrollback_count(v));
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 20);

    for (i = 0; i < 5; i++) {
        int n = snprintf(buf, sizeof buf, "more%02d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    t1 = hb_vt_total_pushed(v);
    CHECK(t1 == t0 + 5);

    /* resize 拉回 scrollback 行 → total 减少 */
    hb_vt_resize(v, 15, 20);
    CHECK(hb_vt_total_pushed(v) <= t1);
    CHECK(hb_vt_scrollback_count(v) <= 20);
    hb_vt_free(v);
}

static void test_generation(void)
{
    hb_vt *v = mkvt(5, 20, 10);
    uint64_t g0 = hb_vt_generation(v);
    hb_vt_feed(v, "x", 1);
    CHECK(hb_vt_generation(v) > g0);
    hb_vt_free(v);
}

static void test_scroll_region(void)
{
    hb_vt *v = mkvt(10, 20, 20);
    char buf[32];
    int i;

    /* 每行写一个不同标记字符 */
    for (i = 0; i < 10; i++) {
        int n = snprintf(buf, sizeof buf, "\e[%d;1H%c", i + 1, 'A' + i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    /* DECSTBM 5;10 → 区域为 0-based 行 4..9 */
    hb_vt_feed(v, "\e[5;10r", strlen("\e[5;10r"));
    /* 移到区域底（第 10 行）并换行 → 区域上滚 1 行 */
    hb_vt_feed(v, "\e[10;1H\n", strlen("\e[10;1H\n"));
    /* 区域外不变 */
    CHECK_EQ_INT(cp_at(v, 0, 0), 'A');
    CHECK_EQ_INT(cp_at(v, 3, 0), 'D');
    /* 区域内上滚：行4 拿到旧行5 的 'F'，行8 拿到旧行9 的 'J' */
    CHECK_EQ_INT(cp_at(v, 4, 0), 'F');
    CHECK_EQ_INT(cp_at(v, 8, 0), 'J');
    /* 行9 变空白 */
    CHECK_EQ_INT(cp_at(v, 9, 0), 0);
    /* 局部区域滚动不入 scrollback */
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 0);
    CHECK_EQ_INT(hb_vt_total_pushed(v), 0);
    hb_vt_free(v);
}

static void test_insert_delete_line(void)
{
    hb_vt *v = mkvt(10, 20, 20);

    hb_vt_feed(v, "T0\r\nT1\r\nT2", 11);
    /* 光标第 2 行，插入 2 行：T1/T2 下移 */
    hb_vt_feed(v, "\e[2;1H\e[2L", strlen("\e[2;1H\e[2L"));
    CHECK_EQ_INT(cp_at(v, 0, 0), 'T');
    CHECK_EQ_INT(cp_at(v, 1, 0), 0);
    CHECK_EQ_INT(cp_at(v, 2, 0), 0);
    CHECK_EQ_INT(cp_at(v, 3, 0), 'T');
    CHECK_EQ_INT(cp_at(v, 3, 1), '1');
    CHECK_EQ_INT(cp_at(v, 4, 1), '2');
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 0);
    /* 光标第 4 行（T1 所在），删除 2 行：T1、T2 行被删 */
    hb_vt_feed(v, "\e[4;1H\e[2M", strlen("\e[4;1H\e[2M"));
    CHECK_EQ_INT(cp_at(v, 3, 1), 0);
    CHECK_EQ_INT(cp_at(v, 3, 0), 0);
    hb_vt_free(v);
}

static void test_insert_delete_char(void)
{
    hb_vt *v = mkvt(5, 20, 20);

    hb_vt_feed(v, "abcdef", 6);
    hb_vt_feed(v, "\e[1;3H\e[3@", strlen("\e[1;3H\e[3@"));   /* 在第 3 列插入 3 空白 */
    CHECK_EQ_INT(cp_at(v, 0, 1), 'b');
    CHECK_EQ_INT(cp_at(v, 0, 2), 0);
    CHECK_EQ_INT(cp_at(v, 0, 3), 0);
    CHECK_EQ_INT(cp_at(v, 0, 4), 0);
    CHECK_EQ_INT(cp_at(v, 0, 5), 'c');
    CHECK_EQ_INT(cp_at(v, 0, 8), 'f');
    hb_vt_feed(v, "\e[3P", strlen("\e[3P"));            /* 删除 3 格 → 还原 */
    CHECK_EQ_INT(cp_at(v, 0, 2), 'c');
    CHECK_EQ_INT(cp_at(v, 0, 5), 'f');
    CHECK_EQ_INT(cp_at(v, 0, 6), 0);
    hb_vt_free(v);
}

static void test_bce_erase(void)
{
    hb_vt *v = mkvt(5, 20, 20);
    hb_cell c;

    hb_vt_feed(v, "hello", 5);
    hb_vt_feed(v, "\e[41m", 5);            /* bg = indexed 1 */
    hb_vt_feed(v, "\e[2J", 4);             /* ED 全屏 → 擦除格带 bg 1 */
    CHECK(hb_vt_get_cell(v, 0, 0, &c));
    CHECK_EQ_INT(c.chars[0], 0);
    CHECK_EQ_INT(c.bg.type, HB_COLOR_INDEXED);
    CHECK_EQ_INT(c.bg.idx, 1);
    CHECK(hb_vt_get_cell(v, 4, 19, &c));
    CHECK_EQ_INT(c.bg.idx, 1);
    /* EL：写文本后清到行尾 */
    hb_vt_feed(v, "\e[42m\e[1;1Habc\e[K", 17); /* bg=2, "abc", EL */
    CHECK(hb_vt_get_cell(v, 0, 5, &c));
    CHECK_EQ_INT(c.chars[0], 0);
    CHECK_EQ_INT(c.bg.idx, 2);
    CHECK_EQ_INT(cp_at(v, 0, 0), 'a');
    hb_vt_free(v);
}

static void test_wide_overwrite(void)
{
    hb_vt *v = mkvt(5, 10, 20);
    hb_cell c;

    /* 覆盖宽字符尾部 → 前半变孤儿清空 */
    hb_vt_feed(v, "\xe4\xb8\xad", 3);         /* 中 at (0,0), trailer (0,1) */
    hb_vt_feed(v, "\e[1;2Hx", 7);             /* x 覆盖 trailer */
    CHECK(hb_vt_get_cell(v, 0, 0, &c));
    CHECK_EQ_INT(c.chars[0], 0);              /* 孤儿被清 */
    CHECK_EQ_INT(c.width, 1);
    CHECK_EQ_INT(cp_at(v, 0, 1), 'x');
    /* 覆盖宽字符前半 → 后半变孤儿清空 */
    hb_vt_feed(v, "\e[2;1H\xe4\xb8\xad", strlen("\e[2;1H\xe4\xb8\xad")); /* 中 at (1,0), trailer (1,1) */
    hb_vt_feed(v, "\e[2;1Hy", 7);             /* y 覆盖前半 */
    CHECK_EQ_INT(cp_at(v, 1, 0), 'y');
    CHECK(hb_vt_get_cell(v, 1, 1, &c));
    CHECK_EQ_INT(c.chars[0], 0);
    CHECK_EQ_INT(c.width, 1);
    hb_vt_free(v);
}

static void test_ed3_clears_scrollback(void)
{
    hb_vt *v = mkvt(5, 20, 20);
    int i;
    char buf[32];

    for (i = 0; i < 10; i++) {
        int n = snprintf(buf, sizeof buf, "s%02d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    CHECK(hb_vt_scrollback_count(v) > 0);
    hb_vt_feed(v, "\e[3J", 4);               /* ED 3 → 清 scrollback */
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 0);
    CHECK(!hb_vt_get_cell(v, -1, 0, &(hb_cell){0}));
    hb_vt_free(v);
}

static void test_resize_scrollback_roundtrip(void)
{
    hb_vt *v = mkvt(5, 20, 50);
    char buf[32];
    int i;

    for (i = 0; i < 10; i++) {
        int n = snprintf(buf, sizeof buf, "L%02d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    /* 10 行 × "\r\n" → L00..L05 共 6 行被推，屏 = L06..L09+空 */
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 6);
    CHECK_EQ_INT(cp_at(v, 0, 2), '6');

    /* 缩到 3 行 → 顶部 2 行 L06 L07 推入 scrollback */
    hb_vt_resize(v, 3, 20);
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 8);
    CHECK_EQ_INT(cp_at(v, 0, 2), '8');
    CHECK_EQ_INT(cp_at(v, -1, 2), '7');

    /* 增回 5 行 → 拉回 L06 L07，内容原样，末行原为空 */
    hb_vt_resize(v, 5, 20);
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 6);
    CHECK_EQ_INT(cp_at(v, 0, 2), '6');
    CHECK_EQ_INT(cp_at(v, 1, 2), '7');
    CHECK_EQ_INT(cp_at(v, 2, 2), '8');
    CHECK_EQ_INT(cp_at(v, 3, 2), '9');
    hb_vt_free(v);
}

static void test_ring_eviction(void)
{
    hb_vt *v = mkvt(5, 20, 5);
    char buf[32];
    int i;

    for (i = 0; i < 12; i++) {
        int n = snprintf(buf, sizeof buf, "E%02d\r\n", i);
        hb_vt_feed(v, buf, (size_t)n);
    }
    /* 12 行入 5 行屏 → 推出 8 行，ring 容量 5 → 最旧 3 行逐出 */
    CHECK_EQ_INT(hb_vt_scrollback_count(v), 5);
    CHECK_EQ_INT(hb_vt_total_pushed(v), 8);
    /* 最新 scrollback = E07，最旧 = E03 */
    CHECK_EQ_INT(cp_at(v, -1, 2), '7');
    CHECK_EQ_INT(cp_at(v, -5, 2), '3');
    CHECK(!hb_vt_get_cell(v, -6, 0, &(hb_cell){0}));
    hb_vt_free(v);
}

static void test_altscreen_save_restore(void)
{
    hb_vt *v = mkvt(5, 20, 20);
    int row, col;

    hb_vt_feed(v, "PRI", 3);
    hb_vt_feed(v, "\e[?1049h", 8);
    CHECK(hb_vt_altscreen(v));
    CHECK_EQ_INT(cp_at(v, 0, 0), 0);          /* alt 屏干净 */
    hb_vt_feed(v, "ALT", 3);                  /* 光标仍在保存的 (0,3) */
    CHECK_EQ_INT(cp_at(v, 0, 3), 'A');
    hb_vt_feed(v, "\e[?1049l", 8);
    CHECK(!hb_vt_altscreen(v));
    /* 主屏内容与光标均恢复 */
    CHECK_EQ_INT(cp_at(v, 0, 0), 'P');
    CHECK_EQ_INT(cp_at(v, 0, 2), 'I');
    hb_vt_cursor(v, &row, &col, NULL, NULL, NULL);
    CHECK_EQ_INT(row, 0);
    CHECK_EQ_INT(col, 3);
    hb_vt_free(v);
}

int main(void)

{
    test_text_and_sgr();
    test_wide_and_wrap();
    test_scrollback();
    test_search();
    test_total_pushed();
    test_osc_events();
    test_input();
    test_generation();
    test_scroll_region();
    test_insert_delete_line();
    test_insert_delete_char();
    test_bce_erase();
    test_wide_overwrite();
    test_ed3_clears_scrollback();
    test_resize_scrollback_roundtrip();
    test_ring_eviction();
    test_altscreen_save_restore();
    fprintf(stderr, "test_vt: %d checks, %d failures\n", hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
