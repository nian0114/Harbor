/* test_snippet.c — Snippet {{var}} / {{var=default}} 提取与渲染。 */

#include "test.h"

#include "harbor/snippet.h"
#include "harbor/common.h"

#include <stdlib.h>

static void test_vars_basic(void)
{
    char **ns = NULL, **ds = NULL;
    size_t n;

    n = hb_snippet_vars("tar -czf {{out}} {{dir}} && ls {{dir}}", &ns, &ds);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_STR(ns[0], "out");
    CHECK_EQ_STR(ns[1], "dir");
    CHECK(ds[0] == NULL && ds[1] == NULL);
    hb_strfreev(ns, n);
    hb_strfreev(ds, n);
}

static void test_vars_defaults(void)
{
    char **ns = NULL, **ds = NULL;
    size_t n = hb_snippet_vars(
        "ssh {{user=x}}@{{host}} -p {{port=22}} tail -f {{file=/var/log/syslog}}",
        &ns, &ds);
    CHECK_EQ_INT(n, 4);
    CHECK_EQ_STR(ns[0], "user");
    CHECK_EQ_STR(ds[0], "x");
    CHECK_EQ_STR(ns[1], "host");
    CHECK(ds[1] == NULL);
    CHECK_EQ_STR(ns[2], "port");
    CHECK_EQ_STR(ds[2], "22");
    CHECK_EQ_STR(ns[3], "file");
    CHECK_EQ_STR(ds[3], "/var/log/syslog");
    hb_strfreev(ns, n);
    hb_strfreev(ds, n);
}

static void test_vars_edge(void)
{
    char **ns = NULL, **ds = NULL;
    size_t n;

    /* 无变量 */
    n = hb_snippet_vars("echo plain text", &ns, &ds);
    CHECK_EQ_INT(n, 0);
    CHECK(ns == NULL && ds == NULL);

    /* NULL body */
    CHECK_EQ_INT(hb_snippet_vars(NULL, &ns, &ds), 0);

    /* 相邻变量 / 名字字符集 */
    n = hb_snippet_vars("{{a}}{{b_c-d.e}}", &ns, &ds);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_STR(ns[0], "a");
    CHECK_EQ_STR(ns[1], "b_c-d.e");
    hb_strfreev(ns, n);
    hb_strfreev(ds, n);
    ns = ds = NULL;

    /* 畸形：空格/空名/不闭合 → 全部不算变量 */
    n = hb_snippet_vars("{{ }} {{}} {{x} {{bad name}} {{{ok}}} {{unclosed",
                        &ns, &ds);
    /* "{{{ok}}}"：第三个 { 紧跟的 "{ok}}" 中 name 含 '{'? 不——
     * "{{{ok}}}" 扫描：p="{{" 后 name 起点是 '{'，非法 → skip 2 字节；
     * 从 "{ok}}}" 的 "{{" 处重新扫描？strstr 从 p+2 找下一个 "{{"：
     * 该串剩余 "{ok}}}" 中无 "{{"，故 0 个。unclosed 不闭合 → 0 */
    CHECK_EQ_INT(n, 0);
    CHECK(ns == NULL);
}

static void test_render(void)
{
    char *r;
    const char *names[] = { "dir", "who" };
    const char *vals[]  = { "/var/log", "world" };

    r = hb_snippet_render("ls {{dir}}; echo {{who}}", names, vals, 2);
    CHECK_EQ_STR(r, "ls /var/log; echo world");
    free(r);

    /* NULL value → 回退默认值；无默认 → 原样保留 */
    {
        const char *n2[] = { "path", "rest" };
        const char *v2[] = { NULL, "all" };
        r = hb_snippet_render("tail {{path=/tmp/x}}; echo {{rest}}; {{unk}}",
                              n2, v2, 2);
        CHECK_EQ_STR(r, "tail /tmp/x; echo all; {{unk}}");
        free(r);
    }

    /* 默认值含符号 */
    {
        const char *n3[] = { "cmd" };
        const char *v3[] = { NULL };
        r = hb_snippet_render("{{cmd=echo a|grep b}}!", n3, v3, 1);
        CHECK_EQ_STR(r, "echo a|grep b!");
        free(r);
    }

    /* 畸形原样保留 */
    r = hb_snippet_render("{{ }} and {{x} and {{unclosed", NULL, NULL, 0);
    CHECK_EQ_STR(r, "{{ }} and {{x} and {{unclosed");
    free(r);

    /* 同一变量多次出现 */
    {
        const char *n4[] = { "f" };
        const char *v4[] = { "X" };
        r = hb_snippet_render("{{f}}-{{f}}-{{f=def}}", n4, v4, 1);
        CHECK_EQ_STR(r, "X-X-X");
        free(r);
    }

    /* 无变量 body 原样返回 */
    r = hb_snippet_render("nothing", NULL, NULL, 0);
    CHECK_EQ_STR(r, "nothing");
    free(r);
}

int main(void)
{
    test_vars_basic();
    test_vars_defaults();
    test_vars_edge();
    test_render();
    fprintf(stderr, "test_snippet: %d checks, %d failures\n",
            hb_t_checks, hb_t_failures);
    TEST_RETURN();
}
