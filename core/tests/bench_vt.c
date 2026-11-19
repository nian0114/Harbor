/* VT 层吞吐基准：~50MB 混合文本灌入 50x190 终端，打印 MB/s。
 * `meson test --benchmark` 或 `meson benchmark` 运行；不参与普通 test。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harbor/vt.h"

#define TOTAL_BYTES (50u * 1024 * 1024)
#define CHUNK       (64u * 1024)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    hb_vt *v;
    char *data;
    size_t cap = TOTAL_BYTES + CHUNK, len = 0;
    unsigned long line = 1;
    double t0, t1;

    data = malloc(cap);
    if (!data)
        return 1;

    /* ~90% 普通 seq 行，10% 带 SGR 颜色 + CJK 宽字符 */
    while (len + 256 < cap && len < TOTAL_BYTES) {
        if (line % 10 == 0)
            len += (size_t)snprintf(data + len, cap - len,
                                    "\e[1;3%dm%lu\e[0m 中文宽字符 mixed %lu\r\n",
                                    (int)(line % 8), (unsigned long)line,
                                    (unsigned long)line);
        else
            len += (size_t)snprintf(data + len, cap - len,
                                    "%lu\r\n", (unsigned long)line);
        line++;
    }

    v = hb_vt_new(50, 190, 10000);
    if (!v) {
        free(data);
        return 1;
    }

    t0 = now_s();
    {
        size_t off = 0;
        while (off < len) {
            size_t n = len - off;
            if (n > CHUNK)
                n = CHUNK;
            hb_vt_feed(v, data + off, n);
            off += n;
        }
    }
    t1 = now_s();

    printf("fed %.1f MB (%lu lines) in %.3f s -> %.1f MB/s\n",
           (double)len / (1024.0 * 1024.0), line - 1,
           t1 - t0, (double)len / (1024.0 * 1024.0) / (t1 - t0));

    hb_vt_free(v);
    free(data);
    return 0;
}
