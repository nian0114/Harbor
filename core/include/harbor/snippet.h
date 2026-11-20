#ifndef HARBOR_SNIPPET_H
#define HARBOR_SNIPPET_H

/* Snippet 变量（{{name}} / {{name=default}}）的提取与渲染（FR-08）。
 * 纯字符串处理，无 vault/GUI 依赖。 */

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 提取去重后的变量名（首次出现顺序）。
 * 语法 {{name}} 或 {{name=default}}；name 限 [A-Za-z0-9_.-]+。
 * names/defaults 输出 malloc 的字符串数组（defaults 无默认值为 NULL 元素），
 * 调用方用 hb_strfreev 释放。names/defaults 均可传 NULL。 */
size_t hb_snippet_vars(const char *body, char ***names, char ***defaults);

/* 渲染：把每个 names[i] 的 {{name}} / {{name=default}} 换成 values[i]；
 * values[i]==NULL 时回落到该变量的默认值（仍无则原样保留）。
 * 未在 names 中列出的变量原样保留。返回 malloc 的新串。 */
char  *hb_snippet_render(const char *body,
                         const char *const *names,
                         const char *const *values,
                         size_t n);

#ifdef __cplusplus
}
#endif

#endif /* HARBOR_SNIPPET_H */
