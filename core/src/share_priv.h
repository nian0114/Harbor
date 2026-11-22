#ifndef HB_SHARE_PRIV_H
#define HB_SHARE_PRIV_H

/* FR-12 内部接口：仅 core 内部与测试使用（CPace 固定标量注入、G/ISK 观测）。 */

#include "harbor/share.h"

/* CPace over ristretto255（§6）。调用方持有 code(6 数字) 与 link_secret(16)。 */
typedef struct {
    uint8_t  G[32];          /* 生成元（测试观测用） */
    uint8_t *x;              /* sodium 32 己方标量 */
    uint8_t  Y[32];          /* 己方公钥 = x·G */
    uint8_t *sid;            /* share_id(ASCII) || nonce_g */
    size_t   sid_len;
} hb_cpace;

/* fixed_scalar：NULL → 随机；非 NULL 用其 32 字节（测试向量）。 */
hb_cpace *hb__cpace_new(const char *share_id, const uint8_t nonce_g[16],
                        const char code[7], const uint8_t link_secret[16],
                        const uint8_t fixed_scalar[32]);
void      hb__cpace_free(hb_cpace *c);

/* 校验 peer_Y、K = x·peer_Y、ISK = H("hb:cpace:isk"||lv(sid)||K||Ya||Yb)。
 * Ya/Yb 必须按 (guest 公钥, owner 公钥) 顺序传入。K 用完即擦。 */
hb_err hb__cpace_isk(hb_cpace *c, const uint8_t peer_Y[32],
                     const uint8_t Ya[32], const uint8_t Yb[32],
                     uint8_t isk[64]);

/* K_pake = crypto_kdf_derive_from_key(32, 1, "hbpake01", ISK[32..64]) */
void hb__share_k_pake(const uint8_t isk[64], uint8_t out[32]);

/* 帧头（§4.1）：type(1)|to(4BE)|from(4BE)|seq(8BE)，17 字节 */
#define HB_FR_HDRLEN 17

/* 帧 AD："hb:share:v1"||0||share_id||0||hdr17；返回 malloc 缓冲 */
uint8_t *hb__share_ad(const char *share_id, const uint8_t hdr[HB_FR_HDRLEN],
                      size_t *ad_len);

/* 组帧/解封（引擎内部与测试共用；payload = key ? seal : 明文） */
hb_share_frame hb__share_mk_frame(const char *share_id, uint8_t type,
                                  uint32_t to, uint32_t from, uint64_t seq,
                                  const uint8_t *key,
                                  const uint8_t *pt, size_t n);
hb_err hb__share_open_frame(const char *share_id, const uint8_t *data,
                            size_t n, const uint8_t *key,
                            uint8_t *type, uint32_t *to, uint32_t *from,
                            uint64_t *seq, uint8_t **pt, size_t *pt_len);

#endif
