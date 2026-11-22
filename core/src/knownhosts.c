/* FR-09：OpenSSH known_hosts → vault "known" 对象。
 *
 * 行格式：[marker] hosts keytype base64 [comment]
 *   - @cert-authority / @revoked 等 marker 行：暂不支持，警告跳过。
 *   - hosts 为逗号列表；含 * ? ! 的通配/取反项跳过并警告
 *    （libssh2 匹配不了），其余名字照常导入。
 *   - 普通名 → (host, port=22)；[h]:p → (h, p)；|1|salt|hash 哈希名
 *     → host=token、port=0（libssh2 连接时自行匹配）。
 *   - 去重键 (host,port,keytype,key)：对 vault 已有对象与本文件内都判重；
 *     同 (host,port,keytype) 不同 key → 跳过并告警冲突，绝不静默覆盖。
 */

#include "harbor/knownhosts.h"
#include "harbor/ssh.h"

#include <sodium.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KH_MAX_FILE (8u * 1024 * 1024)

typedef struct {
    hb_vault *v;
    hb_import_result *r;
    char **dk;  size_t ndk;   /* 去重键 "host\fport\fkeytype\fkey"（已有+新导入） */
    char **ck;  size_t nck;   /* 冲突键 "host\fport\fkeytype" */
} imp_t;

static void warn(hb_import_result *r, const char *fmt, ...)
{
    char buf[512];
    char **nw;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    nw = realloc(r->warnings, (r->nwarn + 1) * sizeof *nw);
    if (!nw)
        return;
    r->warnings = nw;
    r->warnings[r->nwarn++] = strdup(buf);
}

static void r_id(hb_import_result *r, const char *id)
{
    char **nw = realloc(r->created_ids, (r->ncreated + 1) * sizeof *nw);
    if (!nw)
        return;
    r->created_ids = nw;
    r->created_ids[r->ncreated++] = strdup(id);
}

/* \f 不会出现在任一字段里 */
static char *dup_key(const char *host, int port,
                     const char *keytype, const char *key)
{
    char *s;
    size_t n = strlen(host) + strlen(keytype) + strlen(key) + 32;
    s = malloc(n);
    if (s)
        snprintf(s, n, "%s\f%d\f%s\f%s", host, port, keytype, key);
    return s;
}

static char *ck_key(const char *host, int port, const char *keytype)
{
    char *s;
    size_t n = strlen(host) + strlen(keytype) + 32;
    s = malloc(n);
    if (s)
        snprintf(s, n, "%s\f%d\f%s", host, port, keytype);
    return s;
}

static int strv_has(char **v, size_t n, const char *s)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (v[i] && !strcmp(v[i], s))
            return 1;
    return 0;
}

static void strv_push(char ***v, size_t *n, char *s)
{
    char **nw;
    if (!s)
        return;
    nw = realloc(*v, (*n + 1) * sizeof *nw);
    if (!nw) {
        free(s);
        return;
    }
    *v = nw;
    (*v)[(*n)++] = s;
}

static void strv_free(char **v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

/* 加载 vault 已有 known 的两类键 */
static void load_existing(imp_t *im)
{
    hb_obj **objs = NULL;
    size_t n = 0, i;
    if (hb_vault_list(im->v, "known", &objs, &n) != HB_OK) {
        warn(im->r, HB_TR("无法读取现有 known 记录，按空处理"));
        return;
    }
    for (i = 0; i < n; i++) {
        const char *h = hb_obj_get_str(objs[i], "host");
        const char *kt = hb_obj_get_str(objs[i], "keytype");
        const char *k = hb_obj_get_str(objs[i], "key");
        int p = (int)hb_obj_get_int(objs[i], "port", 22);
        if (!h || !kt || !k)
            continue;
        strv_push(&im->dk, &im->ndk, dup_key(h, p, kt, k));
        strv_push(&im->ck, &im->nck, ck_key(h, p, kt));
    }
    hb_obj_list_free(objs, n);
}

/* 解析单个 hosts 列表项 → host/port；pattern/畸形 → false（可能带警告）。 */
static int parse_name(const char *tok, char **host_out, int *port_out,
                      hb_import_result *r, int lineno)
{
    size_t l = strlen(tok);
    if (!l)
        return 0;
    if (tok[0] == '|') {                       /* 哈希条目 */
        *host_out = strdup(tok);
        *port_out = 0;
        return *host_out != NULL;
    }
    if (strpbrk(tok, "*?!")) {
        warn(r, HB_TR("第 %d 行：跳过通配/取反模式 '%s'（libssh2 无法匹配）"),
             lineno, tok);
        return 0;
    }
    if (tok[0] == '[') {                       /* [host]:port */
        const char *rb = strchr(tok, ']');
        if (rb && rb[1] == ':' && rb + 2 < tok + l) {
            *host_out = strndup(tok + 1, (size_t)(rb - tok - 1));
            *port_out = atoi(rb + 2);
            return *host_out != NULL;
        }
        warn(r, HB_TR("第 %d 行：无法解析 '%s'"), lineno, tok);
        return 0;
    }
    *host_out = strdup(tok);
    *port_out = 22;
    return *host_out != NULL;
}

/* 重建该条目的标准 known_hosts 行（哈希名与 port≠22 原格式保留） */
static char *make_line(const char *host, int port,
                       const char *keytype, const char *key)
{
    char *s;
    size_t n = strlen(host) + strlen(keytype) + strlen(key) + 32;
    s = malloc(n);
    if (!s)
        return NULL;
    if (port == 22 || host[0] == '|')
        snprintf(s, n, "%s %s %s", host, keytype, key);
    else
        snprintf(s, n, "[%s]:%d %s %s", host, port, keytype, key);
    return s;
}

/* base64 解码成功 → 算 SHA256 指纹（malloc，调用方 free；失败返回 0） */
static int b64_key_ok(const char *b64, char **fp_out)
{
    unsigned char bin[8192];
    size_t blen = 0;
    const char *end = NULL;
    if (sodium_base642bin(bin, sizeof bin, b64, strlen(b64),
                          NULL, &blen, &end,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
        return 0;
    if (end && *end)                           /* 尾部垃圾 → 坏行 */
        return 0;
    if (!blen)
        return 0;
    *fp_out = hb_ssh_fingerprint_sha256(bin, blen);
    return *fp_out != NULL;
}

static void import_one(imp_t *im, const char *host, int port,
                       const char *keytype, const char *b64,
                       const char *fp, int lineno)
{
    char *dk, *ck, *line;
    hb_obj *o;

    dk = dup_key(host, port, keytype, b64);
    ck = ck_key(host, port, keytype);
    if (!dk || !ck) {
        free(dk); free(ck);
        return;
    }
    if (strv_has(im->dk, im->ndk, dk)) {       /* 完全重复 */
        im->r->skipped++;
        free(dk); free(ck);
        return;
    }
    if (strv_has(im->ck, im->nck, ck)) {       /* 同键不同 key → 冲突 */
        warn(im->r,
             HB_TR("第 %d 行：%s:%d 的 %s 与已有记录冲突（可能已更换主机密钥），已跳过"),
             lineno, host, port, keytype);
        im->r->skipped++;
        free(dk); free(ck);
        return;
    }
    line = make_line(host, port, keytype, b64);
    o = hb_obj_new("known");
    if (!o || !line) {
        free(dk); free(ck); free(line);
        if (o) hb_obj_free(o);
        return;
    }
    hb_obj_set_str(o, "host", host);
    hb_obj_set_int(o, "port", port);
    hb_obj_set_str(o, "keytype", keytype);
    hb_obj_set_str(o, "key", b64);
    hb_obj_set_str(o, "fingerprint", fp ? fp : "");
    hb_obj_set_int(o, "added", hb_now_ms());
    hb_obj_set_str(o, "line", line);
    if (hb_vault_put(im->v, o) == HB_OK) {
        im->r->created++;
        r_id(im->r, hb_obj_id(o));
        strv_push(&im->dk, &im->ndk, dk);
        strv_push(&im->ck, &im->nck, ck);
        dk = ck = NULL;
    }
    hb_obj_free(o);
    free(dk); free(ck); free(line);
}

static void import_line(imp_t *im, char *line, int lineno)
{
    char *hosts, *keytype, *b64, *p, *save = NULL;
    char *fp = NULL;

    p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#')
        return;
    if (*p == '@') {                          /* @cert-authority / @revoked … */
        char m[64];
        size_t ml = strcspn(p, " \t");
        if (ml >= sizeof m) ml = sizeof m - 1;
        memcpy(m, p, ml); m[ml] = 0;
        warn(im->r, HB_TR("第 %d 行：%s 条目暂不支持，已跳过"), lineno, m);
        return;
    }

    hosts = p;
    p += strcspn(p, " \t");
    if (*p) *p++ = 0;
    while (*p == ' ' || *p == '\t') p++;
    keytype = p;
    p += strcspn(p, " \t");
    if (*p) *p++ = 0;
    while (*p == ' ' || *p == '\t') p++;
    b64 = p;
    p += strcspn(p, " \t\r\n");
    if (*p) *p = 0;                           /* comment 丢弃 */

    if (!*hosts || !*keytype || !*b64) {
        warn(im->r, HB_TR("第 %d 行：字段不足，已跳过"), lineno);
        return;
    }
    if (!b64_key_ok(b64, &fp)) {
        warn(im->r, HB_TR("第 %d 行：base64 密钥解码失败，已跳过"), lineno);
        return;
    }

    for (p = strtok_r(hosts, ",", &save); p;
         p = strtok_r(NULL, ",", &save)) {
        char *host = NULL;
        int port = 22;
        if (parse_name(p, &host, &port, im->r, lineno)) {
            import_one(im, host, port, keytype, b64, fp, lineno);
            free(host);
        }
    }
    free(fp);
}

hb_err hb_known_import_text(hb_vault *v, const char *text,
                            hb_import_result *r)
{
    imp_t im;
    char *copy, *line, *save = NULL;
    int lineno = 0;

    if (!v || !text || !r)
        return HB_ERR_INVALID;
    memset(&im, 0, sizeof im);
    im.v = v;
    im.r = r;
    load_existing(&im);
    copy = strdup(text);
    if (!copy)
        return HB_ERR_NOMEM;
    for (line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        import_line(&im, line, lineno);
    }
    free(copy);
    strv_free(im.dk, im.ndk);
    strv_free(im.ck, im.nck);
    return HB_OK;
}

hb_err hb_known_import_file(hb_vault *v, const char *path,
                            hb_import_result *r)
{
    struct stat stt;
    FILE *f;
    char *buf;
    size_t n;
    hb_err e;

    if (!v || !path || !r)
        return HB_ERR_INVALID;
    if (stat(path, &stt) != 0)
        return HB_ERR_IO;
    if (!S_ISREG(stt.st_mode) || stt.st_size < 0 ||
        (uint64_t)stt.st_size > KH_MAX_FILE)
        return HB_ERR_INVALID;
    f = fopen(path, "rb");
    if (!f)
        return HB_ERR_IO;
    buf = malloc((size_t)stt.st_size + 1);
    if (!buf) {
        fclose(f);
        return HB_ERR_NOMEM;
    }
    n = fread(buf, 1, (size_t)stt.st_size, f);
    fclose(f);
    buf[n] = 0;
    e = hb_known_import_text(v, buf, r);
    free(buf);
    return e;
}

hb_err hb_known_export_text(hb_vault *v, char **out)
{
    hb_obj **objs = NULL;
    size_t n = 0, i, len = 0, cap = 0;
    char *buf = NULL;

    if (!v || !out)
        return HB_ERR_INVALID;
    *out = NULL;
    if (hb_vault_list(v, "known", &objs, &n) != HB_OK)
        return HB_ERR_DB;
    for (i = 0; i < n; i++) {
        const char *line = hb_obj_get_str(objs[i], "line");
        char *mk = NULL;
        size_t l;
        if (!line || !*line) {                 /* 无原始行 → 按字段重建 */
            const char *h = hb_obj_get_str(objs[i], "host");
            const char *kt = hb_obj_get_str(objs[i], "keytype");
            const char *k = hb_obj_get_str(objs[i], "key");
            int port = (int)hb_obj_get_int(objs[i], "port", 22);
            if (h && kt && k)
                mk = make_line(h, port, kt, k);
            line = mk;
        }
        if (line && *line) {
            l = strlen(line);
            if (len + l + 2 > cap) {
                size_t nc = cap ? cap * 2 : 1024;
                char *nb;
                while (len + l + 2 > nc) nc *= 2;
                nb = realloc(buf, nc);
                if (!nb) {
                    free(mk);
                    hb_obj_list_free(objs, n);
                    free(buf);
                    return HB_ERR_NOMEM;
                }
                buf = nb; cap = nc;
            }
            memcpy(buf + len, line, l);
            len += l;
            buf[len++] = '\n';
        }
        free(mk);
    }
    hb_obj_list_free(objs, n);
    if (!buf)
        buf = strdup("");
    else
        buf[len] = 0;
    *out = buf;
    return HB_OK;
}
