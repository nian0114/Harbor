#ifndef HB_PF_H
#define HB_PF_H

#include "common.h"
#include "session.h"
#include "ssh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 端口转发（FR-06）：Local / Remote / Dynamic(SOCKS5)。                 */
/*                                                                     */
/* 一条规则一个 hb_session（HB_SSH_MODE_FORWARD）：走完整 SSH 连接       */
/* 管线（ProxyJump / TOFU / 认证 / keepalive / 取消），连上后转发         */
/* 引擎接管 session 线程。状态经 HB_SESS_EV_FORWARD 事件上报。           */
/* ------------------------------------------------------------------ */

typedef enum {
    HB_PF_LOCAL,    /* -L：本地监听 → 经 SSH 服务器连 dest */
    HB_PF_REMOTE,   /* -R：服务器监听 → 本地连 dest */
    HB_PF_DYNAMIC,  /* -D：本地监听 → SOCKS5，目标由客户端指定 */
} hb_pf_kind;

typedef struct {
    hb_pf_kind  kind;
    const char *bind_addr;  /* NULL/"" → "127.0.0.1"；数字 IPv4/IPv6 或 localhost */
    int         bind_port;  /* local/dynamic: 0 = 随机空闲端口；remote: 0 = 服务器挑选 */
    const char *dest_addr;  /* local: 由 SSH 服务器解析；remote: 本地解析；dynamic: 忽略 */
    int         dest_port;
} hb_pf_rule;

/* 建立转发会话。local/dynamic 的监听器在调用线程同步绑定（冲突立即
 * 失败）：返回 NULL 且 *err = malloc 的中文消息（如
 * "127.0.0.1:13306 已被占用（Address already in use）"）。
 * *bound_port = 实际本地端口（local/dynamic），remote 恒为 0
 *（服务端端口由随后的 LISTENING 事件上报）。两个出参都可为 NULL。
 * 失败时不留任何已绑定资源。 */
hb_session *hb_session_new_forward(const hb_ssh_opts *opts,
                                   const hb_pf_rule *rule,
                                   int *bound_port, char **err);

/* 预检绑定冲突（FR-06 保存前校验）：用与真监听器完全相同的 socket
 * 选项 bind() 后立即关闭。HB_OK = 可绑；否则错误码 + *err 中文消息。
 * addr 仅接受数字 IPv4/IPv6 或 "localhost"（→127.0.0.1）；NULL/"" = 默认。 */
hb_err hb_pf_probe_bind(const char *addr, int port, char **err);

/* HB_SESS_EV_FORWARD 事件的负载（hb_sess_event.pf，owned）。 */
typedef enum {
    HB_PF_EV_LISTENING,  /* 监听已就绪；bound_port = 实际端口（remote = 服务端端口） */
    HB_PF_EV_STATS,      /* 累计字节与连接计数 */
    HB_PF_EV_WARN,       /* 非致命异常；message = 中文说明 */
} hb_pf_evkind;

typedef struct hb_pf_status {
    hb_pf_evkind kind;
    int      bound_port;
    uint64_t bytes_up, bytes_down;  /* 累计；up = 发往隧道对侧 */
    uint32_t active, total;         /* 当前活跃 / 累计接受的连接数 */
    char    *message;               /* WARN 文本（owned）；其他类型为 NULL */
} hb_pf_status;

void hb_pf_status_free(hb_pf_status *st);

#ifdef __cplusplus
}
#endif

#endif /* HB_PF_H */
