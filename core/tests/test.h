#ifndef HB_TEST_H
#define HB_TEST_H

/* 极简单元测试宏：失败打印 file:line，统计失败数。 */

#include <stdio.h>
#include <string.h>

static int hb_t_failures;
static int hb_t_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        hb_t_checks++;                                                       \
        if (!(cond)) {                                                       \
            hb_t_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                     \
                    __FILE__, __LINE__, #cond);                              \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                   \
    do {                                                                     \
        hb_t_checks++;                                                       \
        long long _a = (long long)(a), _b = (long long)(b);                  \
        if (_a != _b) {                                                      \
            hb_t_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK_EQ_INT failed: %s=%lld %s=%lld\n", \
                    __FILE__, __LINE__, #a, _a, #b, _b);                     \
        }                                                                    \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                   \
    do {                                                                     \
        hb_t_checks++;                                                       \
        const char *_a = (a), *_b = (b);                                     \
        if ((_a == NULL) != (_b == NULL) ||                                  \
            (_a && _b && strcmp(_a, _b) != 0)) {                             \
            hb_t_failures++;                                                 \
            fprintf(stderr,                                                \
                    "%s:%d: CHECK_EQ_STR failed: %s=\"%s\" %s=\"%s\"\n",     \
                    __FILE__, __LINE__,                                      \
                    #a, _a ? _a : "(null)", #b, _b ? _b : "(null)");         \
        }                                                                    \
    } while (0)

#define TEST_RETURN() return hb_t_failures ? 1 : 0

#endif
