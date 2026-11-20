#include "harbor/keys.h"
#include "harbor/ssh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <sodium.h>

/* ------------------------------ wire 写缓冲 ------------------------------ */

typedef struct { unsigned char *b; size_t len, cap; } wbuf;

static int wb_need(wbuf *w, size_t n)
{
    unsigned char *p;
    size_t nc;
    if (w->len + n <= w->cap)
        return 1;
    nc = w->cap ? w->cap * 2 : 256;
    while (nc < w->len + n)
        nc *= 2;
    p = realloc(w->b, nc);
    if (!p)
        return 0;
    w->b = p;
    w->cap = nc;
    return 1;
}
static int wb_put(wbuf *w, const void *p, size_t n)
{
    if (!wb_need(w, n)) return 0;
    memcpy(w->b + w->len, p, n);
    w->len += n;
    return 1;
}
static int wb_u32(wbuf *w, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                           (unsigned char)(v >> 8), (unsigned char)v };
    return wb_put(w, b, 4);
}
static int wb_str(wbuf *w, const void *p, size_t n)
{ return wb_u32(w, (uint32_t)n) && wb_put(w, p, n); }
static int wb_cstr(wbuf *w, const char *s)
{ return wb_str(w, s, strlen(s)); }
/* mpint：二进制补码；零 → 空串；最高位置位时补 0x00 */
static int wb_bn(wbuf *w, const BIGNUM *bn)
{
    unsigned char tmp[2048];
    int n = BN_num_bytes(bn), i = 0;
    if (n > (int)sizeof tmp || BN_bn2bin(bn, tmp) != n)
        return 0;
    while (i < n && tmp[i] == 0)
        i++;
    if (i == n)
        return wb_str(w, "", 0);
    if (tmp[i] & 0x80) {
        unsigned char z = 0;
        if (!wb_u32(w, (uint32_t)(n - i + 1)) || !wb_put(w, &z, 1))
            return 0;
        return wb_put(w, tmp + i, (size_t)(n - i));
    }
    return wb_str(w, tmp + i, (size_t)(n - i));
}
static void wb_free(wbuf *w, int secret)
{
    if (!w->b) return;
    if (secret) OPENSSL_cleanse(w->b, w->cap);
    free(w->b);
    w->b = NULL; w->len = w->cap = 0;
}

/* ------------------------------ wire 读游标 ------------------------------ */

typedef struct { const unsigned char *p; size_t len; int err; } rbuf;

static int rb_get(rbuf *r, const unsigned char **p, size_t n)
{
    if (r->len < n) { r->err = 1; return 0; }
    *p = r->p;
    r->p += n; r->len -= n;
    return 1;
}
static int rb_u32(rbuf *r, uint32_t *v)
{
    const unsigned char *p;
    if (!rb_get(r, &p, 4)) return 0;
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
    return 1;
}
static int rb_str(rbuf *r, const unsigned char **p, size_t *n)
{
    uint32_t l;
    if (!rb_u32(r, &l)) return 0;
    *n = l;
    return rb_get(r, p, l);
}
static int rb_skip_str(rbuf *r)
{
    const unsigned char *p; size_t n;
    return rb_str(r, &p, &n);
}

/* ------------------------------ base64 / PEM ------------------------------ */

static char *b64_enc(const unsigned char *d, size_t n)
{
    size_t cap = sodium_base64_ENCODED_LEN(n, sodium_base64_VARIANT_ORIGINAL);
    char *s = malloc(cap);
    if (!s) return NULL;
    sodium_bin2base64(s, cap, d, n, sodium_base64_VARIANT_ORIGINAL);
    return s;
}

/* "-----BEGIN T-----\n" + 70 列 base64 + "-----END T-----\n" */
static char *pem_wrap(const char *type, const unsigned char *der, size_t n)
{
    char *b64 = b64_enc(der, n), *out, *o;
    size_t tl, i, lines;
    if (!b64) return NULL;
    tl = strlen(b64);
    lines = (tl + 69) / 70;
    out = malloc(strlen(type) * 2 + tl + lines + 80);
    if (!out) { free(b64); return NULL; }
    o = out + sprintf(out, "-----BEGIN %s-----\n", type);
    for (i = 0; i < tl; i += 70) {
        size_t c = tl - i < 70 ? tl - i : 70;
        memcpy(o, b64 + i, c);
        o += c;
        *o++ = '\n';
    }
    o += sprintf(o, "-----END %s-----\n", type);
    *o = 0;
    free(b64);
    return out;
}

/* 二进制子串查找 */
static const unsigned char *find_lit(const unsigned char *d, size_t n,
                                     const char *lit)
{
    size_t l = strlen(lit), i;
    if (l > n) return NULL;
    for (i = 0; i + l <= n; i++)
        if (!memcmp(d + i, lit, l)) return d + i;
    return NULL;
}

/* ------------------------- EVP_PKEY → SSH wire ------------------------- */

/* 公钥 blob + （可选）私钥类型字段。alg 写入调用方的 algbuf（>=32 字节）。 */
static hb_err wire_from_pkey(EVP_PKEY *pk, wbuf *pub, wbuf *privf,
                             char *algbuf, const char **kind, int *bits)
{
    int id = EVP_PKEY_base_id(pk);

    if (id == EVP_PKEY_ED25519) {
        unsigned char pb[32], sk[64];
        size_t pl = sizeof pb, sl = 32;
        if (EVP_PKEY_get_raw_public_key(pk, pb, &pl) != 1 || pl != 32 ||
            EVP_PKEY_get_raw_private_key(pk, sk, &sl) != 1 || sl != 32) {
            OPENSSL_cleanse(sk, sizeof sk);
            return HB_ERR_CRYPTO;
        }
        memcpy(sk + 32, pb, 32);                 /* OpenSSH: seed‖pub */
        strcpy(algbuf, "ssh-ed25519");
        *kind = "ed25519"; *bits = 256;
        if (!wb_cstr(pub, algbuf) || !wb_str(pub, pb, 32))
            return HB_ERR_NOMEM;
        if (privf && (!wb_cstr(privf, algbuf) || !wb_str(privf, pb, 32) ||
                      !wb_str(privf, sk, 64))) {
            OPENSSL_cleanse(sk, sizeof sk);
            return HB_ERR_NOMEM;
        }
        OPENSSL_cleanse(sk, sizeof sk);
        return HB_OK;
    }
    if (id == EVP_PKEY_EC) {
        char grp[64] = {0};
        size_t gl = 0, ql = 0;
        unsigned char q[200];
        const char *curve = NULL;
        BIGNUM *d = NULL;
        hb_err e;

        if (EVP_PKEY_get_utf8_string_param(pk, OSSL_PKEY_PARAM_GROUP_NAME,
                                           grp, sizeof grp, &gl) != 1)
            return HB_ERR_CRYPTO;
        if (!strcmp(grp, "P-256") || !strcmp(grp, "prime256v1") ||
            !strcmp(grp, "secp256r1")) {
            curve = "nistp256"; *bits = 256;
        } else if (!strcmp(grp, "P-384") || !strcmp(grp, "secp384r1")) {
            curve = "nistp384"; *bits = 384;
        } else if (!strcmp(grp, "P-521") || !strcmp(grp, "secp521r1")) {
            curve = "nistp521"; *bits = 521;
        } else {
            return HB_ERR_INVALID;               /* 只支持 NIST P-曲线 */
        }
        if (EVP_PKEY_get_octet_string_param(pk, OSSL_PKEY_PARAM_PUB_KEY,
                                            q, sizeof q, &ql) != 1)
            return HB_ERR_CRYPTO;
        *kind = "ecdsa";
        snprintf(algbuf, 32, "ecdsa-sha2-%s", curve);
        if (!wb_cstr(pub, algbuf) || !wb_cstr(pub, curve) ||
            !wb_str(pub, q, ql))
            return HB_ERR_NOMEM;
        if (privf) {
            if (EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_PRIV_KEY,
                                      &d) != 1)
                return HB_ERR_CRYPTO;
            e = (!wb_cstr(privf, algbuf) || !wb_cstr(privf, curve) ||
                 !wb_str(privf, q, ql) || !wb_bn(privf, d))
                    ? HB_ERR_NOMEM : HB_OK;
            BN_clear_free(d);
            return e;
        }
        return HB_OK;
    }
    if (id == EVP_PKEY_RSA) {
        BIGNUM *n = NULL, *e = NULL, *d = NULL, *p = NULL, *q = NULL,
               *iqmp = NULL;
        hb_err re;

        if (EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_N, &n) != 1 ||
            EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_E, &e) != 1)
            return HB_ERR_CRYPTO;
        strcpy(algbuf, "ssh-rsa");
        *kind = "rsa"; *bits = BN_num_bits(n);
        if (!wb_cstr(pub, algbuf) || !wb_bn(pub, e) || !wb_bn(pub, n))
            re = HB_ERR_NOMEM;
        else
            re = HB_OK;
        if (re == HB_OK && privf) {
            if (EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_D, &d) != 1 ||
                EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_COEFFICIENT1,
                                      &iqmp) != 1 ||
                EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_FACTOR1, &p) != 1 ||
                EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_FACTOR2, &q) != 1)
                re = HB_ERR_CRYPTO;
            /* OpenSSH 私钥顺序：n, e, d, iqmp, p, q */
            else if (!wb_cstr(privf, algbuf) || !wb_bn(privf, n) ||
                     !wb_bn(privf, e) || !wb_bn(privf, d) ||
                     !wb_bn(privf, iqmp) || !wb_bn(privf, p) ||
                     !wb_bn(privf, q))
                re = HB_ERR_NOMEM;
        }
        BN_free(n); BN_free(e);
        BN_clear_free(d); BN_clear_free(iqmp);
        BN_clear_free(p); BN_clear_free(q);
        return re;
    }
    return HB_ERR_INVALID;                        /* DSA 等不支持 */
}

/* --------------------- openssh-key-v1 装配（无加密） --------------------- */

static hb_err write_v1(const wbuf *pubblob, const wbuf *privf,
                       const char *comment, char **pem_out)
{
    wbuf all = {0}, sec = {0};
    uint32_t check;
    int pad;
    hb_err e = HB_ERR_NOMEM;

    randombytes_buf(&check, sizeof check);
    if (!wb_put(&all, "openssh-key-v1\0", 15) ||
        !wb_str(&all, "none", 4) ||           /* ciphername */
        !wb_str(&all, "none", 4) ||           /* kdfname */
        !wb_str(&all, "", 0) ||               /* kdfoptions */
        !wb_u32(&all, 1) ||                   /* nkeys */
        !wb_str(&all, pubblob->b, pubblob->len))
        goto out;
    if (!wb_u32(&sec, check) || !wb_u32(&sec, check) ||
        !wb_put(&sec, privf->b, privf->len) ||
        !wb_cstr(&sec, comment ? comment : ""))
        goto out;
    pad = 1;                                   /* 填充 1,2,3… 到 8 倍数 */
    while (sec.len % 8) {
        unsigned char c = (unsigned char)pad++;
        if (!wb_put(&sec, &c, 1))
            goto out;
    }
    if (!wb_str(&all, sec.b, sec.len))
        goto out;
    *pem_out = pem_wrap("OPENSSH PRIVATE KEY", all.b, all.len);
    e = *pem_out ? HB_OK : HB_ERR_NOMEM;
out:
    wb_free(&all, 0);
    wb_free(&sec, 1);
    return e;
}

/* kind/bits/public_line/fingerprint/comment 一次填齐 */
static hb_err fill_info(hb_key_info *o, const char *alg, const char *kind,
                        int bits, const unsigned char *blob, size_t blen,
                        const char *comment)
{
    char *b64 = b64_enc(blob, blen);
    if (!b64) return HB_ERR_NOMEM;
    {
        int has_c = comment && *comment;
        size_t need = strlen(alg) + strlen(b64) +
                      (has_c ? strlen(comment) : 0) + 3;
        o->public_line = malloc(need);
        if (o->public_line)
            snprintf(o->public_line, need, "%s %s%s%s", alg, b64,
                     has_c ? " " : "", has_c ? comment : "");
    }
    free(b64);
    o->kind = strdup(kind);
    o->fingerprint = hb_ssh_fingerprint_sha256(blob, blen);
    o->comment = comment && *comment ? strdup(comment) : NULL;
    o->bits = bits;
    if (!o->kind || !o->public_line || !o->fingerprint ||
        (comment && *comment && !o->comment))
        return HB_ERR_NOMEM;
    return HB_OK;
}

/* 把原始私钥文本存进 sodium 锁页内存 */
static char *secret_copy(const unsigned char *d, size_t n)
{
    char *s = sodium_malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, d, n);
    s[n] = 0;
    return s;
}

/* ------------------------------ 生成 ------------------------------ */

hb_err hb_key_generate(hb_key_type t, int rsa_bits, const char *comment,
                       hb_key_info *out)
{
    EVP_PKEY_CTX *ctx = NULL;
    EVP_PKEY *pk = NULL;
    wbuf pub = {0}, privf = {0};
    const char *kind = NULL;
    char alg[40];
    char *pem = NULL;
    int bits = 0;
    hb_err e = HB_ERR_CRYPTO;

    if (!out) return HB_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (hb_core_init() != 0) return HB_ERR_CRYPTO;

    switch (t) {
    case HB_KEY_ED25519:
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
        break;
    case HB_KEY_ECDSA_P256: case HB_KEY_ECDSA_P384: case HB_KEY_ECDSA_P521: {
        const char *g = t == HB_KEY_ECDSA_P256 ? "P-256"
                      : t == HB_KEY_ECDSA_P384 ? "P-384" : "P-521";
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
        if (ctx && (EVP_PKEY_keygen_init(ctx) <= 0 ||
                    EVP_PKEY_CTX_set_group_name(ctx, g) <= 0))
            goto out;
        break;
    }
    case HB_KEY_RSA:
        if (rsa_bits != 3072 && rsa_bits != 4096)
            return HB_ERR_INVALID;
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
        if (ctx && (EVP_PKEY_keygen_init(ctx) <= 0 ||
                    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, rsa_bits) <= 0))
            goto out;
        break;
    default:
        return HB_ERR_INVALID;
    }
    if (!ctx) return HB_ERR_CRYPTO;
    if (t == HB_KEY_ED25519 && EVP_PKEY_keygen_init(ctx) <= 0)
        goto out;
    if (EVP_PKEY_keygen(ctx, &pk) <= 0)
        goto out;

    e = wire_from_pkey(pk, &pub, &privf, alg, &kind, &bits);
    if (e != HB_OK)
        goto out;
    e = write_v1(&pub, &privf, comment, &pem);
    if (e != HB_OK)
        goto out;
    e = fill_info(out, alg, kind, bits, pub.b, pub.len, comment);
    if (e != HB_OK)
        goto out;
    out->private_text = hb_secret_strdup(pem);
    out->format = HB_KEYFMT_OPENSSH;
    out->encrypted = false;
    if (!out->private_text)
        e = HB_ERR_NOMEM;
out:
    if (e != HB_OK)
        hb_key_info_clear(out);
    if (pem) {
        OPENSSL_cleanse(pem, strlen(pem));
        free(pem);
    }
    wb_free(&pub, 0);
    wb_free(&privf, 1);
    EVP_PKEY_free(pk);
    EVP_PKEY_CTX_free(ctx);
    return e;
}

/* ------------------------------ 公钥 blob 解析 ------------------------------ */

/* blob 首字段 = alg（写入 algbuf，>=40 字节）；返回 kind/bits
 * （RSA 位从 mpint n 数出）。 */
static hb_err blob_info(const unsigned char *blob, size_t blen,
                        char *algbuf, const char **kind, int *bits)
{
    rbuf r = { blob, blen, 0 };
    const unsigned char *a;
    size_t al;

    if (!rb_str(&r, &a, &al) || al >= 40)
        return HB_ERR_INVALID;
    memcpy(algbuf, a, al);
    algbuf[al] = 0;

    if (!strcmp(algbuf, "ssh-ed25519")) {
        *kind = "ed25519"; *bits = 256;
    } else if (!strncmp(algbuf, "ecdsa-sha2-nistp", 16)) {
        int b = atoi(algbuf + 16);
        if (b != 256 && b != 384 && b != 521)
            return HB_ERR_INVALID;
        *kind = "ecdsa"; *bits = b;
    } else if (!strcmp(algbuf, "ssh-rsa")) {
        const unsigned char *mp;
        size_t ml;
        *kind = "rsa";
        if (!rb_skip_str(&r))                 /* e */
            return HB_ERR_INVALID;
        if (!rb_str(&r, &mp, &ml))            /* n（mpint） */
            return HB_ERR_INVALID;
        while (ml && !*mp) { mp++; ml--; }    /* 去符号填充 0x00 */
        *bits = 0;
        if (ml) {
            int lb = 0;
            unsigned char c = mp[0];
            while (c) { lb++; c >>= 1; }
            *bits = (int)(ml - 1) * 8 + lb;
        }
    } else {
        return HB_ERR_INVALID;                /* ssh-dss 等 */
    }
    return HB_OK;
}

/* ------------------------------ inspect: OpenSSH v1 ------------------------------ */

static hb_err inspect_openssh(const unsigned char *data, size_t len,
                            hb_key_info *out)
{
    static const char MB[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
    static const char ME[] = "-----END OPENSSH PRIVATE KEY-----";
    const unsigned char *b, *e;
    unsigned char *b64t = NULL, *der = NULL;
    size_t b64l = 0, derl = 0, i;
    rbuf r;
    const unsigned char *cipher = NULL, *pubb = NULL, *sec = NULL;
    size_t cl = 0, pl = 0, sl = 0;
    const char *kind = NULL;
    char alg[40];
    int bits = 0, skip;
    hb_err re = HB_ERR_INVALID;

    b = find_lit(data, len, MB);
    if (!b) return HB_ERR_INVALID;
    b += sizeof MB - 1;
    e = find_lit(b, len - (size_t)(b - data), ME);
    if (!e) return HB_ERR_INVALID;

    /* 收 base64（跳过空白与 PEM 头行，v1 没有头行） */
    b64t = malloc((size_t)(e - b) + 1);
    if (!b64t) return HB_ERR_NOMEM;
    for (i = 0; b + i < e; i++)
        if (b[i] != ' ' && b[i] != '\t' && b[i] != '\r' && b[i] != '\n')
            b64t[b64l++] = b[i];
    der = malloc(b64l + 4);
    if (!der || sodium_base642bin(der, b64l + 4, (const char *)b64t, b64l,
                                  NULL, &derl, NULL,
                                  sodium_base64_VARIANT_ORIGINAL) != 0)
        goto out;

    r.p = der; r.len = derl; r.err = 0;
    {
        const unsigned char *m;
        uint32_t nkeys = 0;
        if (!rb_get(&r, &m, 15) || memcmp(m, "openssh-key-v1\0", 15))
            goto out;
        if (!rb_str(&r, &cipher, &cl) ||          /* ciphername */
            !rb_skip_str(&r) ||                   /* kdfname */
            !rb_skip_str(&r) ||                   /* kdfoptions */
            !rb_u32(&r, &nkeys) || nkeys != 1 ||
            !rb_str(&r, &pubb, &pl))              /* 公钥 blob */
            goto out;
    }
    if (blob_info(pubb, pl, alg, &kind, &bits) != HB_OK)
        goto out;
    if (!rb_str(&r, &sec, &sl))                   /* 私钥段 */
        goto out;
    if (r.err) goto out;

    if (fill_info(out, alg, kind, bits, pubb, pl, NULL) != HB_OK) {
        re = HB_ERR_NOMEM;
        goto out;
    }
    out->encrypted = !(cl == 4 && !memcmp(cipher, "none", 4));
    out->format = HB_KEYFMT_OPENSSH;

    /* 未加密：私钥段里取 comment（checkint 校验），并补进 public_line */
    if (!out->encrypted) {
        rbuf s2 = { sec, sl, 0 };
        uint32_t c1, c2;
        if (rb_u32(&s2, &c1) && rb_u32(&s2, &c2) && c1 == c2) {
            skip = !strcmp(alg, "ssh-ed25519") ? 2
                 : !strncmp(alg, "ecdsa-", 6)     ? 3 : 6;
            if (rb_skip_str(&s2)) {               /* 类型串（与 alg 相同） */
                int ok = 1, j;
                const unsigned char *cm;
                size_t cml = 0;
                for (j = 0; j < skip && ok; j++)
                    ok = rb_skip_str(&s2);
                if (ok && rb_str(&s2, &cm, &cml) && cml) {
                    char *nl = malloc(strlen(out->public_line) + cml + 2);
                    if (nl) {
                        sprintf(nl, "%s %.*s", out->public_line,
                                (int)cml, (const char *)cm);
                        free(out->public_line);
                        out->public_line = nl;
                    }
                    out->comment = strndup((const char *)cm, cml);
                }
            }
        }
    }
    out->private_text = secret_copy(data, len);
    re = out->private_text ? HB_OK : HB_ERR_NOMEM;
out:
    if (b64t) OPENSSL_cleanse(b64t, b64l);
    if (der) OPENSSL_cleanse(der, derl);
    free(b64t);
    free(der);
    if (re != HB_OK)
        hb_key_info_clear(out);
    return re;
}

/* ------------------------------ inspect: PEM / PKCS8 ------------------------------ */

static int pw_cb(char *buf, int size, int rwflag, void *ud)
{
    const char *pw = ud;
    size_t n;
    (void)rwflag;
    if (!pw) return 0;
    n = strlen(pw);
    if ((int)n > size) n = (size_t)size;
    memcpy(buf, pw, n);
    return (int)n;
}

static hb_err inspect_pem(const unsigned char *data, size_t len,
                          const char *passphrase, hb_key_info *out)
{
    int trad_rsa = find_lit(data, len, "BEGIN RSA PRIVATE KEY") != NULL;
    int trad_ec  = find_lit(data, len, "BEGIN EC PRIVATE KEY") != NULL;
    int pkcs8e   = find_lit(data, len,
                            "BEGIN ENCRYPTED PRIVATE KEY") != NULL;
    int pkcs8    = !pkcs8e &&
                   find_lit(data, len, "BEGIN PRIVATE KEY") != NULL;
    int encrypted;
    const char *kind = NULL;
    char alg[40];
    int bits = 0;
    hb_err re = HB_ERR_INVALID;
    BIO *bio = NULL;
    EVP_PKEY *pk = NULL;
    wbuf pub = {0};

    if (find_lit(data, len, "BEGIN DSA PRIVATE KEY"))
        return HB_ERR_INVALID;
    if (!trad_rsa && !trad_ec && !pkcs8 && !pkcs8e)
        return HB_ERR_INVALID;

    encrypted = pkcs8e ||
        ((trad_rsa || trad_ec) &&
         find_lit(data, len, "Proc-Type:") != NULL);

    /* 加密但无口令：只报头部能确定的 */
    if (encrypted && !passphrase) {
        out->kind = trad_rsa ? strdup("rsa")
                  : trad_ec  ? strdup("ecdsa") : NULL;
        out->encrypted = true;
        out->format = pkcs8e ? HB_KEYFMT_PKCS8 : HB_KEYFMT_PEM;
        out->private_text = secret_copy(data, len);
        if (!out->private_text)
            re = HB_ERR_NOMEM;
        else
            re = HB_OK;
        if (re != HB_OK)
            hb_key_info_clear(out);
        return re;
    }

    bio = BIO_new_mem_buf(data, (int)len);
    if (!bio) return HB_ERR_NOMEM;
    pk = PEM_read_bio_PrivateKey(bio, NULL, pw_cb, (void *)passphrase);
    BIO_free(bio);
    if (!pk) {
        /* armor 能解而私钥解不出 → 口令错；armor 都坏了 → 不是有效文件 */
        char *name = NULL, *hdr = NULL;
        unsigned char *dd = NULL;
        long dl = 0;
        int armor;
        bio = BIO_new_mem_buf(data, (int)len);
        armor = bio && PEM_read_bio(bio, &name, &hdr, &dd, &dl);
        if (bio) BIO_free(bio);
        OPENSSL_free(name); OPENSSL_free(hdr); OPENSSL_free(dd);
        return (armor && encrypted) ? HB_ERR_PASSPHRASE : HB_ERR_INVALID;
    }

    re = wire_from_pkey(pk, &pub, NULL, alg, &kind, &bits);
    EVP_PKEY_free(pk);
    if (re != HB_OK)
        goto out;
    re = fill_info(out, alg, kind, bits, pub.b, pub.len, NULL);
    if (re != HB_OK)
        goto out;
    out->encrypted = encrypted;
    out->format = (trad_rsa || trad_ec) ? HB_KEYFMT_PEM : HB_KEYFMT_PKCS8;
    out->private_text = secret_copy(data, len);
    if (!out->private_text)
        re = HB_ERR_NOMEM;
out:
    wb_free(&pub, 0);
    if (re != HB_OK)
        hb_key_info_clear(out);
    return re;
}

/* ------------------------------ inspect: PKCS#12 ------------------------------ */

static hb_err inspect_p12(const unsigned char *data, size_t len,
                          const char *passphrase, hb_key_info *out)
{
    const unsigned char *p = data;
    PKCS12 *p12;
    EVP_PKEY *pk = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *ca = NULL;
    wbuf pub = {0}, privf = {0};
    const char *kind = NULL;
    char alg[40];
    char *pem = NULL;
    int bits = 0;
    hb_err re;

    p12 = d2i_PKCS12(NULL, &p, (long)len);
    if (!p12)
        return HB_ERR_INVALID;
    if (PKCS12_parse(p12, passphrase ? passphrase : "",
                     &pk, &cert, &ca) != 1) {
        PKCS12_free(p12);
        return HB_ERR_PASSPHRASE;
    }
    PKCS12_free(p12);
    X509_free(cert);
    if (ca) sk_X509_pop_free(ca, X509_free);
    if (!pk)
        return HB_ERR_INVALID;

    re = wire_from_pkey(pk, &pub, &privf, alg, &kind, &bits);
    EVP_PKEY_free(pk);
    if (re != HB_OK)
        goto out;
    re = write_v1(&pub, &privf, NULL, &pem);
    if (re != HB_OK)
        goto out;
    re = fill_info(out, alg, kind, bits, pub.b, pub.len, NULL);
    if (re != HB_OK)
        goto out;
    /* libssh2 读不了 p12：转成未加密 openssh-key-v1 */
    out->private_text = hb_secret_strdup(pem);
    out->format = HB_KEYFMT_PKCS12;
    out->encrypted = false;
    if (!out->private_text)
        re = HB_ERR_NOMEM;
out:
    if (pem) {
        OPENSSL_cleanse(pem, strlen(pem));
        free(pem);
    }
    wb_free(&pub, 0);
    wb_free(&privf, 1);
    if (re != HB_OK)
        hb_key_info_clear(out);
    return re;
}

/* ------------------------------ 对外 API ------------------------------ */

hb_err hb_key_inspect(const unsigned char *data, size_t len,
                      const char *passphrase, hb_key_info *out)
{
    if (!out) return HB_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!data || len < 16)
        return HB_ERR_INVALID;
    if (hb_core_init() != 0)
        return HB_ERR_CRYPTO;

    if (find_lit(data, len, "-----BEGIN OPENSSH PRIVATE KEY-----"))
        return inspect_openssh(data, len, out);
    if (find_lit(data, len, "-----BEGIN"))
        return inspect_pem(data, len, passphrase, out);
    return inspect_p12(data, len, passphrase, out);
}

hb_err hb_key_inspect_file(const char *path, const char *passphrase,
                           hb_key_info *out)
{
    int fd;
    struct stat st;
    unsigned char *buf = NULL;
    size_t got = 0;
    hb_err e;

    if (!out) return HB_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!path) return HB_ERR_INVALID;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return HB_ERR_IO;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > 1024 * 1024) {
        close(fd);
        return HB_ERR_INVALID;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return HB_ERR_NOMEM;
    }
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return HB_ERR_IO;
        }
        if (!r) break;
        got += (size_t)r;
    }
    close(fd);
    e = hb_key_inspect(buf, got, passphrase, out);
    OPENSSL_cleanse(buf, got);
    free(buf);
    return e;
}

void hb_key_info_clear(hb_key_info *k)
{
    if (!k) return;
    free(k->kind);
    free(k->public_line);
    free(k->fingerprint);
    free(k->comment);
    hb_secret_free(k->private_text);
    memset(k, 0, sizeof *k);
}
