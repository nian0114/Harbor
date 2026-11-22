#ifndef HB_SHARE_H
#define HB_SHARE_H

#include "common.h"
#include "session.h"
#include "vt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* FR-12 共享 Console 核心引擎（docs/api/share-protocol.md）。             */
/*                                                                     */
/* 传输无关：引擎产出/消费 Relay 帧（WS binary）与 JSON 文本控制消息，      */
/* HTTP/WebSocket 由平台层做。所有密钥驻留 sodium 内存并在用完即擦。        */
/*                                                                     */
/* 线程模型：owner 的 tap / run_on_thread 回调跑在 session 线程；          */
/* 平台在主线程取走 outbound 帧与事件（wake fd 可读 → drain）。            */
/* ------------------------------------------------------------------ */

/* 帧类型（§4.2） */
enum {
    HB_FR_SNAPSHOT = 0x01,
    HB_FR_DELTA    = 0x02,
    HB_FR_RESIZE   = 0x03,
    HB_FR_PRESENCE = 0x04,
    HB_FR_NOTICE   = 0x05,
    HB_FR_INPUT    = 0x10,
    HB_FR_SNAP_REQ = 0x11,
    HB_FR_JOIN     = 0x12,
    HB_FR_GRANT    = 0x13,
    HB_FR_PAKE1    = 0x20,
    HB_FR_PAKE2    = 0x21,
    HB_FR_PAKE3    = 0x22,
    HB_FR_KEY      = 0x23,
};

enum {
    HB_SHARE_VIEW   = 0,
    HB_SHARE_COLLAB = 1,
};

/* 一条待发往 Relay 的消息。is_text=true → WS 文本（JSON 控制帧，Relay
 * 自身协议如 kick/pake_fail）；false → §4.1 binary 帧，to 为目标 id
 * （0=广播给全部参与者，1=所有者）。data 归调用方，hb_share_frame_clear 释放。 */
typedef struct {
    bool     is_text;
    uint32_t to;
    uint8_t *data;
    size_t   len;
} hb_share_frame;

void hb_share_frame_clear(hb_share_frame *f);

/* 引擎 → UI 的事件 */
typedef enum {
    HB_SH_EV_JOINED,          /* id + name/device（owner 与 peer 都报） */
    HB_SH_EV_LEFT,            /* id */
    HB_SH_EV_PRESENCE,        /* text = presence JSON */
    HB_SH_EV_NOTICE,          /* text = 服务器/所有者提示（如只读） */
    HB_SH_EV_PERM,            /* code = HB_SHARE_VIEW/COLLAB（GRANT 收到） */
    HB_SH_EV_SNAPSHOT,        /* peer：新快照已应用 */
    HB_SH_EV_RESIZE,          /* code = rows<<16 | cols */
    HB_SH_EV_ENDED,           /* text = reason：stopped|owner_gone|expired|kicked|... */
    HB_SH_EV_INPUT_REJECTED,  /* owner：id 的 INPUT 被拒绝（已回 NOTICE） */
    HB_SH_EV_PAKE_OK,         /* id：guest PAKE 成功 */
    HB_SH_EV_PAKE_FAIL,       /* id：guest PAKE 失败（peer 端表示本地失败） */
    HB_SH_EV_LOCKED,          /* peer：guest 加入被锁定 */
    HB_SH_EV_ERROR,           /* text = 描述 */
} hb_share_ev_type;

typedef struct {
    hb_share_ev_type type;
    uint32_t id;             /* 相关参与者 id（无则 0） */
    int      code;
    char    *name, *device, *text;   /* owned；hb_share_event_clear */
} hb_share_event;

void hb_share_event_clear(hb_share_event *e);

/* ============================ 所有者 ============================ */

typedef struct hb_share_owner hb_share_owner;

/* share_id：客户端生成的 uuid v4 字符串；vk：账号 VK（meta/k_in 封装用）。 */
hb_share_owner *hb_share_owner_new(const char *share_id, const uint8_t vk[32],
                                   int mode /* HB_SHARE_* */);

/* POST /shares 用的 meta 字段：seal(VK, json, "hb:share:meta:v1"||0||id) 的
 * base64url。free() 释放。started 自动取当前 ms。 */
char *hb_share_owner_meta(hb_share_owner *o, const char *title,
                          const char *host, const char *owner_device);

/* §3 链接：harbor://share/<id>?s=<b64url(server_url)>#<b64url(link_secret)>。
 * fragment 不出客户端。free() 释放。 */
char *hb_share_owner_link(hb_share_owner *o, const char *server_url);

/* 6 位配对码（内部指针，引擎存活期间有效） */
const char *hb_share_owner_code(hb_share_owner *o);
const char *hb_share_owner_id(hb_share_owner *o);

/* 挂到会话：安装输出 tap（DELTA 流）。重复调用会替换。detach 传 NULL。 */
void hb_share_owner_attach(hb_share_owner *o, hb_session *sess);

/* Relay 消息入口：WS 文本（is_text=true，Relay JSON）或 binary 帧。 */
void hb_share_owner_incoming(hb_share_owner *o, const uint8_t *data,
                             size_t len, bool is_text);

/* 平台 drain：fd 可读 → next_frame/next_event 直到 false。 */
int  hb_share_owner_fd(hb_share_owner *o);
bool hb_share_owner_next_frame(hb_share_owner *o, hb_share_frame *out);
bool hb_share_owner_next_event(hb_share_owner *o, hb_share_event *ev);

/* 定时器：下次需要 tick 的 hb_now_ms 时刻，-1 = 无。 */
int64_t hb_share_owner_next_ms(hb_share_owner *o);
void     hb_share_owner_tick(hb_share_owner *o);

void hb_share_owner_set_mode(hb_share_owner *o, int mode);
void hb_share_owner_kick(hb_share_owner *o, uint32_t participant_id);
/* 重新生成配对码与 link_secret（解锁 PAKE；平台应同时 POST /shares/{id}/guest） */
void hb_share_owner_regen_guest(hb_share_owner *o);

int hb_share_owner_mode(hb_share_owner *o);
int hb_share_owner_count(hb_share_owner *o);        /* 参与者数 */

void hb_share_owner_free(hb_share_owner *o);

/* ============================ 参与者 ============================ */

typedef struct hb_share_peer hb_share_peer;

/* member：share_id + VK + 服务端 GET /shares 拿到的 meta blob。
 * device/name 填入 JOIN。失败返回 NULL 并置 *err。 */
hb_share_peer *hb_share_peer_new_member(const char *share_id,
                                        const uint8_t vk[32],
                                        const char *meta_b64,
                                        const char *device, const char *name,
                                        hb_err *err);

/* guest：§3 链接字符串 + 6 位配对码。 */
hb_share_peer *hb_share_peer_new_guest(const char *link, const char *code,
                                       hb_err *err);

void hb_share_peer_free(hb_share_peer *p);

const char *hb_share_peer_share_id(hb_share_peer *p);
const char *hb_share_peer_server(hb_share_peer *p);   /* guest：链接中的 server_url，否则 NULL */
hb_vt      *hb_share_peer_vt(hb_share_peer *p);
int         hb_share_peer_perm(hb_share_peer *p);     /* HB_SHARE_*，未授权时 VIEW */
const char *hb_share_peer_title(hb_share_peer *p);    /* member：meta 中的 title */
uint32_t    hb_share_peer_self_id(hb_share_peer *p);  /* hello 之后的自身 id */

void hb_share_peer_incoming(hb_share_peer *p, const uint8_t *data,
                            size_t len, bool is_text);

int  hb_share_peer_fd(hb_share_peer *p);
bool hb_share_peer_next_frame(hb_share_peer *p, hb_share_frame *out);
bool hb_share_peer_next_event(hb_share_peer *p, hb_share_event *ev);

int64_t hb_share_peer_next_ms(hb_share_peer *p);
void     hb_share_peer_tick(hb_share_peer *p);

/* 发送输入（collab）。只读时返回 HB_ERR_INVALID（UI 提示只读）。 */
hb_err hb_share_peer_input(hb_share_peer *p, const char *bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif
