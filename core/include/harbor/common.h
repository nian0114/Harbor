#ifndef HB_COMMON_H
#define HB_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HB_OK = 0,
    HB_ERR_NOMEM,
    HB_ERR_IO,
    HB_ERR_DB,
    HB_ERR_CRYPTO,
    HB_ERR_NOT_FOUND,
    HB_ERR_INVALID,
    HB_ERR_EXISTS,
} hb_err;

const char *hb_strerror(hb_err e);

/* idempotent；初始化 libsodium 等全局状态。所有会用到随机数/加密的入口都会自动调用。 */
int hb_core_init(void);

/* 随机 v4 UUID，输出形如 "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"（含 NUL，37 字节）。 */
void hb_uuid(char out[37]);

/* CLOCK_REALTIME 毫秒。 */
int64_t hb_now_ms(void);

/* 复制字符串到 sodium_malloc 的锁定内存（NUL 结尾）；用完 hb_secret_free 清零释放。 */
char *hb_secret_strdup(const char *s);
/* NULL 安全。 */
void hb_secret_free(char *s);

/* 释放 hb_* API 返回的 malloc 字符串数组。 */
void hb_strfreev(char **v, size_t n);

#ifdef __cplusplus
}
#endif

#endif
