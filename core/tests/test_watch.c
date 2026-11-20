/* test_watch — FR-13：watch_scenarios.txt 场景回放 + 引擎单元 +
 * 规则测试器 + shell 集成脚本校验。
 * watch_scenarios.txt 位于源码树（构建时经 -DWATCH_SCEN_FILE 注入路径）。 */

#include "harbor/harbor_core.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef WATCH_SCEN_FILE
#define WATCH_SCEN_FILE "data/watch_scenarios.txt"
#endif

static char *dupstrn(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---------------------------------------------------------- unescape */

static void unescape(const char *s, char *out)
{
    char *o = out;
    while (*s) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        switch (*s) {
        case 'e':  *o++ = 0x1b; s++; break;
        case 'a':  *o++ = 0x07; s++; break;
        case 'r':  *o++ = '\r'; s++; break;
        case 'n':  *o++ = '\n'; s++; break;
        case 't':  *o++ = '\t'; s++; break;
        case '\\': *o++ = '\\'; s++; break;
        case 'x': {
            int v = 0, k;
            s++;
            for (k = 0; k < 2 && *s; k++) {
                char c = *s;
                int d = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0) break;
                v = v * 16 + d;
                s++;
            }
            *o++ = (char)v;
            break;
        }
        default: *o++ = *s ? *s++ : 0; break;
        }
    }
    *o = 0;
}

/* ---------------------------------------------------------- scenario model */

enum { EV_OUT, EV_IN, EV_133, EV_BELL, EV_OSC9, EV_OSC777, EV_EOF };

typedef struct {
    int64_t t;
    int     type;
    int     code;          /* 133 code / eof code */
    char    mark;
    char   *s1, *s2;       /* out bytes / osc9 body / osc777 title|body */
} wev;

typedef struct {
    char    name[64];
    int     expect;        /* 0 none, 1 ok, 2 err */
    char    signal[16];
    char    neg[16];
    wev    *evs;
    size_t  nev, cev;
    int64_t end_ms;
} wscen;

static void scen_ev(wscen *s, const wev *e)
{
    if (s->nev == s->cev) {
        s->cev = s->cev ? s->cev * 2 : 16;
        s->evs = realloc(s->evs, s->cev * sizeof *s->evs);
    }
    s->evs[s->nev++] = *e;
}

static void scen_free(wscen *s, size_t n)
{
    size_t i, j;
    for (i = 0; i < n; i++) {
        for (j = 0; j < s[i].nev; j++) {
            free(s[i].evs[j].s1);
            free(s[i].evs[j].s2);
        }
        free(s[i].evs);
    }
    free(s);
}

static int64_t parse_ms(const char *s, const char **end)
{
    char *e;
    long v = strtol(s, &e, 10);
    *end = e;
    return v;
}

static wscen *parse_scenarios(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "r");
    wscen *list = NULL, *cur = NULL;
    size_t n = 0, cap = 0;
    char  buf[8192];

    if (!f) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    while (fgets(buf, sizeof buf, f)) {
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;
        if (!strncmp(p, "scenario ", 9)) {
            char *q = p + 9, *tok;
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                list = realloc(list, cap * sizeof *list);
            }
            cur = &list[n++];
            memset(cur, 0, sizeof *cur);
            tok = strtok(q, " \t\n");
            snprintf(cur->name, sizeof cur->name, "%s", tok ? tok : "?");
            tok = strtok(NULL, " \t\n");
            cur->expect = tok && !strcmp(tok, "ok") ? 1
                        : tok && !strcmp(tok, "err") ? 2 : 0;
            while ((tok = strtok(NULL, " \t\n"))) {
                if (!strncmp(tok, "neg:", 4))
                    snprintf(cur->neg, sizeof cur->neg, "%s", tok + 4);
                else
                    snprintf(cur->signal, sizeof cur->signal, "%s", tok);
            }
            continue;
        }
        if (*p != '@' || !cur) continue;
        {
            const char *q = p + 1;
            wev e;
            int64_t a, b, step = 0;
            memset(&e, 0, sizeof e);
            a = parse_ms(q, &q);
            if (*q == '-') {
                b = parse_ms(q + 1, &q);
                if (*q == '/') step = parse_ms(q + 1, &q);
            } else b = a;
            while (*q == ' ') q++;

            if (!strncmp(q, "out ", 4)) {
                char raw[8192], *tmp;
                const char *str = q + 4;
                size_t rl = strlen(str);
                int64_t tt;
                while (rl && (str[rl - 1] == '\n' || str[rl - 1] == '\r')) rl--;
                tmp = dupstrn(str, rl);
                unescape(tmp, raw);
                free(tmp);
                e.type = EV_OUT;
                if (!step) {
                    e.t = a;
                    e.s1 = strdup(raw);
                    scen_ev(cur, &e);
                } else {
                    for (tt = a; tt <= b; tt += step) {
                        e.t = tt;
                        e.s1 = strdup(raw);
                        scen_ev(cur, &e);
                    }
                }
                continue;
            }

            e.t = a;
            if (!strncmp(q, "in", 2))            { e.type = EV_IN;   scen_ev(cur, &e); }
            else if (!strncmp(q, "bell", 4))     { e.type = EV_BELL; scen_ev(cur, &e); }
            else if (!strncmp(q, "eof", 3)) {
                e.type = EV_EOF;
                e.code = (int)strtol(q + 3, NULL, 10);
                scen_ev(cur, &e);
            } else if (!strncmp(q, "end", 3)) {
                cur->end_ms = a;
            } else if (!strncmp(q, "133", 3)) {
                e.type = EV_133;
                e.mark = q[4];
                if (e.mark == 'D')
                    e.code = (int)strtol(q + 5, NULL, 10);
                scen_ev(cur, &e);
            } else if (!strncmp(q, "osc777", 6)) {
                char *rest = (char *)q + 7, *bar;
                e.type = EV_OSC777;
                rest[strcspn(rest, "\n")] = 0;
                bar = strchr(rest, '|');
                if (bar) *bar++ = 0;
                e.s1 = strdup(rest);
                e.s2 = strdup(bar ? bar : "");
                scen_ev(cur, &e);
            } else if (!strncmp(q, "osc9", 4)) {
                char *rest = (char *)q + 5;
                e.type = EV_OSC9;
                rest[strcspn(rest, "\n")] = 0;
                e.s1 = strdup(rest);
                scen_ev(cur, &e);
            }
        }
    }
    fclose(f);
    *out_n = n;
    return list;
}

/* ---------------------------------------------------------- runner */

static hb_watch_sig sig_of(const char *name)
{
    if (!strcmp(name, "eof"))    return HB_WATCH_SIG_EOF;
    if (!strcmp(name, "osc133")) return HB_WATCH_SIG_OSC133;
    if (!strcmp(name, "rule"))   return HB_WATCH_SIG_RULES;
    if (!strcmp(name, "idle"))   return HB_WATCH_SIG_IDLE;
    return HB_WATCH_SIG_TERMNOTIFY;  /* bell|osc9|osc777 */
}

static const char *sig_name(hb_watch_sig s)
{
    switch (s) {
    case HB_WATCH_SIG_EOF:        return "eof";
    case HB_WATCH_SIG_OSC133:     return "osc133";
    case HB_WATCH_SIG_RULES:      return "rule";
    case HB_WATCH_SIG_IDLE:       return "idle";
    case HB_WATCH_SIG_TERMNOTIFY: return "termnotify";
    }
    return "?";
}

static void apply_ev(hb_watch *w, const wev *e)
{
    switch (e->type) {
    case EV_OUT:    hb_watch_feed(w, e->s1, strlen(e->s1), e->t); break;
    case EV_IN:     hb_watch_input(w, e->t); break;
    case EV_133:    hb_watch_osc133(w, e->mark, e->code, e->t); break;
    case EV_BELL:   hb_watch_bell(w, e->t); break;
    case EV_OSC9:   hb_watch_notify(w, NULL, e->s1, e->t); break;
    case EV_OSC777: hb_watch_notify(w, e->s1, e->s2, e->t); break;
    case EV_EOF:    hb_watch_eof(w, e->code, e->t); break;
    }
}

/* 首 hit（grid 每 100ms 且每个指令后立即 poll）。返回 1 = 有 hit。 */
static int run_scenario(const wscen *s, hb_watch_hit *hit)
{
    hb_watch_cfg c;
    hb_watch *w;
    size_t ei = 0;
    int64_t g = 0;
    int got = 0;

    hb_watch_cfg_defaults(&c);
    w = hb_watch_new(&c);
    memset(hit, 0, sizeof *hit);

    while (!got && ei < s->nev) {
        int64_t et = s->evs[ei].t;
        while (!got && g < et && g <= s->end_ms) {
            got = hb_watch_poll(w, g, hit, NULL);
            g += 100;
        }
        if (got || et > s->end_ms) break;
        apply_ev(w, &s->evs[ei++]);
        got = hb_watch_poll(w, et, hit, NULL);
        g = (et / 100 + 1) * 100;
    }
    while (!got && g <= s->end_ms) {
        got = hb_watch_poll(w, g, hit, NULL);
        g += 100;
    }
    hb_watch_free(w);
    return got;
}

static void test_scenarios(void)
{
    size_t n = 0, i;
    wscen *sc = parse_scenarios(WATCH_SCEN_FILE, &n);
    int pass = 0, pos = 0, pos_pass = 0;
    int vim_n = 0, vim_fp = 0, top_n = 0, top_fp = 0, idle_n = 0, idle_fp = 0;
    double recall;

    CHECK(sc != NULL);
    if (!sc) return;
    CHECK_EQ_INT((int)n, 20);

    printf("%-24s %-12s %-30s %s\n", "scenario", "expect", "got", "result");
    for (i = 0; i < n; i++) {
        wscen *s = &sc[i];
        hb_watch_hit h;
        int got = run_scenario(s, &h);
        int good = 0;
        char gotbuf[160];

        if (!got) snprintf(gotbuf, sizeof gotbuf, "(no hit)");
        else snprintf(gotbuf, sizeof gotbuf, "%s/%s/%s dur=%lld",
                      sig_name(h.signal), h.ok ? "ok" : "err",
                      h.rule ? h.rule : "?", (long long)h.duration_ms);

        if (s->expect == 0)      good = !got;
        else if (s->expect == 1) good = got && h.ok;
        else                     good = got && !h.ok;
        if (good && s->signal[0] && got)
            good = (h.signal == sig_of(s->signal));

        printf("%-24s %-12s %-30s %s\n", s->name,
               s->expect == 0 ? "none" : s->expect == 1 ? "ok" : "err",
               gotbuf, good ? "PASS" : "FAIL");

        if (s->expect) { pos++; if (good) pos_pass++; }
        if (!strcmp(s->neg, "vim"))  { vim_n++;  if (got) vim_fp++; }
        if (!strcmp(s->neg, "top"))  { top_n++;  if (got) top_fp++; }
        if (!strcmp(s->neg, "idle")) { idle_n++; if (got) idle_fp++; }
        if (good) pass++;
        if (got) hb_watch_hit_clear(&h);
    }

    recall = pos ? (double)pos_pass / pos : 0;
    printf("pass %d/%zu  recall %.2f  FP vim %d/%d top %d/%d idle %d/%d\n",
           pass, n, recall, vim_fp, vim_n, top_fp, top_n, idle_fp, idle_n);
    CHECK_EQ_INT(pass, (int)n);
    CHECK(recall >= 0.95);
    CHECK(vim_n == 0 || (double)vim_fp / vim_n <= 0.05);
    CHECK(top_n == 0 || (double)top_fp / top_n <= 0.05);
    CHECK(idle_n == 0 || (double)idle_fp / idle_n <= 0.05);
    scen_free(sc, n);
}

/* ---------------------------------------------------------- unit helpers */

static bool wait_hit(hb_watch *w, int64_t *t, int64_t delta, hb_watch_hit *h)
{
    int64_t until = *t + delta;
    for (; *t <= until; *t += 50)
        if (hb_watch_poll(w, *t, h, NULL)) return true;
    return false;
}

static void feed_line(hb_watch *w, const char *s, int64_t t)
{
    hb_watch_feed(w, s, strlen(s), t);
    hb_watch_feed(w, "\r\n", 2, t);
}

/* ---------------------------------------------------------- unit checks */

static void test_units(void)
{
    hb_watch_cfg c;
    hb_watch *w;
    hb_watch_hit h;
    int64_t t;

    hb_watch_cfg_defaults(&c);

    /* CSI/OSC strip + \r 覆写：可见文本应为 "bbaZZ" */
    {
        hb_watch_rule ur = { "t", "^bbaZZ$", NULL };
        c.rules = &ur; c.nrules = 1;
        w = hb_watch_new(&c);
        c.rules = NULL; c.nrules = 0;
        const char *junk = "aaa\x1b[31mZZ\x1b]8;;http://x\x07\rbb";
        hb_watch_input(w, 0);
        t = 6000;
        hb_watch_feed(w, junk, strlen(junk), t);
        hb_watch_feed(w, "\r\n", 2, t);
        memset(&h, 0, sizeof h);
        CHECK(wait_hit(w, &t, 5000, &h));
        if (h.rule) {
            CHECK_EQ_STR(h.rule, "t");
            CHECK_EQ_STR(h.line, "bbaZZ");
            hb_watch_hit_clear(&h);
        }
        hb_watch_free(w);
    }

    /* alt screen 跨 feed 拆分的序列；alt 上规则抑制，退出后恢复 */
    w = hb_watch_new(&c);
    hb_watch_input(w, 0);
    t = 6000;
    hb_watch_feed(w, "\e[?104", 6, t);
    hb_watch_feed(w, "9h", 2, t + 1);
    feed_line(w, "make: *** [x] Error 2", t + 10);
    memset(&h, 0, sizeof h);
    CHECK(!wait_hit(w, &t, 5000, &h));
    hb_watch_feed(w, "\e[?1049l", 8, t);
    feed_line(w, "make: *** [x] Error 2", t);
    CHECK(wait_hit(w, &t, 5000, &h));
    CHECK(!h.ok && h.signal == HB_WATCH_SIG_RULES);
    hb_watch_hit_clear(&h);
    hb_watch_free(w);

    /* 裸 D（无 C）忽略；短任务（< min_duration）忽略 */
    w = hb_watch_new(&c);
    hb_watch_input(w, 0);
    hb_watch_osc133(w, 'D', 0, 100);
    hb_watch_osc133(w, 'B', -1, 200);
    hb_watch_osc133(w, 'C', -1, 300);
    hb_watch_osc133(w, 'D', 0, 400);
    t = 0; memset(&h, 0, sizeof h);
    CHECK(!wait_hit(w, &t, 9000, &h));
    hb_watch_free(w);

    /* err 先于 ok；一任务一命中；输入才重武装 */
    w = hb_watch_new(&c);
    hb_watch_input(w, 0);
    t = 6000;
    feed_line(w, "===== 2 failed, 126 passed in 34.56s =====", t);
    memset(&h, 0, sizeof h);
    CHECK(wait_hit(w, &t, 5000, &h));
    CHECK(!h.ok && h.signal == HB_WATCH_SIG_RULES);
    CHECK_EQ_STR(h.rule, "pytest");
    hb_watch_hit_clear(&h);
    feed_line(w, "===== 1 failed in 1.0s =====", t);
    CHECK(!wait_hit(w, &t, 6000, &h));            /* 已解除武装 */
    hb_watch_input(w, t);                          /* 重武装 */
    t += 6000;                                     /* 需 ≥ min_duration */
    feed_line(w, "===== 3 failed in 2.0s =====", t);
    CHECK(wait_hit(w, &t, 5000, &h) && !h.ok);
    hb_watch_hit_clear(&h);
    hb_watch_free(w);

    /* 流式输出取消：匹配后 >3 条非匹配换行 */
    w = hb_watch_new(&c);
    hb_watch_input(w, 0);
    t = 6000;
    feed_line(w, "Tokens: 1k sent, 1k received.", t);
    feed_line(w, "more output 1", t + 100);
    feed_line(w, "more output 2", t + 200);
    feed_line(w, "more output 3", t + 300);
    feed_line(w, "more output 4", t + 400);
    memset(&h, 0, sizeof h);
    CHECK(!wait_hit(w, &t, 6000, &h));
    hb_watch_free(w);

    /* BEL：输入后忽略 + 限速 */
    w = hb_watch_new(&c);
    hb_watch_input(w, 0);
    hb_watch_bell(w, 500);
    hb_watch_bell(w, 3000);
    hb_watch_bell(w, 4000);
    t = 0; memset(&h, 0, sizeof h);
    CHECK(wait_hit(w, &t, 3000, &h));
    CHECK(h.ok && h.signal == HB_WATCH_SIG_TERMNOTIFY);
    CHECK_EQ_STR(h.rule, "BEL");
    hb_watch_hit_clear(&h);
    CHECK(!wait_hit(w, &t, 6000, &h));
    hb_watch_free(w);

    /* 133 B..C 命令回显捕获：提示符在 B 前已输出，捕获到的应只是命令 */
    w = hb_watch_new(&c);
    hb_watch_osc133(w, 'A', -1, 0);
    hb_watch_feed(w, "u@h:~$ ", 7, 0);
    hb_watch_osc133(w, 'B', -1, 10);
    hb_watch_input(w, 50);
    hb_watch_feed(w, "make -j8\r\n", 10, 60);
    hb_watch_osc133(w, 'C', -1, 100);
    hb_watch_osc133(w, 'D', 0, 7000);
    t = 7000; memset(&h, 0, sizeof h);
    CHECK(wait_hit(w, &t, 1000, &h));
    if (h.rule) {
        CHECK(h.signal == HB_WATCH_SIG_OSC133 && h.ok && h.exit_code == 0);
        CHECK_EQ_STR(h.command, "make -j8");
        hb_watch_hit_clear(&h);
    }
    hb_watch_free(w);

    /* 用户规则生效；坏 regex → hb_watch_new NULL */
    {
        hb_watch_rule ur  = { "mine", "^DONE$", NULL };
        hb_watch_rule bad = { "bad", "([", NULL };
        c.rules = &ur; c.nrules = 1;
        w = hb_watch_new(&c);
        c.rules = NULL; c.nrules = 0;
        CHECK(w != NULL);
        hb_watch_input(w, 0);
        t = 6000;
        feed_line(w, "DONE", t);
        memset(&h, 0, sizeof h);
        CHECK(wait_hit(w, &t, 5000, &h) && h.ok);
        CHECK_EQ_STR(h.rule, "mine");
        hb_watch_hit_clear(&h);
        hb_watch_free(w);
        c.rules = &bad; c.nrules = 1;
        CHECK(hb_watch_new(&c) == NULL);
        c.rules = NULL; c.nrules = 0;
    }
}

/* ---------------------------------------------------------- rule tester */

static void test_rule_tester(void)
{
    static const struct { const char *rule, *sample; bool ok; } cases[] = {
        { "aider",   "Tokens: 12k sent, 1.2k received. Cost: $0.05 message, $0.41 session.", true },
        { "pytest",  "================== 128 passed in 34.56s ==================", true },
        { "pytest",  "===== 2 failed, 126 passed in 34.56s =====", false },
        { "cargo",   "    Finished `release` profile [optimized] target(s) in 41.20s", true },
        { "cargo",   "error: could not compile `foo` due to previous error", false },
        { "make",    "make: *** [Makefile:12: all] Error 2", false },
        { "vite",    "\xe2\x9c\x93 built in 12.34s", true },
        { "webpack", "webpack 5.90.1 compiled successfully in 1234 ms", true },
        { "webpack", "compiled with 2 errors", false },
        { "npm",     "npm ERR! code E404", false },
        { "go-test", "ok  \tgithub.com/x/y\t0.012s", true },
        { "go-test", "FAIL\tgithub.com/x/y", false },
        { "wget",    "\xe2\x80\x98ubuntu.iso\xe2\x80\x99 saved [4932407296/4932407296]", true },
        { "git",     "Resolving deltas: 100% (1234/1234), done.", true },
        { "generic-done", "\xe2\x9c\x93 deploy completed", true },
    };
    size_t i;
    hb_watch_hit h;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(&h, 0, sizeof h);
        CHECK(hb_watch_test_text(NULL, cases[i].sample, &h));
        if (h.rule) {
            CHECK_EQ_STR(h.rule, cases[i].rule);
            CHECK(h.ok == cases[i].ok);
            hb_watch_hit_clear(&h);
        }
    }
    /* AI 流式文本整块喂入不得误中 */
    memset(&h, 0, sizeof h);
    CHECK(!hb_watch_test_text(NULL,
        "the previous task completed successfully, now running tests and "
        "checking the build output", &h));
    /* 坏规则配置 → false 而非崩溃 */
    {
        hb_watch_cfg cc; hb_watch_rule bad = { "b", "(", NULL };
        hb_watch_cfg_defaults(&cc);
        cc.rules = &bad; cc.nrules = 1;
        CHECK(!hb_watch_test_text(&cc, "x", &h));
    }
}

/* ---------------------------------------------------------- shell scripts */

static char *write_tmp(const char *txt)
{
    char path[] = "/tmp/hb_si_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    if (write(fd, txt, strlen(txt)) < 0) { close(fd); return NULL; }
    close(fd);
    return strdup(path);
}

static bool have(const char *tool)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

static void test_shell_scripts(void)
{
    static const char *const sh[] = { "bash", "zsh", "fish" };
    size_t i;

    for (i = 0; i < 3; i++) {
        const char *script = hb_shell_integration_script(sh[i]);
        char *path, cmd[600];
        int rc;
        CHECK(script != NULL);
        if (!script) continue;
        if (!have(sh[i])) {
            printf("  %s not installed, -n check skipped\n", sh[i]);
            continue;
        }
        path = write_tmp(script);
        CHECK(path != NULL);
        snprintf(cmd, sizeof cmd, "%s -n %s", sh[i], path);
        rc = system(cmd);
        CHECK(rc != -1 && WEXITSTATUS(rc) == 0);
        unlink(path);
        free(path);
    }
    CHECK(hb_shell_integration_script("tcsh") == NULL);
    CHECK(hb_shell_integration_script(NULL) == NULL);

    /* bash 端到端：pty 中 bash --norc -i，source 后
     * true/false 应产生 133 A,B 提示符标记 + C,D;code 命令标记。 */
    if (have("bash")) {
        char *path = write_tmp(hb_shell_integration_script("bash"));
        hb_pty_opts o = { "/bin/bash", "exec bash --norc -i",
                          NULL, NULL, 24, 80 };
        hb_pty *p = NULL;
        char *buf = malloc(1 << 20);
        size_t bl = 0;
        int fd = -1, i;
        const char *cmds[] = { "true\n", "false\n", "sleep 0.1\n", "exit\n" };
        struct pollfd pfd;
        int64_t deadline;

        CHECK(path != NULL);
        CHECK(hb_pty_spawn(&o, &p) == HB_OK && p);
        fd = hb_pty_fd(p);
        pfd.fd = fd; pfd.events = POLLIN;

        /* 等初始提示符出现再 source */
        deadline = hb_now_ms() + 4000;
        while (hb_now_ms() < deadline && !memmem(buf, bl, "$ ", 2)) {
            char tmp[4096]; ssize_t r;
            if (poll(&pfd, 1, 200) <= 0) continue;
            r = read(fd, tmp, sizeof tmp);
            if (r > 0) { memcpy(buf + bl, tmp, (size_t)r); bl += (size_t)r; }
        }
        {
            char src[600];
            snprintf(src, sizeof src, "source %s\n", path);
            if (write(fd, src, strlen(src)) < 0) perror("write");
        }
        usleep(400000);
        for (i = 0; i < 4; i++) {
            if (write(fd, cmds[i], strlen(cmds[i])) < 0) perror("write");
            usleep(300000);
        }
        deadline = hb_now_ms() + 5000;
        while (hb_now_ms() < deadline) {
            char tmp[4096]; ssize_t r;
            if (poll(&pfd, 1, 300) <= 0) continue;
            r = read(fd, tmp, sizeof tmp);
            if (r <= 0) break;
            memcpy(buf + bl, tmp, (size_t)r); bl += (size_t)r;
        }
        buf[bl] = 0;
        hb_pty_free(p);

        {
            size_t rem;
            char *a, *b, *cc, *d0, *c2, *d1;
            a = memmem(buf, bl, "133;A", 5);
            rem = a ? bl - (size_t)(a - buf) : 0;
            b = a ? memmem(a, rem, "133;B", 5) : NULL;
            rem = b ? rem - (size_t)(b - a) : 0;
            cc = b ? memmem(b, rem, "133;C", 5) : NULL;
            rem = cc ? rem - (size_t)(cc - b) : 0;
            d0 = cc ? memmem(cc, rem, "133;D;0", 7) : NULL;
            rem = d0 ? rem - (size_t)(d0 - cc) : 0;
            c2 = d0 ? memmem(d0, rem, "133;C", 5) : NULL;
            rem = c2 ? rem - (size_t)(c2 - d0) : 0;
            d1 = c2 ? memmem(c2, rem, "133;D;1", 7) : NULL;
            CHECK(a && b && cc && d0);
            CHECK(c2 && d1);
            if (!(a && b && cc && d0 && c2 && d1)) {
                FILE *df = fopen("/tmp/hb_si_debug.bin", "w");
                if (df) { fwrite(buf, 1, bl, df); fclose(df); }
                fprintf(stderr, "si capture dumped to /tmp/hb_si_debug.bin\n");
            }
        }
        unlink(path);
        free(path);
        free(buf);
    }
}

int main(void)
{
    test_scenarios();
    test_units();
    test_rule_tester();
    test_shell_scripts();
    printf("test_watch: %d checks, %d failures\n", hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
