#ifndef HB_PTY_H
#define HB_PTY_H

#include "common.h"

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *shell;       /* NULL → $SHELL → getpwuid shell → /bin/sh */
    const char *command;     /* 非 NULL → 运行 `shell -c command`（非登录） */
    const char *cwd;         /* NULL → $HOME */
    const char *const *env_extra; /* "K=V"，NULL 结尾，可为 NULL */
    int rows, cols;
} hb_pty_opts;

typedef struct hb_pty hb_pty;

/* forkpty 启动；无 command 时为登录 shell（argv[0]="-basename"）。
 * 设置 TERM=xterm-256color COLORTERM=truecolor TERM_PROGRAM=Harbor。
 * fd 置 O_NONBLOCK | O_CLOEXEC。 */
hb_err hb_pty_spawn(const hb_pty_opts *opts, hb_pty **out);

int    hb_pty_fd(hb_pty *p);
pid_t  hb_pty_pid(hb_pty *p);

hb_err hb_pty_resize(hb_pty *p, int rows, int cols);   /* TIOCSWINSZ → SIGWINCH */

/* waitpid WNOHANG；exit_code = 退出码或 128+信号。返回 false = 还在运行。 */
bool   hb_pty_reap(hb_pty *p, int *exit_code);

/* 对进程组发 SIGHUP。 */
void   hb_pty_hangup(hb_pty *p);

/* 关闭 fd；若仍存活：SIGHUP → 短暂等待 → SIGKILL → waitpid 收尾。 */
void   hb_pty_free(hb_pty *p);

#ifdef __cplusplus
}
#endif

#endif
