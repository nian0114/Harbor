#include "test.h"

#include "harbor/session.h"
#include "harbor/watch.h"

#include <poll.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

/* 等到至少一类事件发生；返回 drain 出的事件数，evs 存 copy */
static int drain_events(hb_session *s, hb_sess_event *evs, int max,
                        int timeout_ms)
{
    int n = 0;
    int waited = 0;
    for (;;) {
        struct pollfd pf = { .fd = hb_session_event_fd(s),
                             .events = POLLIN, .revents = 0 };
        int r = poll(&pf, 1, 100), i;
        if (r > 0 && (pf.revents & POLLIN)) {
            while (n < max && hb_session_next_event(s, &evs[n]))
                n++;
        }
        /* 子进程 exit 可能在 OUTPUT 之后才到达：见到 EXIT 再收尾 */
        for (i = 0; i < n; i++)
            if (evs[i].type == HB_SESS_EV_EXIT) {
                while (n < max && hb_session_next_event(s, &evs[n]))
                    n++;
                return n;
            }
        waited += 100;
        if (waited > timeout_ms)
            return n;
    }
}

/* 持续 drain 直到出现某类型事件或超时 */
static int wait_for(hb_session *s, hb_sess_ev_type type, hb_sess_event *found,
                    hb_sess_event *scratch, int timeout_ms)
{
    int waited = 0;
    while (waited <= timeout_ms) {
        hb_sess_event ev;
        while (hb_session_next_event(s, &ev)) {
            if (ev.type == type) {
                if (found)
                    *found = ev;
                else
                    hb_sess_event_clear(&ev);
                if (scratch)
                    hb_sess_event_clear(scratch);
                return 1;
            }
            if (scratch)
                *scratch = ev;
            else
                hb_sess_event_clear(&ev);
            scratch = NULL;
        }
        usleep(20000);
        waited += 20;
    }
    return 0;
}

static int screen_has(hb_session *s, const char *text)
{
    hb_vt *vt = hb_session_vt(s);
    int rows, cols, r, found = 0;
    hb_vt_size(vt, &rows, &cols);
    hb_vt_lock(vt);
    for (r = 0; r < rows && !found; r++) {
        char *t = hb_vt_get_text(vt, r, 0, r, cols, false);
        if (t && strstr(t, text))
            found = 1;
        free(t);
    }
    hb_vt_unlock(vt);
    return found;
}

static void test_local_exit(void)
{
    hb_pty_opts o = { .shell = "/bin/sh", .command = "printf 'abc\\n'; exit 7",
                      .rows = 10, .cols = 40 };
    hb_session *s = hb_session_new_local(&o, 100);
    hb_sess_event evs[64];
    hb_sess_event *exit_ev = NULL;
    int n, i;
    int saw_output = 0;
    char *line;

    CHECK(s != NULL);
    n = drain_events(s, evs, 64, 3000);
    for (i = 0; i < n; i++) {
        if (evs[i].type == HB_SESS_EV_OUTPUT)
            saw_output = 1;
        if (evs[i].type == HB_SESS_EV_EXIT)
            exit_ev = &evs[i];
    }
    CHECK(saw_output);
    CHECK(exit_ev != NULL);
    if (exit_ev)
        CHECK_EQ_INT(exit_ev->code, 7);

    line = hb_vt_get_text(hb_session_vt(s), 0, 0, 0, 40, false);
    CHECK(line && strncmp(line, "abc", 3) == 0);
    free(line);
    CHECK(!hb_session_alive(s));
    for (i = 0; i < n; i++)
        hb_sess_event_clear(&evs[i]);
    hb_session_free(s);
}

static void test_local_interactive(void)
{
    hb_pty_opts o = { .shell = "/bin/sh", .command = "cat",
                      .rows = 10, .cols = 40 };
    hb_session *s = hb_session_new_local(&o, 100);
    hb_sess_event ev;
    int i, ok = 0;

    CHECK(s != NULL);
    hb_session_write(s, "ping\n", 5);
    /* 等回显出现在屏幕上（OUTPUT 事件驱动） */
    for (i = 0; i < 100 && !ok; i++) {
        while (hb_session_next_event(s, &ev))
            hb_sess_event_clear(&ev);
        if (screen_has(s, "ping")) {
            ok = 1;
            break;
        }
        usleep(20000);
    }
    CHECK(ok);

    /* Ctrl+C → cat 收到 SIGINT */
    hb_session_unichar(s, 'c', HB_MOD_CTRL);
    memset(&ev, 0, sizeof ev);
    ok = wait_for(s, HB_SESS_EV_EXIT, &ev, NULL, 3000);
    CHECK(ok);
    if (ok)
        CHECK_EQ_INT(ev.code, 130);   /* 128 + SIGINT(2) */
    hb_sess_event_clear(&ev);
    hb_session_free(s);
}

/* steal_vt + new_local_vt：A 会话写的内容在 B 会话接管 vt 后仍可读 */
static void test_vt_reuse(void)
{
    hb_pty_opts o = { .shell = "/bin/sh",
                      .command = "printf 'KEEPTHIS\\n'",
                      .rows = 10, .cols = 40 };
    hb_session *sa = hb_session_new_local(&o, 100);
    hb_session *sb;
    hb_vt *vt;
    hb_sess_event ev;
    char *line;
    int ok;

    CHECK(sa != NULL);
    if (!sa)
        return;
    ok = wait_for(sa, HB_SESS_EV_EXIT, &ev, NULL, 3000);
    CHECK(ok);
    hb_sess_event_clear(&ev);

    vt = hb_session_steal_vt(sa);
    CHECK(vt != NULL);
    CHECK(hb_session_vt(sa) == NULL);

    {
        hb_pty_opts o2 = { .shell = "/bin/sh",
                           .command = "printf 'SECOND\\n'",
                           .rows = 10, .cols = 40 };
        sb = hb_session_new_local_vt(&o2, vt, 100);
    }
    CHECK(sb != NULL);
    if (!sb) {
        hb_vt_free(vt);
        hb_session_free(sa);
        return;
    }
    CHECK(hb_session_vt(sb) == vt);
    ok = wait_for(sb, HB_SESS_EV_EXIT, &ev, NULL, 3000);
    CHECK(ok);
    hb_sess_event_clear(&ev);

    /* A 的 "KEEPTHIS" 与 B 的 "SECOND" 都在同一个 vt 里 */
    line = hb_vt_get_text(hb_session_vt(sb), 0, 0, 0, 39, false);
    CHECK(line && strncmp(line, "KEEPTHIS", 8) == 0);
    free(line);
    line = hb_vt_get_text(hb_session_vt(sb), 1, 0, 1, 39, false);
    CHECK(line && strncmp(line, "SECOND", 6) == 0);
    free(line);

    hb_session_free(sb);
    hb_session_free(sa);
}

/* FR-13：watch 引擎接入 session。
 * 用 sh 脚本在一条命令里输出 aider 完成行、响铃并 exit 3 ——
 * 期望：rule hit → （可选 BEL）→ EOF hit(exit_code 3, ok=false) → EXIT，
 * EOF hit 必须排在 EXIT 之前。cfg 用 min_duration=0 以压缩时间线。 */
static void test_watch_session(void)
{
    hb_pty_opts o = { .shell = "/bin/sh",
        .command = "sleep 0.3; "
                   "printf 'Tokens: 1k sent, 1k received.\\n'; "
                   "sleep 0.1; exit 3",
        .rows = 10, .cols = 60 };
    hb_session *s;
    hb_watch_cfg c;
    hb_sess_event evs[64];
    hb_sess_event *watch1 = NULL, *eof_hit = NULL, *exit_ev = NULL;
    int n, i, saw_rule = 0;

    hb_watch_cfg_defaults(&c);
    c.min_duration_ms = 0;
    c.confirm_quiet_ms = 50;
    s = hb_session_new_local(&o, 100);
    CHECK(s != NULL);
    hb_session_set_watch(s, &c);
    hb_session_write(s, "x\n", 2);   /* 用户输入 → armed */

    n = drain_events(s, evs, 64, 4000);
    for (i = 0; i < n; i++) {
        if (evs[i].type == HB_SESS_EV_WATCH && !watch1)
            watch1 = &evs[i];
        if (evs[i].type == HB_SESS_EV_EXIT)
            exit_ev = &evs[i];
    }
    /* rule hit（aider）应在 EOF hit 前出现且是第一个 WATCH */
    CHECK(watch1 != NULL);
    if (watch1) {
        saw_rule = watch1->watch &&
                   watch1->watch->signal == HB_WATCH_SIG_RULES;
        CHECK(saw_rule);
        if (watch1->watch && watch1->watch->rule)
            CHECK_EQ_STR(watch1->watch->rule, "aider");
    }
    /* 队列里最后一个 WATCH 必须是 EOF（exit 3 → ok=false），且在 EXIT 前 */
    for (i = 0; i < n; i++)
        if (evs[i].type == HB_SESS_EV_WATCH &&
            evs[i].watch && evs[i].watch->signal == HB_WATCH_SIG_EOF)
            eof_hit = &evs[i];
    CHECK(eof_hit != NULL);
    if (eof_hit) {
        CHECK(eof_hit->watch->exit_code == 3);
        CHECK(!eof_hit->watch->ok);
        CHECK(exit_ev != NULL && eof_hit < exit_ev);
    }
    for (i = 0; i < n; i++)
        hb_sess_event_clear(&evs[i]);
    hb_session_free(s);
}

/* set_watch(NULL) 中途关闭：之后不再有 WATCH 事件 */
static void test_watch_off(void)
{
    hb_pty_opts o = { .shell = "/bin/sh", .command = "cat",
                      .rows = 10, .cols = 60 };
    hb_session *s;
    hb_watch_cfg c;
    hb_sess_event ev;
    int i, saw = 0;

    hb_watch_cfg_defaults(&c);
    c.min_duration_ms = 0;
    c.confirm_quiet_ms = 50;
    s = hb_session_new_local(&o, 100);
    CHECK(s != NULL);
    hb_session_set_watch(s, &c);
    hb_session_write(s, "Tokens: 2k sent, 1k received.\n", 30);
    usleep(600000);
    while (hb_session_next_event(s, &ev)) {   /* 先排干第一只 hit */
        if (ev.type == HB_SESS_EV_WATCH)
            saw = 1;
        hb_sess_event_clear(&ev);
    }
    CHECK(saw);                               /* 开着确实打得出 */

    hb_session_set_watch(s, NULL);            /* 关掉 */
    hb_session_write(s, "Tokens: 3k sent, 1k received.\n", 30);
    hb_session_close(s);
    saw = 0;
    for (i = 0; i < 150; i++) {               /* ~3s 内不应再有 WATCH */
        while (hb_session_next_event(s, &ev)) {
            if (ev.type == HB_SESS_EV_WATCH &&
                ev.watch && ev.watch->signal == HB_WATCH_SIG_RULES)
                saw = 1;
            hb_sess_event_clear(&ev);
        }
        if (!hb_session_alive(s)) {
            /* 排干残余再判 */
            while (hb_session_next_event(s, &ev)) {
                if (ev.type == HB_SESS_EV_WATCH &&
                    ev.watch && ev.watch->signal == HB_WATCH_SIG_RULES)
                    saw = 1;
                hb_sess_event_clear(&ev);
            }
            break;
        }
        usleep(20000);
    }
    CHECK(!saw);
    hb_session_free(s);
}

/* ------------------------------ Shell History（FR-08） ------------------------------ */

/* 逐字符走 hb_session_unichar + ENTER（同终端真实输入路径） */
static void hist_type(hb_session *s, const char *str)
{
    const char *p;
    for (p = str; *p; p++)
        /* GTK 对需 Shift 的字符（大写、"、:、| …）带 Shift 修饰位，照样模拟 */
        hb_session_unichar(s, (uint32_t)(unsigned char)*p,
                           (isupper((unsigned char)*p) ||
                            strchr("~!@#$%^&*()_+{}|:\"<>?", *p))
                               ? HB_MOD_SHIFT : 0);
}

static void hist_enter(hb_session *s)
{
    hb_session_key(s, HB_KEY_ENTER, 0);
}

static void hist_type_cmd(hb_session *s, const char *cmd)
{
    hist_type(s, cmd);
    usleep(200000);                    /* 等回显回环（真实键入必有此延迟） */
    hist_enter(s);
}

static void hist_stash(hb_session *s, char cmds[][256], int *n, int max)
{
    hb_sess_event ev;
    while (hb_session_next_event(s, &ev)) {
        if (ev.type == HB_SESS_EV_COMMAND && *n < max) {
            const char *t = ev.text ? ev.text : "";
            strncpy(cmds[*n], t, 255);
            cmds[*n][255] = 0;
            (*n)++;
        }
        hb_sess_event_clear(&ev);
    }
}

/* 等到下一个 OSC133 'B'（提示符画完），最长 timeout_ms；
 * 期间到达的 COMMAND 事件一并收集进 cmds，不丢弃。 */
static int wait_osc_b(hb_session *s, char cmds[][256], int *n, int max,
                      int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        hb_sess_event ev;
        while (hb_session_next_event(s, &ev)) {
            int is_b = ev.type == HB_SESS_EV_OSC133 && ev.mark == 'B';
            if (ev.type == HB_SESS_EV_COMMAND && *n < max) {
                const char *t = ev.text ? ev.text : "";
                strncpy(cmds[*n], t, 255);
                cmds[*n][255] = 0;
                (*n)++;
            }
            hb_sess_event_clear(&ev);
            if (is_b)
                return 1;
        }
        usleep(20000);
        waited += 20;
    }
    return 0;
}

/* 收集 timeout_ms 内全部 COMMAND 文本（其余事件丢弃）。
 * *n 为起始下标（之前 wait_osc_b 可能已收集过）。 */
static int hist_collect(hb_session *s, char cmds[][256], int *n, int max,
                        int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        hist_stash(s, cmds, n, max);
        usleep(20000);
        waited += 20;
    }
    return *n;
}

static int hist_has(char cmds[][256], int n, const char *want)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(cmds[i], want) == 0)
            return 1;
    return 0;
}

static int wait_prompt(hb_session *s, int timeout_ms)
{
    int waited = 0;
    hb_sess_event ev;
    while (waited < timeout_ms && !screen_has(s, "$")) {
        while (hb_session_next_event(s, &ev))
            hb_sess_event_clear(&ev);
        usleep(100000);
        waited += 100;
    }
    while (hb_session_next_event(s, &ev))
        hb_sess_event_clear(&ev);
    return 1;
}

static hb_session *spawn_bash(void)
{
    hb_pty_opts o = { .shell = "/bin/sh",
                      .command = "exec bash --norc -i",
                      .rows = 24, .cols = 80 };
    return hb_session_new_local(&o, 200);
}

/* 回退启发式：普通命令采集；read -s 密码不采；[sudo] password 提示不采；
 * 前导空格（ignorespace）不采。 */
static void test_history_heuristic(void)
{
    hb_session *s;
    char cmds[16][256];
    int n;

    s = spawn_bash();
    CHECK(s != NULL);
    if (!s)
        return;
    hb_session_set_history(s, true);
    wait_prompt(s, 4000);

    hist_type_cmd(s, "echo hist-a");
    usleep(500000);

    /* read -s：秘密无回显 → 不采（read 命令本身是会采集的） */
    hist_type_cmd(s, "read -s -p \"Password: \" x");
    usleep(600000);
    hist_type(s, "s3cret-hist");
    usleep(300000);                    /* 若会回显此刻必已到达 */
    hist_enter(s);
    usleep(600000);

    /* read -p（有回显）但提示是密码样式 → 不采 */
    hist_type_cmd(s, "read -p \"[sudo] password for x: \" y");
    usleep(600000);
    hist_type(s, "sudo-secret-99");
    usleep(300000);
    hist_enter(s);
    usleep(600000);

    hist_type_cmd(s, " echo spaced-out");    /* 前导空格 → 不采 */
    usleep(500000);
    hist_type_cmd(s, "echo hist-b");
    usleep(800000);

    n = 0;
    hist_collect(s, cmds, &n, 16, 1500);
    CHECK(hist_has(cmds, n, "echo hist-a"));
    CHECK(hist_has(cmds, n, "read -s -p \"Password: \" x"));
    CHECK(hist_has(cmds, n, "read -p \"[sudo] password for x: \" y"));
    CHECK(hist_has(cmds, n, "echo hist-b"));
    CHECK(!hist_has(cmds, n, "s3cret-hist"));
    CHECK(!hist_has(cmds, n, "sudo-secret-99"));
    CHECK(!hist_has(cmds, n, " echo spaced-out"));
    CHECK(!hist_has(cmds, n, "echo spaced-out"));   /* trim 后也不应出现 */
    hb_session_close(s);
    hb_session_free(s);
}

/* OSC 133：集成脚本下命令取自 B..C 回显；C 之后的输入（如 read 的应答）
 * 绝不入史。 */
static void test_history_osc133(void)
{
    char tmp[] = "/tmp/hb-si-XXXXXX";
    hb_session *s;
    char cmds[16][256];
    int n = 0, fd;
    const char *script = hb_shell_integration_script("bash");
    char src[700];

    if (!script) {
        fprintf(stderr, "  bash integration script missing, skipped\n");
        return;
    }
    fd = mkstemp(tmp);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    if (write(fd, script, strlen(script)) < 0) perror("write");
    close(fd);

    s = spawn_bash();
    CHECK(s != NULL);
    if (!s) { unlink(tmp); return; }
    hb_session_set_history(s, true);
    wait_prompt(s, 4000);

    snprintf(src, sizeof src, "source %s\n", tmp);
    hb_session_write(s, src, strlen(src));
    CHECK(wait_osc_b(s, cmds, &n, 16, 4000));  /* source 完成 + 带 B 的提示符 */

    hist_type_cmd(s, "echo osc-abc");
    CHECK(wait_osc_b(s, cmds, &n, 16, 4000));

    /* read 的应答在 C 之后输入 → 不采集 */
    hist_type_cmd(s, "read -p \"answer: \" v");
    usleep(600000);
    hist_type(s, "post-c-secret");
    usleep(300000);
    hist_enter(s);
    CHECK(wait_osc_b(s, cmds, &n, 16, 4000));

    hist_type_cmd(s, "echo osc-tail");
    usleep(800000);

    hist_collect(s, cmds, &n, 16, 1500);
    CHECK(hist_has(cmds, n, "echo osc-abc"));
    CHECK(hist_has(cmds, n, "read -p \"answer: \" v"));
    CHECK(hist_has(cmds, n, "echo osc-tail"));
    CHECK(!hist_has(cmds, n, "post-c-secret"));
    hb_session_close(s);
    hb_session_free(s);
    unlink(tmp);
}

int main(void)
{
    test_local_exit();
    test_local_interactive();
    test_vt_reuse();
    test_watch_session();
    test_watch_off();
    test_history_heuristic();
    test_history_osc133();
    fprintf(stderr, "test_session: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
