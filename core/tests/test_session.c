#include "test.h"

#include "harbor/session.h"

#include <poll.h>
#include <stdlib.h>
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
        int r = poll(&pf, 1, 100);
        if (r > 0 && (pf.revents & POLLIN)) {
            while (n < max && hb_session_next_event(s, &evs[n]))
                n++;
        }
        if (n > 0)
            return n;
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

int main(void)
{
    test_local_exit();
    test_local_interactive();
    fprintf(stderr, "test_session: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
