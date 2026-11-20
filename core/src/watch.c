/* hb_watch — 命令完成监测引擎（FR-13）。纯 C、无线程，时间由调用方注入。
 *
 * 行模型：一维覆盖缓冲。可打印字节在 pos 处覆写（len = max(len,pos)），
 * '\r' 令 pos=0（进度行覆写），'\n' 提交当前行交给规则匹配并清空，
 * CSI K/0K 在 pos 处截断、2K 清空。匹配对"已提交行 + 静默 ≥300ms 的当前
 * 部分行"生效，以覆盖 wget/git 进度条这类无换行的终结输出。
 *
 * 任务与武装：用户输入或 133 C 开启一个任务（armed）。规则/空闲/OSC133
 * 命中需要 armed；命中后解除武装直到下一次输入或 C —— 每个任务至多一
 * 次命中，避免 spinner/重绘反复触发。EOF 与 OSC 9/777 始终上报（应用显
 * 式请求/会话结束），BEL 不需要 armed 但命中后同样解除武装。
 *
 * 规则命中为两段式：匹配 → pending（latest matching line wins）→
 * confirm_quiet_ms 无输出后确认入队；期间 >3 条换行结尾的非匹配行取消
 * 之（持续流式输出），部分行不计入。133 D 是同任务权威结果，到达时丢弃
 * pending。
 */

#include "harbor/watch.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HB_LINE_CAP      16384
#define HB_HIT_LINE_CAP  200
#define HB_PARTIAL_QUIET_MS 300
#define HB_BELL_RATE_MS  5000

/* ---------------------------------------------------------------- presets */

static const hb_watch_rule hb__presets[] = {
    { "aider",   "^Tokens: .* sent, .* received", NULL },
    { "pytest",  "^=+ .*[0-9]+ passed.* in [0-9.]+s",
                 "^=+ .*[0-9]+ (failed|error|errors).* in [0-9.]+s" },
    { "cargo",   "^ *Finished .* in [0-9.]+s", "^error: could not compile" },
    { "make",    NULL, "^make(\\[[0-9]+\\])?: \\*\\*\\* .*Error [0-9]+" },
    { "vite",    "✓ built in [0-9.]+", NULL },
    /* "compiled successfully" 需避开 "recompiled"/"completed successfully" */
    { "webpack", "(^|[^A-Za-z])compiled successfully",
                 "(^|[^A-Za-z])compiled with [0-9]+ error" },
    { "npm",     NULL, "^npm (ERR!|error)" },
    /* go 输出以 TAB 分隔；"FAILED xxx" 不得误中 ^FAIL */
    { "go-test", "^ok[[:space:]]+[^[:space:]]+[[:space:]]+[0-9.]+s",
                 "^FAIL([[:space:]]|$)" },
    { "wget",    "saved \\[[0-9]+(/[0-9]+)?\\]", NULL },
    { "git",     "^Resolving deltas: 100%.*done", NULL },
    /* 通用完成标志放最后；必须有显式 ✓/✔ 前缀，否则 AI 叙述文本会误中 */
    { "generic-done", "^(✔|✓) .*(completed|done|finished)", NULL },
};

const hb_watch_rule *hb_watch_presets(size_t *n)
{
    if (n) *n = sizeof hb__presets / sizeof hb__presets[0];
    return hb__presets;
}

/* ---------------------------------------------------------------- structs */

typedef struct {
    char   *name;
    regex_t ok, err;
    bool    has_ok, has_err;
} hb__wrule;

typedef struct {
    hb_watch_hit h;
} hb__qh;

enum { ST_TEXT, ST_ESC, ST_CSI, ST_OSC, ST_DCS, ST_ESC_I,
       ST_OSC_ESC, ST_DCS_ESC };

struct hb_watch {
    unsigned signals;
    int64_t  min_duration_ms, confirm_quiet_ms;
    int64_t  idle_active_ms, idle_quiet_ms, bell_after_input_ms;
    bool     idle_on;

    hb__wrule *rules;
    size_t     nrules;

    /* escape 解析器 */
    int    st;
    size_t str_guard;                    /* OSC/DCS 长度保护 */
    char   csi_param[24]; size_t csi_plen;

    /* 行缓冲（覆盖模型） */
    char   *line; size_t len, pos;
    bool    line_dirty;
    int64_t line_change;

    bool    alt;

    /* 任务 */
    bool    armed;
    int64_t task_start;                  /* -1 = 无 */
    int64_t last_input;                  /* -1 = 无 */
    int64_t any_out;                     /* 最近一次输出活动 */
    int64_t first_out, last_out;         /* 本任务输出活动窗口 */

    /* 133 B..C 回显捕获 */
    bool    in_bc, c_seen;
    char   *cmd; size_t cmd_len, cmd_cap;
    char   *last_cmd;

    /* pending 规则命中 */
    bool    pend, pend_ok;
    char    pend_rule[96];
    char    pend_line[HB_HIT_LINE_CAP + 1];
    int     pend_miss;

    int64_t last_bell;
    bool    eof_done;

    hb__qh *q; size_t qh, qt, qc;
};

/* ---------------------------------------------------------------- helpers */

void hb_watch_cfg_defaults(hb_watch_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->signals             = HB_WATCH_SIG_EOF | HB_WATCH_SIG_OSC133 |
                             HB_WATCH_SIG_RULES | HB_WATCH_SIG_TERMNOTIFY;
    c->presets             = true;
    c->min_duration_ms     = 5000;
    c->confirm_quiet_ms    = 1500;
    c->idle_active_ms      = 10000;
    c->idle_quiet_ms       = 5000;
    c->bell_after_input_ms = 2000;
}

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void trim_span(const char *s, size_t n, const char **os, size_t *on)
{
    size_t a = 0, b = n;
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    *os = s + a;
    *on = b - a;
}

void hb_watch_hit_clear(hb_watch_hit *h)
{
    if (!h) return;
    free(h->rule);
    free(h->line);
    free(h->command);
    free(h->title);
    memset(h, 0, sizeof *h);
}

static void qdrop(hb__qh *e) { hb_watch_hit_clear(&e->h); }

static bool qpush(hb_watch *w, const hb_watch_hit *h)
{
    if (w->qt - w->qh >= w->qc) {
        hb__qh *nq;
        size_t nc = w->qc ? w->qc * 2 : 8;
        if (w->qh) {                       /* 有空洞先搬移而非扩容 */
            memmove(w->q, w->q + w->qh, (w->qt - w->qh) * sizeof *w->q);
            w->qt -= w->qh; w->qh = 0;
            if (w->qt < w->qc) { w->q[w->qt++].h = *h; return true; }
        }
        nq = realloc(w->q, nc * sizeof *nq);
        if (!nq) return false;
        w->q = nq;
        w->qc = nc;
    }
    w->q[w->qt++].h = *h;
    return true;
}

static bool qpop(hb_watch *w, hb_watch_hit *out)
{
    if (w->qh >= w->qt) return false;
    *out = w->q[w->qh++].h;
    if (w->qh == w->qt) w->qh = w->qt = 0;
    return true;
}

/* ---------------------------------------------------------------- rules */

static int add_rules(hb_watch *w, const hb_watch_rule *rs, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        hb__wrule *r = &w->rules[w->nrules];
        memset(r, 0, sizeof *r);
        r->name = strdup(rs[i].name ? rs[i].name : "?");
        if (!r->name) return -1;
        if (rs[i].ok_re) {
            if (regcomp(&r->ok, rs[i].ok_re, REG_EXTENDED | REG_ICASE | REG_NOSUB))
                return -1;
            r->has_ok = true;
        }
        if (rs[i].err_re) {
            if (regcomp(&r->err, rs[i].err_re,
                        REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
                if (r->has_ok) regfree(&r->ok);
                return -1;
            }
            r->has_err = true;
        }
        w->nrules++;
    }
    return 0;
}

hb_watch *hb_watch_new(const hb_watch_cfg *c)
{
    hb_watch_cfg cc;
    hb_watch *w;
    size_t i, cap;

    if (c) cc = *c; else hb_watch_cfg_defaults(&cc);
    w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->signals            = cc.signals;
    w->min_duration_ms    = cc.min_duration_ms    >= 0 ? cc.min_duration_ms    : 5000;
    w->confirm_quiet_ms   = cc.confirm_quiet_ms   >= 0 ? cc.confirm_quiet_ms   : 1500;
    w->idle_active_ms     = cc.idle_active_ms     >= 0 ? cc.idle_active_ms     : 10000;
    w->idle_quiet_ms      = cc.idle_quiet_ms      >= 0 ? cc.idle_quiet_ms      : 5000;
    w->bell_after_input_ms = cc.bell_after_input_ms >= 0 ? cc.bell_after_input_ms : 2000;
    w->idle_on            = !!(w->signals & HB_WATCH_SIG_IDLE);
    w->task_start = w->last_input = w->any_out = -1;
    w->first_out = w->last_out = -1;
    w->last_bell = -1;

    cap = (cc.presets ? sizeof hb__presets / sizeof hb__presets[0] : 0) + cc.nrules;
    w->rules = calloc(cap ? cap : 1, sizeof *w->rules);
    if (!w->rules) { free(w); return NULL; }

    /* 用户规则在前，可覆盖同名预设语义 */
    if (add_rules(w, cc.rules, cc.nrules) ||
        (cc.presets && add_rules(w, hb__presets,
                                 sizeof hb__presets / sizeof hb__presets[0]))) {
        for (i = 0; i < cap; i++) {
            free(w->rules[i].name);
            if (w->rules[i].has_ok)  regfree(&w->rules[i].ok);
            if (w->rules[i].has_err) regfree(&w->rules[i].err);
        }
        free(w->rules);
        free(w);
        return NULL;
    }
    return w;
}

void hb_watch_free(hb_watch *w)
{
    size_t i;
    if (!w) return;
    for (i = 0; i < w->nrules; i++) {
        free(w->rules[i].name);
        if (w->rules[i].has_ok)  regfree(&w->rules[i].ok);
        if (w->rules[i].has_err) regfree(&w->rules[i].err);
    }
    free(w->rules);
    free(w->line);
    free(w->cmd);
    free(w->last_cmd);
    while (w->qh < w->qt) qdrop(&w->q[w->qh++]);
    free(w->q);
    free(w);
}

/* ---------------------------------------------------------------- matching */

/* s 为已 strip 的可见文本（长度 n，可能含内嵌 NUL 之外任意字节）。 */
static bool rule_match(hb_watch *w, const char *s, size_t n,
                       const char **name, bool *ok)
{
    size_t i;
    char  *tmp;
    if (!(w->signals & HB_WATCH_SIG_RULES)) return false;
    tmp = dup_n(s, n);
    if (!tmp) return false;
    for (i = 0; i < w->nrules; i++) {
        hb__wrule *r = &w->rules[i];
        if (r->has_err && !regexec(&r->err, tmp, 0, NULL, 0)) {
            *name = r->name; *ok = false; free(tmp); return true;
        }
        if (r->has_ok && !regexec(&r->ok, tmp, 0, NULL, 0)) {
            *name = r->name; *ok = true;  free(tmp); return true;
        }
    }
    free(tmp);
    return false;
}

static void disarm(hb_watch *w)
{
    w->armed = false;
    w->pend  = false;
    w->pend_miss = 0;
}

/* 对已提交行 / 静默部分行做规则匹配；partial 行命中同样置 pending。
 * 任务时长在匹配点判定（fast-fail 的任务不该在静默后补报）。 */
static void try_match(hb_watch *w, const char *s, size_t n,
                      bool partial, int64_t now)
{
    const char *name;
    const char *ts; size_t tn;
    bool ok;

    if (!w->armed || w->alt) return;
    if (w->task_start < 0 || now - w->task_start < w->min_duration_ms) {
        if (!partial && w->pend && ++w->pend_miss > 3) w->pend = false;
        return;
    }
    if (rule_match(w, s, n, &name, &ok)) {
        w->pend = true;
        w->pend_ok = ok;
        snprintf(w->pend_rule, sizeof w->pend_rule, "%s", name);
        trim_span(s, n, &ts, &tn);
        if (tn > HB_HIT_LINE_CAP) tn = HB_HIT_LINE_CAP;
        memcpy(w->pend_line, ts, tn);
        w->pend_line[tn] = 0;
        w->pend_miss = 0;
    } else if (!partial && w->pend && ++w->pend_miss > 3) {
        w->pend = false;
    }
}

/* ---------------------------------------------------------------- stream */

static void activity(hb_watch *w, int64_t now)
{
    w->any_out = now;
    if (w->armed) {
        if (w->first_out < 0) w->first_out = now;
        w->last_out = now;
    }
}

static void line_reset(hb_watch *w) { w->len = w->pos = 0; }

static void put_ch(hb_watch *w, char c, int64_t now)
{
    if (w->pos >= HB_LINE_CAP - 1) return;
    if (!w->line) {
        w->line = malloc(HB_LINE_CAP);
        if (!w->line) return;
    }
    w->line[w->pos++] = c;
    if (w->pos > w->len) w->len = w->pos;
    w->line_dirty = true;
    w->line_change = now;
    if ((unsigned char)c > ' ') activity(w, now);
}

static void commit_line(hb_watch *w, int64_t now)
{
    const char *ts; size_t tn;
    if (!w->line || !w->len) { line_reset(w); return; }
    trim_span(w->line, w->len, &ts, &tn);
    if (tn) try_match(w, ts, tn, false, now);
    line_reset(w);
    w->line_dirty = false;
}

/* CSI 终结：仅关心 ?1049/1047/47 h|l（alt screen）与 K（行擦除）。 */
static void csi_final(hb_watch *w, char fin)
{
    w->csi_param[w->csi_plen] = 0;
    if (w->csi_plen && w->csi_param[0] == '?') {
        if (fin == 'h' || fin == 'l') {
            char *p = w->csi_param + 1, *e;
            while (*p) {
                long v = strtol(p, &e, 10);
                if (e == p) break;
                if (v == 1049 || v == 1047 || v == 47)
                    w->alt = (fin == 'h');
                p = (*e == ';') ? e + 1 : e;
                if (*p && e == p) break;
            }
        }
    } else if (fin == 'K') {
        long v = w->csi_plen ? strtol(w->csi_param, NULL, 10) : 0;
        if (v == 0)      w->len = w->pos;          /* 擦至行尾 */
        else if (v == 2) w->len = w->pos = 0;      /* 整行擦除 */
    }
    w->csi_plen = 0;
}

void hb_watch_feed(hb_watch *w, const char *bytes, size_t len, int64_t now_ms)
{
    size_t i;
    if (!w || !bytes) return;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (w->alt) activity(w, now_ms);   /* 备用屏上任何字节都算活动 */

        switch (w->st) {
        case ST_TEXT:
            /* 133 B..C 之间回显的可见文本单独累积，C 时取最后一行 = 命令 */
            if (w->in_bc && w->cmd_len < HB_LINE_CAP - 2 &&
                (c >= 0x20 || c == '\r' || c == '\n') && c != 0x7f) {
                char ec = (c == '\r' || c == '\n') ? '\n' : (char)c;
                if (ec != '\n' || (w->cmd_len && w->cmd[w->cmd_len - 1] != '\n')) {
                    if (w->cmd_len + 1 > w->cmd_cap) {
                        size_t nc = w->cmd_cap ? w->cmd_cap * 2 : 256;
                        char *ncmd = realloc(w->cmd, nc);
                        if (ncmd) { w->cmd = ncmd; w->cmd_cap = nc; }
                    }
                    if (w->cmd_len < w->cmd_cap) w->cmd[w->cmd_len++] = ec;
                }
            }
            if (c == 0x1b) { w->st = ST_ESC; break; }
            if (c == '\r') { w->pos = 0; break; }
            if (c == '\n') { commit_line(w, now_ms); break; }
            if (c == '\b') { if (w->pos) w->pos--; break; }
            if (c == '\t') { put_ch(w, ' ', now_ms); break; }
            if (c < 0x20 || c == 0x7f) break;
            put_ch(w, (char)c, now_ms);
            break;
        case ST_ESC:
            switch (c) {
            case '[': w->st = ST_CSI; w->csi_plen = 0; break;
            case ']': w->st = ST_OSC; w->str_guard = 0; break;
            case 'P': case 'X': case '^': case '_':
                w->st = ST_DCS; w->str_guard = 0; break;
            case '(': case ')': case '#': case '%': case '*': case '+':
                w->st = ST_ESC_I; break;
            default:  w->st = ST_TEXT; break;
            }
            break;
        case ST_ESC_I:
            w->st = ST_TEXT;
            break;
        case ST_CSI:
            if (c >= 0x40 && c <= 0x7e) { csi_final(w, (char)c); w->st = ST_TEXT; }
            else if (w->csi_plen < sizeof w->csi_param - 1)
                w->csi_param[w->csi_plen++] = (char)c;
            break;
        case ST_OSC:
            if (c == 0x07) w->st = ST_TEXT;
            else if (c == 0x1b) w->st = ST_OSC_ESC;
            else if (++w->str_guard > 65536) w->st = ST_TEXT;
            break;
        case ST_OSC_ESC:
            w->st = (c == '\\') ? ST_TEXT : ST_OSC;
            break;
        case ST_DCS:
            if (c == 0x1b) w->st = ST_DCS_ESC;
            else if (++w->str_guard > 65536) w->st = ST_TEXT;
            break;
        case ST_DCS_ESC:
            w->st = (c == '\\') ? ST_TEXT : ST_DCS;
            break;
        }
    }
}

/* ---------------------------------------------------------------- events */

void hb_watch_input(hb_watch *w, int64_t now_ms)
{
    if (!w) return;
    w->last_input = now_ms;
    w->task_start = now_ms;
    w->armed = true;
    w->pend = false;
    w->pend_miss = 0;
    w->first_out = w->last_out = -1;
}

/* B..C 捕获的最后一个非空行 = 回显命令（提示符在 B 之前已输出）。 */
static char *extract_command(hb_watch *w)
{
    const char *s, *e;
    if (!w->cmd || !w->cmd_len) return NULL;
    s = w->cmd + w->cmd_len;
    e = w->cmd;
    while (s > e && s[-1] == '\n') s--;
    while (s > e) {
        const char *ls = s;
        const char *ts; size_t tn;
        while (ls > e && ls[-1] != '\n') ls--;
        trim_span(ls, (size_t)(s - ls), &ts, &tn);
        if (tn) return dup_n(ts, tn > HB_HIT_LINE_CAP ? HB_HIT_LINE_CAP : tn);
        s = ls;
        while (s > e && s[-1] == '\n') s--;
    }
    return NULL;
}

void hb_watch_osc133(hb_watch *w, char mark, int code, int64_t now_ms)
{
    hb_watch_hit h;
    if (!w) return;
    switch (mark) {
    case 'A':
        w->in_bc = false;
        break;
    case 'B':
        w->in_bc = true;
        w->cmd_len = 0;
        break;
    case 'C': {
        w->in_bc = false;
        free(w->last_cmd);
        w->last_cmd = extract_command(w);
        w->task_start = now_ms;
        w->armed = true;
        w->pend = false;
        w->pend_miss = 0;
        w->first_out = w->last_out = -1;
        w->c_seen = true;
        break;
    }
    case 'D': {
        int64_t dur;
        if (!w->c_seen) break;               /* 提示符重绘的裸 D */
        w->c_seen = false;
        w->pend = false;                     /* 133 为同任务权威结果 */
        dur = now_ms - w->task_start;
        if (dur < w->min_duration_ms) break; /* ls 之类的瞬时命令 */
        if (!(w->signals & HB_WATCH_SIG_OSC133)) { disarm(w); break; }
        memset(&h, 0, sizeof h);
        h.signal = HB_WATCH_SIG_OSC133;
        h.ok = (code == 0);
        h.exit_code = code;
        h.rule = strdup("OSC 133");
        h.command = w->last_cmd ? strdup(w->last_cmd) : NULL;
        h.duration_ms = dur;
        qpush(w, &h);
        disarm(w);
        break;
    }
    default: break;
    }
}

void hb_watch_bell(hb_watch *w, int64_t now_ms)
{
    hb_watch_hit h;
    if (!w || !(w->signals & HB_WATCH_SIG_TERMNOTIFY)) return;
    if (w->last_input >= 0 && now_ms - w->last_input < w->bell_after_input_ms)
        return;                                   /* Tab 补全等交互回铃 */
    if (w->last_bell >= 0 && now_ms - w->last_bell < HB_BELL_RATE_MS)
        return;
    w->last_bell = now_ms;
    memset(&h, 0, sizeof h);
    h.signal = HB_WATCH_SIG_TERMNOTIFY;
    h.ok = true;
    h.exit_code = -1;
    h.rule = strdup("BEL");
    h.duration_ms = (w->task_start >= 0) ? now_ms - w->task_start : -1;
    qpush(w, &h);
    disarm(w);
}

void hb_watch_notify(hb_watch *w, const char *title, const char *body,
                     int64_t now_ms)
{
    hb_watch_hit h;
    const char *ts; size_t tn;
    if (!w || !(w->signals & HB_WATCH_SIG_TERMNOTIFY)) return;
    memset(&h, 0, sizeof h);
    h.signal = HB_WATCH_SIG_TERMNOTIFY;
    h.ok = true;
    h.exit_code = -1;
    h.rule = strdup(title ? "OSC 777" : "OSC 9");
    if (body) {
        trim_span(body, strlen(body), &ts, &tn);
        if (tn > HB_HIT_LINE_CAP) tn = HB_HIT_LINE_CAP;
        h.line = dup_n(ts, tn);
    }
    h.title = title ? strdup(title) : NULL;
    h.duration_ms = (w->task_start >= 0) ? now_ms - w->task_start : -1;
    qpush(w, &h);
    /* 不解除武装：应用通知与任务结果彼此独立 */
}

void hb_watch_eof(hb_watch *w, int code, int64_t now_ms)
{
    hb_watch_hit h;
    (void)now_ms;
    if (!w || w->eof_done || !(w->signals & HB_WATCH_SIG_EOF)) return;
    w->eof_done = true;
    memset(&h, 0, sizeof h);
    h.signal = HB_WATCH_SIG_EOF;
    h.ok = (code == 0 || code == -1);
    h.exit_code = code;
    h.rule = strdup("会话结束（EOF）");
    h.duration_ms = -1;
    qpush(w, &h);
    w->armed = false;
    w->pend = false;
}

/* ---------------------------------------------------------------- poll */

/* 部分行是否像在等待输入（提示符/确认提示）。 */
static bool prompt_like(const char *s, size_t n)
{
    static const char *const sfx[] = { "$ ", "# ", "% ", "> ", ": ", "? " };
    static const char utf[] = "\xe2\x9d\xaf ";       /* ❯ + space */
    size_t i;
    if (!s || !n) return false;
    if (n >= 5 && (memmem(s, n, "[y/N]", 5) || memmem(s, n, "[Y/n]", 5)))
        return true;
    for (i = 0; i < sizeof sfx / sizeof sfx[0]; i++)
        if (n >= 2 && !memcmp(s + n - 2, sfx[i], 2)) return true;
    return n >= 4 && !memcmp(s + n - 4, utf, 4);
}

static void push_rule_hit(hb_watch *w, int64_t now)
{
    hb_watch_hit h;
    memset(&h, 0, sizeof h);
    h.signal = HB_WATCH_SIG_RULES;
    h.ok = w->pend_ok;
    h.exit_code = -1;
    h.rule = strdup(w->pend_rule);
    h.line = strdup(w->pend_line);
    h.duration_ms = (w->task_start >= 0) ? now - w->task_start : -1;
    qpush(w, &h);
    disarm(w);
}

bool hb_watch_poll(hb_watch *w, int64_t now_ms, hb_watch_hit *out,
                   int64_t *next_ms)
{
    bool got = false;
    if (!w) return false;

    /* 静默 ≥300ms 的部分行也参与规则匹配（无换行的进度终结行） */
    if (w->line_dirty && w->len && w->armed && !w->alt &&
        now_ms - w->line_change >= HB_PARTIAL_QUIET_MS) {
        w->line_dirty = false;
        try_match(w, w->line, w->len, true, now_ms);
    }

    /* 规则 pending 确认：confirm_quiet_ms 无输出 */
    if (w->pend && now_ms - w->any_out >= w->confirm_quiet_ms)
        push_rule_hit(w, now_ms);

    /* 空闲启发式（默认关）：任务活动窗 ≥ idle_active，随后静默 ≥ idle_quiet，
     * 且当前部分行像在等待输入 */
    if (w->idle_on && w->armed && !w->alt && !w->pend &&
        w->first_out >= 0 &&
        w->last_out - w->first_out >= w->idle_active_ms &&
        now_ms - w->last_out >= w->idle_quiet_ms &&
        w->task_start >= 0 && now_ms - w->task_start >= w->min_duration_ms &&
        w->line && prompt_like(w->line, w->len)) {
        hb_watch_hit h;
        memset(&h, 0, sizeof h);
        h.signal = HB_WATCH_SIG_IDLE;
        h.ok = true;
        h.exit_code = -1;
        h.rule = strdup("空闲启发式");
        {
            const char *ts; size_t tn;
            trim_span(w->line, w->len, &ts, &tn);
            if (tn > HB_HIT_LINE_CAP) tn = HB_HIT_LINE_CAP;
            h.line = dup_n(ts, tn);
        }
        h.duration_ms = now_ms - w->task_start;
        qpush(w, &h);
        disarm(w);
    }

    if (out) got = qpop(w, out);

    if (next_ms) {
        int64_t n = -1, c;
        if (w->qh < w->qt) n = now_ms;                       /* 队列还有 */
        if (w->pend) {
            c = w->any_out + w->confirm_quiet_ms;
            if (n < 0 || c < n) n = c;
        }
        if (w->line_dirty && w->len && w->armed && !w->alt) {
            c = w->line_change + HB_PARTIAL_QUIET_MS;
            if (n < 0 || c < n) n = c;
        }
        if (w->idle_on && w->armed && !w->alt && w->first_out >= 0 &&
            w->last_out - w->first_out < w->idle_active_ms) {
            c = w->first_out + w->idle_active_ms;
            if (n < 0 || c < n) n = c;
        } else if (w->idle_on && w->armed && !w->alt && w->first_out >= 0) {
            c = w->last_out + w->idle_quiet_ms;
            if (n < 0 || c < n) n = c;
        }
        *next_ms = n;
    }
    return got;
}

/* ---------------------------------------------------------------- tester */

bool hb_watch_test_text(const hb_watch_cfg *c, const char *sample,
                        hb_watch_hit *out)
{
    hb_watch_cfg cc;
    hb_watch *w;
    hb_watch_hit tmp;
    char *copy, *p, *nl;
    int64_t t, end;
    int i = 0;
    bool got = false;

    if (!sample) return false;
    if (c) cc = *c; else hb_watch_cfg_defaults(&cc);
    w = hb_watch_new(&cc);
    if (!w) return false;

    hb_watch_input(w, 0);
    /* 让任务起始早于样本 min_duration_ms，规则命中才有有效时长 */
    t = cc.min_duration_ms > 0 ? cc.min_duration_ms : 0;

    copy = strdup(sample);
    if (!copy) { hb_watch_free(w); return false; }
    for (p = copy; ; p = nl + 1) {
        size_t n;
        nl = strchr(p, '\n');
        n = nl ? (size_t)(nl - p) : strlen(p);
        if (n) {
            hb_watch_feed(w, p, n, t + (int64_t)i * 50);
            hb_watch_feed(w, "\n", 1, t + (int64_t)i * 50);
            i++;
        }
        if (!nl) break;
    }
    free(copy);

    memset(&tmp, 0, sizeof tmp);
    end = t + (int64_t)i * 50 + cc.confirm_quiet_ms + HB_PARTIAL_QUIET_MS + 500;
    for (; t <= end && !got; t += 50)
        got = hb_watch_poll(w, t, &tmp, NULL);
    hb_watch_free(w);
    if (!got) return false;
    if (out) *out = tmp; else hb_watch_hit_clear(&tmp);
    return true;
}

/* ------------------------------------------------------- shell integration */

/* bash ≥4.4：PROMPT_COMMAND 发 D;$?+A，PS0 发 C，PS1 前缀发 B。
 * 不覆盖已有 PROMPT_COMMAND（兼容 bash≥5.1 的数组形态），source 两次幂等。 */
static const char hb__si_bash[] =
"# Harbor shell integration (bash >= 4.4)\n"
"case $- in\n"
"    *i*) ;;\n"
"    *) return 0 2>/dev/null || exit 0 ;;\n"
"esac\n"
"[ -n \"${__HARBOR_SI:-}\" ] && return 0 2>/dev/null\n"
"__HARBOR_SI=1\n"
"__hb_si_precmd() {\n"
"    local __hb_s=$?\n"
"    printf '\\033]133;D;%s\\007' \"$__hb_s\"\n"
"    printf '\\033]133;A\\007'\n"
"}\n"
"if declare -p PROMPT_COMMAND 2>/dev/null | grep -q '^declare -[^ ]*a'\n"
"then\n"
"    PROMPT_COMMAND=(__hb_si_precmd \"${PROMPT_COMMAND[@]}\")\n"
"else\n"
"    PROMPT_COMMAND=\"__hb_si_precmd${PROMPT_COMMAND:+;}${PROMPT_COMMAND:-}\"\n"
"fi\n"
"PS0='\\[\\033]133;C\\007\\]'\n"
"PS1='\\[\\033]133;B\\007\\]'\"$PS1\"\n";

static const char hb__si_zsh[] =
"# Harbor shell integration (zsh)\n"
"case $- in\n"
"    *i*) ;;\n"
"    *) return 0 2>/dev/null || exit 0 ;;\n"
"esac\n"
"[ -n \"${__HARBOR_SI:-}\" ] && return 0 2>/dev/null\n"
"__HARBOR_SI=1\n"
"autoload -Uz add-zsh-hook\n"
"__hb_si_precmd() {\n"
"    local __hb_s=$?\n"
"    printf '\\033]133;D;%s\\007' \"$__hb_s\"\n"
"    printf '\\033]133;A\\007'\n"
"}\n"
"__hb_si_preexec() { printf '\\033]133;C\\007'; }\n"
"add-zsh-hook precmd  __hb_si_precmd\n"
"add-zsh-hook preexec __hb_si_preexec\n"
"PS1=\"%{$(printf '\\033]133;B\\007')%}$PS1\"\n";

static const char hb__si_fish[] =
"# Harbor shell integration (fish)\n"
"status is-interactive; or return\n"
"set -q __HARBOR_SI; and return\n"
"set -g __HARBOR_SI 1\n"
"function __hb_si_on_prompt --on-event fish_prompt\n"
"    printf '\\033]133;D;%s\\007' $status\n"
"    printf '\\033]133;A\\007'\n"
"end\n"
"function __hb_si_preexec --on-event fish_preexec\n"
"    printf '\\033]133;C\\007'\n"
"end\n"
"if functions -q fish_prompt\n"
"    functions -c fish_prompt __hb_si_orig_prompt\n"
"    function fish_prompt\n"
"        __hb_si_orig_prompt\n"
"        printf '\\033]133;B\\007'\n"
"    end\n"
"end\n";

const char *hb_shell_integration_script(const char *shell)
{
    if (!shell) return NULL;
    if (!strcmp(shell, "bash")) return hb__si_bash;
    if (!strcmp(shell, "zsh"))  return hb__si_zsh;
    if (!strcmp(shell, "fish")) return hb__si_fish;
    return NULL;
}
