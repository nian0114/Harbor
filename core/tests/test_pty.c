#include "test.h"

#include "harbor/pty.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 读 fd 直到 EOF/EIO 或超时；返回累计字节数。 */
static size_t read_until_eof(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t len = 0;
    int waited = 0;
    for (;;) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pf, 1, 100);
        if (r > 0 && (pf.revents & POLLIN)) {
            ssize_t n = read(fd, buf + len, cap - len - 1);
            if (n > 0) {
                len += (size_t)n;
                waited = 0;
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EINTR))
                break;
        } else if (r == 0) {
            waited += 100;
            if (waited > timeout_ms)
                break;
        }
        if (r > 0 && (pf.revents & (POLLHUP | POLLERR | POLLNVAL)))
            break;
        if (r < 0 && errno != EINTR)
            break;
    }
    buf[len] = 0;
    return len;
}

static void test_spawn_exit(void)
{
    hb_pty_opts o = { .shell = "/bin/sh", .command = "printf hello; exit 3",
                      .rows = 24, .cols = 80 };
    hb_pty *p = NULL;
    char buf[4096];
    int code = -1;
    int i;

    CHECK_EQ_INT(hb_pty_spawn(&o, &p), HB_OK);
    CHECK(p != NULL);
    read_until_eof(hb_pty_fd(p), buf, sizeof buf, 3000);
    CHECK(strstr(buf, "hello") != NULL);
    for (i = 0; i < 50 && !hb_pty_reap(p, &code); i++)
        usleep(10000);
    CHECK_EQ_INT(code, 3);
    hb_pty_free(p);
}

static void test_env(void)
{
    hb_pty_opts o = { .shell = "/bin/sh",
                      .command = "printf '%s|%s|%s' \"$TERM\" \"$COLORTERM\" \"$TERM_PROGRAM\"; exit 0",
                      .rows = 24, .cols = 80 };
    hb_pty *p = NULL;
    char buf[4096];
    int code = -1, i;

    CHECK_EQ_INT(hb_pty_spawn(&o, &p), HB_OK);
    read_until_eof(hb_pty_fd(p), buf, sizeof buf, 3000);
    CHECK(strstr(buf, "xterm-256color|truecolor|Harbor") != NULL);
    for (i = 0; i < 50 && !hb_pty_reap(p, &code); i++)
        usleep(10000);
    CHECK_EQ_INT(code, 0);
    hb_pty_free(p);
}

static void test_resize(void)
{
    hb_pty_opts o = { .shell = "/bin/sh", .command = NULL,
                      .rows = 24, .cols = 80 };
    hb_pty *p = NULL;
    char buf[4096];
    int code = -1, i;

    CHECK_EQ_INT(hb_pty_spawn(&o, &p), HB_OK);
    CHECK_EQ_INT(hb_pty_resize(p, 10, 30), HB_OK);
    /* 先 resize 再让 shell 报尺寸 */
    {
        const char *cmd = "stty size; exit\n";
        ssize_t off = 0, want = (ssize_t)strlen(cmd);
        while (off < want) {
            ssize_t n = write(hb_pty_fd(p), cmd + off, (size_t)(want - off));
            if (n > 0) off += n;
            else if (n < 0 && errno == EAGAIN) usleep(5000);
            else break;
        }
    }
    read_until_eof(hb_pty_fd(p), buf, sizeof buf, 3000);
    CHECK(strstr(buf, "10 30") != NULL);
    for (i = 0; i < 50 && !hb_pty_reap(p, &code); i++)
        usleep(10000);
    CHECK_EQ_INT(code, 0);
    hb_pty_free(p);
}

/* 子进程不应继承父进程的信号处置/屏蔽：忽略 SIGPIPE + 阻塞 SIGUSR1
 * 后 spawn，读 /proc/self/status 验证 SigIgn 无 SIGPIPE 位、SigBlk 全 0。 */
static void test_signal_state(void)
{
    struct sigaction sa = { .sa_handler = SIG_IGN };
    struct sigaction old_sa;
    sigset_t blk, old_mask;
    hb_pty_opts o = { .shell = "/bin/sh",
                      .command = "cat /proc/self/status | grep -E '^Sig(Ign|Blk)'; exit 0",
                      .rows = 24, .cols = 80 };
    hb_pty *p = NULL;
    char buf[4096];
    int code = -1, i;
    char *ign, *blkline;
    unsigned long long ign_v, blk_v;

    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, &old_sa);
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blk, &old_mask);

    CHECK_EQ_INT(hb_pty_spawn(&o, &p), HB_OK);
    read_until_eof(hb_pty_fd(p), buf, sizeof buf, 3000);
    for (i = 0; i < 50 && !hb_pty_reap(p, &code); i++)
        usleep(10000);
    hb_pty_free(p);

    sigaction(SIGPIPE, &old_sa, NULL);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    ign = strstr(buf, "SigIgn:");
    blkline = strstr(buf, "SigBlk:");
    CHECK(ign != NULL);
    CHECK(blkline != NULL);
    if (ign) {
        ign_v = strtoull(ign + 7, NULL, 16);
        CHECK((ign_v & (1ULL << (SIGPIPE - 1))) == 0);
    }
    if (blkline) {
        blk_v = strtoull(blkline + 7, NULL, 16);
        CHECK_EQ_INT(blk_v, 0);
    }
}

int main(void)
{
    test_spawn_exit();
    test_env();
    test_resize();
    test_signal_state();
    fprintf(stderr, "test_pty: %d checks, %d failures\n", hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
