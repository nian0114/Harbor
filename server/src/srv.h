#ifndef HB_SRV_H
#define HB_SRV_H

/* harbor-server：FR-11 同步服务端（docs/api/sync-protocol.md §6）。
 * 零知识：只存密文 blob 与元数据，永不接触明文/密钥。 */

#include <sqlite3.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sodium.h>

#define HB_SRV_ID_MAX    64
#define HB_SRV_TYPE_MAX  32
#define HB_SRV_BLOB_MAX  (1 << 20)   /* blob 解码后上限 1 MiB */
#define HB_SRV_PUSH_MAX  200         /* 单批 changes 上限 */
#define HB_SRV_PULL_MAX  500         /* 单页 objects 上限 */
#define HB_SRV_IP_LIMIT  20          /* 每 IP 认证类请求 / 分钟 */
#define HB_SRV_FAIL_MAX  5           /* 连续失败锁定阈值 */
#define HB_SRV_LOCK_MAX_MS (3600LL * 1000) /* 锁定时长上限 1 h */

typedef struct hb_srv hb_srv;

/* ---- util.c ---- */

/* 严格 base64url（无填充）：拒绝 '='、非法字符、len%4==1。成功返回 malloc'd
 * 或 NULL。 */
uint8_t *hbu_b64_decode(const char *s, size_t *n_out);
char    *hbu_b64_encode(const uint8_t *d, size_t n);
char    *hbu_sha256_hex(const uint8_t *d, size_t n); /* malloc'd 64 hex */
void     hbu_hmac_sha256(const uint8_t *key, size_t kn,
                         const uint8_t *data, size_t dn, uint8_t out[32]);
char    *hbu_email_norm(const char *email); /* trim + ASCII 小写，malloc'd */
char    *hbu_base32_encode(const uint8_t *d, size_t n); /* RFC 4648 无填充 */
/* RFC 6238 TOTP（SHA1、6 位、30 s 步长）：返回 0..999999。 */
uint32_t hbu_totp(const uint8_t *secret, size_t n, int64_t step);
/* 验证 6 位 code：容忍 ±1 步；命中步 > *last_step 才算有效（防窗口内重放），
 * 命中时把命中步写回 *used_step。返回是否有效。 */
bool     hbu_totp_check(const uint8_t *secret, size_t n, const char *code,
                        int64_t now_ms, int64_t last_used, int64_t *used_step);

int64_t  hbu_now_ms(void);
bool     hbu_valid_id(const char *id);    /* ≤64 [A-Za-z0-9-] */
bool     hbu_valid_type(const char *t);   /* ≤32 [a-z_] */

/* 请求体解析为 JsonObject（失败 NULL，调用方 respond bad_request）。 */
JsonObject *hbu_body_json(SoupServerMessage *msg);
const char *hbu_jstr(JsonObject *o, const char *key);
int64_t     hbu_jint(JsonObject *o, const char *key, int64_t fallback, bool *ok);

void hbu_respond_json(SoupServerMessage *msg, guint status, const char *json);
/* 统一错误体 {"error":code,"message":msg, ...}；extra_json 为并入的裸 JSON
 * 片段（如 "\"retry_after\":60"），可 NULL。 */
void hbu_error(SoupServerMessage *msg, guint status, const char *code,
               const char *message, const char *extra_json);

/* ---- db.c ---- */

typedef struct {
    char   *id;
    char   *email;
    char   *auth_hash;     /* crypto_pwhash_str(auth_key) */
    char   *kdf_salt;      /* base64url */
    int64_t kdf_ops, kdf_mem;
    char   *wrapped_vk_pw; /* base64url */
    char   *rc_hash;       /* crypto_pwhash_str(rc_auth) */
    char   *wrapped_vk_rc; /* base64url */
    uint8_t totp_secret[20];
    bool    totp_pending;  /* totp_secret 列是否有待启用的 secret */
    bool    totp_enabled;
    int64_t totp_last_step;
    int     fail_count;
    int64_t locked_until;  /* ms */
    int64_t created;
} HbAcct;

typedef struct {
    char   *id, *name, *platform;
    int64_t created, last_seen;
    bool    revoked, wipe;
} HbDevice;

typedef struct {
    char   *id, *type;
    int64_t seq, updated_at;
    uint8_t *blob;
    size_t  blob_n;
} HbObject;

sqlite3 *hbd_open(const char *dir, GError **err);

int  hbd_acct_by_email(sqlite3 *db, const char *email, HbAcct **out);
int  hbd_acct_by_id(sqlite3 *db, const char *id, HbAcct **out);
int  hbd_acct_insert(sqlite3 *db, const HbAcct *a);
int  hbd_acct_set_lock(sqlite3 *db, const char *id, int fail, int64_t until);
int  hbd_acct_set_rc(sqlite3 *db, const char *id, const char *rc_hash,
                     const char *wrapped_vk_rc);
int  hbd_acct_set_auth(sqlite3 *db, const char *id, const char *auth_hash,
                       const char *salt, int64_t ops, int64_t mem,
                       const char *wrapped_vk_pw);
int  hbd_acct_set_totp(sqlite3 *db, const char *id, const uint8_t secret[20],
                       bool pending, bool enabled, int64_t last_step);
void hbd_acct_free(HbAcct *a);

int  hbd_device_add(sqlite3 *db, const char *id, const char *acct,
                    const char *name, const char *platform);
int  hbd_device_by_id(sqlite3 *db, const char *id, char **acct_out,
                      bool *revoked, bool *wipe);
/* 返回该账号未吊销设备列表（GArray of HbDevice，hbd_devices_free）。 */
GArray *hbd_devices_list(sqlite3 *db, const char *acct);
void   hbd_devices_free(GArray *arr);
int    hbd_device_revoke(sqlite3 *db, const char *acct, const char *id,
                         bool wipe);
int    hbd_device_touch(sqlite3 *db, const char *id, int64_t now_ms);

/* token：仅存 SHA256 hex；吊销设备的 token 行保留（墓碑）以便回
 * device_revoked。 */
int  hbd_token_add(sqlite3 *db, const char *hash, const char *device,
                   const char *acct);
/* 命中 → 0 且输出 device/acct；未命中 → HB_SRV_NOT_FOUND。 */
int  hbd_token_lookup(sqlite3 *db, const char *hash, char **device_out,
                      char **acct_out);

int  hbd_recovery_add(sqlite3 *db, const char *hash, const char *acct,
                      int64_t expires);
/* 命中且未过期 → 0 + acct；否则 NOT_FOUND。 */
int  hbd_recovery_lookup(sqlite3 *db, const char *hash, char **acct_out);
int  hbd_recovery_del(sqlite3 *db, const char *hash);

/* 对象：返回该账号 seq>since 的前 limit 条（按 seq 升序）。 */
GArray *hbd_objects_since(sqlite3 *db, const char *acct, int64_t since,
                          int64_t limit);
void    hbd_objects_free(GArray *arr);
/* 查 (acct,id) 当前 seq；不存在 → -1。 */
int64_t hbd_object_seq(sqlite3 *db, const char *acct, const char *id);
int     hbd_object_get(sqlite3 *db, const char *acct, const char *id,
                       HbObject *out);
/* 事务内：取下一个 seq（account_seq 自增）。 */
int64_t hbd_seq_next(sqlite3 *db, const char *acct);
int     hbd_object_put(sqlite3 *db, const char *acct, const char *id,
                       const char *type, int64_t seq, const uint8_t *blob,
                       size_t bn, int64_t updated_at);

/* ---- server context ---- */

struct hb_srv {
    sqlite3    *db;
    char       *data_dir;
    uint8_t     secret[32];     /* server.secret：伪造 salt 的 HMAC key */
    char        dummy_hash[crypto_pwhash_STRBYTES]; /* 未知邮箱的等时校验 */
    int64_t     lock_base_ms;   /* 锁定基准时长（测试可经 env 缩短） */
    int         ip_limit;       /* 每 IP 认证类请求/分钟 */
    GHashTable *ip_hits;        /* ip -> IpWin*，认证类限流 */
    GThreadPool *hash_pool;     /* 有界 Argon2 线程池（--hash-threads） */
    int         hash_threads;   /* 池线程数 */
    int         hash_queue;     /* 排队+运行中任务上限（--hash-queue） */
    gint        hash_pending;   /* 原子计数 */
    GPtrArray  *sockets;        /* WsClient* */
    SoupServer *soup;
    GMainLoop  *loop;
    guint       log_level;      /* 0 err 1 warn 2 info 3 debug */
};

typedef struct {
    hb_srv                  *srv;
    SoupWebsocketConnection *conn;
    char                    *account_id;
    char                    *device_id;
    int64_t                  last_pong;
    guint                    ping_src;
    bool                     dead;
} WsClient;

#define hb_log(s, lvl, ...)                                                   \
    do {                                                                      \
        if ((lvl) <= (s)->log_level)                                          \
            g_printerr("[hb-%s] ",                                            \
                       (lvl) == 0 ? "error" :                                 \
                       (lvl) == 1 ? "warn " :                                 \
                       (lvl) == 2 ? "info " : "debug"),                       \
            g_printerr(__VA_ARGS__),                                          \
            g_printerr("\n");                                                 \
    } while (0)

/* 认证中间件：校验 Bearer token。成功返回 true 并输出 account/device id
 * （malloc'd）；失败时已写出响应（401 unauthorized / device_revoked）返回
 * false。同时更新 last_seen（60 s 节流）。 */
bool hb_auth_check(hb_srv *srv, SoupServerMessage *msg,
                   char **account_out, char **device_out);
/* 认证类 IP 限流：超限返回 true（已写 429）。 */
bool hb_rate_limited(hb_srv *srv, SoupServerMessage *msg);
/* WebSocket 注册/通知（main loop）。 */
void hb_ws_notify_changed(hb_srv *srv, const char *account, int64_t seq);
void hb_ws_notify_revoked(hb_srv *srv, const char *device_id, bool wipe);
void hb_ws_client_free(WsClient *c);
void http_attach(hb_srv *srv);

#endif
