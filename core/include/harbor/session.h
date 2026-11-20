#ifndef HB_SESSION_H
#define HB_SESSION_H

#include "common.h"
#include "pty.h"
#include "vt.h"
#include "watch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* hb_session：带线程的终端会话（传输 + vt + UI 事件队列）。              */
/*                                                                     */
/* UI 侧消费模型：hb_session_event_fd() 拿到可读 fd 接入主循环            */
/* （Linux 用 g_unix_fd_add），可读时循环 hb_session_next_event 取干。     */
/* 屏幕状态在 hb_session_vt() 上，UI 读时加 hb_vt_lock。                  */
/* ------------------------------------------------------------------ */

typedef enum {
    HB_SESS_EV_OUTPUT,      /* 屏幕有变化（最多一条合并事件） */
    HB_SESS_EV_TITLE,
    HB_SESS_EV_BELL,
    HB_SESS_EV_OSC133,
    HB_SESS_EV_NOTIFY,
    HB_SESS_EV_CLIPBOARD,
    HB_SESS_EV_CWD,
    HB_SESS_EV_ALTSCREEN,
    HB_SESS_EV_STAGE,       /* code=阶段序号, text=阶段描述行 */
    HB_SESS_EV_CONNECTED,
    HB_SESS_EV_EXIT,        /* code=退出码, text=原因 */
    HB_SESS_EV_ERROR,       /* code=错误类别, text=人类可读信息 */
    /* 主机密钥确认（TOFU/MITM）：
     *   code  0=未知主机，1=密钥已变更
     *   text  "host:port"
     *   text2 "ssh-ed25519 SHA256:xxxx"（算法名 + 指纹）
     *   extra 第 1 行 = 可直接存入 known_hosts 的完整行；
     *         其余行 = OpenSSH `ssh-keygen -lv` 同款 randomart 框。
     * 连接线程挂起等 hb_session_hostkey_reply。 */
    HB_SESS_EV_HOSTKEY,
    /* 交互式输入请求：
     *   code  0=密码，1=私钥口令（passphrase）
     *   text  提示语
     *   mark  'J' 跳板跳 / 'T' 目标；text2 = hop 下标（"0"…njumps 为目标）
     * 连接线程挂起等 hb_session_prompt_reply（NULL = 取消）。 */
    HB_SESS_EV_PROMPT,
    /* 目标主机 OS 探测（hb_ssh_opts.detect_os；非致命）：
     *   text  /etc/os-release 的 ID（"ubuntu"/"debian"/…）
     *   text2 PRETTY_NAME */
    HB_SESS_EV_OSINFO,
    /* 完成监控命中（FR-13）：watch 指向深拷贝的 hb_watch_hit（owned）。 */
    HB_SESS_EV_WATCH,
    /* Shell History 命令采集（FR-08）：text = 命令行。
     * OSC 133 时取自 B..C 回显文本；否则用输入启发式（密码提示不采）。 */
    HB_SESS_EV_COMMAND,
    /* SFTP 请求结果（FR-05）：sftp 指向 hb_sftp_result（owned）。 */
    HB_SESS_EV_SFTP,
} hb_sess_ev_type;

struct hb_sftp_result;

typedef struct {
    hb_sess_ev_type type;
    char   mark;            /* 0 / 'J' 跳板跳 / 'T' 目标 */
    int    code;
    char  *text, *text2;    /* owned；hb_sess_event_clear 释放 */
    char  *extra;           /* owned；HOSTKEY 的 known_hosts 行 + randomart */
    hb_watch_hit *watch;    /* owned；仅 HB_SESS_EV_WATCH */
    struct hb_sftp_result *sftp;    /* owned；仅 HB_SESS_EV_SFTP */
    int64_t ts_ms;
} hb_sess_event;

void hb_sess_event_clear(hb_sess_event *ev);

typedef struct hb_session hb_session;

/* 本地 PTY 会话；立即启动线程（spawn 失败返回 NULL）。 */
hb_session *hb_session_new_local(const hb_pty_opts *opts, int scrollback_lines);
/* 同上但复用已有 vt（重连保 scrollback）；新会话接管所有权并 resize 到
 * opts 的 rows/cols。reuse 为 NULL 等价于 hb_session_new_local。
 * 失败时 reuse 仍归调用方。 */
hb_session *hb_session_new_local_vt(const hb_pty_opts *opts, hb_vt *reuse,
                                    int scrollback_lines);

int   hb_session_event_fd(hb_session *s);
bool  hb_session_next_event(hb_session *s, hb_sess_event *out);

hb_vt *hb_session_vt(hb_session *s);

/* EXIT 事件之后调用：取走 vt 所有权（scrollback 原样保留），交给
 * hb_session_new_local_vt / hb_ssh_opts.reuse_vt 的新会话。
 * 偷取后本 session 的 hb_session_vt() 返回 NULL；hb_session_free 照常。 */
hb_vt *hb_session_steal_vt(hb_session *s);

void hb_session_write(hb_session *s, const char *data, size_t len);
void hb_session_key(hb_session *s, hb_key key, int mods);
void hb_session_unichar(hb_session *s, uint32_t c, int mods);
void hb_session_paste(hb_session *s, const char *utf8);
void hb_session_resize(hb_session *s, int rows, int cols);

bool hb_session_alive(hb_session *s);
/* HOSTKEY 事件应答：accept 是否信任并继续（未知=TOFU 记入，变更=强制继续）。 */
void hb_session_hostkey_reply(hb_session *s, bool accept);
/* PROMPT 事件应答：secret 会复制进锁定内存并在用完即擦；NULL = 取消连接。 */
void hb_session_prompt_reply(hb_session *s, const char *secret);
void hb_session_close(hb_session *s);   /* 异步：请线程挂断，随后出 EXIT 事件 */
void hb_session_free(hb_session *s);    /* join 线程；close 与否都安全 */

/* 原始输出 tap：在 session 线程上、vt feed 之前调用（watch 引擎挂这里）。 */
typedef void (*hb_session_tap_fn)(hb_session *s, const char *bytes, size_t len, void *ud);
void hb_session_set_tap(hb_session *s, hb_session_tap_fn fn, void *ud);

/* NULL cfg = 关闭完成监控；非 NULL 深拷贝并立即在 session 线程生效。
 * 运行中调用安全：引擎替换原子发生在 session 互斥锁内。
 * 命中经 HB_SESS_EV_WATCH 上报（EOF hit 保证在 EXIT 之前）。 */
void hb_session_set_watch(hb_session *s, const hb_watch_cfg *cfg);

/* Shell History 采集开关（默认关）。开启后每执行一条命令发
 * HB_SESS_EV_COMMAND；密码/敏感输入不入事件（启发式 + OSC 133 都过滤）。 */
void hb_session_set_history(hb_session *s, bool on);

#ifdef __cplusplus
}
#endif

#endif
