#ifndef HB_SESSION_H
#define HB_SESSION_H

#include "common.h"
#include "pty.h"
#include "vt.h"

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
} hb_sess_ev_type;

typedef struct {
    hb_sess_ev_type type;
    char   mark;
    int    code;
    char  *text, *text2;    /* owned；hb_sess_event_clear 释放 */
    int64_t ts_ms;
} hb_sess_event;

void hb_sess_event_clear(hb_sess_event *ev);

typedef struct hb_session hb_session;

/* 本地 PTY 会话；立即启动线程（spawn 失败返回 NULL）。 */
hb_session *hb_session_new_local(const hb_pty_opts *opts, int scrollback_lines);

int   hb_session_event_fd(hb_session *s);
bool  hb_session_next_event(hb_session *s, hb_sess_event *out);

hb_vt *hb_session_vt(hb_session *s);

void hb_session_write(hb_session *s, const char *data, size_t len);
void hb_session_key(hb_session *s, hb_key key, int mods);
void hb_session_unichar(hb_session *s, uint32_t c, int mods);
void hb_session_paste(hb_session *s, const char *utf8);
void hb_session_resize(hb_session *s, int rows, int cols);

bool hb_session_alive(hb_session *s);
void hb_session_close(hb_session *s);   /* 异步：请线程挂断，随后出 EXIT 事件 */
void hb_session_free(hb_session *s);    /* join 线程；close 与否都安全 */

/* 原始输出 tap：在 session 线程上、vt feed 之前调用（watch 引擎挂这里）。 */
typedef void (*hb_session_tap_fn)(hb_session *s, const char *bytes, size_t len, void *ud);
void hb_session_set_tap(hb_session *s, hb_session_tap_fn fn, void *ud);

#ifdef __cplusplus
}
#endif

#endif
