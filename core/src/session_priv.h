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

    /* 可选：session 线程上先跑的阻塞连接阶段（ssh 握手等）。
     * 期间可用 hb__session_emit 发 STAGE / CONNECTED / ERROR 事件。
     * 返回 0 成功；非 0 则线程直接收尾（emit EXIT）。可为 NULL。 */
    int      (*connect)(void *ctx, hb_session *s);
} hb_session_transport;

/* 构造通用 session：vt 尺寸 rows×cols + scrollback 行；立即起线程。 */
hb_session *hb_session_new_transport(hb_session_transport *tr,
                                     int rows, int cols, int scrollback_lines);

/* 供 transport->connect 使用：推一个事件到 UI 队列（做字符串拷贝）。 */
void hb__session_emit(hb_session *s, hb_sess_ev_type type, char mark, int code,
                      const char *text, const char *text2);

#endif
