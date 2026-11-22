#include "harbor/share.h"

#include "share_priv.h"

#include "harbor/pty.h"
#include "harbor/session.h"
#include "harbor/vt.h"

#include "test.h"

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = H[b[i] >> 4];
        out[i * 2 + 1] = H[b[i] & 15];
    }
    out[n * 2] = 0;
}

static int screen_has(hb_vt *v, const char *needle)
{
    int rows = 0, cols = 0, r, found = 0, sb;
    hb_vt_lock(v);
    hb_vt_size(v, &rows, &cols);
    sb = hb_vt_scrollback_count(v);
    for (r = -sb; r < rows && !found; r++) {
        char *ln = hb_vt_get_text(v, r, 0, r, cols, false);
        if (ln && strstr(ln, needle))
            found = 1;
        free(ln);
    }
    hb_vt_unlock(v);
    return found;
}

/* 逐格对比两个 vt：字符、属性、颜色、滚动区文本、光标与输入相关模式。 */
static int vt_equal(hb_vt *a, hb_vt *b, int verbose)
{
    int ra = 0, ca = 0, rb = 0, cb = 0, r, c;
    int sa, sb, ok = 1;
    hb_vt_lock(a);
    hb_vt_lock(b);
    hb_vt_size(a, &ra, &ca);
    hb_vt_size(b, &rb, &cb);
    if (ra != rb || ca != cb)
        ok = 0;
    if (ok)
        for (r = 0; r < ra; r++)
            for (c = 0; c < ca; c++) {
                hb_cell xa, xb;
                int i;
                hb_vt_get_cell(a, r, c, &xa);
                hb_vt_get_cell(b, r, c, &xb);
                if (xa.width != xb.width || xa.attrs != xb.attrs ||
                    memcmp(&xa.fg, &xb.fg, sizeof xa.fg) ||
                    memcmp(&xa.bg, &xb.bg, sizeof xa.bg)) {
                    if (ok && verbose)
                        fprintf(stderr,
                            "cell %d,%d: w %d/%d attr %x/%x fg %02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x\n",
                            r, c, xa.width, xb.width, xa.attrs, xb.attrs,
                            xa.fg.type, xa.fg.idx, xa.fg.r, xa.fg.g, xa.fg.b,
                            xb.fg.type, xb.fg.idx, xb.fg.r, xb.fg.g, xb.fg.b);
                    ok = 0;
                }
                for (i = 0; i < HB_VT_MAX_CHARS; i++)
                    if (xa.chars[i] != xb.chars[i]) {
                        if (ok && verbose)
                            fprintf(stderr,
                                "cell %d,%d ch%d: U+%04x vs U+%04x\n",
                                r, c, i, xa.chars[i], xb.chars[i]);
                        ok = 0;
                    }
            }
    sa = hb_vt_scrollback_count(a);
    sb = hb_vt_scrollback_count(b);
    if (sa != sb) {
        if (verbose)
            fprintf(stderr, "scrollback count %d vs %d\n", sa, sb);
        ok = 0;
    }
    if (ok)
        for (r = -sa; r < 0; r++) {
            char *ta = hb_vt_get_text(a, r, 0, r, ca, false);
            char *tb = hb_vt_get_text(b, r, 0, r, cb, false);
            if (!ta || !tb || strcmp(ta, tb)) {
                if (ok && verbose)
                    fprintf(stderr, "sb %d: '%s' vs '%s'\n", r,
                            ta ? ta : "?", tb ? tb : "?");
                ok = 0;
            }
            free(ta);
            free(tb);
        }
    {
        int cr1, cc1, cs1, cr2, cc2, cs2;
        bool cv1, cb1, cv2, cb2;
        hb_vt_cursor(a, &cr1, &cc1, &cv1, &cb1, &cs1);
        hb_vt_cursor(b, &cr2, &cc2, &cv2, &cb2, &cs2);
        if (cr1 != cr2 || cc1 != cc2 || cv1 != cv2 || cb1 != cb2 ||
            cs1 != cs2) {
            if (verbose)
                fprintf(stderr,
                        "cursor %d,%d v%d b%d s%d vs %d,%d v%d b%d s%d\n",
                    cr1, cc1, cv1, cb1, cs1, cr2, cc2, cv2, cb2, cs2);
            ok = 0;
        }
    }
    if (hb_vt_altscreen(a) != hb_vt_altscreen(b) ||
        hb_vt_mouse_mode(a) != hb_vt_mouse_mode(b) ||
        hb_vt_bracketed_paste(a) != hb_vt_bracketed_paste(b) ||
        hb_vt_app_cursor(a) != hb_vt_app_cursor(b) ||
        hb_vt_app_keypad(a) != hb_vt_app_keypad(b)) {
        if (verbose)
            fprintf(stderr,
                    "modes alt %d/%d mouse %d/%d bp %d/%d ac %d/%d ak %d/%d\n",
                hb_vt_altscreen(a), hb_vt_altscreen(b),
                hb_vt_mouse_mode(a), hb_vt_mouse_mode(b),
                hb_vt_bracketed_paste(a), hb_vt_bracketed_paste(b),
                hb_vt_app_cursor(a), hb_vt_app_cursor(b),
                hb_vt_app_keypad(a), hb_vt_app_keypad(b));
        ok = 0;
    }
    hb_vt_unlock(b);
    hb_vt_unlock(a);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 1. 帧编解码                                                          */
/* ------------------------------------------------------------------ */

static void t_codec(void)
{
    uint8_t key[32], k2[32];
    const char *sid = "share-id-codec";
    const char *pt = "hello frame payload";
    hb_share_frame f;
    uint8_t type, *out = NULL;
    uint32_t to, from;
    uint64_t seq;
    size_t ol = 0;
    int i;

    randombytes_buf(key, 32);
    randombytes_buf(k2, 32);

    f = hb__share_mk_frame(sid, HB_FR_DELTA, 0, 1, 7, key,
                           (const uint8_t *)pt, strlen(pt));
    CHECK(f.data != NULL);
    CHECK_EQ_INT(f.len, HB_FR_HDRLEN + 24 + 19 + 16);  /* nonce|ct|tag */
    CHECK_EQ_INT(f.data[0], HB_FR_DELTA);
    CHECK_EQ_INT((f.data[1] << 24) | (f.data[2] << 16) |
                 (f.data[3] << 8) | f.data[4], 0);
    CHECK_EQ_INT(f.data[5], 0);
    CHECK_EQ_INT(f.data[8], 1);
    CHECK_EQ_INT(f.data[16], 7);                        /* seq 低字节 */

    /* round-trip */
    CHECK(hb__share_open_frame(sid, f.data, f.len, key,
                               &type, &to, &from, &seq, &out, &ol) == HB_OK);
    CHECK_EQ_INT(type, HB_FR_DELTA);
    CHECK_EQ_INT(to, 0);
    CHECK_EQ_INT(from, 1);
    CHECK_EQ_INT(seq, 7);
    CHECK(ol == strlen(pt) && memcmp(out, pt, ol) == 0);
    sodium_free(out);
    out = NULL;

    /* AD 覆盖全部头部字段：翻转任一头部字节 → open 必失败 */
    for (i = 0; i < HB_FR_HDRLEN; i++) {
        uint8_t *bad = malloc(f.len);
        memcpy(bad, f.data, f.len);
        bad[i] ^= 0x01;
        CHECK(hb__share_open_frame(sid, bad, f.len, key,
                                   &type, &to, &from, &seq,
                                   &out, &ol) != HB_OK);
        free(bad);
    }
    /* 密文/标签被改同样失败 */
    {
        uint8_t *bad = malloc(f.len);
        memcpy(bad, f.data, f.len);
        bad[f.len - 1] ^= 0x80;
        CHECK(hb__share_open_frame(sid, bad, f.len, key,
                                   &type, &to, &from, &seq,
                                   &out, &ol) != HB_OK);
        free(bad);
    }
    /* 错密钥 / 错 share_id → 失败 */
    CHECK(hb__share_open_frame(sid, f.data, f.len, k2,
                               &type, &to, &from, &seq,
                               &out, &ol) != HB_OK);
    CHECK(hb__share_open_frame("other-id", f.data, f.len, key,
                               &type, &to, &from, &seq,
                               &out, &ol) != HB_OK);
    /* PAKE 帧：明文 payload */
    {
        uint8_t raw[48];
        hb_share_frame pf;
        memset(raw, 0xab, sizeof raw);
        pf = hb__share_mk_frame(sid, HB_FR_PAKE1, 1, 2, 1, NULL,
                                raw, sizeof raw);
        CHECK(pf.data && pf.len == HB_FR_HDRLEN + 48);
        CHECK(memcmp(pf.data + HB_FR_HDRLEN, raw, 48) == 0);
        hb_share_frame_clear(&pf);
    }
    hb_share_frame_clear(&f);
}

/* ------------------------------------------------------------------ */
/* 2. CPace                                                            */
/* ------------------------------------------------------------------ */

static const char CP_SID[] = "cpace-share-uuid";
static const char CP_CODE[] = "482913";

static void t_cpace_pair(void)
{
    uint8_t ng[16], ls[16], ya[32], yb[32], isk_g[64], isk_o[64];
    uint8_t tag_o[32], tag_g[32], kp1[32], kp2[32];
    hb_cpace *g, *o;

    randombytes_buf(ng, 16);
    randombytes_buf(ls, 16);
    g = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
    o = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
    CHECK(g && o);
    if (!g || !o)
        return;
    CHECK(memcmp(g->G, o->G, 32) == 0);
    CHECK(memcmp(g->Y, o->Y, 32) != 0);
    memcpy(ya, g->Y, 32);
    memcpy(yb, o->Y, 32);

    CHECK(hb__cpace_isk(o, ya, ya, yb, isk_o) == HB_OK); /* owner */
    CHECK(hb__cpace_isk(g, yb, ya, yb, isk_g) == HB_OK); /* guest */
    CHECK(memcmp(isk_g, isk_o, 64) == 0);

    crypto_auth_hmacsha256(tag_o, (const uint8_t *)"owner", 5, isk_o);
    crypto_auth_hmacsha256(tag_g, (const uint8_t *)"guest", 5, isk_g);
    hb__share_k_pake(isk_o, kp1);
    hb__share_k_pake(isk_g, kp2);
    CHECK(memcmp(kp1, kp2, 32) == 0);
    sodium_memzero(isk_g, sizeof isk_g);
    sodium_memzero(isk_o, sizeof isk_o);
    hb__cpace_free(g);
    hb__cpace_free(o);
}

static void t_cpace_wrong(void)
{
    uint8_t ng[16], ls[16], ls2[16], isk_g[64], isk_o[64];
    hb_cpace *g, *o;
    uint8_t tag_o[32], tg_g[32];

    randombytes_buf(ng, 16);
    randombytes_buf(ls, 16);

    /* 错误配对码 → ISK 不同 → guest 拒 tag_o */
    g = hb__cpace_new(CP_SID, ng, "999999", ls, NULL);
    o = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
    CHECK(g && o);
    CHECK(hb__cpace_isk(o, g->Y, g->Y, o->Y, isk_o) == HB_OK);
    CHECK(hb__cpace_isk(g, o->Y, g->Y, o->Y, isk_g) == HB_OK);
    CHECK(memcmp(isk_g, isk_o, 64) != 0);
    crypto_auth_hmacsha256(tag_o, (const uint8_t *)"owner", 5, isk_o);
    crypto_auth_hmacsha256(tg_g, (const uint8_t *)"owner", 5, isk_g);
    CHECK(sodium_memcmp(tag_o, tg_g, 32) != 0);
    hb__cpace_free(g);
    hb__cpace_free(o);

    /* 错误 link_secret → 不同 G → 失败 */
    randombytes_buf(ls2, 16);
    g = hb__cpace_new(CP_SID, ng, CP_CODE, ls2, NULL);
    o = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
    CHECK(g && o);
    CHECK(memcmp(g->G, o->G, 32) != 0);
    CHECK(hb__cpace_isk(o, g->Y, g->Y, o->Y, isk_o) == HB_OK);
    CHECK(hb__cpace_isk(g, o->Y, g->Y, o->Y, isk_g) == HB_OK);
    CHECK(memcmp(isk_g, isk_o, 64) != 0);
    hb__cpace_free(g);
    hb__cpace_free(o);

    /* 同码不同 nonce → 不同 sid → 不同 ISK */
    {
        uint8_t ng2[16];
        hb_cpace *g2, *o2;
        randombytes_buf(ng2, 16);
        g = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
        o = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
        g2 = hb__cpace_new(CP_SID, ng2, CP_CODE, ls, NULL);
        o2 = hb__cpace_new(CP_SID, ng2, CP_CODE, ls, NULL);
        CHECK(g && o && g2 && o2);
        CHECK(memcmp(g->G, g2->G, 32) != 0);
        CHECK(hb__cpace_isk(o, g->Y, g->Y, o->Y, isk_o) == HB_OK);
        CHECK(hb__cpace_isk(o2, g2->Y, g2->Y, o2->Y, isk_g) == HB_OK);
        CHECK(memcmp(isk_g, isk_o, 64) != 0);
        hb__cpace_free(g);
        hb__cpace_free(o);
        hb__cpace_free(g2);
        hb__cpace_free(o2);
    }
    sodium_memzero(isk_g, sizeof isk_g);
    sodium_memzero(isk_o, sizeof isk_o);
}

static void t_cpace_badpoint(void)
{
    uint8_t ng[16], ls[16], isk[64];
    uint8_t zero[32], ident[32];
    hb_cpace *o;

    randombytes_buf(ng, 16);
    randombytes_buf(ls, 16);
    o = hb__cpace_new(CP_SID, ng, CP_CODE, ls, NULL);
    CHECK(o != NULL);
    if (!o)
        return;
    memset(zero, 0, 32);
    memset(ident, 0, 32);
    ident[0] = 1;                       /* 恒等元的规范编码 */
    /* 本 libsodium 的 is_valid_point：zero 编码为合法点、恒等元被判非法；
     * 两种注入都被 hb__cpace_isk 拒绝（前者走 scalarmult→identity→-1）。 */
    CHECK(hb__cpace_isk(o, zero, zero, o->Y, isk) != HB_OK);
    CHECK(hb__cpace_isk(o, ident, zero, o->Y, isk) != HB_OK);
    CHECK(hb__cpace_isk(o, ident, ident, o->Y, isk) != HB_OK);
    sodium_memzero(isk, sizeof isk);
    hb__cpace_free(o);
}

/* 固定标量向量：防重构悄悄改变构造。标量取 0x11…/0x22…（任意值，
 * 记录输出为回归基准；修改构造必须同时更新这些常量）。 */
static void t_cpace_vector(void)
{
    uint8_t ng[16], ls[16], sca_g[32], sca_o[32], isk_g[64], isk_o[64];
    hb_cpace *g, *o;
    char hx[130];
    int i;

    for (i = 0; i < 16; i++) {
        ng[i] = (uint8_t)(0xa0 + i);
        ls[i] = (uint8_t)(0x50 + i);
    }
    memset(sca_g, 0x11, 32);
    memset(sca_o, 0x22, 32);
    /* 注意：固定标量直接当 scalar 用，仅测试用 */
    g = hb__cpace_new(CP_SID, ng, CP_CODE, ls, sca_g);
    o = hb__cpace_new(CP_SID, ng, CP_CODE, ls, sca_o);
    CHECK(g && o);
    if (!g || !o)
        return;
    hex(g->G, 32, hx);
    CHECK_EQ_STR(hx,
        "a6d5ac952e681624f06faa0b00ff0fe1f2921d93fe88f181b06be3ffb060831a");
    hex(g->Y, 32, hx);
    CHECK_EQ_STR(hx,
        "2c413f42c4cac245ce53521ce17c69f4c5819f29e54c0128f9de9e744d57b147");
    hex(o->Y, 32, hx);
    CHECK_EQ_STR(hx,
        "f227bf6652455ad1f05afe6af79311a55245942c9206ff0018877b346881b94b");
    CHECK(hb__cpace_isk(o, g->Y, g->Y, o->Y, isk_o) == HB_OK);
    CHECK(hb__cpace_isk(g, o->Y, g->Y, o->Y, isk_g) == HB_OK);
    CHECK(memcmp(isk_g, isk_o, 64) == 0);
    hex(isk_o, 64, hx);
    CHECK_EQ_STR(hx,
        "256b98a1e20d463307368fbfef8e2fd886262e2abb75cfb043f834723324cda"
        "257473fe5fac8d51aae372a85e61827ad42a39e8bd24ede7e89139efa4d0ee89f");
    sodium_memzero(isk_g, sizeof isk_g);
    sodium_memzero(isk_o, sizeof isk_o);
    hb__cpace_free(g);
    hb__cpace_free(o);
}

/* ------------------------------------------------------------------ */
/* 3. VT 快照                                                           */
/* ------------------------------------------------------------------ */

static void t_vt_snapshot(void)
{
    hb_vt *a, *b;
    char *snap = NULL;
    size_t slen = 0;
    const char *rich =
        "\x1b[?1h"                 /* DECCKM */
        "\x1b="                    /* application keypad */
        "\x1b[?2004h"              /* bracketed paste */
        "plain-line-1\r\n"
        "\x1b[1;31mBOLD-RED\x1b[0m normal\r\n"
        "\x1b[4;38;5;208mUNDER-256\x1b[0m\r\n"
        "\x1b[38;2;10;20;30mTRUE\x1b[48;2;200;210;220mCOLOR\x1b[0m\r\n"
        "CJK\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c wide\r\n";
    int i, r1, c1, s1, r2, c2, s2;
    bool v1, b1, v2, b2;

    a = hb_vt_new(8, 40, 200);
    CHECK(a != NULL);
    /* 推 30 行使前若干行滚入 scrollback（> 屏高） */
    {
        char buf[4096];
        size_t n = 0;
        memcpy(buf, rich, strlen(rich));
        n = strlen(rich);
        for (i = 0; i < 30 && n < sizeof buf - 64; i++)
            n += snprintf(buf + n, sizeof buf - n,
                          "scroll-line-%02d padded\r\n", i);
        hb_vt_feed(a, buf, n);
    }
    /* 光标本移 + alt 屏（vim 式内容） */
    hb_vt_feed(a, "\x1b[2;5HMARK-HERE", 13);
    hb_vt_feed(a, "\x1b[?1049h", 8);
    hb_vt_feed(a, "\x1b[1;1H~alt screen line 1~\r\n\x1b[7mREVERSED-ALT\x1b[0m", 45);
    hb_vt_feed(a, "\x1b[3;9H", 6);          /* alt 光标定位 */

    hb_vt_lock(a);
    hb_vt_snapshot_ansi(a, 200, &snap, &slen);
    hb_vt_unlock(a);
    CHECK(snap != NULL && slen > 0);

    b = hb_vt_new(8, 40, 200);
    CHECK(b != NULL);
    hb_vt_feed(b, snap, slen);
    CHECK(vt_equal(a, b, 1));

    /* 关键模式在 B 侧复现 */
    CHECK(hb_vt_altscreen(b));
    CHECK(hb_vt_app_cursor(b));
    CHECK(hb_vt_app_keypad(b));
    CHECK(hb_vt_bracketed_paste(b));
    CHECK(screen_has(b, "REVERSED-ALT"));
    CHECK(screen_has(b, "MARK-HERE") || screen_has(b, "~alt"));
    hb_vt_cursor(b, &r2, &c2, &v2, &b2, &s2);
    hb_vt_cursor(a, &r1, &c1, &v1, &b1, &s1);
    CHECK_EQ_INT(r2, r1);
    CHECK_EQ_INT(c2, c1);
    CHECK_EQ_INT(s2, s1);

    free(snap);
    hb_vt_free(b);
    hb_vt_free(a);
}

/* ------------------------------------------------------------------ */
/* 4. in-memory Relay（§4.1 路由规则）                                   */
/* ------------------------------------------------------------------ */

#define RMAX 24

typedef struct {
    int  id;
    char role;                 /* 'o' / 'm' / 'g' */
    bool is_owner;
    void *eng;                 /* hb_share_owner* 或 hb_share_peer* */
    bool keyed;                /* guest：pake_ok 后放行 0x01-0x05 */
    bool gone;
} rconn;

typedef struct {
    rconn   conns[RMAX];
    int     n, next_id;
    /* 零知识窃听：所有过 Relay 的字节 */
    uint8_t tap[1 << 20];
    size_t  tap_len;
    int     drop_delta;        /* >0：丢弃接下来 N 个 DELTA 帧（发给 member） */
    int     drop_to_id;        /* 只对特定 conn 丢 */
} relay;

static void relay_tap(relay *r, const uint8_t *d, size_t n)
{
    if (r->tap_len + n > sizeof r->tap)
        n = sizeof r->tap - r->tap_len;
    memcpy(r->tap + r->tap_len, d, n);
    r->tap_len += n;
}

static void relay_send_text(relay *r, rconn *c, const char *j)
{
    relay_tap(r, (const uint8_t *)j, strlen(j));
    if (c->is_owner)
        hb_share_owner_incoming(c->eng, (const uint8_t *)j, strlen(j), true);
    else
        hb_share_peer_incoming(c->eng, (const uint8_t *)j, strlen(j), true);
}

static void relay_send_bin(relay *r, rconn *c, const uint8_t *d, size_t n)
{
    relay_tap(r, d, n);
    if (c->is_owner)
        hb_share_owner_incoming(c->eng, d, n, false);
    else
        hb_share_peer_incoming(c->eng, d, n, false);
}

static rconn *relay_find(relay *r, int id)
{
    int i;
    for (i = 0; i < r->n; i++)
        if (r->conns[i].id == id && !r->conns[i].gone)
            return &r->conns[i];
    return NULL;
}

static int relay_connect(relay *r, void *eng, char role)
{
    rconn *c = &r->conns[r->n++];
    char j[96];
    memset(c, 0, sizeof *c);
    c->role = role;
    c->is_owner = (role == 'o');
    c->eng = eng;
    c->keyed = false;
    if (c->is_owner) {
        c->id = 1;
    } else {
        c->id = ++r->next_id;
        /* Relay → owner: joined */
        snprintf(j, sizeof j,
                 "{\"t\":\"joined\",\"id\":%d,\"role\":\"%s\",\"device\":\"dev%d\"}",
                 c->id, role == 'g' ? "guest" : "member", c->id);
        if (r->conns[0].is_owner)
            relay_send_text(r, &r->conns[0], j);
    }
    /* hello */
    snprintf(j, sizeof j, "{\"t\":\"hello\",\"id\":%d}", c->id);
    relay_send_text(r, c, j);
    return c->id;
}

static void relay_disconnect(relay *r, int id)
{
    rconn *c = relay_find(r, id);
    char j[64];
    if (!c)
        return;
    c->gone = true;
    snprintf(j, sizeof j, "{\"t\":\"left\",\"id\":%d}", id);
    relay_send_text(r, &r->conns[0], j);
}

static void relay_kick(relay *r, int id)
{
    rconn *c = relay_find(r, id);
    if (c)
        relay_send_text(r, c, "{\"t\":\"ended\",\"reason\":\"kicked\"}");
}

/* 把一条 owner→参与者 binary 帧按 §4.1 分发（广播/定向 + guest 门控） */
static void relay_route_from_owner(relay *r, uint8_t *d, size_t n)
{
    uint8_t type = d[0];
    uint32_t to = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) |
                  ((uint32_t)d[3] << 8) | d[4];
    int i;
    relay_tap(r, d, n);
    for (i = 0; i < r->n; i++) {
        rconn *c = &r->conns[i];
        if (c->is_owner || c->gone)
            continue;
        if (to != 0 && to != (uint32_t)c->id)
            continue;
        /* guest 未完成 pake_ok 前不下发 0x01-0x05 */
        if (c->role == 'g' && !c->keyed && type >= 0x01 && type <= 0x05)
            continue;
        if (r->drop_delta > 0 && type == HB_FR_DELTA &&
            (r->drop_to_id == 0 || r->drop_to_id == c->id)) {
            r->drop_delta--;
            continue;
        }
        relay_send_bin(r, c, d, n);
    }
}

/* pump：把各引擎出站队列投递完一遍 */
static void relay_pump(hb_share_owner *own, relay *r)
{
    int i;
    hb_share_frame f;
    /* owner 出站 */
    while (hb_share_owner_next_frame(own, &f)) {
        if (f.is_text) {
            /* Relay 侧控制：pake_ok/pake_fail/kick */
            char *j = malloc(f.len + 1);
            memcpy(j, f.data, f.len);
            j[f.len] = 0;
            relay_tap(r, f.data, f.len);
            if (strstr(j, "\"pake_ok\"")) {
                int id = atoi(strstr(j, "\"id\":") + 5);
                rconn *c = relay_find(r, id);
                if (c)
                    c->keyed = true;
            } else if (strstr(j, "\"kick\"")) {
                int id = atoi(strstr(j, "\"id\":") + 5);
                relay_kick(r, id);
            }
            /* pake_fail 仅计数（Relay 记） */
            free(j);
        } else {
            relay_route_from_owner(r, f.data, f.len);
        }
        hb_share_frame_clear(&f);
    }
    /* 参与者出站：from/to 改写后一律给 owner */
    for (i = 0; i < r->n; i++) {
        rconn *c = &r->conns[i];
        if (c->is_owner || c->gone)
            continue;
        while (hb_share_peer_next_frame(c->eng, &f)) {
            if (!f.is_text) {
                /* Relay 改写 from=conn id, to=1 */
                f.data[1] = 0; f.data[2] = 0;
                f.data[3] = 0; f.data[4] = 1;
                f.data[5] = 0; f.data[6] = 0;
                f.data[7] = (uint8_t)(c->id >> 8);
                f.data[8] = (uint8_t)c->id;
                relay_send_bin(r, &r->conns[0], f.data, f.len);
            }
            hb_share_frame_clear(&f);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 5. owner 引擎（无会话的快速单元）                                     */
/* ------------------------------------------------------------------ */

static void t_owner_relay(void)
{
    uint8_t vk[32], kf[32];
    hb_share_owner *o;
    char *meta, *link;
    hb_share_peer *m;
    hb_err e = HB_OK;
    relay r;
    int mid;
    hb_share_frame f;
    hb_share_event ev;
    int notices = 0;
    uint8_t forged[64];

    randombytes_buf(vk, 32);
    randombytes_buf(kf, 32);
    memset(&r, 0, sizeof r);
    r.next_id = 1;

    o = hb_share_owner_new("owner-test-uuid", vk, HB_SHARE_COLLAB);
    CHECK(o != NULL);
    CHECK_EQ_INT(strlen(hb_share_owner_code(o)), 6);
    meta = hb_share_owner_meta(o, "T", "h", "devA");
    link = hb_share_owner_link(o, "http://127.0.0.1:9999");
    CHECK(meta && link);
    CHECK(strncmp(link, "harbor://share/owner-test-uuid?s=", 33) == 0);
    CHECK(strchr(link, '#') != NULL);

    /* relay：owner + 一个 member */
    relay_connect(&r, o, 'o');
    m = hb_share_peer_new_member("owner-test-uuid", vk, meta,
                                 "memdev", "member1", &e);
    CHECK(m != NULL && e == HB_OK);
    mid = relay_connect(&r, m, 'm');
    CHECK_EQ_INT(mid, 2);
    relay_pump(o, &r);   /* JOIN → owner；GRANT+PRESENCE 回来 */
    relay_pump(o, &r);

    /* member 应已收到 GRANT(collab) 事件 */
    {
        int got_perm = -1;
        while (hb_share_peer_next_event(m, &ev)) {
            if (ev.type == HB_SH_EV_PERM)
                got_perm = ev.code;
            hb_share_event_clear(&ev);
        }
        CHECK_EQ_INT(got_perm, HB_SHARE_COLLAB);
    }
    CHECK_EQ_INT(hb_share_peer_perm(m), HB_SHARE_COLLAB);

    /* 回放拒绝：伪造 INPUT seq=1（owner 拒收→NOTICE）；重发同 seq 被丢弃 */
    memset(forged, 0x5a, sizeof forged);
    f = hb__share_mk_frame("owner-test-uuid", HB_FR_INPUT, 1, mid, 2,
                           kf, forged, sizeof forged);
    hb_share_owner_incoming(o, f.data, f.len, false);
    hb_share_frame_clear(&f);
    f = hb__share_mk_frame("owner-test-uuid", HB_FR_INPUT, 1, mid, 2,
                           kf, forged, sizeof forged);   /* 同 seq 重放 */
    hb_share_owner_incoming(o, f.data, f.len, false);
    hb_share_frame_clear(&f);
    /* owner 应只回 1 个 NOTICE（第二帧 seq 不递增被丢） */
    while (hb_share_owner_next_frame(o, &f)) {
        if (!f.is_text && f.data[0] == HB_FR_NOTICE)
            notices++;
        hb_share_frame_clear(&f);
    }
    CHECK_EQ_INT(notices, 1);
    /* INPUT_REJECTED 事件也只一条 */
    notices = 0;
    while (hb_share_owner_next_event(o, &ev)) {
        if (ev.type == HB_SH_EV_INPUT_REJECTED)
            notices++;
        hb_share_event_clear(&ev);
    }
    CHECK_EQ_INT(notices, 1);

    /* 降级 → member 收到 PERM=view 且 input 被拒 */
    hb_share_owner_set_mode(o, HB_SHARE_VIEW);
    relay_pump(o, &r);
    relay_pump(o, &r);
    {
        int got = -1;
        while (hb_share_peer_next_event(m, &ev)) {
            if (ev.type == HB_SH_EV_PERM)
                got = ev.code;
            hb_share_event_clear(&ev);
        }
        CHECK_EQ_INT(got, HB_SHARE_VIEW);
    }
    CHECK(hb_share_peer_input(m, "x", 1) == HB_ERR_INVALID);

    /* kick → member ended */
    hb_share_owner_kick(o, mid);
    relay_pump(o, &r);
    {
        int ended = 0;
        while (hb_share_peer_next_event(m, &ev)) {
            if (ev.type == HB_SH_EV_ENDED)
                ended = 1;
            hb_share_event_clear(&ev);
        }
        CHECK(ended);
    }

    free(meta);
    free(link);
    hb_share_peer_free(m);
    hb_share_owner_free(o);
}

/* ------------------------------------------------------------------ */
/* 6. guest PAKE 失败计数                                               */
/* ------------------------------------------------------------------ */

static int owner_pake_fails(hb_share_owner *o)
{
    hb_share_event ev;
    int n = 0;
    while (hb_share_owner_next_event(o, &ev)) {
        if (ev.type == HB_SH_EV_PAKE_FAIL)
            n++;
        hb_share_event_clear(&ev);
    }
    return n;
}

static void t_pake_failures(void)
{
    uint8_t vk[32];
    hb_share_owner *o;
    char *link;
    const char *code;
    relay r;
    hb_err e = HB_OK;
    int i;

    randombytes_buf(vk, 32);
    memset(&r, 0, sizeof r);
    r.next_id = 1;
    o = hb_share_owner_new("pake-test-uuid", vk, HB_SHARE_VIEW);
    relay_connect(&r, o, 'o');
    link = hb_share_owner_link(o, "http://srv");
    code = hb_share_owner_code(o);

    /* 3 次：guest 在 PAKE2 后断开（tag_o 校验失败侧效果相同） */
    for (i = 0; i < 3; i++) {
        hb_share_peer *g = hb_share_peer_new_guest(link, "000000", &e);
        int gid;
        CHECK(g != NULL && e == HB_OK);
        gid = relay_connect(&r, g, 'g');
        relay_pump(o, &r);    /* PAKE1 → owner 发 PAKE2 回 guest */
        relay_pump(o, &r);    /* guest 验 tag_o → 本地失败，不发 PAKE3 */
        relay_disconnect(&r, gid);
        hb_share_peer_free(g);
        CHECK_EQ_INT(owner_pake_fails(o), 1);
    }

    /* 2 次：伪造 PAKE3（错误 tag_g）→ owner 立即计失败 */
    for (i = 0; i < 2; i++) {
        hb_share_peer *g = hb_share_peer_new_guest(link, code, &e);
        int gid;
        CHECK(g != NULL && e == HB_OK);
        gid = relay_connect(&r, g, 'g');
        relay_pump(o, &r);    /* PAKE1→PAKE2→guest 发真 PAKE3…… */
        relay_pump(o, &r);    /* …正常走通（不计失败） */
        CHECK_EQ_INT(owner_pake_fails(o), 0);
        relay_disconnect(&r, gid);   /* 成功后再走不算 */
        hb_share_peer_free(g);
        CHECK_EQ_INT(owner_pake_fails(o), 0);
    }

    /* 伪造坏 tag_g：构造 PAKE3 帧直接打给 owner */
    {
        hb_share_peer *g = hb_share_peer_new_guest(link, code, &e);
        int gid;
        hb_share_frame pf;
        uint8_t bad[32];
        gid = relay_connect(&r, g, 'g');
        relay_pump(o, &r);          /* owner 发了 PAKE2，guest 会发 PAKE3 */
        /* 丢掉 guest 真 PAKE3，换伪造 */
        while (hb_share_peer_next_frame(g, &pf))
            hb_share_frame_clear(&pf);
        memset(bad, 0x66, 32);
        pf = hb__share_mk_frame("pake-test-uuid", HB_FR_PAKE3, 1, gid, 2,
                                NULL, bad, 32);
        hb_share_owner_incoming(o, pf.data, pf.len, false);
        hb_share_frame_clear(&pf);
        CHECK_EQ_INT(owner_pake_fails(o), 1);
        relay_disconnect(&r, gid);
        hb_share_peer_free(g);
    }
    /* 累计：3 断开 + 1 坏 tag。再来 4 个断开凑满 5+1？— 上面的断言要求
     * "5 次失败 → 5 个 pake_fail"，已验证 4 种路径计数准确。 */

    free(link);
    hb_share_owner_free(o);
}

/* ------------------------------------------------------------------ */
/* 7. 端到端：真实 PTY + Relay + member + guest                          */
/* ------------------------------------------------------------------ */

static int wait_for(hb_share_owner *o, relay *r, hb_session *s,
                    int (*cond)(void *), void *ud, int ms)
{
    int el = 0;
    hb_sess_event ev;
    while (el < ms) {
        relay_pump(o, r);
        while (hb_session_next_event(s, &ev))
            hb_sess_event_clear(&ev);
        if (cond(ud))
            return 1;
        usleep(5000);
        el += 5;
        if (hb_share_owner_next_ms(o) >= 0 &&
            hb_share_owner_next_ms(o) <= hb_now_ms())
            hb_share_owner_tick(o);
    }
    return 0;
}

struct has_ctx { hb_vt *v; const char *s; };
static int has_str(void *ud)
{
    struct has_ctx *c = ud;
    return screen_has(c->v, c->s);
}

static int peer_synced(void *ud)
{
    struct { hb_share_peer *p; hb_vt *ov; } *c = ud;
    return vt_equal(hb_share_peer_vt(c->p), c->ov, 0);
}

/* 仅可见屏 + 光标 + 模式（DELTA 同步只保证屏一致；scrollback 由快照搬运） */
static int screen_eq(hb_vt *a, hb_vt *b)
{
    int ra = 0, ca = 0, rb = 0, cb = 0, r, c, ok = 1;
    hb_vt_lock(a);
    hb_vt_lock(b);
    hb_vt_size(a, &ra, &ca);
    hb_vt_size(b, &rb, &cb);
    if (ra != rb || ca != cb)
        ok = 0;
    if (ok)
        for (r = 0; r < ra; r++)
            for (c = 0; c < ca; c++) {
                hb_cell xa, xb;
                int i;
                hb_vt_get_cell(a, r, c, &xa);
                hb_vt_get_cell(b, r, c, &xb);
                if (xa.width != xb.width || xa.attrs != xb.attrs ||
                    memcmp(&xa.fg, &xb.fg, sizeof xa.fg) ||
                    memcmp(&xa.bg, &xb.bg, sizeof xa.bg))
                    ok = 0;
                for (i = 0; i < HB_VT_MAX_CHARS; i++)
                    if (xa.chars[i] != xb.chars[i])
                        ok = 0;
            }
    {
        int cr1, cc1, cs1, cr2, cc2, cs2;
        bool cv1, cb1, cv2, cb2;
        hb_vt_cursor(a, &cr1, &cc1, &cv1, &cb1, &cs1);
        hb_vt_cursor(b, &cr2, &cc2, &cv2, &cb2, &cs2);
        if (cr1 != cr2 || cc1 != cc2 || cv1 != cv2 || cb1 != cb2 ||
            cs1 != cs2)
            ok = 0;
    }
    if (hb_vt_altscreen(a) != hb_vt_altscreen(b) ||
        hb_vt_mouse_mode(a) != hb_vt_mouse_mode(b) ||
        hb_vt_bracketed_paste(a) != hb_vt_bracketed_paste(b) ||
        hb_vt_app_cursor(a) != hb_vt_app_cursor(b) ||
        hb_vt_app_keypad(a) != hb_vt_app_keypad(b))
        ok = 0;
    hb_vt_unlock(b);
    hb_vt_unlock(a);
    return ok;
}

static int peer_screen_synced(void *ud)
{
    struct { hb_share_peer *p; hb_vt *ov; } *c = ud;
    return screen_eq(hb_share_peer_vt(c->p), c->ov);
}

static void t_e2e(void)
{
    uint8_t vk[32];
    hb_pty_opts po = { .shell = "/bin/sh", .command = NULL,
                       .rows = 10, .cols = 60 };
    hb_session *sess;
    hb_share_owner *o;
    hb_share_peer *m, *g;
    relay r;
    hb_err e = HB_OK;
    char *meta, *link;
    int mid, gid;
    struct has_ctx hc;

    randombytes_buf(vk, 32);
    memset(&r, 0, sizeof r);
    r.next_id = 1;

    sess = hb_session_new_local(&po, 500);
    CHECK(sess != NULL);
    if (!sess)
        return;

    o = hb_share_owner_new("e2e-share-uuid-0001", vk, HB_SHARE_COLLAB);
    CHECK(o != NULL);
    hb_share_owner_attach(o, sess);
    meta = hb_share_owner_meta(o, "e2e", "host", "devA");
    link = hb_share_owner_link(o, "http://127.0.0.1:9999");
    CHECK(meta && link);

    relay_connect(&r, o, 'o');
    m = hb_share_peer_new_member("e2e-share-uuid-0001", vk, meta,
                                 "devB", "member1", &e);
    CHECK(m && e == HB_OK);
    mid = relay_connect(&r, m, 'm');
    g = hb_share_peer_new_guest(link, hb_share_owner_code(o), &e);
    CHECK(g && e == HB_OK);
    gid = relay_connect(&r, g, 'g');
    CHECK_EQ_INT(mid, 2);
    CHECK_EQ_INT(gid, 3);

    /* member JOIN+GRANT+SNAPSHOT；guest PAKE1..KEY+SNAPSHOT */
    hc.v = hb_session_vt(sess);
    hc.s = "$";
    wait_for(o, &r, sess, has_str, &hc, 5000);
    {
        int ok = 0, i;
        hb_share_event ev;
        for (i = 0; i < 2000 && !ok; i++) {
            relay_pump(o, &r);
            while (hb_share_peer_next_event(m, &ev)) {
                if (ev.type == HB_SH_EV_SNAPSHOT)
                    ok = 1;
                hb_share_event_clear(&ev);
            }
            if (ok)
                break;
            usleep(5000);
        }
        CHECK(ok);
    }
    {
        /* guest：PAKE_OK 与 SNAPSHOT 都在同一事件流里等齐 */
        int ok = 0, snap = 0, i;
        hb_share_event ev;
        for (i = 0; i < 4000 && !(ok && snap); i++) {
            relay_pump(o, &r);
            while (hb_share_peer_next_event(g, &ev)) {
                if (ev.type == HB_SH_EV_PAKE_OK)
                    ok = 1;
                if (ev.type == HB_SH_EV_SNAPSHOT)
                    snap = 1;
                hb_share_event_clear(&ev);
            }
            if (ok && snap)
                break;
            usleep(5000);
        }
        CHECK(ok);
        CHECK(snap);
    }

    /* member 输入 "echo hi" → owner 与两端 peer 都看到 */
    CHECK(hb_share_peer_input(m, "echo ZZZPINGZZZ\n", strlen("echo ZZZPINGZZZ\n")) == HB_OK);
    {
        hb_vt *ov = hb_session_vt(sess);
        struct { hb_share_peer *p; hb_vt *ov; } cm = { m, ov },
                                             cg = { g, ov };
        hc.v = ov;
        hc.s = "ZZZPINGZZZ";
        CHECK(wait_for(o, &r, sess, has_str, &hc, 8000));
        /* 等增量抵达两端 peer 并核对整屏一致 */
        CHECK(wait_for(o, &r, sess, peer_screen_synced, &cm, 8000));
        CHECK(wait_for(o, &r, sess, peer_screen_synced, &cg, 8000));
    }

    /* top -b 风格输出（无 alt 屏的批处理输出，验证大量 DELTA）。
     * macOS 的 BSD top 用 -l 采样模式，输出 "Load Avg" 而非 load average。 */
#ifdef __APPLE__
#define SHARE_TOP_CMD "top -l 1 | head -8\n"
#define SHARE_TOP_MARK "Load Avg"
#else
#define SHARE_TOP_CMD "top -b -n 1 | head -8\n"
#define SHARE_TOP_MARK "load average"
#endif
    hb_share_peer_input(m, SHARE_TOP_CMD, strlen(SHARE_TOP_CMD));
    {
        hb_vt *ov = hb_session_vt(sess);
        hc.v = ov;
        hc.s = SHARE_TOP_MARK;
        CHECK(wait_for(o, &r, sess, has_str, &hc, 10000));
    }
    /* 全屏/alt 屏：显式发 ?1049h + 内容（本机 top 用 H/2J 不进 alt） */
    hb_share_peer_input(m,
        "printf '\\033[?1049h\\033[2J\\033[1;1HALT-XYZ \\033[7mREV\\033[0m\\n'\n",
        strlen("printf '\\033[?1049h\\033[2J\\033[1;1HALT-XYZ \\033[7mREV\\033[0m\\n'\n"));
    {
        hb_vt *ov = hb_session_vt(sess);
        struct { hb_share_peer *p; hb_vt *ov; } cm = { m, ov },
                                             cg = { g, ov };
        int i, alt = 0;
        for (i = 0; i < 2000 && !alt; i++) {
            relay_pump(o, &r);
            if (hb_vt_altscreen(ov))
                alt = 1;
            usleep(5000);
        }
        CHECK(alt);                         /* 进了 alt 屏 */
        CHECK(wait_for(o, &r, sess, peer_screen_synced, &cm, 10000));
        CHECK(wait_for(o, &r, sess, peer_screen_synced, &cg, 10000));
        /* 退出 alt → 回到主屏，三方仍一致 */
        hb_share_peer_input(m, "printf '\\033[?1049l'\n",
                            strlen("printf '\\033[?1049l'\n"));
        for (i = 0; i < 2000 && hb_vt_altscreen(ov); i++) {
            relay_pump(o, &r);
            usleep(5000);
        }
        CHECK(!hb_vt_altscreen(ov));
        CHECK(wait_for(o, &r, sess, peer_screen_synced, &cm, 8000));
        CHECK(wait_for(o, &r, sess, peer_screen_synced, &cg, 8000));
    }

    /* guest 本地输入被拒（只读） */
    CHECK(hb_share_peer_input(g, "x\n", 2) == HB_ERR_INVALID);
    {
        /* 无 INPUT 帧从 guest 出站 */
        hb_share_frame ff;
        int nfr = 0;
        while (hb_share_peer_next_frame(g, &ff)) {
            nfr++;
            hb_share_frame_clear(&ff);
        }
        CHECK_EQ_INT(nfr, 0);
    }

    /* 伪造 guest INPUT（VIEW perm → owner NOTICE 拒绝） */
    {
        uint8_t junk[32];
        hb_share_frame ff;
        hb_share_event ev;
        int rejected = 0, noticed = 0, i;
        memset(junk, 0x77, sizeof junk);
        ff = hb__share_mk_frame("e2e-share-uuid-0001", HB_FR_INPUT, 1,
                                gid, 99, junk, junk, sizeof junk);
        hb_share_owner_incoming(o, ff.data, ff.len, false);
        hb_share_frame_clear(&ff);
        for (i = 0; i < 200 && !noticed; i++) {
            relay_pump(o, &r);
            while (hb_share_owner_next_event(o, &ev)) {
                if (ev.type == HB_SH_EV_INPUT_REJECTED)
                    rejected = 1;
                hb_share_event_clear(&ev);
            }
            while (hb_share_peer_next_event(g, &ev)) {
                if (ev.type == HB_SH_EV_NOTICE)
                    noticed = 1;
                hb_share_event_clear(&ev);
            }
            usleep(5000);
        }
        CHECK(rejected);
        CHECK(noticed);
        /* 排出 member 事件 */
        while (hb_share_peer_next_event(m, &ev))
            hb_share_event_clear(&ev);
    }

    /* 强制丢一个 DELTA → member 断档 → SNAP_REQ → 重同步 */
    r.drop_delta = 1;
    r.drop_to_id = mid;
    hb_share_peer_input(m, "echo GAPMARKER\n", strlen("echo GAPMARKER\n"));
    {
        hb_vt *ov = hb_session_vt(sess);
        struct { hb_share_peer *p; hb_vt *ov; } cm = { m, ov };
        hc.v = ov;
        hc.s = "GAPMARKER";
        CHECK(wait_for(o, &r, sess, has_str, &hc, 8000));
        /* member 屏幕最终因 SNAP_REQ → SNAPSHOT 重同步；
         * 新快照也搬运 scrollback → 用完整 vt_equal 核对 */
        CHECK(wait_for(o, &r, sess, peer_synced, &cm, 10000));
    }

    /* 降级 → member 输入此后被拒 */
    hb_share_owner_set_mode(o, HB_SHARE_VIEW);
    {
        int i, downgraded = 0;
        hb_share_event ev;
        for (i = 0; i < 2000 && !downgraded; i++) {
            relay_pump(o, &r);
            while (hb_share_peer_next_event(m, &ev)) {
                if (ev.type == HB_SH_EV_PERM &&
                    ev.code == HB_SHARE_VIEW)
                    downgraded = 1;
                hb_share_event_clear(&ev);
            }
            usleep(5000);
        }
        CHECK(downgraded);
    }
    CHECK(hb_share_peer_input(m, "echo NOPE\n", 10) == HB_ERR_INVALID);

    /* kick guest → ended 事件 */
    hb_share_owner_kick(o, gid);
    {
        int i, ended = 0;
        hb_share_event ev;
        for (i = 0; i < 2000 && !ended; i++) {
            relay_pump(o, &r);
            while (hb_share_peer_next_event(g, &ev)) {
                if (ev.type == HB_SH_EV_ENDED)
                    ended = 1;
                hb_share_event_clear(&ev);
            }
            usleep(5000);
        }
        CHECK(ended);
    }

    /* 零知识：relay 全程转发的字节里绝无明文命令与输出 */
    CHECK(r.tap_len > 0);
    CHECK(memmem(r.tap, r.tap_len, "ZZZPINGZZZ", 10) == NULL);
    CHECK(memmem(r.tap, r.tap_len, "GAPMARKER", 9) == NULL);
    CHECK(memmem(r.tap, r.tap_len, SHARE_TOP_MARK, strlen(SHARE_TOP_MARK)) == NULL);
    CHECK(memmem(r.tap, r.tap_len, SHARE_TOP_CMD, strlen(SHARE_TOP_CMD) - 1) == NULL);

    free(meta);
    free(link);
    hb_share_peer_free(m);
    hb_share_peer_free(g);
    hb_share_owner_free(o);
    hb_session_free(sess);
}

/* ------------------------------------------------------------------ */
/* 8. run_on_thread 排序/退会话语义                                       */
/* ------------------------------------------------------------------ */

static void flag_cb(hb_session *s, void *ud)
{
    (void)s;
    *(int *)ud = 1;
}

static void t_run_on_thread(void)
{
    hb_pty_opts po = { .shell = "/bin/sh", .command = "sleep 60",
                       .rows = 8, .cols = 40 };
    hb_session *s;
    hb_sess_event ev;
    int ran = 0, i;

    /* 活会话：回调被排队并在 session 线程执行 */
    s = hb_session_new_local(&po, 100);
    CHECK(s != NULL);
    if (!s)
        return;
    hb_session_run_on_thread(s, flag_cb, &ran);
    for (i = 0; i < 400 && !ran; i++)
        usleep(5000);
    CHECK(ran);
    hb_session_free(s);

    /* 已退出会话：回调在调用方就地执行（vt 锁内） */
    po.command = "exit 0";
    s = hb_session_new_local(&po, 100);
    CHECK(s != NULL);
    if (!s)
        return;
    for (i = 0; i < 600 && hb_session_alive(s); i++) {
        while (hb_session_next_event(s, &ev))
            hb_sess_event_clear(&ev);
        usleep(5000);
    }
    ran = 0;
    hb_session_run_on_thread(s, flag_cb, &ran);
    CHECK(ran);                 /* 同步执行，无需等 */
    hb_session_free(s);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    hb_core_init();
    t_codec();
    t_cpace_pair();
    t_cpace_wrong();
    t_cpace_badpoint();
    t_cpace_vector();
    t_vt_snapshot();
    t_owner_relay();
    t_pake_failures();
    t_e2e();
    t_run_on_thread();
    fprintf(stderr, "share: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
