#ifndef HARBOR_KEYS_H
#define HARBOR_KEYS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 私钥（Keychain）：生成 OpenSSH openssh-key-v1 格式私钥、              */
/* 检查/转换外部私钥文件。生成的 private_text 进 vault secret（落盘即密文）。*/
/* ------------------------------------------------------------------ */

typedef enum { HB_KEY_ED25519, HB_KEY_ECDSA_P256, HB_KEY_ECDSA_P384,
               HB_KEY_ECDSA_P521, HB_KEY_RSA } hb_key_type;
typedef enum { HB_KEYFMT_OPENSSH, HB_KEYFMT_PEM, HB_KEYFMT_PKCS8,
               HB_KEYFMT_PKCS12 } hb_key_format;

typedef struct {
    char *kind;          /* "ed25519" | "ecdsa" | "rsa"（data-model kind） */
    int   bits;          /* ed25519 恒 256；256/384/521；RSA 模数位；0 = 未知 */
    char *public_line;   /* "ssh-ed25519 AAAA… comment"；未知（加密 PEM 无口令）→ NULL */
    char *fingerprint;   /* "SHA256:…"（hb_ssh_fingerprint_sha256）；未知 → NULL */
    char *comment;       /* NULL = 无 */
    bool  encrypted;     /* 源文本需要口令 */
    hb_key_format format;
    char *private_text;  /* hb_secret 内存：要存为 vault "private" secret 的文本 */
} hb_key_info;

/* 释放全部字段；private_text 走 hb_secret_free。NULL 安全。 */
void   hb_key_info_clear(hb_key_info *k);

/* comment 可 NULL；RSA 仅接受 rsa_bits = 3072 | 4096，其它 → HB_ERR_INVALID。 */
hb_err hb_key_generate(hb_key_type t, int rsa_bits, const char *comment,
                       hb_key_info *out);
/* 识别内容并提取元数据。加密的 OpenSSH v1 公钥在明文头部，故无口令也能给出
 * public/fingerprint——但口令不会被校验（没有 bcrypt_pbkdf；错误口令会在
 * 首次使用时由 libssh2 报出）。PEM/PKCS8/PKCS12 的口令在此校验：
 * 错误 → HB_ERR_PASSPHRASE；PKCS12 成功后 private_text 已转成 openssh-key-v1。 */
hb_err hb_key_inspect(const unsigned char *data, size_t len,
                      const char *passphrase, hb_key_info *out);
/* 仅接受普通文件，>1 MiB → HB_ERR_INVALID。 */
hb_err hb_key_inspect_file(const char *path, const char *passphrase,
                           hb_key_info *out);

#ifdef __cplusplus
}
#endif

#endif /* HARBOR_KEYS_H */
