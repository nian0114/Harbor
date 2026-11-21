/* 用户态 sshd fixture：hostkey + client key + authorized_keys +
 * sshd_config（含 Subsystem sftp internal-sftp）→ 起 127.0.0.1:随机端口。 */

#include "sshd_fixture.h"

#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int port;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (void *)&a, sizeof a) || getsockname(fd, (void *)&a, &al)) {
        close(fd);
        return 0;
    }
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static char *slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return NULL;
    n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void field2(const char *line, char *out, size_t cap)
{
    /* 第 2 个空白分隔字段（hostkey.pub 的 base64） */
    const char *p = line;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' &&
           n + 1 < cap)
        n++;
    memcpy(out, p, n);
    out[n] = 0;
}

int hb_sshd_start(hb_sshd *fx)
{
    char path[PATH_MAX + 64];
    char b64[600];
    char extra[sizeof fx->extra_cfg];

    /* 调用方可在 start 前填 extra_cfg；先取出再清零。 */
    memcpy(extra, fx->extra_cfg, sizeof extra);
    memset(fx, 0, sizeof *fx);
    memcpy(fx->extra_cfg, extra, sizeof extra);
    fx->extra_cfg[sizeof fx->extra_cfg - 1] = 0;
    if (access("/usr/sbin/sshd", X_OK) != 0 ||
        access("/usr/bin/ssh-keygen", X_OK) != 0)
        return 77;
    if (!getpwuid(getuid()) || !getpwuid(getuid())->pw_name)
        return 77;
    snprintf(fx->user, sizeof fx->user, "%s", getpwuid(getuid())->pw_name);

    snprintf(fx->dir, sizeof fx->dir, "/tmp/hb-sftp-test-XXXXXX");
    if (!mkdtemp(fx->dir))
        return -1;
    fx->port = free_port();
    if (!fx->port)
        return -1;

    snprintf(path, sizeof path,
             "/usr/bin/ssh-keygen -t ed25519 -N '' -f %s/hostkey -q",
             fx->dir);
    if (system(path) != 0)
        return -1;
    snprintf(path, sizeof path,
             "/usr/bin/ssh-keygen -t ed25519 -N '' -f %s/client -q",
             fx->dir);
    if (system(path) != 0)
        return -1;
    snprintf(fx->keyfile, sizeof fx->keyfile, "%s/client", fx->dir);

    {
        FILE *f;
        snprintf(path, sizeof path, "%s/authorized_keys", fx->dir);
        f = fopen(path, "w");
        if (!f)
            return -1;
        {
            char pub[1024];
            snprintf(path, sizeof path, "%s/client.pub", fx->dir);
            if (!slurp(path, pub, sizeof pub)) {
                fclose(f);
                return -1;
            }
            fputs(pub, f);
        }
        fclose(f);
    }

    snprintf(path, sizeof path, "%s/sshd_config", fx->dir);
    {
        FILE *f = fopen(path, "w");
        if (!f)
            return -1;
        /* extra_cfg 先写：sshd 多数指令取"先出现的值"，
         * 用它才能覆盖下面的 AllowTcpForwarding yes 等默认行。 */
        if (fx->extra_cfg[0])
            fputs(fx->extra_cfg, f);
        fprintf(f,
                "Port %d\n"
                "ListenAddress 127.0.0.1\n"
                "HostKey %s/hostkey\n"
                "PidFile %s/sshd.pid\n"
                "AuthorizedKeysFile %s/authorized_keys\n"
                "UsePAM no\n"
                "StrictModes no\n"
                "PasswordAuthentication no\n"
                "KbdInteractiveAuthentication no\n"
                "AllowTcpForwarding yes\n"
                "PermitTTY yes\n"
                "Subsystem sftp internal-sftp\n"
                "LogLevel ERROR\n",
                fx->port, fx->dir, fx->dir, fx->dir);
        fclose(f);
    }

    snprintf(path, sizeof path, "%s/hostkey.pub", fx->dir);
    {
        char pub[1200];
        if (!slurp(path, pub, sizeof pub))
            return -1;
        field2(pub, b64, sizeof b64);
        snprintf(fx->khline, sizeof fx->khline,
                 "[127.0.0.1]:%d ssh-ed25519 %s", fx->port, b64);
    }

    {
        char kb[16384];
        snprintf(path, sizeof path, "%s/client", fx->dir);
        if (!slurp(path, kb, sizeof kb))
            return -1;
        snprintf(fx->key, sizeof fx->key, "%s", kb);
    }

    fx->pid = fork();
    if (fx->pid == 0) {
        char cfg[PATH_MAX + 64], logp[PATH_MAX + 64];
        int errfd;
        snprintf(cfg, sizeof cfg, "%s/sshd_config", fx->dir);
        snprintf(logp, sizeof logp, "%s/sshd.err", fx->dir);
        errfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (errfd >= 0)
            dup2(errfd, STDERR_FILENO);
        execl("/usr/sbin/sshd", "/usr/sbin/sshd", "-D", "-e", "-f", cfg,
              NULL);
        _exit(127);
    }
    if (fx->pid < 0)
        return -1;

    {
        int i, ok = 0;
        for (i = 0; i < 100 && !ok; i++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)fx->port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(fd, (void *)&a, sizeof a) == 0)
                ok = 1;
            close(fd);
            if (!ok)
                usleep(50000);
        }
        if (!ok)
            return -1;
    }
    return 0;
}

void hb_sshd_stop(hb_sshd *fx)
{
    char cmd[PATH_MAX + 32];
    if (fx->pid > 0) {
        kill(fx->pid, SIGTERM);
        waitpid(fx->pid, NULL, 0);
        fx->pid = -1;
    }
    if (fx->dir[0]) {
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", fx->dir);
        if (system(cmd) != 0) {}
    }
}
