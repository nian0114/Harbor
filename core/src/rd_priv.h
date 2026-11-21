#ifndef HB_RD_PRIV_H
#define HB_RD_PRIV_H

/* rd 模块内部共享：事件队列、帧缓冲、输入队列、答复通道、worker 线程。 */

#include "harbor/rd.h"

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    RD_IN_POINTER,   /* x,y,buttons */
    RD_IN_KEY,       /* keysym,x11_keycode,down */
    RD_IN_CLIPBOARD, /* text */
    RD_IN_SIZE,      /* w,h */
    RD_IN_QUALITY,   /* x = hb_rd_quality */
} rd_in_type;

typedef struct {
    rd_in_type t;
    int x, y;
    unsigned buttons;
    uint32_t keysym, x11kc;
    bool down;
    int w, h;
    char *text; /* CLIPBOARD */
} rd_in;

typedef struct rd_ev_node {
    hb_rd_event ev;
    struct rd_ev_node *next;
} rd_ev_node;

struct hb_rd {
    hb_rd_opts o;      /* 深拷贝；password/user/domain 用 strdup，password 用 sodium */
    char *password;    /* sodium_malloc'd，认证完成后 hb__rd_wipe_password 清零 */

    /* 事件队列：UI 侧读；FRAME 合并至队列至多一条 */
    pthread_mutex_t ev_mu;
    rd_ev_node *ev_head, *ev_tail;
    rd_ev_node *ev_frame; /* 当前合并中的 FRAME 节点 */
    int ev_r, ev_w;       /* 唤醒管道（ev_r = hb_rd_event_fd） */

    /* 帧缓冲：BGRX32，worker 写、UI 在锁内读 */
    pthread_mutex_t fb_mu;
    uint8_t *fb;
    int fb_w, fb_h, fb_stride;
    bool fb_ready;

    /* 输入队列：UI 写、worker 读；cmd 管道唤醒 worker */
    pthread_mutex_t in_mu;
    rd_in *in_q;
    size_t in_len, in_cap;
    unsigned in_last_ptr_buttons; /* 最近入队的 POINTER buttons（cap 判"纯移动"用） */
    int cmd_r, cmd_w;

    /* CERT / PROMPT 答复：worker 阻塞等待。arm → push event → wait；
     * reply 可在 wait 进入前到达（rp_done 保证不丢）。 */
    pthread_mutex_t rp_mu;
    pthread_cond_t rp_cv;
    int cert_accept; /* -1 pending */
    bool rp_done;
    char *rp_user, *rp_domain;
    char *rp_password; /* sodium */
    bool rp_cancel;

    /* worker */
    pthread_t th;
    bool th_started;
    volatile bool closing; /* hb_rd_close 置位 */
    bool exited;           /* worker 结束时（在 ev_mu 下） */

    /* 后端私有 */
    void *be;
};

/* rd.c 通用工具（worker 线程内调用） */
void hb__rd_stage(hb_rd *rd, const char *text);
void hb__rd_push(hb_rd *rd, const hb_rd_event *ev);   /* 拷贝 ev（指针字段不接管） */
void hb__rd_push_owned(hb_rd *rd, hb_rd_event *ev);   /* 接管 ev 内 malloc 字段 */
void hb__rd_error(hb_rd *rd, hb_rd_err code, const char *cn, const char *detail);
void hb__rd_connected(hb_rd *rd, int w, int h, const char *name, const char *sec);
void hb__rd_resize(hb_rd *rd, int w, int h);
void hb__rd_frame(hb_rd *rd, int x, int y, int w, int h); /* 合并损坏 + 至多一条 */
void hb__rd_clipboard(hb_rd *rd, const char *utf8);
void hb__rd_cursor(hb_rd *rd, int w, int h, int hx, int hy, const uint8_t *bgra);
void hb__rd_exit(hb_rd *rd);

/* 帧缓冲：必要时重建（尺寸变化），把损坏区像素拷进来并发 FRAME。 */
bool hb__rd_fb_update(hb_rd *rd, const uint8_t *src, int src_stride,
                      int w, int h, int x, int y, int dw, int dh);

/* 输入：UI 侧 enqueue（rd.c 导出给 API）；worker 侧 drain。 */
size_t hb__rd_in_drain(hb_rd *rd, rd_in *out, size_t cap); /* 返回条数；text 归调用方 */
size_t hb__rd_in_pending(hb_rd *rd);                        /* 剩余未取条数 */
int hb__rd_in_wait_fd(hb_rd *rd);                         /* cmd_r，poll 用 */
void hb__rd_in_drain_fd(hb_rd *rd);                       /* 读空 cmd_r */
void hb__rd_wake(hb_rd *rd);                              /* 往 cmd_w 写一字节 */

/* CERT/PROMPT 阻塞等待（worker 内）：先 arm 再 push 事件再 wait，保证早到的
 * reply 不丢失。cert: 返回 accept 布尔。prompt: 填充输出字段（调用方 free）。 */
void hb__rd_arm_cert(hb_rd *rd);
bool hb__rd_wait_cert(hb_rd *rd); /* true=accept */
void hb__rd_arm_prompt(hb_rd *rd);
bool hb__rd_wait_prompt(hb_rd *rd, char **user, char **password, char **domain);
/* password 返回为 sodium_malloc，调用方 sodium_free 或转交 */

bool hb__rd_closing(hb_rd *rd);
void hb__rd_wipe_password(hb_rd *rd); /* 认证完成后擦除 rd->password */

/* 后端入口（各自文件内的 worker）与 abort（打断阻塞 I/O，hb_rd_close 调用） */
void hb__rd_vnc_run(hb_rd *rd);
void hb__rd_rdp_run(hb_rd *rd);
void hb__rd_vnc_abort(hb_rd *rd);
void hb__rd_rdp_abort(hb_rd *rd);
void hb__rd_be_close(hb_rd *rd);
void hb__rd_vnc_destroy(hb_rd *rd); /* join 之后释放 be */
void hb__rd_rdp_destroy(hb_rd *rd);

/* opts 深拷贝/释放 */
bool hb__rd_opts_copy(hb_rd *rd, const hb_rd_opts *o);
void hb__rd_opts_free(hb_rd *rd);

#endif
