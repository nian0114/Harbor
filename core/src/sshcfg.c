/* sshcfg.c — OpenSSH ssh_config 子集解析（主机导入用）。
 *
 * 语义（尽量贴近 ssh_config(5)）：
 * - 关键字大小写不敏感；"Key value" / "Key=value" / "Key = value" 皆可；
 *   双引号值；# 注释与空行；容忍 CRLF。
 * - Host 行可带多个模式；* / ? 为通配（fnmatch，大小写不敏感），! 取反。
 *   具体别名 = 无通配、无 ! 的模式，按文件顺序去重（先出现者赢）。
 * - 每个别名按文件顺序遍历所有块取有效配置；某正模式命中且无取反模式命中
 *   时该块适用；每个关键字"先取得者赢"（尾部 Host * 只兜底）。
 * - Include 在所在位置按文本展开（含 Host 块内：被包含文件在自带 Host 行
 *   之前的行归入当前块；之后的块独立，其后外层行也归最新块 —— 与 ssh 的
 *   文本拼接一致）。相对路径相对 base_dir；无匹配静默；最深 16 层。
 * - Match 块整体跳过（至下个 Host/Match），每个 Match 行一条警告。
 * - ProxyCommand / *Forward / CertificateFile 警告并忽略；其它未知关键字
 *   静默忽略。Port / ServerAliveInterval 非法值警告（该关键字视为已取到）。
 */

#include "harbor/hostio.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_INCLUDE_DEPTH 16

typedef enum {
    KN_HOSTNAME, KN_USER, KN_PORT, KN_IDFILE, KN_PJUMP, KN_KALIVE,
} kn_t;

typedef struct {
    kn_t  kn;
    char *val;        /* 标量取首 token；ProxyJump 为逗号拼接原串 */
    char *file;
    int   line;
} ent;

typedef struct {
    char **pos;       /* NULL 结尾正模式 */
    char **neg;       /* NULL 结尾取反模式（去 !） */
    ent  *ents; size_t nent, cap;
    char *file; int line;   /* Host 行位置；file==NULL → 全局块 */
    int   global;
} blk;

typedef struct {
    blk   *blocks; size_t nblk, cap;
    int    cur;                 /* 当前块下标 */
    int    in_match;
    char **warn; size_t nwarn, wcap;
    char  *base_dir;
    int    depth;
} parser;

/* ------------------------------ 小工具 ------------------------------ */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void strv_push(char ***v, size_t *n, size_t *cap, const char *s)
{
    if (*n + 1 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        char **nv = realloc(*v, nc * sizeof(char *));
        if (!nv) return;
        *v = nv; *cap = nc;
    }
    (*v)[*n] = xstrdup(s);
    if ((*v)[*n]) (*n)++;
}

static void warnf(parser *p, const char *file, int line,
                  const char *fmt, ...)
{
    char msg[512], loc[600];
    va_list ap;
    if (!file) file = "config";
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(loc, sizeof loc, "%s:%d: %s", file, line, msg);
    strv_push(&p->warn, &p->nwarn, &p->wcap, loc);
}

/* 双引号感知的空白分词；引号被去除。返回 NULL 结尾数组。 */
static char **split_args(const char *s, size_t *out_n)
{
    char **v = NULL;
    size_t n = 0, cap = 0;
    const char *p = s;

    while (*p) {
        char tok[4096];
        size_t tl = 0;
        int inq = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        while (*p && (inq || !isspace((unsigned char)*p))) {
            if (*p == '"') { inq = !inq; p++; continue; }
            if (tl + 1 < sizeof tok) tok[tl++] = *p;
            p++;
        }
        tok[tl] = '\0';
        strv_push(&v, &n, &cap, tok);
    }
    if (n + 1 >= cap) {
        size_t nc = cap ? cap * 2 : 8;
        char **nv = realloc(v, nc * sizeof(char *));
        if (!nv) { *out_n = n; return v; }
        v = nv;
    }
    v[n] = NULL;
    *out_n = n;
    return v;
}

static void strv_free(char **v)
{
    size_t i;
    if (!v) return;
    for (i = 0; v[i]; i++) free(v[i]);
    free(v);
}

/* ------------------------------ 块管理 ------------------------------ */

static blk *blk_new(parser *p, const char **pos, const char **neg,
                    const char *file, int line, int global)
{
    blk *b;
    if (p->nblk + 1 > p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 16;
        blk *nb = realloc(p->blocks, nc * sizeof *nb);
        if (!nb) return NULL;
        p->blocks = nb; p->cap = nc;
    }
    b = &p->blocks[p->nblk++];
    memset(b, 0, sizeof *b);
    {
        size_t i, n;
        n = 0;
        for (i = 0; pos && pos[i]; i++) n++;
        b->pos = calloc(n + 1, sizeof(char *));
        for (i = 0; i < n; i++) b->pos[i] = xstrdup(pos[i]);
        n = 0;
        for (i = 0; neg && neg[i]; i++) n++;
        b->neg = calloc(n + 1, sizeof(char *));
        for (i = 0; i < n; i++) b->neg[i] = xstrdup(neg[i]);
    }
    b->file = file ? xstrdup(file) : NULL;
    b->line = line;
    b->global = global;
    return b;
}

static void ent_push(blk *b, kn_t kn, const char *val,
                     const char *file, int line)
{
    ent *e;
    if (b->nent + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8;
        ent *ne = realloc(b->ents, nc * sizeof *ne);
        if (!ne) return;
        b->ents = ne; b->cap = nc;
    }
    e = &b->ents[b->nent++];
    e->kn = kn;
    e->val = xstrdup(val ? val : "");
    e->file = file ? xstrdup(file) : NULL;
    e->line = line;
}

/* ------------------------------ 展开 ------------------------------ */

static const char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (h && *h) return h;
    {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir) return pw->pw_dir;
    }
    return NULL;
}

static const char *local_user(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) return pw->pw_name;
    {
        const char *u = getenv("USER");
        if (u && *u) return u;
    }
    return "user";
}

/* "~/x" / "~" → HOME；"~u/x" → getpwnam(u)->pw_dir；其余原样 + 警告。 */
static char *tilde_expand(parser *p, const char *s,
                          const char *file, int line)
{
    const char *h;
    if (s[0] != '~')
        return xstrdup(s);
    if (s[1] == '/' || s[1] == '\0') {
        h = home_dir();
        if (!h) return xstrdup(s);
        {
            size_t n = strlen(h) + strlen(s + 1) + 1;
            char *r = malloc(n);
            if (!r) return xstrdup(s);
            snprintf(r, n, "%s%s", h, s + 1);
            return r;
        }
    }
    {
        const char *sl = strchr(s, '/');
        char *un = sl ? xstrndup(s + 1, (size_t)(sl - s - 1)) : xstrdup(s + 1);
        struct passwd *pw = un ? getpwnam(un) : NULL;
        free(un);
        if (!pw) {
            warnf(p, file, line, HB_TR("~user 形式无法展开，已保留原文"));
            return xstrdup(s);
        }
        {
            size_t n = strlen(pw->pw_dir) + (sl ? strlen(sl) : 0) + 1;
            char *r = malloc(n);
            if (!r) return xstrdup(s);
            snprintf(r, n, "%s%s", pw->pw_dir, sl ? sl : "");
            return r;
        }
    }
}

/* %d HOME、%u 本地用户、%h 有效主机名、%r 远端用户（缺省=本地）、%% → %；
 * 其它 %x 保留并警告。 */
static char *expand_tokens(parser *p, const char *s, const char *hostname,
                           const char *ruser, const char *file, int line)
{
    const char *home = home_dir();
    const char *lu = local_user();
    size_t cap = strlen(s) + 64, n = 0;
    char *out = malloc(cap), *w;
    const char *q;
    if (!out) return NULL;
    w = out;
    for (q = s; *q; q++) {
        const char *rep = NULL;
        size_t rl;
        if (*q != '%' || !q[1]) {
            if (n + 2 > cap) { cap *= 2; out = realloc(out, cap); w = out + n; }
            *w++ = *q; n++;
            continue;
        }
        q++;
        switch (*q) {
        case 'd': rep = home; break;
        case 'u': rep = lu; break;
        case 'h': rep = hostname; break;
        case 'r': rep = ruser && *ruser ? ruser : lu; break;
        case '%': rep = "%"; break;
        default:
            warnf(p, file, line,
                  HB_TR("IdentityFile 含未支持的令牌 %%%c，已保留原文"), *q);
            rep = NULL;
            if (n + 3 > cap) { cap *= 2; out = realloc(out, cap); w = out + n; }
            *w++ = '%'; *w++ = *q; n += 2;
            continue;
        }
        if (!rep) rep = "";
        rl = strlen(rep);
        if (n + rl + 2 > cap) {
            while (n + rl + 2 > cap) cap *= 2;
            out = realloc(out, cap); w = out + n;
        }
        memcpy(w, rep, rl); w += rl; n += rl;
    }
    *w = '\0';
    return out;
}

/* ------------------------------ 行解析 ------------------------------ */

static int kw_id(const char *k)   /* 返回枚举或 -1；大小写不敏感 */
{
    static const struct { const char *s; kn_t k; } tab[] = {
        { "hostname",            KN_HOSTNAME },
        { "user",                KN_USER },
        { "port",                KN_PORT },
        { "identityfile",        KN_IDFILE },
        { "proxyjump",           KN_PJUMP },
        { "serveraliveinterval", KN_KALIVE },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcasecmp(tab[i].s, k)) return (int)tab[i].k;
    return -1;
}

static int kw_is(const char *a, const char *b) { return !strcasecmp(a, b); }

static hb_err parse_include(parser *p, char **args, size_t nargs,
                            const char *file, int line);

static hb_err parse_text(parser *p, const char *path, const char *text)
{
    char *copy = xstrdup(text);
    char *save = NULL, *line;
    int lineno = 0;
    hb_err e = HB_OK;
    if (!copy) return HB_ERR_NOMEM;

    for (line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *kstart, *kend, *vstart;
        char key[64];
        size_t kl;
        char **args;
        size_t nargs = 0;
        lineno++;
        if (line[0] && line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = '\0';
        kstart = line;
        while (*kstart && isspace((unsigned char)*kstart)) kstart++;
        if (!*kstart || *kstart == '#')
            continue;
        kend = kstart;
        while (*kend && !isspace((unsigned char)*kend) && *kend != '=')
            kend++;
        kl = (size_t)(kend - kstart);
        if (kl >= sizeof key) kl = sizeof key - 1;
        memcpy(key, kstart, kl); key[kl] = '\0';
        vstart = kend;
        while (*vstart && isspace((unsigned char)*vstart)) vstart++;
        if (*vstart == '=') {
            vstart++;
            while (*vstart && isspace((unsigned char)*vstart)) vstart++;
        }
        args = split_args(vstart, &nargs);

        if (kw_is(key, "host")) {
            const char **pos = NULL, **neg = NULL;
            size_t i;
            blk *b;
            pos = calloc(nargs + 1, sizeof(char *));
            neg = calloc(nargs + 1, sizeof(char *));
            if (pos && neg) {
                size_t np = 0, nn = 0;
                for (i = 0; i < nargs; i++) {
                    if (args[i][0] == '!')
                        neg[nn++] = args[i] + 1;
                    else
                        pos[np++] = args[i];
                }
                b = blk_new(p, pos, neg, path, lineno, 0);
                p->cur = b ? (int)(b - p->blocks) : p->cur;
            }
            free(pos); free(neg);
            p->in_match = 0;
        } else if (kw_is(key, "match")) {
            warnf(p, path, lineno, HB_TR("Match 块暂不支持，已忽略"));
            p->in_match = 1;
        } else if (p->in_match) {
            /* Match 块内一切跳过（含 Include/未知关键字） */
        } else if (kw_is(key, "include")) {
            e = parse_include(p, args, nargs, path, lineno);
        } else {
            int kn = kw_id(key);
            if (kn >= 0 && p->cur >= 0) {
                const char *v = NULL;
                if (kn == KN_PJUMP) {
                    /* ProxyJump 取整行去引号后的剩余部分再逗号切分；
                     * 用 args 拼回单串，容忍 "a, b" 写法。 */
                    size_t i, L = 1;
                    char *j;
                    for (i = 0; i < nargs; i++) L += strlen(args[i]) + 1;
                    j = malloc(L);
                    if (j) {
                        j[0] = '\0';
                        for (i = 0; i < nargs; i++) {
                            if (i) strcat(j, " ");
                            strcat(j, args[i]);
                        }
                        v = j;
                        ent_push(&p->blocks[p->cur], kn, v, path, lineno);
                        free(j);
                        j = NULL;
                    }
                } else if (nargs > 0) {
                    ent_push(&p->blocks[p->cur], kn, args[0], path, lineno);
                }
            } else if (!strcasecmp(key, "proxycommand") ||
                       !strcasecmp(key, "localforward") ||
                       !strcasecmp(key, "remoteforward") ||
                       !strcasecmp(key, "dynamicforward") ||
                       !strcasecmp(key, "certificatefile")) {
                warnf(p, path, lineno, HB_TR("%s 暂不支持，已忽略"), key);
            }
            /* 其它未知关键字静默忽略 */
        }
        {
            size_t i;
            for (i = 0; i < nargs; i++) free(args[i]);
            free(args);
        }
        if (e != HB_OK)
            break;
    }
    free(copy);
    return e;
}

static hb_err parse_file_into(parser *p, const char *path)
{
    int fd;
    struct stat st;
    char *text;
    size_t off = 0;
    hb_err e;

    if (p->depth >= MAX_INCLUDE_DEPTH) {
        warnf(p, path, 0, HB_TR("Include 嵌套过深（>%d），已忽略"),
              MAX_INCLUDE_DEPTH);
        return HB_OK;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        warnf(p, path, 0, HB_TR("无法读取 %s：%s"), path, strerror(errno));
        return HB_OK;             /* 单个 Include 失败不致命 */
    }
    /* glob 可能匹配目录/设备/管道：只读普通文件，限 1 MiB */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        warnf(p, path, 0, HB_TR("不是普通文件，已忽略"));
        close(fd);
        return HB_OK;
    }
    if (st.st_size > 1024 * 1024) {
        warnf(p, path, 0, HB_TR("文件过大（>1 MiB），已忽略"));
        close(fd);
        return HB_OK;
    }
    text = malloc((size_t)st.st_size + 1);
    if (!text) { close(fd); return HB_ERR_NOMEM; }
    while (off < (size_t)st.st_size) {
        ssize_t g = read(fd, text + off, (size_t)st.st_size - off);
        if (g < 0) {
            if (errno == EINTR) continue;
            warnf(p, path, 0, HB_TR("读取失败：%s"), strerror(errno));
            free(text); close(fd);
            return HB_OK;
        }
        if (g == 0) break;
        off += (size_t)g;
    }
    close(fd);
    text[off] = '\0';
    p->depth++;
    e = parse_text(p, path, text);
    p->depth--;
    free(text);
    return e;
}

static hb_err parse_include(parser *p, char **args, size_t nargs,
                            const char *file, int line)
{
    size_t i;
    (void)file;
    for (i = 0; i < nargs; i++) {
        char *ex = tilde_expand(p, args[i], file, line);
        const char *full = ex;
        char *joined = NULL;
        glob_t g;
        size_t gi;
        if (!ex) continue;
        if (ex[0] != '/' && p->base_dir) {
            size_t n = strlen(p->base_dir) + strlen(ex) + 2;
            joined = malloc(n);
            if (joined) snprintf(joined, n, "%s/%s", p->base_dir, ex);
            full = joined ? joined : ex;
        }
        memset(&g, 0, sizeof g);
        if (glob(full, 0, NULL, &g) == 0) {
            for (gi = 0; gi < g.gl_pathc; gi++) {
                hb_err e = parse_file_into(p, g.gl_pathv[gi]);
                if (e != HB_OK) { globfree(&g); free(joined); free(ex); return e; }
            }
        }
        /* 无匹配静默 */
        globfree(&g);
        free(joined);
        free(ex);
    }
    return HB_OK;
}

/* ------------------------------ 有效值 ------------------------------ */

static char *ci_lower(const char *s)
{
    char *r = xstrdup(s), *w;
    if (!r) return NULL;
    for (w = r; *w; w++) *w = (char)tolower((unsigned char)*w);
    return r;
}

static int blk_applies(const blk *b, const char *alias)
{
    size_t i;
    int pos_hit = 0;
    char *la;
    if (b->global) return 1;
    la = ci_lower(alias);
    if (!la) return 0;
    for (i = 0; b->pos && b->pos[i]; i++) {
        char *lp = ci_lower(b->pos[i]);
        int m = lp && fnmatch(lp, la, 0) == 0;
        free(lp);
        if (m) { pos_hit = 1; break; }
    }
    if (!pos_hit) { free(la); return 0; }
    for (i = 0; b->neg && b->neg[i]; i++) {
        char *lp = ci_lower(b->neg[i]);
        int m = lp && fnmatch(lp, la, 0) == 0;
        free(lp);
        if (m) { free(la); return 0; }
    }
    free(la);
    return 1;
}

/* 逗号切分 + 去空白；单元素 "none" → NULL。 */
static char **pj_split(const char *s)
{
    char *copy, *save = NULL, *tok;
    char **v = NULL;
    size_t n = 0, cap = 0;
    copy = xstrdup(s);
    if (!copy) return NULL;
    for (tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        {
            size_t L = strlen(tok);
            while (L && isspace((unsigned char)tok[L - 1])) tok[--L] = '\0';
        }
        if (*tok)
            strv_push(&v, &n, &cap, tok);
    }
    free(copy);
    if (n == 1 && !strcasecmp(v[0], "none")) {
        strv_free(v);
        return NULL;
    }
    {
        char **nv = realloc(v, (n + 1) * sizeof *nv);
        if (nv) { v = nv; v[n] = NULL; }
    }
    return v;
}

static long parse_long(const char *s, int *ok)
{
    char *e;
    long v;
    while (*s && isspace((unsigned char)*s)) s++;
    errno = 0;
    v = strtol(s, &e, 10);
    while (*e && isspace((unsigned char)*e)) e++;
    *ok = !errno && e != s && *e == '\0';
    return v;
}

/* 收集别名列表：文件顺序、非通配、非取反、大小写不敏感去重（先赢）。 */
static void collect_aliases(parser *p, hb_sshcfg *out)
{
    size_t bi, i;
    for (bi = 0; bi < p->nblk; bi++) {
        blk *b = &p->blocks[bi];
        if (b->global) continue;
        for (i = 0; b->pos && b->pos[i]; i++) {
            const char *pat = b->pos[i];
            size_t j;
            int dup = 0;
            if (strchr(pat, '*') || strchr(pat, '?'))
                continue;
            for (j = 0; j < out->n; j++)
                if (!strcasecmp(out->hosts[j].alias, pat)) { dup = 1; break; }
            if (dup) continue;
            {
                hb_sshcfg_host *nh = realloc(out->hosts,
                    (out->n + 1) * sizeof *nh);
                if (!nh) return;
                out->hosts = nh;
                memset(&out->hosts[out->n], 0, sizeof *nh);
                out->hosts[out->n].alias = xstrdup(pat);
                {
                    char src[600];
                    snprintf(src, sizeof src, "%s:%d",
                             b->file ? b->file : "config", b->line);
                    out->hosts[out->n].source = xstrdup(src);
                }
                out->n++;
            }
        }
    }
}

static void resolve_one(parser *p, hb_sshcfg_host *h)
{
    const char *a = h->alias;
    int have_host = 0, have_user = 0, have_port = 0, have_idf = 0,
        have_pj = 0, have_ka = 0;
    char *raw_idf = NULL, *idf_file = NULL;
    int idf_line = 0;
    size_t bi, ei;

    for (bi = 0; bi < p->nblk; bi++) {
        blk *b = &p->blocks[bi];
        if (!blk_applies(b, a)) continue;
        for (ei = 0; ei < b->nent; ei++) {
            ent *e = &b->ents[ei];
            switch (e->kn) {
            case KN_HOSTNAME:
                if (!have_host) { have_host = 1; h->hostname = xstrdup(e->val); }
                break;
            case KN_USER:
                if (!have_user) { have_user = 1; h->user = xstrdup(e->val); }
                break;
            case KN_PORT:
                if (!have_port) {
                    int ok;
                    long v = parse_long(e->val, &ok);
                    have_port = 1;
                    if (!ok || v < 1 || v > 65535)
                        warnf(p, e->file, e->line,
                              HB_TR("Port 无效（'%s'），已忽略"), e->val);
                    else
                        h->port = (int)v;
                }
                break;
            case KN_IDFILE:
                if (!have_idf) {
                    have_idf = 1;
                    raw_idf = tilde_expand(p, e->val, e->file, e->line);
                    idf_file = e->file; idf_line = e->line;
                }
                break;
            case KN_PJUMP:
                if (!have_pj) { have_pj = 1; h->proxy_jump = pj_split(e->val); }
                break;
            case KN_KALIVE:
                if (!have_ka) {
                    int ok;
                    long v = parse_long(e->val, &ok);
                    have_ka = 1;
                    if (!ok || v < 0 || v > 86400)
                        warnf(p, e->file, e->line,
                              HB_TR("ServerAliveInterval 无效（'%s'），已忽略"), e->val);
                    else
                        h->keepalive = (int)v;
                }
                break;
            }
        }
    }
    if (!h->hostname)
        h->hostname = xstrdup(a);
    if (raw_idf) {
        h->identity_file = expand_tokens(p, raw_idf, h->hostname, h->user,
                                         idf_file, idf_line);
        free(raw_idf);
    }
}

/* ------------------------------ 公共入口 ------------------------------ */

static hb_err run_parse(parser *p, hb_sshcfg *out)
{
    size_t i;
    memset(out, 0, sizeof *out);
    collect_aliases(p, out);
    for (i = 0; i < out->n; i++)
        resolve_one(p, &out->hosts[i]);
    out->warnings = p->warn;
    out->nwarn = p->nwarn;
    p->warn = NULL; p->nwarn = 0;
    return HB_OK;
}

static void parser_init(parser *p, const char *base_dir)
{
    static const char *star[] = { "*", NULL };
    memset(p, 0, sizeof *p);
    p->cur = -1;
    p->base_dir = base_dir ? xstrdup(base_dir) : NULL;
    {
        blk *g = blk_new(p, star, NULL, NULL, 0, 1);
        p->cur = g ? 0 : -1;
    }
}

hb_err hb_sshcfg_parse_string(const char *text, const char *base_dir,
                              hb_sshcfg *out)
{
    parser p;
    hb_err e;
    if (!text || !out) return HB_ERR_INVALID;
    parser_init(&p, base_dir);
    e = parse_text(&p, NULL, text);
    if (e == HB_OK)
        e = run_parse(&p, out);
    else
        memset(out, 0, sizeof *out);
    /* 清理 parser（warnings 已移交） */
    {
        size_t i, j;
        for (i = 0; i < p.nblk; i++) {
            blk *b = &p.blocks[i];
            strv_free(b->pos); strv_free(b->neg);
            for (j = 0; j < b->nent; j++) {
                free(b->ents[j].val); free(b->ents[j].file);
            }
            free(b->ents); free(b->file);
        }
        free(p.blocks);
        hb_strfreev(p.warn, p.nwarn);
        free(p.base_dir);
    }
    return e;
}

hb_err hb_sshcfg_parse_file(const char *path, hb_sshcfg *out)
{
    FILE *f;
    char *text, *dir;
    long len;
    const char *sl;
    hb_err e;

    if (!path || !out) return HB_ERR_INVALID;
    f = fopen(path, "rb");
    if (!f) return HB_ERR_IO;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = malloc((size_t)len + 1);
    if (!text) { fclose(f); return HB_ERR_NOMEM; }
    if (fread(text, 1, (size_t)len, f) != (size_t)len && len > 0) {
        free(text); fclose(f);
        return HB_ERR_IO;
    }
    text[len] = '\0';
    fclose(f);
    /* base_dir = 文件所在目录 */
    sl = strrchr(path, '/');
    dir = sl ? xstrndup(path, (size_t)(sl - path)) : xstrdup(".");
    e = hb_sshcfg_parse_string(text, dir, out);
    free(dir);
    free(text);
    return e;
}

void hb_sshcfg_free(hb_sshcfg *c)
{
    size_t i;
    if (!c) return;
    for (i = 0; i < c->n; i++) {
        hb_sshcfg_host *h = &c->hosts[i];
        free(h->alias); free(h->hostname); free(h->user);
        free(h->identity_file); free(h->source);
        strv_free(h->proxy_jump);
    }
    free(c->hosts);
    hb_strfreev(c->warnings, c->nwarn);
    memset(c, 0, sizeof *c);
}
