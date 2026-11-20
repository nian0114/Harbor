#ifndef HB_WATCH_H
#define HB_WATCH_H

/* ------------------------------------------------------------------ */
/* hb_watch：命令完成监测引擎（FR-13）。                                  */
/*                                                                     */
/* 纯 C、POSIX regex、无线程；时间由调用方注入（便于测试与 UI 集成）。      */
/* 消费模型：UI 把 PTY 原始输出喂给 hb_watch_feed，把 vt 已解析的          */
/* OSC133 / BEL / OSC 9|777 事件转给 hb_watch_osc133 / hb_watch_bell /   */
/* hb_watch_notify，把用户输入转给 hb_watch_input；随后按                 */
/* hb_watch_poll 返回的 next_ms 定时轮询取 hit（FIFO）。                  */
/*                                                                     */
/* 注意：加密 OpenSSH 私钥不适用本模块；本模块不读取任何 vault 数据。      */
/* ------------------------------------------------------------------ */

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HB_WATCH_SIG_EOF        = 1,
    HB_WATCH_SIG_OSC133     = 2,
    HB_WATCH_SIG_RULES      = 4,
    HB_WATCH_SIG_IDLE       = 8,
    HB_WATCH_SIG_TERMNOTIFY = 16,   /* BEL 与 OSC 9/777 应用主动通知 */
} hb_watch_sig;

typedef struct {
    const char *name;      /* "pytest", "user:1", … */
    const char *ok_re;     /* ERE, case-insensitive; NULL = none */
    const char *err_re;    /* ERE; NULL = none */
} hb_watch_rule;

typedef struct {
    unsigned signals;              /* default EOF|OSC133|RULES|TERMNOTIFY (IDLE off) */
    bool     presets;              /* built-in presets on (default true) */
    const hb_watch_rule *rules; size_t nrules;   /* extra user rules (global + per-host, caller merges) */
    int      min_duration_ms;      /* OSC133/rule/idle: ignore tasks shorter than this (default 5000) */
    int      confirm_quiet_ms;     /* rule hit confirmed after this much silence (default 1500) */
    int      idle_active_ms;       /* idle: ≥ this much output activity … (default 10000) */
    int      idle_quiet_ms;        /* … then ≥ this much silence (default 5000) */
    int      bell_after_input_ms;  /* BEL within this of user input = interactive feedback, ignored (default 2000) */
} hb_watch_cfg;

typedef struct {
    hb_watch_sig signal;
    bool     ok;
    int      exit_code;        /* OSC133 D / EOF code; -1 unknown */
    char    *rule;             /* preset/rule name or human label ("OSC 133", "会话结束（EOF）", "BEL", …) */
    char    *line;             /* matched/notified text (stripped, ≤200 bytes); NULL if none */
    char    *command;          /* OSC133: echoed command between B and C, stripped/trimmed; else NULL */
    int64_t  duration_ms;      /* from task start (last user input or 133 C) to the hit; -1 unknown */
    char    *title;            /* OSC 777 title; NULL otherwise */
} hb_watch_hit;

typedef struct hb_watch hb_watch;

void hb_watch_cfg_defaults(hb_watch_cfg *c);
hb_watch *hb_watch_new(const hb_watch_cfg *c);   /* deep-copies; compiles regexes; NULL on bad regex */
void hb_watch_free(hb_watch *w);
void hb_watch_feed(hb_watch *w, const char *bytes, size_t len, int64_t now_ms);  /* raw PTY output */
void hb_watch_input(hb_watch *w, int64_t now_ms);                  /* user keystroke/paste */
void hb_watch_osc133(hb_watch *w, char mark, int code, int64_t now_ms);
void hb_watch_bell(hb_watch *w, int64_t now_ms);
void hb_watch_notify(hb_watch *w, const char *title, const char *body, int64_t now_ms);  /* OSC 9 (title NULL) / 777 */
void hb_watch_eof(hb_watch *w, int code, int64_t now_ms);
/* pops one pending hit (FIFO); next_ms (may be NULL) = when to poll again, -1 = only on new input */
bool hb_watch_poll(hb_watch *w, int64_t now_ms, hb_watch_hit *out, int64_t *next_ms);
void hb_watch_hit_clear(hb_watch_hit *h);
/* rule tester (FR-13 acceptance 6): feeds text as lines 50 ms apart after a synthetic input, then
 * silence; returns true + hit if anything fires. Uses the same engine. */
bool hb_watch_test_text(const hb_watch_cfg *c, const char *sample, hb_watch_hit *out);
const hb_watch_rule *hb_watch_presets(size_t *n);
/* shell integration: static script text for "bash" | "zsh" | "fish"; NULL otherwise */
const char *hb_shell_integration_script(const char *shell);

#ifdef __cplusplus
}
#endif

#endif /* HB_WATCH_H */
