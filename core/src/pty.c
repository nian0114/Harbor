#include "harbor/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif

extern char **environ;

struct hb_pty {
    int   fd;
    pid_t pid;
    bool  dead;
    int   exit_code;
};

/* 预分配好的子进程上下文：fork 后子进程里只允许 async-signal-safe 调用。 */
struct spawn_ctx {
    char    *path;      /* shell 绝对路径 */
    char   **argv;
    char   **envp;
    const char *cwd;
    sigset_t empty_mask;
};

static const char *pick_shell(const hb_pty_opts *o)
{
    const char *s;
    struct passwd *pw;
    if (o->shell && o->shell[0])
        return o->shell;
    s = getenv("SHELL");
    if (s && s[0])
        return s;
    pw = getpwuid(getuid());
    if (pw && pw->pw_shell && pw->pw_shell[0])
        return pw->pw_shell;
    return "/bin/sh";
}

/* PATH 搜索（父进程做，允许分配）。无 '/' 的名字才需要找。 */
static char *resolve_path(const char *name)
{
    const char *path, *p;
    if (strchr(name, '/'))
        return strdup(name);
    path = getenv("PATH");
    if (!path || !path[0])
        path = "/bin:/usr/bin";
    p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        char *full = malloc(dlen + 1 + strlen(name) + 1);
        if (full) {
            memcpy(full, p, dlen);
            full[dlen] = '/';
            strcpy(full + dlen + 1, name);
            if (access(full, X_OK) == 0)
                return full;
            free(full);
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return strdup(name);   /* 找不到也给 execve 一个路径，失败 _exit(127) */
}

/* envp：environ 拷贝 + TERM/COLORTERM/TERM_PROGRAM/env_extra 覆盖（后者赢）。 */
static char **build_envp(const hb_pty_opts *o)
{
    static const char *const base[] = {
        "TERM=xterm-256color",
        "COLORTERM=truecolor",
        "TERM_PROGRAM=Harbor",
    };
    size_t nenv = 0, nadd = 0, cap, i;
    char **envp;
    const char *const *e;

    while (environ[nenv])
        nenv++;
    for (e = o->env_extra; e && *e; e++)
        nadd++;
    cap = nenv + nadd + 3 + 1;
    envp = calloc(cap, sizeof *envp);
    if (!envp)
        return NULL;
    for (i = 0; i < nenv; i++)
        envp[i] = environ[i];
    nenv = i;

    for (i = 0; i < 3; i++) {
        size_t klen = strchr(base[i], '=') - base[i];
        size_t j;
        for (j = 0; j < nenv; j++)
            if (strncmp(envp[j], base[i], klen) == 0 && envp[j][klen] == '=') {
                envp[j] = (char *)base[i];
                break;
            }
        if (j == nenv)
            envp[nenv++] = (char *)base[i];
    }
    if (o->env_extra) {
        for (e = o->env_extra; *e; e++) {
            const char *eq = strchr(*e, '=');
            size_t klen, j;
            if (!eq)
                continue;
            klen = (size_t)(eq - *e);
            for (j = 0; j < nenv; j++)
                if (strncmp(envp[j], *e, klen) == 0 && envp[j][klen] == '=') {
                    envp[j] = (char *)*e;
                    break;
                }
            if (j == nenv)
                envp[nenv++] = (char *)*e;
        }
    }
    envp[nenv] = NULL;
    return envp;
}

static char **build_argv(const hb_pty_opts *o, const char *shell)
{
    char **argv;
    if (o->command) {
        argv = calloc(4, sizeof *argv);
        if (argv) {
            argv[0] = (char *)shell;
            argv[1] = "-c";
            argv[2] = (char *)o->command;
        }
    } else {
        const char *base = strrchr(shell, '/');
        char *a0;
        base = base ? base + 1 : shell;
        argv = calloc(2, sizeof *argv);
        a0 = malloc(strlen(base) + 2);
        if (argv && a0) {
            a0[0] = '-';
            strcpy(a0 + 1, base);
            argv[0] = a0;
        }
    }
    return argv;
}

/* 子进程（fork 后）：只允许 async-signal-safe 调用。 */
static void child_exec(const struct spawn_ctx *c)
{
    /* 还原信号处置：继承的 SIG_IGN（如 GLib 忽略 SIGPIPE）会随 exec
     * 漏到 shell，破坏管道；屏蔽集也可能被父进程带进来。 */
    struct sigaction sa = { .sa_handler = SIG_DFL };
    int i;
    for (i = 1; i < NSIG; i++)
        sigaction(i, &sa, NULL);
    sigprocmask(SIG_SETMASK, &c->empty_mask, NULL);

    if (c->cwd && c->cwd[0])
        chdir(c->cwd);
    execve(c->path, c->argv, c->envp);
    _exit(127);
}

static void free_spawn_ctx(struct spawn_ctx *c)
{
    if (c->argv) {
        if (!c->argv[1] && c->argv[0])
            free(c->argv[0]);   /* 登录 shell 的 "-name" 是单独分配的 */
        free(c->argv);
    }
    free(c->envp);
    free(c->path);
}

hb_err hb_pty_spawn(const hb_pty_opts *opts, hb_pty **out)
{
    hb_pty *p;
    struct spawn_ctx c = {0};
    struct winsize ws = {0, 0, 0, 0};
    pid_t pid;
    int fd;
    const char *shell;

    *out = NULL;
    if (!opts)
        return HB_ERR_INVALID;
    shell = pick_shell(opts);
    ws.ws_row = (unsigned short)(opts->rows > 0 ? opts->rows : 24);
    ws.ws_col = (unsigned short)(opts->cols > 0 ? opts->cols : 80);

    /* fork 前把所有需要分配的东西做完（多线程进程里 malloc 锁可能
     * 被别的线程占着）。 */
    c.path = resolve_path(shell);
    c.argv = build_argv(opts, shell);
    c.envp = build_envp(opts);
    c.cwd = (opts->cwd && opts->cwd[0]) ? opts->cwd : getenv("HOME");
    sigemptyset(&c.empty_mask);
    if (!c.path || !c.argv || !c.envp) {
        free_spawn_ctx(&c);
        return HB_ERR_NOMEM;
    }

    p = calloc(1, sizeof *p);
    if (!p) {
        free_spawn_ctx(&c);
        return HB_ERR_NOMEM;
    }

    pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) {
        free(p);
        free_spawn_ctx(&c);
        return HB_ERR_IO;
    }
    if (pid == 0)
        child_exec(&c);
    free_spawn_ctx(&c);

    p->fd = fd;
    p->pid = pid;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    *out = p;
    return HB_OK;
}

int hb_pty_fd(hb_pty *p)  { return p ? p->fd : -1; }
pid_t hb_pty_pid(hb_pty *p) { return p ? p->pid : -1; }

hb_err hb_pty_resize(hb_pty *p, int rows, int cols)
{
    struct winsize ws;
    if (!p || rows <= 0 || cols <= 0)
        return HB_ERR_INVALID;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if (ioctl(p->fd, TIOCSWINSZ, &ws) != 0)
        return HB_ERR_IO;
    kill(-p->pid, SIGWINCH);
    return HB_OK;
}

bool hb_pty_reap(hb_pty *p, int *exit_code)
{
    int st;
    pid_t r;
    if (!p || p->dead) {
        if (exit_code && p)
            *exit_code = p->exit_code;
        return true;
    }
    r = waitpid(p->pid, &st, WNOHANG);
    if (r <= 0)
        return false;
    p->dead = true;
    if (WIFEXITED(st))
        p->exit_code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st))
        p->exit_code = 128 + WTERMSIG(st);
    else
        p->exit_code = st;
    if (exit_code)
        *exit_code = p->exit_code;
    return true;
}

void hb_pty_hangup(hb_pty *p)
{
    if (p && !p->dead)
        kill(-p->pid, SIGHUP);
}

void hb_pty_free(hb_pty *p)
{
    int code;
    int i;
    if (!p)
        return;
    if (p->fd >= 0)
        close(p->fd);
    if (!p->dead) {
        kill(-p->pid, SIGHUP);
        for (i = 0; i < 20 && !hb_pty_reap(p, &code); i++)
            usleep(5000);
        if (!p->dead) {
            kill(-p->pid, SIGKILL);
            waitpid(p->pid, NULL, 0);
            p->dead = true;
        }
    }
    free(p);
}
