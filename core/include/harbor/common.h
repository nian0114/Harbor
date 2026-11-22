#ifndef HARBOR_COMMON_H
#define HARBOR_COMMON_H

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
    HB_ERR_PASSPHRASE,
} hb_err;

const char *hb_strerror(hb_err e);

/* 用户可见文案翻译：msgid 即中文原文，en 目录提供英文。
 * 构建带 NLS（libintl/dgettext 可用）时经 domain "harbor" 查表，
 * 否则原样返回中文。core 不调用 setlocale —— 由进程入口（如
 * linux/src/main.c）负责 locale 与 bindtextdomain。
 * 只包裹 UI 展示文案；落库/协议/比较用的数据串保持原样（HB_N_ 仅为
 * xgettext 提取标记，语义同 N_()）。 */
#ifdef ENABLE_NLS
const char *hb__tr(const char *s);
# define HB_TR(s)  hb__tr(s)
#else
# define HB_TR(s)  (s)
#endif
#define HB_N_(s)   (s)

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

#endif /* HARBOR_COMMON_H */
