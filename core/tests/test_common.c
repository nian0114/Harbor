/* test_common.c — hb_set_translator / HB_TR 无 NLS 钩子。 */

#include "test.h"

#include "harbor/common.h"

#include <pthread.h>

static const char *fake_tr(const char *msgid)
{
    if (!strcmp(msgid, "中文"))
        return "Chinese";
    return NULL;                    /* 未翻译 → 回退原串 */
}

static void *tr_thread(void *ud)
{
    int *ok = ud;
    /* 会话线程并发调用路径：读已装好的表应得到译串 */
    *ok = !strcmp(HB_TR("中文"), "Chinese") &&
          !strcmp(HB_TR("未收录"), "未收录");
    return NULL;
}

int main(void)
{
    pthread_t t;
    int ok = 0;

    /* 未设置 → 原样返回 msgid */
    CHECK_EQ_STR(HB_TR("中文"), "中文");
    CHECK_EQ_STR(HB_TR("hello"), "hello");

    hb_set_translator(fake_tr);
    CHECK_EQ_STR(HB_TR("中文"), "Chinese");
    CHECK_EQ_STR(HB_TR("未收录"), "未收录");   /* NULL → 原串 */

    /* 线程并发读取 */
    pthread_create(&t, NULL, tr_thread, &ok);
    pthread_join(t, NULL);
    CHECK(ok == 1);

    /* 卸掉钩子 → 回到恒等 */
    hb_set_translator(NULL);
    CHECK_EQ_STR(HB_TR("中文"), "中文");

    fprintf(stderr, "test_common: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
