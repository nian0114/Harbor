#ifndef HB_SSH_H
#define HB_SSH_H

#include "common.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* SSH 传输：libssh2 + OpenSSL。一个 hb_session 一个 SSH 连接，           */
/* 可选 ProxyJump 跳板链。连接在 session 线程上跑，TOFU / 密码等          */
/* 交互通过 HB_SESS_EV_HOSTKEY / HB_SESS_EV_PROMPT 事件 + 应答 API 完成。*/
/* ------------------------------------------------------------------ */

typedef enum {
    HB_SSH_AUTH_AUTO,       /* agent → identity_file → 密码（缺则 PROMPT） */
    HB_SSH_AUTH_PASSWORD,   /* userauth_password；服务器只给 kbi 时自动回落 */
    HB_SSH_AUTH_KEY,        /* privkey 内存文本或 identity_file */
    HB_SSH_AUTH_AGENT,      /* 仅 agent（需 SSH_AUTH_SOCK） */
    HB_SSH_AUTH_NONE,       /* userauth_none */
} hb_ssh_auth;

typedef struct {
    const char *host;
    int         port;
    const char *user;
    hb_ssh_auth auth;
    const char *password;        /* NULL → PROMPT */
    const char *privkey;         /* OpenSSH/PEM 私钥文本（vault 内存密钥），NULL 无 */
    const char *identity_file;   /* 磁盘私钥路径（~/.ssh/config 导入），连接时读取 */
    const char *passphrase;      /* NULL → 加密私钥时 PROMPT */
    const char *label;           /* 日志显示名，如主机别名 */
} hb_ssh_hop;

typedef enum {
    HB_SSH_MODE_SHELL = 0,  /* 交互 shell / exec（默认） */
    HB_SSH_MODE_SFTP        /* SFTP 子系统：无 PTY，detect_os 忽略 */
} hb_ssh_mode;

typedef struct {
    hb_ssh_hop         target;
    const hb_ssh_hop  *jumps;      /* 外 → 内排列的 ProxyJump 链 */
    size_t             njumps;
    const char *const *known_lines;   /* known_hosts 行（"[h]:p type b64" 或 |1| 哈希） */
    size_t             nknown;
    int                connect_timeout_s;   /* 0 → 默认 15s，TCP 阶段用 */
    int                keepalive_s;           /* 0 = 关 */
    const char        *term;                  /* NULL → xterm-256color */
    const char *const *env;                   /* 可选 "K=V"，channel setenv（忽略失败） */
    const char        *command;               /* NULL = 交互 shell；否则 exec */
    hb_vt             *reuse_vt;   /* 非 NULL → 复用 vt（重连保 scrollback） */
    bool               detect_os;  /* 认证后探测 /etc/os-release → OSINFO 事件 */
    hb_ssh_mode        mode;       /* HB_SSH_MODE_*，默认 SHELL */
} hb_ssh_opts;

/* 深拷贝 opts（密钥/密码进 sodium 锁定内存，认证后即擦除）。 */
hb_session *hb_session_new_ssh(const hb_ssh_opts *opts,
                               int rows, int cols, int scrollback_lines);

/* SFTP 模式会话 = new_ssh(mode=SFTP)：无 PTY/终端，结果经
 * HB_SESS_EV_SFTP 事件上报；API 见 harbor/sftp.h。 */
hb_session *hb_session_new_sftp(const hb_ssh_opts *opts);

/* 错误类别：作为 HB_SESS_EV_ERROR 事件的 code。 */
typedef enum {
    HB_SSH_ERR_RESOLVE = 1,     /* 解析失败 */
    HB_SSH_ERR_REFUSED,         /* TCP 拒绝 */
    HB_SSH_ERR_UNREACHABLE,     /* 主机不可达（非超时） */
    HB_SSH_ERR_TIMEOUT,         /* 连接/握手超时 */
    HB_SSH_ERR_HANDSHAKE,       /* SSH 握手失败 */
    HB_SSH_ERR_HOSTKEY_REJECTED,/* 未知主机密钥被拒绝 */
    HB_SSH_ERR_HOSTKEY_CHANGED, /* 密钥已变更且被拒绝 */
    HB_SSH_ERR_AUTH,            /* 认证失败 */
    HB_SSH_ERR_CHANNEL,         /* channel / shell 建立失败 */
    HB_SSH_ERR_DISCONNECTED,    /* 已连接后断开 */
    HB_SSH_ERR_CANCELLED,       /* 用户取消 / hb_session_close */
} hb_ssh_err;

/* vault 日志用的结果名：unreachable|auth_failed|hostkey_changed|
 * hostkey_rejected|timeout|error|cancelled */
const char *hb_ssh_err_name(int code);

/* "SHA256:..."（base64 无 padding）。free() 释放。 */
char *hb_ssh_fingerprint_sha256(const unsigned char *key, size_t len);

/* OpenSSH drunken-bishop 17×9 randomart，与 `ssh-keygen -lv` 字节一致。
 * keytype_label 形如 "ED25519 256"；sha256 = 32 字节摘要。free() 释放。 */
char *hb_ssh_randomart(const char *keytype_label,
                       const unsigned char *sha256, size_t len);

/* 解析快速连接串：user@host[:port] / host[:port] / [v6][:port] / 裸 v6。
 * 成功返回 HB_OK；*user 无 user@ 时为 NULL；*port 未指定时为 0。free() 释放。 */
hb_err hb_ssh_parse_target(const char *s, char **user, char **host, int *port);

#ifdef __cplusplus
}
#endif

#endif /* HB_SSH_H */
