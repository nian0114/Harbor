#ifndef HB_VAULT_PRIV_H
#define HB_VAULT_PRIV_H

/* 内部接口（core 内部 + 测试使用，非公共 API）。 */

#include "harbor/vault.h"

/* 解密一个 secret blob 到 sodium_malloc 缓冲区（二进制 map 明文）。
 * 认证失败（错误的 DEK / 数据被篡改）返回 HB_ERR_CRYPTO。
 * out/out_len 成功时为 plaintext（调用方 sodium_free / sodium_memzero）。 */
hb_err hb__vault_secret_decrypt(hb_vault *v, const char *id, const char *type,
                                const uint8_t *blob, size_t blob_len,
                                uint8_t **out, size_t *out_len);

#endif
