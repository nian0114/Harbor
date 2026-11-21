/*
 * rd.h — FR-07 VNC/RDP 远程桌面客户端（GTK 无关）
 *
 * 模型：每条连接一个 hb_rd 实例，内部一个 worker 线程独占驱动
 * libvncclient / FreeRDP（两者都不是线程安全的，UI 线程绝不能直接调它们）。
 * UI 侧通过 hb_rd_event_fd() 等待、hb_rd_next_event() 取事件；
 * 输入通过 hb_rd_pointer/hb_rd_key/hb_rd_clipboard 排队后由 worker 发送。
 *
 * 事件所有权：hb_rd_next_event 填充的 hb_rd_event，其 text/text2/px
 * 字段为 malloc 内存，由调用方在 hb_rd_event_clear() 中释放。
 * 队列中 FRAME 至多一条（合并损坏区域）；其余事件按序排队。
 *
 * 证书指纹格式（opts.cert_fp / CERT 事件 text）：
 *   "SHA256:" + 服务端证书 DER 编码的 SHA-256 摘要的 base64（标准字母表，无填充，
 *   同 OpenSSH 密钥指纹风格，例如 "SHA256:abCd...9Z"）。
 * Harbor 是证书信任的唯一来源：FreeRDP 的 known_hosts 按 host:port 记录，
 * 而我们总是经 SSH 隧道连 127.0.0.1:<随机端口>，会互相串；因此 rd 模块总是
 * 自己比对指纹，接受也仅限本次会话（绝不写 FreeRDP 的证书库）。
 */

#ifndef HARBOR_RD_H
#define HARBOR_RD_H

#include <stdbool.h>
#include <stdint.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HB_RD_VNC, HB_RD_RDP } hb_rd_proto;
typedef enum { HB_RD_Q_FAST, HB_RD_Q_BALANCED, HB_RD_Q_HIGH } hb_rd_quality; /* 流畅/均衡/高清 */

typedef enum {
    HB_RD_ERR_REFUSED = 1,   /* 连接被拒 */
    HB_RD_ERR_TIMEOUT,       /* 连接/握手超时 */
    HB_RD_ERR_AUTH,          /* 认证失败（密码错误 / NLA 凭据被拒） */
    HB_RD_ERR_CERT_REJECTED, /* 用户拒绝证书 */
    HB_RD_ERR_TLS,           /* TLS/加密协商失败 */
    HB_RD_ERR_PROTOCOL,      /* 协议错误 */
    HB_RD_ERR_DISCONNECTED,  /* 连接中断 */
    HB_RD_ERR_CANCELLED,     /* 用户取消（PROMPT 回复 NULL / 关闭） */
} hb_rd_err;

typedef enum {
    HB_RD_EV_STAGE,      /* text = 阶段描述（连接中/握手/认证…） */
    HB_RD_EV_CONNECTED,  /* w,h = 桌面尺寸；text = 桌面名；text2 = "NLA · TLS" / "VncAuth · ZRLE" */
    HB_RD_EV_RESIZE,     /* w,h = 新尺寸 */
    HB_RD_EV_FRAME,      /* 帧缓冲有更新；x,y,w,h = 自上次 FRAME 以来的损坏并集；队列中至多一条 */
    HB_RD_EV_CURSOR,     /* 光标形状变化；px = BGRA 图（w*h*4），x,y = hotspot */
    HB_RD_EV_CLIPBOARD,  /* text = 远端剪贴板内容（UTF-8） */
    HB_RD_EV_CERT,       /* code 0 未知 / 1 已变更；text = "SHA256:…" 指纹；text2 = subject/issuer/host 摘要；
                            worker 阻塞等待 hb_rd_cert_reply */
    HB_RD_EV_PROMPT,     /* code 0 = 只要密码（VNC）/ 1 = user+password+domain（RDP）；text = 提示；
                            worker 阻塞等待 hb_rd_prompt_reply */
    HB_RD_EV_ERROR,      /* code = hb_rd_err；text = 中文可读信息；text2 = 技术细节 */
    HB_RD_EV_EXIT,       /* worker 线程已退出（hb_rd_free 前的最后事件） */
} hb_rd_event_type;

typedef struct {
    hb_rd_event_type type;
    int code;
    int x, y, w, h;
    int hx, hy;        /* CURSOR hotspot */
    uint8_t *px;       /* CURSOR: malloc'd BGRA，w*h*4；否则 NULL */
    char *text, *text2; /* malloc'd，可 NULL */
} hb_rd_event;

typedef struct {
    hb_rd_proto proto;
    const char *host;
    int port;
    const char *user, *password, *domain; /* password NULL → 需要时 PROMPT 事件 */
    hb_rd_quality quality;
    bool view_only;                        /* 丢弃所有输入 */
    int width, height;                     /* RDP 初始桌面尺寸；VNC 忽略 */
    const char *cert_fp;                   /* RDP："SHA256:…" 受信指纹；NULL = 未知 → CERT */
    const char *tls_hostname;              /* RDP：证书 UI 中显示的真实主机名（隧道下是 127.0.0.1） */
    const char *freerdp_config_dir;        /* RDP：FreeRDP_ConfigPath（私有 0700 目录）；NULL = 默认 */
    int connect_timeout_s;                 /* 0 → 15 */
} hb_rd_opts;

typedef struct hb_rd hb_rd;

/* 深拷贝选项（口令进 sodium 锁定内存，认证完成后立即擦除），启动 worker 线程。
 * 失败返回 NULL（此时没有事件可读）。 */
hb_rd *hb_rd_new(const hb_rd_opts *o);
int hb_rd_event_fd(hb_rd *rd);
bool hb_rd_next_event(hb_rd *rd, hb_rd_event *ev);
void hb_rd_event_clear(hb_rd_event *ev);

/* 答复 CERT：accept=true 仅本次会话接受；false → ERROR CERT_REJECTED + 断开。 */
void hb_rd_cert_reply(hb_rd *rd, bool accept);
/* 答复 PROMPT：password NULL = 取消 → ERROR CANCELLED（RDP 会复制 user/domain；VNC 只用 password）。 */
void hb_rd_prompt_reply(hb_rd *rd, const char *user, const char *password, const char *domain);

/* 帧缓冲：32 位 BGRX（内存字节序 B,G,R,X，即 LE uint32 0xXXRRGGBB），stride 字节。
 * 仅在拷贝期间持有锁：返回后尽快 unlock；锁内 worker 的像素拷贝会阻塞（几十 µs 级）。 */
bool hb_rd_fb_lock(hb_rd *rd, const uint8_t **px, int *w, int *h, int *stride);
void hb_rd_fb_unlock(hb_rd *rd);

/* 输入：线程安全，排队给 rd worker 发送；view_only 时被丢弃。
 * buttons: bit0 左 bit1 中 bit2 右 bit3/4 滚轮上下 bit5/6 滚轮左右 */
void hb_rd_pointer(hb_rd *rd, int x, int y, unsigned buttons);
/* VNC 发 keysym；RDP 用 freerdp 键盘表把 x11_keycode 映射为 scancode（含扩展键）。 */
void hb_rd_key(hb_rd *rd, uint32_t keysym, uint32_t x11_keycode, bool down);
void hb_rd_clipboard(hb_rd *rd, const char *utf8);
/* RDP：Display Control 通道调整远端分辨率（服务端不支持则无操作）；VNC 无操作。 */
void hb_rd_request_size(hb_rd *rd, int w, int h);
/* VNC 即时重新协商编码/画质；RDP 存下、下次连接生效。 */
void hb_rd_set_quality(hb_rd *rd, hb_rd_quality q);

/* 异步关闭：置位后 worker 尽快退出并发 EXIT；hb_rd_free 有界等待线程汇合。 */
void hb_rd_close(hb_rd *rd);
void hb_rd_free(hb_rd *rd);

#ifdef __cplusplus
}
#endif

#endif
