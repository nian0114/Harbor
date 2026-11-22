/* 跨平台 socket 辅助（core 内部头）。
 *
 * Linux 用 SOCK_NONBLOCK|SOCK_CLOEXEC、accept4、MSG_NOSIGNAL 一步到位；
 * macOS 没有这些：socket()/accept() 后 fcntl 两步补齐，send() 的
 * SIGPIPE 抑制改用 SO_NOSIGPIPE（在 fd 创建时设置）。
 *
 * HB_SOCK_NONBLOCK / HB_SOCK_CLOEXEC：type 参数携带的"语义 flag"，
 * Linux 下映射到原生 SOCK_*，macOS 下由 wrapper 剥离后用 fcntl 实现。 */
#ifndef HB_SOCKUTIL_H
#define HB_SOCKUTIL_H

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#define HB_SOCK_NONBLOCK 0x10000
#define HB_SOCK_CLOEXEC  0x20000
#define HB_MSG_NOSIGNAL  0
#else
#define HB_SOCK_NONBLOCK SOCK_NONBLOCK
#define HB_SOCK_CLOEXEC  SOCK_CLOEXEC
#define HB_MSG_NOSIGNAL  MSG_NOSIGNAL
#endif

static inline int hb__fd_flags(int fd, int type)
{
    if (type & HB_SOCK_NONBLOCK) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
            return -1;
    }
    if (type & HB_SOCK_CLOEXEC) {
        int fl = fcntl(fd, F_GETFD, 0);
        if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
            return -1;
    }
    return 0;
}

/* macOS 对要写数据的 socket 关掉 SIGPIPE（send 返回 EPIPE 代替）。
 * Linux 侧是 no-op（send 用 MSG_NOSIGNAL 逐次指定）。 */
static inline int hb__sock_no_sigpipe(int fd)
{
#ifdef __APPLE__
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
    return 0;
#endif
}

static inline int hb__socket(int domain, int type, int protocol)
{
    int fd;
#ifdef __APPLE__
    fd = socket(domain, type & ~(HB_SOCK_NONBLOCK | HB_SOCK_CLOEXEC),
                protocol);
    if (fd >= 0 && hb__fd_flags(fd, type) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        fd = -1;
    }
#else
    /* SOCK_NONBLOCK|SOCK_CLOEXEC 由内核原子应用 */
    fd = socket(domain, type, protocol);
#endif
    if (fd >= 0 && hb__sock_no_sigpipe(fd) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        fd = -1;
    }
    return fd;
}

static inline int hb__accept4(int fd, struct sockaddr *addr, socklen_t *len,
                              int flags)
{
#ifdef __APPLE__
    int c = accept(fd, addr, len);
    if (c >= 0 && hb__fd_flags(c, flags) < 0) {
        int e = errno;
        close(c);
        errno = e;
        return -1;
    }
#else
    int c = accept4(fd, addr, len, flags);
#endif
    if (c >= 0 && hb__sock_no_sigpipe(c) < 0) {
        int e = errno;
        close(c);
        errno = e;
        return -1;
    }
    return c;
}

static inline int hb__socketpair(int domain, int type, int protocol, int sv[2])
{
#ifdef __APPLE__
    if (socketpair(domain, type & ~(HB_SOCK_NONBLOCK | HB_SOCK_CLOEXEC),
                   protocol, sv) < 0)
        return -1;
    if (hb__fd_flags(sv[0], type) < 0 || hb__fd_flags(sv[1], type) < 0) {
        int e = errno;
        close(sv[0]);
        close(sv[1]);
        errno = e;
        return -1;
    }
#else
    if (socketpair(domain, type, protocol, sv) < 0)
        return -1;
#endif
    if (hb__sock_no_sigpipe(sv[0]) < 0 || hb__sock_no_sigpipe(sv[1]) < 0) {
        int e = errno;
        close(sv[0]);
        close(sv[1]);
        errno = e;
        return -1;
    }
    return 0;
}

#endif /* HB_SOCKUTIL_H */
