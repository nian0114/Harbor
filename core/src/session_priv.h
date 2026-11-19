#ifndef HB_SESSION_PRIV_H
#define HB_SESSION_PRIV_H

/* 传输层 vtable（内部）：pty 现在、ssh 下一单元接入。 */

#include "harbor/session.h"

typedef struct {
    void *ctx;

    int      (*fd)(void *ctx);                                   /* poll 用 fd */
    ssize_t  (*read)(void *ctx, char *buf, size_t len);          /* EAGAIN 表示暂无 */
    ssize_t  (*write)(void *ctx, const char *buf, size_t len);   /* 可短写/EAGAIN */
    int      (*resize)(void *ctx, int rows, int cols);           /* 0 ok */
    bool     (*poll_exit)(void *ctx, int *code);                 /* true=已结束 */
    void     (*close)(void *ctx);                                /* 挂断（SIGHUP 等价物） */
    void     (*destroy)(void *ctx);                              /* 释放 transport 自身 */

    /* 可选：返回距下次 tick 的毫秒数作为 poll 超时（-1 = 不需要 tick）。
     * 到点（或被 I/O 唤醒）后会再次被调 —— 内部自己判断是否到期再干活，
     * ssh keepalive 挂这里。 */
    int      (*tick)(void *ctx);

    /* 可选：EXIT 事件的 text；NULL → "process exited"。 */
    const char *(*exit_text)(void *ctx);

    /* 可选：session 线程上先跑的阻塞连接阶段（ssh 握手等）。
     * 期间可用 hb__session_emit(_ex) 发 STAGE / CONNECTED / HOSTKEY /
     * PROMPT / ERROR 事件；失败时 transport 自己 emit 过 ERROR，
     * 返回非 0 线程只补一条 EXIT(-1)。可为 NULL。 */
    int      (*connect)(void *ctx, hb_session *s);
} hb_session_transport;

/* 构造通用 session：vt 尺寸 rows×cols + scrollback 行；立即起线程。
 * reuse_vt 非 NULL 时接管其所有权（尺寸改 rows×cols，scrollback 上限不变）。 */
hb_session *hb_session_new_transport(hb_session_transport *tr,
                                     int rows, int cols, int scrollback_lines,
                                     hb_vt *reuse_vt);

/* 供 transport 使用：推一个事件到 UI 队列（做字符串拷贝）。 */
void hb__session_emit(hb_session *s, hb_sess_ev_type type, char mark, int code,
                      const char *text, const char *text2);
/* 同上，多一个 ev.extra 字段（HOSTKEY 事件用）。 */
void hb__session_emit_ex(hb_session *s, hb_sess_ev_type type, char mark,
                         int code, const char *text, const char *text2,
                         const char *extra);

/* connect 阶段用的取消检查：hb_session_close 被调则返回 true。 */
bool hb__session_cancelled(hb_session *s);

/* 应答管道（HOSTKEY / PROMPT）：
 * hb__session_reply_reset 在 emit 询问事件之前调用，设置等待类型；
 * hb__session_reply_wait 阻塞到回复或关闭。
 *   kind: HB__REPLY_HOSTKEY → *accept_out 1=接受 0=拒绝
 *         HB__REPLY_PROMPT  → *sec_out = 回复的密码（sodium_malloc，
 *                             调用方 hb_secret_free；NULL = 用户取消）
 * 返回 1 有回复，0 取消/关闭。 */
#define HB__REPLY_HOSTKEY 1
#define HB__REPLY_PROMPT  2
void hb__session_reply_reset(hb_session *s, int kind);
int  hb__session_reply_wait(hb_session *s, int *accept_out, char **sec_out);

#endif
