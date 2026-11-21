#ifndef HB_SYNC_H
#define HB_SYNC_H

#include "common.h"
#include "vault.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* hb_sync：E2EE 账号同步（docs/api/sync-protocol.md）。                  */
/*                                                                     */
/* 传输无关：core 只生成/消费请求/响应 JSON；平台层负责 HTTP/WebSocket。  */
/* 所有密钥材料（MK/auth_key/wrap_key/VK/E/rc_*）由调用方或本模块放入     */
/* sodium_malloc 内存、用完即擦；永不打印日志。                          */
/* ------------------------------------------------------------------ */

#define HB_SYNC_KEY_LEN  32
#define HB_SYNC_SALT_LEN 16

/* ============================ BIP-39（§3.1） ============================ */

/* 256 位熵 → 24 词空格分隔。out 需要 >= 256 字节。0 = 成功。 */
int hb_bip39_encode(const uint8_t ent[32], char *out, size_t out_sz);
/* 24 词 → 256 位熵。大小写/多余空白容忍；未知词或校验和错 → <0。 */
int hb_bip39_decode(const char *words, uint8_t ent[32]);

/* ============================ 低层密码学 ============================ */

/* seal(key,pt,ad) = nonce(24) || XChaCha20-Poly1305-ietf(pt,ad)。
 * 返回 sodium_malloc'd（调用方 sodium_free）。 */
uint8_t *hb_sync_seal(const uint8_t key[32], const uint8_t *pt, size_t pt_len,
                      const uint8_t *ad, size_t ad_len, size_t *out_len);
/* 解 seal。返回 sodium_malloc'd 明文；认证失败 NULL。 */
uint8_t *hb_sync_open(const uint8_t key[32], const uint8_t *blob, size_t n,
                      const uint8_t *ad, size_t ad_len, size_t *pt_len);

/* MK = crypto_pwhash(32, pw, salt, ops, mem, ARGON2ID13)。mk 为 sodium_malloc 安全区。 */
hb_err hb_sync_pwhash(const char *password, const uint8_t salt[16],
                      uint64_t ops, uint64_t mem, uint8_t mk[32]);
/* auth_key = kdf(MK,1,"hbauth01")；wrap_key = kdf(MK,2,"hbwrap01")。 */
void hb_sync_derive_mk(const uint8_t mk[32], uint8_t auth_key[32], uint8_t wrap_key[32]);
/* rc_wrap = kdf(E,1,"hbrcwrap")；rc_auth = kdf(E,2,"hbrcauth")。 */
void hb_sync_derive_rc(const uint8_t E[32], uint8_t rc_wrap[32], uint8_t rc_auth[32]);

/* ======================== 账号请求体（§6.1） ======================== */
/* 所有 *out 为 malloc'd JSON 字符串（free()）。password 须已由平台层
 * NFKC + UTF-8 规范化。kdf_ops/kdf_mem 传 0 = 默认（MODERATE）。 */

hb_err hb_sync_prelogin_body(const char *email, char **out);
/* 解析 prelogin 响应 {kdf:{salt,ops,mem}}；拒绝 ops<2 或 mem<64MiB（HB_ERR_CRYPTO）。 */
hb_err hb_sync_prelogin_parse(const char *resp, uint8_t salt[16],
                              uint64_t *ops, uint64_t *mem);

/* register：生成随机 VK 与恢复熵 E；*words_out = 24 词空格分隔；
 * vk 写入 vk[32]。 */
hb_err hb_sync_register_body(const char *email, const char *password,
                             const char *dev_name, const char *dev_platform,
                             uint64_t kdf_ops, uint64_t kdf_mem,
                             char **body_out, char **words_out,
                             uint8_t vk[32]);

/* login：从 password+prelogin kdf 派生 MK；*body_out 含 auth_key；
 * wrap_key[32] 输出（sodium 内存语义，调用方用完 sodium_memzero）——
 * 用于 hb_sync_login_parse 解包 VK。 */
hb_err hb_sync_login_body(const char *email, const char *password,
                          const uint8_t salt[16], uint64_t ops, uint64_t mem,
                          const char *totp, const char *dev_name,
                          const char *dev_platform,
                          char **body_out, uint8_t wrap_key[32]);

/* 解析 login 响应：解包 wrapped_vk_pw → vk；输出 account_id/device_id/
 * token/totp_enabled（字符串 malloc'd，可 NULL 不关心，token 可能缺省）。 */
hb_err hb_sync_login_parse(const char *resp, const uint8_t wrap_key[32],
                           uint8_t vk[32], char **account_id,
                           char **device_id, char **token, bool *totp_enabled);

/* 解析 register / recover/finish 响应 {account_id, device_id, token}。 */
hb_err hb_sync_auth_resp_parse(const char *resp, char **account_id,
                               char **device_id, char **token);

hb_err hb_sync_recover_start_body(const char *email, const char *words,
                                  char **body_out);
/* 解析 {recovery_token, wrapped_vk_rc}：words → rc_wrap → vk。
 * recovery_token 为 malloc'd。 */
hb_err hb_sync_recover_start_parse(const char *resp, const char *words,
                                   uint8_t vk[32], char **recovery_token);
/* recover/finish：new_password + 新 kdf + VK → body。
 * kdf_ops/kdf_mem 传 0 = 默认。 */
hb_err hb_sync_recover_finish_body(const char *recovery_token,
                                   const char *new_password,
                                   uint64_t kdf_ops, uint64_t kdf_mem,
                                   const char *dev_name,
                                   const char *dev_platform,
                                   const uint8_t vk[32], char **body_out);

/* POST /password：{auth_key_old, auth_key, kdf, wrapped_vk_pw}。 */
hb_err hb_sync_change_password_body(const char *old_password,
                                    const uint8_t old_salt[16],
                                    uint64_t old_ops, uint64_t old_mem,
                                    const char *new_password,
                                    uint64_t new_ops, uint64_t new_mem,
                                    const uint8_t vk[32], char **body_out);

/* ============================ 同步引擎 ============================ */

typedef struct hb_sync hb_sync;

hb_sync *hb_sync_new(hb_vault *v);
void     hb_sync_free(hb_sync *s);      /* 擦除 VK */
/* VK 复制进 sodium 内存；之后对象封装/解密可用。 */
hb_err   hb_sync_set_vk(hb_sync *s, const uint8_t vk[32]);
void     hb_sync_clear_vk(hb_sync *s);

/* POST /recovery：轮换恢复码（新 E）。password + 当前 kdf → auth_key
 * 证明身份；VK 取自已 set_vk 的 s → wrapped_vk_rc。
 * → body {auth_key, rc_auth, wrapped_vk_rc} + 24 个新词（*words_out，
 * 空格分隔，free）。 */
hb_err   hb_sync_recovery_rotate_body(hb_sync *s, const char *password,
                                      const uint8_t salt[16],
                                      uint64_t ops, uint64_t mem,
                                      char **body_out, char **words_out);

/* 同上但 VK 直接传入 —— 供平台层在 worker 线程调用（不持有 hb_sync）。 */
hb_err   hb_sync_recovery_rotate_body_vk(const uint8_t vk[32],
                                      const char *password,
                                      const uint8_t salt[16],
                                      uint64_t ops, uint64_t mem,
                                      char **body_out, char **words_out);

/* 范围（§4.5）：scope ∈ {"hosts","keys","snippets","forwards","known","settings"}。
 * hosts = host+group 类型。默认全开。 */
bool   hb_sync_scope_get(hb_sync *s, const char *scope);
/* 重新开启某 scope 把游标重置为 0（全量重拉）。 */
hb_err hb_sync_scope_set(hb_sync *s, const char *scope, bool on);

/* 账号元数据（vault meta；device_id 为服务端签发，与本机 _fv 设备 id 无关）。 */
hb_err hb_sync_set_account(hb_sync *s, const char *account_id,
                           const char *email, const char *server,
                           const char *device_id);
/* 未绑定账号返回 HB_ERR_NOT_FOUND；各出参 malloc'd（可 NULL）。 */
hb_err hb_sync_get_account(hb_sync *s, char **account_id, char **email,
                           char **server, char **device_id);

/* ---- 拉（§7.1） ---- */
/* "/v1/objects?since=<cursor>&limit=500"（malloc'd）。 */
hb_err hb_sync_pull_path(hb_sync *s, char **out);

typedef struct {
    size_t  applied;          /* 成功落库条数 */
    size_t  skipped_scope;    /* 范围外跳过 */
    size_t  decrypt_failed;   /* 解密失败（跳过，游标照常前进） */
    bool    more;             /* 服务端还有更多 */
    int64_t cursor;           /* 应用后的游标 */
} hb_sync_pull_stats;

/* 应用 GET /objects 响应。cursor 推进到响应的 next。 */
hb_err hb_sync_apply_pull(hb_sync *s, const char *resp_json,
                          hb_sync_pull_stats *stats);

/* ---- 推（§7.2） ---- */

typedef struct hb_sync_push hb_sync_push;

/* 收集范围内 dirty 对象（≤200）并封装。*out == NULL 表示无变更。 */
hb_err hb_sync_build_push(hb_sync *s, hb_sync_push **out);
/* 请求体 JSON（借用指针，随 push 生命周期）。 */
const char *hb_sync_push_body(const hb_sync_push *p);
/* 应用 POST /objects 响应：ok → 条件标干净+sync_seq；conflict → 与返回
 * 版本合并并置 sync_seq。*need_retry = 有 CAS 冲突需要重推（调用方限 3 次/轮）。 */
hb_err hb_sync_apply_push(hb_sync *s, hb_sync_push *p, const char *resp_json,
                          bool *need_retry);
void hb_sync_push_free(hb_sync_push *p);

/* ---- 冲突（§5） ---- */

typedef struct {
    char   *id;
    char   *type;
    char   *fields_json;   /* JSON 数组，冲突字段名 */
    char   *loser_summary; /* 输方 data JSON（含 _fv） */
    int64_t created;
} hb_sync_conflict;

hb_err hb_sync_conflicts(hb_sync *s, hb_sync_conflict **out, size_t *n);
void   hb_sync_conflicts_free(hb_sync_conflict *c, size_t n);
size_t hb_sync_conflict_count(hb_sync *s);
/* 保留本地：仅删除冲突记录。 */
hb_err hb_sync_conflict_keep_local(hb_sync *s, const char *id);
/* 保留另一版本：对冲突字段以普通 put 写入输方值（产生新戳），再删记录。 */
hb_err hb_sync_conflict_take_other(hb_sync *s, const char *id);

/* ---- 擦除与登出（§7.4/§8） ---- */

/* remote wipe：硬删除范围内全部对象 + 冲突 + 游标/sync_seq。 */
hb_err hb_sync_wipe(hb_sync *s);
/* 登出保留本地：清账号元数据与同步状态（游标/sync_seq/冲突），
 * 保留对象并全部标 dirty。 */
hb_err hb_sync_logout_local(hb_sync *s);

/* ============================ blob（§4.2，测试用） ============================ */

/* blob = 0x01 || seal(VK, sodium_pad(pt,256), "hb:obj:v1"||0||id||0||type)。
 * 返回 sodium_malloc'd。 */
uint8_t *hb_sync_blob_seal(const uint8_t vk[32], const char *id,
                           const char *type, const char *pt_json,
                           size_t *out_len);
/* 解 blob → pt JSON（sodium_malloc'd，含 secret 明文，用完 sodium_memzero）。 */
char *hb_sync_blob_open(const uint8_t vk[32], const char *id,
                        const char *type, const uint8_t *blob, size_t n);

#ifdef __cplusplus
}
#endif

#endif
