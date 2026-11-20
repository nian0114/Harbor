#ifndef HB_SSH_PRIV_H
#define HB_SSH_PRIV_H

/* ssh.c 内部结构对 sftp.c（SFTP 引擎）开放的最小面。
 * 定义只搬不动：c_hop / pump_t / hop / hb_ssh 与等待码保持 ssh.c 语义。 */

#include "harbor/ssh.h"
#include "session_priv.h"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>

#define PUMP_BUF      (32 * 1024)

/* 等待码（远离 -hb_ssh_err 与"换源"的取值域）：取消/超时/socket 故障 */
#define W_CANCEL -100
#define W_TMOUT  -101
#define W_SOCK   -102
#define IS_WAIT(w) ((w) <= W_CANCEL)

typedef struct {
    char       *host;
    int         port;
    char       *user;
    hb_ssh_auth auth;
    char       *password;      /* sodium */
    char       *privkey;       /* sodium */
    char       *identity_file;
    char       *passphrase;    /* sodium */
    char       *label;
} c_hop;

/* ProxyJump 泵：上一跳 session 上的 direct-tcpip channel ↔ socketpair */
typedef struct {
    LIBSSH2_SESSION *sess;
    LIBSSH2_CHANNEL *ch;
    int              hop_fd;   /* 上一跳 session 的底层 socket */
    int              pair;     /* 泵持有的 socketpair 端 */
    pthread_t        th;
    bool             started;
    volatile bool    stop;
    bool             chan_eof;
    bool             pair_eof;
    bool             sent_eof;
    uint8_t          cbuf[PUMP_BUF];   /* pair → channel 暂存 */
    size_t           clen;
    uint8_t          pbuf[PUMP_BUF];   /* channel → pair 暂存 */
    size_t           plen;
} pump_t;

typedef struct {
    c_hop            cfg;
    char             mark;     /* 'J' / 'T' */
    char             pfx[160]; /* "" 或 "[跳板 i/N label] " */
    LIBSSH2_SESSION *sess;
    int              sock;     /* 真 socket（首跳）或 socketpair 端 */
    LIBSSH2_CHANNEL *chan;     /* 目标：shell/exec；跳板：fwd channel */
    pump_t           pump;
    bool             pump_on;
} hop;

typedef struct {
    hb_session *s;
    hop        *hops;
    size_t      nhops;
    char      **known;
    size_t      nknown;
    int         timeout_ms;
    int         keepalive_s;
    char       *term;
    char      **env;
    char       *command;
    int         rows, cols;
    int64_t     ka_next;       /* 下次 keepalive 到期时刻 ms；0 = 关 */
    int64_t     ka_last_in;    /* 最后一次 fd 可读（任意入站包 = 对端活着） */
    int64_t     ka_last_out;   /* 最后一次发出 keepalive 的时刻 ms */
    int         ka_miss;       /* 连续无入站应答的 keepalive 次数 */
    bool        detect_os;
    bool        disconnected;
    bool        disconnect_emitted;
    char        errbuf[256];   /* 技术细节 → ERROR.text2 */
    hb_ssh_mode mode;          /* SHELL / SFTP */
    void       *sftp_eng;      /* SFTP 引擎（sftp.c 所有；SHELL 为 NULL） */
} hb_ssh;

/* ---- sftp.c 需要的 ssh.c 导出（均仅 session 线程上下文调用） ---- */
hop *hb__ssh_target(hb_ssh *h);
int  hb__ssh_wait_dir(hb_ssh *h, hop *hp, int64_t deadline_ms);
void hb__ssh_set_err(hb_ssh *h, const char *fmt, ...);
void hb__ssh_set_lib_err(hb_ssh *h, LIBSSH2_SESSION *sess);
void hb__ssh_emit_error(hb_ssh *h, int code, const char *text);
int  hb__ssh_ka_tick(hb_ssh *h);   /* keepalive/半开检测节拍，同 ssh_tick_op */

/* ---- ssh.c 需要的 sftp.c 导出 ---- */
void *hb__sftp_eng_new(void);
void  hb__sftp_eng_free(void *eng);
int   hb__sftp_eng_bind(void *eng, hb_session *s, LIBSSH2_SFTP *sf);
int   hb__sftp_run(void *ctx, hb_session *s);   /* tr.run 钩子 */

#endif /* HB_SSH_PRIV_H */
