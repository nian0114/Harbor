/* FR-08：Snippet {{var}} / {{var=default}} 变量提取与渲染。 */

#include "harbor/snippet.h"

#include <stdlib.h>
#include <string.h>

static int var_name_ch(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

/* 返回变量 token 的总长度（含 "{{" "}}"），或 0 = 不是合法变量。
 * *name_e 指向 name 末尾（'=' 或 '}' 之前）；*has_def 标记 '=default'。 */
static size_t var_scan(const char *p, const char **name_e, int *has_def)
{
    const char *q = p + 2;                 /* 跳过 "{{" */
    const char *ne;
    if (!var_name_ch(*q))
        return 0;
    ne = q;
    while (var_name_ch(*ne))
        ne++;
    *name_e = ne;
    *has_def = 0;
    if (*ne == '=') {
        const char *d = ne + 1;
        while (*d && *d != '}' && *d != '{')
            d++;
        if (d[0] == '}' && d[1] == '}') {
            *has_def = 1;
            return (size_t)(d - p) + 2;
        }
        return 0;
    }
    if (ne[0] == '}' && ne[1] == '}')
        return (size_t)(ne - p) + 2;
    return 0;
}

/* 把 [s, e) 追加进 buf */
static void push(char **buf, size_t *len, size_t *cap,
                 const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 256;
        char *nb;
        while (*len + n + 1 > nc)
            nc *= 2;
        nb = realloc(*buf, nc);
        if (!nb)
            return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

size_t hb_snippet_vars(const char *body, char ***names, char ***defaults)
{
    char **ns = NULL, **ds = NULL;
    size_t n = 0;
    const char *p;

    if (names)     *names = NULL;
    if (defaults)  *defaults = NULL;
    if (!body)
        return 0;
    for (p = body; (p = strstr(p, "{{")); p += 2) {
        const char *ne = NULL, *de = NULL;
        int hd = 0, dup = 0;
        size_t tl = var_scan(p, &ne, &hd), i;
        if (!tl)
            continue;
        if (hd)
            de = p + tl - 2;           /* default = [ne+1, de) */
        for (i = 0; i < n; i++) {
            if (strlen(ns[i]) == (size_t)(ne - (p + 2)) &&
                !strncmp(ns[i], p + 2, (size_t)(ne - (p + 2)))) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        {
            char **t;
            t = realloc(ns, (n + 1) * sizeof *ns);
            if (!t) goto oom;
            ns = t;
            t = realloc(ds, (n + 1) * sizeof *ds);
            if (!t) goto oom;
            ds = t;
        }
        ns[n] = strndup(p + 2, (size_t)(ne - (p + 2)));
        ds[n] = hd ? strndup(ne + 1, (size_t)(de - (ne + 1))) : NULL;
        if (!ns[n] || (hd && !ds[n]))
            goto oom;
        n++;
    }
    if (names)    *names = ns;
    else          hb_strfreev(ns, n);
    if (defaults) *defaults = ds;
    else          hb_strfreev(ds, n);
    return n;
oom:
    hb_strfreev(ns, n);
    hb_strfreev(ds, n);
    return 0;
}

/* 若 name 在 names[] 里返回其下标，否则 -1 */
static long name_idx(const char *const *names, size_t n,
                     const char *name, size_t nlen)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (names[i] && strlen(names[i]) == nlen &&
            !strncmp(names[i], name, nlen))
            return (long)i;
    }
    return -1;
}

char *hb_snippet_render(const char *body,
                        const char *const *names,
                        const char *const *values,
                        size_t n)
{
    char *out = NULL;
    size_t len = 0, cap = 0;
    const char *p, *seg;

    if (!body)
        return NULL;
    seg = body;
    {
        const char *scan = body;
        while ((p = strstr(scan, "{{"))) {
            const char *ne = NULL;
            int hd = 0;
            size_t tl = var_scan(p, &ne, &hd);
            long ix;
            const char *val = NULL;
            size_t vlen = 0;
            if (!tl) {
                scan = p + 2;          /* 非法 "{{"：跳 2 字节继续找 */
                continue;
            }
            ix = name_idx(names, n, p + 2, (size_t)(ne - (p + 2)));
            if (ix >= 0 && values && values[ix])
                val = values[ix];
            else if (hd)
                val = ne + 1, vlen = (size_t)((p + tl - 2) - (ne + 1));
            else if (ix < 0)
                val = NULL;            /* 未知变量：原样保留 */
            push(&out, &len, &cap, seg, (size_t)(p - seg));
            if (val)
                push(&out, &len, &cap, val,
                     vlen ? vlen : strlen(val));
            else
                push(&out, &len, &cap, p, tl);
            p += tl;
            seg = scan = p;
        }
    }
    push(&out, &len, &cap, seg, strlen(seg));
    return out;
}
