#include "harbor/share.h"

#include "share_priv.h"

#include "harbor/sync.h"     /* hb_sync_seal/open（编码约定与 sync §2 一致） */

#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

#define SNAP_MAX_LINES   1000     /* §5 scrollback 尾部上限 */
#define SNAP_BCAST_MS    10000    /* 10 s 广播快照 */
#define SNAP_REQ_RATE_MS 2000     /* SNAP_REQ 应答/发送限流 */
#define PAKE_TIMEOUT_MS  30000    /* PAKE2 后等 PAKE3 的期限 */
#define MAX_PARTS        16       /* 参与者槽位（Relay 上限 5，留余量） */

#define AD_FRAME "hb:share:v1"
#define AD_META  "hb:share:meta:v1"
#define AD_KIN   "hb:share:kin:v1"

/* ------------------------------------------------------------------ */
/* base64url（约定同 sync §2）                                          */
/* ------------------------------------------------------------------ */

static char *b64_enc(const uint8_t *bin, size_t n)
{
    size_t blen = sodium_base64_encoded_len(
        n, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char *s = malloc(blen);
    if (!s)
        return NULL;
    sodium_bin2base64(s, blen, bin, n,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return s;
}

static uint8_t *b64_dec(const char *s, size_t slen, size_t *n)
{
    size_t cap = slen / 4 * 3 + 4;
    uint8_t *bin = malloc(cap);
    size_t bin_len = 0;
    if (!bin)
        return NULL;
    if (sodium_base642bin(bin, cap, s, slen, NULL, &bin_len, NULL,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        free(bin);
        return NULL;
    }
    *n = bin_len;
    return bin;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p;
    if (!s)
        return NULL;
    p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* 帧编解码（§4.1/4.2）                                                  */
/* ------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

/* AD = "hb:share:v1" || 0x00 || share_id || 0x00 || hdr17（与帧头字节一致） */
uint8_t *hb__share_ad(const char *share_id, const uint8_t hdr[HB_FR_HDRLEN],
                      size_t *ad_len)
{
    size_t sl = strlen(AD_FRAME), il = strlen(share_id);
    uint8_t *ad = malloc(sl + 1 + il + 1 + HB_FR_HDRLEN);
    if (!ad)
        return NULL;
    memcpy(ad, AD_FRAME, sl);
    ad[sl] = 0;
    memcpy(ad + sl + 1, share_id, il);
    ad[sl + 1 + il] = 0;
    memcpy(ad + sl + 1 + il + 1, hdr, HB_FR_HDRLEN);
    *ad_len = sl + 1 + il + 1 + HB_FR_HDRLEN;
    return ad;
}

/* 组帧：payload = key ? seal(key, pt, ad) : pt（PAKE 明文）。 */
hb_share_frame hb__share_mk_frame(const char *share_id, uint8_t type,
                                  uint32_t to, uint32_t from, uint64_t seq,
                                  const uint8_t *key,
                                  const uint8_t *pt, size_t pt_len)
{
    hb_share_frame f;
    uint8_t *pl = NULL;
    size_t pl_len = pt_len;

    memset(&f, 0, sizeof f);
    f.is_text = false;
    f.to = to;
    if (key) {
        uint8_t hdr[HB_FR_HDRLEN], *ad;
        size_t ad_len;
        hdr[0] = type;
        put32(hdr + 1, to);
        put32(hdr + 5, from);
        put64(hdr + 9, seq);
        ad = hb__share_ad(share_id, hdr, &ad_len);
        if (ad) {
            pl = hb_sync_seal(key, pt, pt_len, ad, ad_len, &pl_len);
            free(ad);
        }
        if (!pl)
            return f;
    } else {
        pl = malloc(pt_len ? pt_len : 1);
        if (!pl)
            return f;
        memcpy(pl, pt, pt_len);
    }
    f.data = malloc(HB_FR_HDRLEN + pl_len);
    if (!f.data) {
        free(pl);
        return f;
    }
    f.data[0] = type;
    put32(f.data + 1, to);
    put32(f.data + 5, from);
    put64(f.data + 9, seq);
    memcpy(f.data + HB_FR_HDRLEN, pl, pl_len);
    f.len = HB_FR_HDRLEN + pl_len;
    if (key)
        sodium_free(pl);      /* seal 输出是 sodium 缓冲 */
    else
        free(pl);
    return f;
}

/* 解封：header 各字段经 AD 认证，任何头部字节被改 → open 失败。 */
hb_err hb__share_open_frame(const char *share_id, const uint8_t *data,
                            size_t n, const uint8_t *key,
                            uint8_t *type, uint32_t *to, uint32_t *from,
                            uint64_t *seq, uint8_t **pt, size_t *pt_len)
{
    uint8_t *ad, *out;
    size_t ad_len;
    hb_err e = HB_ERR_CRYPTO;
    if (n < HB_FR_HDRLEN || !key)
        return HB_ERR_INVALID;
    *type = data[0];
    *to = get32(data + 1);
    *from = get32(data + 5);
    *seq = get64(data + 9);
    ad = hb__share_ad(share_id, data, &ad_len);
    if (!ad)
        return HB_ERR_NOMEM;
    out = hb_sync_open(key, data + HB_FR_HDRLEN, n - HB_FR_HDRLEN,
                       ad, ad_len, pt_len);
    free(ad);
    if (out) {
        *pt = out;
        e = HB_OK;
    }
    return e;
}

static hb_share_frame mk_text(const char *json)
{
    hb_share_frame f;
    memset(&f, 0, sizeof f);
    f.is_text = true;
    f.data = (uint8_t *)xstrndup(json, strlen(json));
    f.len = strlen(json);
    return f;
}

void hb_share_frame_clear(hb_share_frame *f)
{
    free(f->data);
    f->data = NULL;
    f->len = 0;
}

void hb_share_event_clear(hb_share_event *e)
{
    free(e->name);
    free(e->device);
    free(e->text);
    e->name = e->device = e->text = NULL;
}

/* ------------------------------------------------------------------ */
/* 出队 + 事件队列 + wake fd（两个引擎共用同一套结构）                      */
/* ------------------------------------------------------------------ */

typedef struct fnode {
    hb_share_frame f;
    struct fnode *next;
} fnode;

typedef struct enode {
    hb_share_event e;
    struct enode *next;
} enode;

typedef struct {
    int     r, w;
    fnode  *fh, *ft;
    enode  *eh, *et;
} outq;

static bool outq_init(outq *q)
{
    int p[2], fl;
    if (pipe(p) != 0)
        return false;
    q->r = p[0];
    q->w = p[1];
    fl = fcntl(q->r, F_GETFL); fcntl(q->r, F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(q->w, F_GETFL); fcntl(q->w, F_SETFL, fl | O_NONBLOCK);
    return true;
}

static void outq_wake(outq *q)
{
    char b = 'q';
    if (write(q->w, &b, 1) < 0) {}
}

static void outq_push(outq *q, hb_share_frame *f)
{
    fnode *n;
    if (!f->data)
        return;
    n = calloc(1, sizeof *n);
    if (!n) {
        hb_share_frame_clear(f);
        return;
    }
    n->f = *f;
    if (q->ft)
        q->ft->next = n;
    else
        q->fh = n;
    q->ft = n;
    memset(f, 0, sizeof *f);
    outq_wake(q);
}

static void outq_ev(outq *q, hb_share_ev_type t, uint32_t id, int code,
                    const char *name, const char *device, const char *text)
{
    enode *n = calloc(1, sizeof *n);
    if (!n)
        return;
    n->e.type = t;
    n->e.id = id;
    n->e.code = code;
    n->e.name = name ? xstrndup(name, strlen(name)) : NULL;
    n->e.device = device ? xstrndup(device, strlen(device)) : NULL;
    n->e.text = text ? xstrndup(text, strlen(text)) : NULL;
    if (q->et)
        q->et->next = n;
    else
        q->eh = n;
    q->et = n;
    outq_wake(q);
}

static bool outq_pop_frame(outq *q, hb_share_frame *out)
{
    fnode *n = q->fh;
    char buf[64];
    if (n) {
        q->fh = n->next;
        if (!q->fh)
            q->ft = NULL;
        *out = n->f;
        free(n);
    }
    if (!q->fh && !q->eh)   /* 全空 → 排干唤醒字节 */
        while (read(q->r, buf, sizeof buf) > 0) {}
    return n != NULL;
}

static bool outq_pop_event(outq *q, hb_share_event *ev)
{
    enode *n = q->eh;
    char buf[64];
    if (n) {
        q->eh = n->next;
        if (!q->eh)
            q->et = NULL;
        *ev = n->e;
        free(n);
    }
    if (!q->fh && !q->eh)
        while (read(q->r, buf, sizeof buf) > 0) {}
    return n != NULL;
}

static void outq_free(outq *q)
{
    fnode *f;
    enode *e;
    while (q->fh) {
        f = q->fh;
        q->fh = f->next;
        hb_share_frame_clear(&f->f);
        free(f);
    }
    while (q->eh) {
        e = q->eh;
        q->eh = e->next;
        hb_share_event_clear(&e->e);
        free(e);
    }
    close(q->r);
    close(q->w);
}

/* ------------------------------------------------------------------ */
/* CPace（§6）                                                          */
/* ------------------------------------------------------------------ */

static void lv_put(uint8_t **p, const uint8_t *x, size_t n)
{
    **p = (uint8_t)n;
    (*p)++;
    memcpy(*p, x, n);
    *p += n;
}

/* sid = share_id || nonce_g；G = from_hash(H("hb:cpace:v1"||lv(PRS)||lv(sid))) */
static hb_err cpace_G(uint8_t G[32], const char *share_id,
                      const uint8_t nonce_g[16],
                      const char code[7], const uint8_t link_secret[16])
{
    static const char TAG[] = "hb:cpace:v1";
    uint8_t prs[22];
    size_t il = strlen(share_id), sl = il + 16;
    size_t mlen = sizeof(TAG) - 1 + 1 + sizeof prs + 1 + sl;
    uint8_t *m = malloc(mlen), *p = m;
    uint8_t h[64];
    int rc = -1;

    memcpy(prs, code, 6);
    memcpy(prs + 6, link_secret, 16);
    memcpy(p, TAG, sizeof(TAG) - 1);
    p += sizeof(TAG) - 1;
    lv_put(&p, prs, sizeof prs);
    *p++ = (uint8_t)sl;
    memcpy(p, share_id, il);
    p += il;
    memcpy(p, nonce_g, 16);
    p += 16;

    crypto_hash_sha512(h, m, mlen);
    rc = crypto_core_ristretto255_from_hash(G, h);
    sodium_memzero(h, sizeof h);
    sodium_memzero(m, mlen);
    free(m);
    sodium_memzero(prs, sizeof prs);
    return rc == 0 ? HB_OK : HB_ERR_CRYPTO;
}

hb_cpace *hb__cpace_new(const char *share_id, const uint8_t nonce_g[16],
                        const char code[7], const uint8_t link_secret[16],
                        const uint8_t fixed_scalar[32])
{
    hb_cpace *c;
    if (hb_core_init() != 0)
        return NULL;
    if (!share_id || !code || strlen(code) != 6)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->x = sodium_malloc(32);
    c->sid = malloc(strlen(share_id) + 16);
    if (!c->x || !c->sid)
        goto fail;
    memcpy(c->sid, share_id, strlen(share_id));
    memcpy(c->sid + strlen(share_id), nonce_g, 16);
    c->sid_len = strlen(share_id) + 16;
    if (cpace_G(c->G, share_id, nonce_g, code, link_secret) != HB_OK)
        goto fail;
    if (fixed_scalar)
        memcpy(c->x, fixed_scalar, 32);
    else
        crypto_core_ristretto255_scalar_random(c->x);
    if (crypto_scalarmult_ristretto255(c->Y, c->x, c->G) != 0)
        goto fail;
    return c;
fail:
    hb__cpace_free(c);
    return NULL;
}

void hb__cpace_free(hb_cpace *c)
{
    if (!c)
        return;
    if (c->x) {
        sodium_memzero(c->x, 32);
        sodium_free(c->x);
    }
    free(c->sid);
    sodium_memzero(c->G, sizeof c->G);
    sodium_memzero(c->Y, sizeof c->Y);
    free(c);
}

hb_err hb__cpace_isk(hb_cpace *c, const uint8_t peer_Y[32],
                     const uint8_t Ya[32], const uint8_t Yb[32],
                     uint8_t isk[64])
{
    static const char TAG[] = "hb:cpace:isk";
    uint8_t K[32];
    uint8_t *m, *p;
    size_t mlen;
    hb_err e = HB_ERR_CRYPTO;

    if (!crypto_core_ristretto255_is_valid_point(peer_Y))
        return HB_ERR_CRYPTO;
    if (crypto_scalarmult_ristretto255(K, c->x, peer_Y) != 0)
        return HB_ERR_CRYPTO;      /* -1：恒等元 */

    mlen = sizeof(TAG) - 1 + 1 + c->sid_len + 32 + 32 + 32;
    m = malloc(mlen);
    if (!m) {
        sodium_memzero(K, sizeof K);
        return HB_ERR_NOMEM;
    }
    p = m;
    memcpy(p, TAG, sizeof(TAG) - 1);
    p += sizeof(TAG) - 1;
    lv_put(&p, c->sid, c->sid_len);
    memcpy(p, K, 32);       p += 32;
    memcpy(p, Ya, 32);      p += 32;
    memcpy(p, Yb, 32);      p += 32;
    crypto_hash_sha512(isk, m, mlen);
    e = HB_OK;
    sodium_memzero(K, sizeof K);
    sodium_memzero(m, mlen);
    free(m);
    return e;
}

void hb__share_k_pake(const uint8_t isk[64], uint8_t out[32])
{
    crypto_kdf_derive_from_key(out, 32, 1, "hbpake01", isk + 32);
}

static void cpace_tag(const uint8_t isk[64], const char *who, uint8_t tag[32])
{
    crypto_auth_hmacsha256(tag, (const uint8_t *)who, strlen(who), isk);
}

/* ------------------------------------------------------------------ */
/* JSON 小工具                                                          */
/* ------------------------------------------------------------------ */

static cJSON *json_parse(const char *s, size_t n)
{
    char *t = xstrndup(s, n);
    cJSON *j;
    if (!t)
        return NULL;
    j = cJSON_Parse(t);
    free(t);
    return j;
}

static const char *json_str(cJSON *j, const char *k)
{
    cJSON *m = cJSON_GetObjectItemCaseSensitive(j, k);
    return (m && cJSON_IsString(m)) ? m->valuestring : NULL;
}

static int json_int(cJSON *j, const char *k, int dfl)
{
    cJSON *m = cJSON_GetObjectItemCaseSensitive(j, k);
    return (m && cJSON_IsNumber(m)) ? (int)m->valuedouble : dfl;
}

/* ------------------------------------------------------------------ */
/* 参与者记录                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t id;
    char     role;            /* 'm' member / 'g' guest */
    int      perm;            /* -1 未授权 / HB_SHARE_* */
    char    *name, *device;
    uint8_t *k_in;            /* sodium 32（collab member） */
    uint64_t last_seq;
    int64_t  last_snap_ms;
    bool     want_snap;
    bool     left;

    /* guest PAKE：0 无 / 1 已发 PAKE2 等 PAKE3 / 2 完成 */
    int      pg;
    int64_t  pg_deadline;
    uint8_t *isk;             /* sodium 64 */
    uint8_t *k_pake;          /* sodium 32 */
    uint8_t  nonce_g[16], Ya[32];
} part;

static void part_wipe(part *p)
{
    if (p->k_in) {
        sodium_memzero(p->k_in, 32);
        sodium_free(p->k_in);
        p->k_in = NULL;
    }
    if (p->isk) {
        sodium_memzero(p->isk, 64);
        sodium_free(p->isk);
        p->isk = NULL;
    }
    if (p->k_pake) {
        sodium_memzero(p->k_pake, 32);
        sodium_free(p->k_pake);
        p->k_pake = NULL;
    }
    free(p->name);
    free(p->device);
    p->name = p->device = NULL;
}

/* ------------------------------------------------------------------ */
/* owner 引擎                                                           */
/* ------------------------------------------------------------------ */

struct hb_share_owner {
    pthread_mutex_t mu;
    int      refs;
    bool     dead;

    char    *share_id;
    uint8_t *vk;              /* sodium 32 */
    int      mode;
    hb_session *sess;
    bool     tap_set;

    uint8_t *k_out;           /* sodium 32 */
    char     code[7];
    uint8_t  link_secret[16];

    uint64_t seq;             /* 发出的 seq */
    uint64_t dseq;            /* 已含最后一个 DELTA 序号 */

    part     parts[MAX_PARTS];
    int      nparts;

    uint64_t snap_tos;        /* 待定向快照 id 位掩码 */
    bool     snap_bcast;
    bool     snap_pending;    /* run_on_thread 回调在途 */
    int64_t  next_bcast;

    int      last_rows, last_cols;

    outq     q;
};

static void own_destroy(hb_share_owner *o)
{
    int i;
    for (i = 0; i < o->nparts; i++)
        part_wipe(&o->parts[i]);
    if (o->k_out) {
        sodium_memzero(o->k_out, 32);
        sodium_free(o->k_out);
    }
    if (o->vk) {
        sodium_memzero(o->vk, 32);
        sodium_free(o->vk);
    }
    sodium_memzero(o->code, sizeof o->code);
    sodium_memzero(o->link_secret, sizeof o->link_secret);
    free(o->share_id);
    outq_free(&o->q);
    pthread_mutex_destroy(&o->mu);
    free(o);
}

static void own_unref(hb_share_owner *o)
{
    bool z;
    pthread_mutex_lock(&o->mu);
    z = (--o->refs == 0);
    pthread_mutex_unlock(&o->mu);
    if (z)
        own_destroy(o);
}

static void own_ref(hb_share_owner *o)
{
    pthread_mutex_lock(&o->mu);
    o->refs++;
    pthread_mutex_unlock(&o->mu);
}

static part *own_find(hb_share_owner *o, uint32_t id)
{
    int i;
    for (i = 0; i < o->nparts; i++)
        if (o->parts[i].id == id)
            return &o->parts[i];
    return NULL;
}

static int own_granted(hb_share_owner *o)
{
    int i, n = 0;
    for (i = 0; i < o->nparts; i++)
        if (o->parts[i].perm >= 0 && !o->parts[i].left)
            n++;
    return n;
}

/* 发出一帧（锁内）：seq 递增 */
static void own_send(hb_share_owner *o, uint8_t type, uint32_t to,
                     const uint8_t *key, const uint8_t *pt, size_t n)
{
    hb_share_frame f;
    f = hb__share_mk_frame(o->share_id, type, to, 1, ++o->seq, key, pt, n);
    outq_push(&o->q, &f);
}

static void own_text(hb_share_owner *o, const char *json)
{
    hb_share_frame f = mk_text(json);
    outq_push(&o->q, &f);
}

static void own_presence(hb_share_owner *o)
{
    cJSON *j = cJSON_CreateObject(), *arr = cJSON_CreateArray();
    char *txt;
    int i;
    for (i = 0; i < o->nparts; i++) {
        part *p = &o->parts[i];
        cJSON *d;
        if (p->left || p->perm < 0)
            continue;
        d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "id", p->id);
        cJSON_AddStringToObject(d, "name",
                                p->name ? p->name
                                        : (p->device ? p->device : ""));
        cJSON_AddStringToObject(d, "perm",
                                p->perm == HB_SHARE_COLLAB ? "collab"
                                                           : "view");
        cJSON_AddItemToArray(arr, d);
    }
    cJSON_AddItemToObject(j, "participants", arr);
    cJSON_AddStringToObject(j, "mode",
                            o->mode == HB_SHARE_COLLAB ? "collab" : "view");
    txt = cJSON_PrintUnformatted(j);
    own_send(o, HB_FR_PRESENCE, 0, o->k_out, (const uint8_t *)txt,
             txt ? strlen(txt) : 0);
    free(txt);
    cJSON_Delete(j);
}

/* run_on_thread 回调：与 dseq 同点截取快照，分发给 snap_tos / 广播。
 * mu 与 vt 锁同持期间 tap 无法进入 → dseq 冻结，快照覆盖的流位置与
 * 记下的 dseq 严格一致（锁序 mu→vt 与 tap 一致）。 */
static void own_snap_cb(hb_session *s, void *ud)
{
    hb_share_owner *o = ud;
    char *ansi = NULL;
    size_t alen = 0;
    uint64_t tos, d = 0;
    bool bcast;
    int rows = 0, cols = 0, alt = 0, i;
    hb_vt *vt;
    uint8_t *pl = NULL;
    size_t pl_len;
    int64_t now = hb_now_ms();

    pthread_mutex_lock(&o->mu);
    o->snap_pending = false;
    if (o->dead) {
        pthread_mutex_unlock(&o->mu);
        own_unref(o);
        return;
    }
    tos = o->snap_tos;
    bcast = o->snap_bcast;
    o->snap_tos = 0;
    o->snap_bcast = false;

    vt = hb_session_vt(s);
    if (vt) {
        hb_vt_lock(vt);
        hb_vt_snapshot_ansi(vt, SNAP_MAX_LINES, &ansi, &alen);
        hb_vt_size(vt, &rows, &cols);
        alt = hb_vt_altscreen(vt) ? 1 : 0;
        d = o->dseq;                 /* 与快照同点的 dseq */
        hb_vt_unlock(vt);
    }
    if (ansi) {
        pl_len = 13 + alen;
        pl = malloc(pl_len);
        if (pl) {
            pl[0] = (uint8_t)(rows >> 8);
            pl[1] = (uint8_t)rows;
            pl[2] = (uint8_t)(cols >> 8);
            pl[3] = (uint8_t)cols;
            pl[4] = (uint8_t)alt;
            put64(pl + 5, d);
            memcpy(pl + 13, ansi, alen);
        }
    }
    if (pl && !o->dead) {
        if (bcast)
            own_send(o, HB_FR_SNAPSHOT, 0, o->k_out, pl, pl_len);
        for (i = 0; i < o->nparts; i++) {
            part *p = &o->parts[i];
            if (p->id < 64 && (tos & (1ULL << p->id))) {
                own_send(o, HB_FR_SNAPSHOT, p->id, o->k_out, pl, pl_len);
                p->last_snap_ms = now;
            }
        }
    }
    pthread_mutex_unlock(&o->mu);
    free(pl);
    free(ansi);
    own_unref(o);
}

/* 标记需要定向/广播快照并保证回调在途（o->mu 已持有时调用；
 * refs 直接自增，不经 own_ref，避免重入同一互斥锁） */
static void own_want_snap_locked(hb_share_owner *o, uint32_t to)
{
    if (to == 0)
        o->snap_bcast = true;
    else if (to < 64)
        o->snap_tos |= 1ULL << to;
    if (!o->snap_pending && o->sess) {
        o->refs++;
        o->snap_pending = true;
        hb_session_run_on_thread(o->sess, own_snap_cb, o);
    }
}

static void own_tap(hb_session *s, const char *bytes, size_t len, void *ud)
{
    hb_share_owner *o = ud;
    int r = 0, c = 0;
    hb_vt *vt;

    own_ref(o);
    pthread_mutex_lock(&o->mu);
    if (!o->dead && o->sess == s && own_granted(o) > 0) {
        vt = hb_session_vt(s);
        if (vt)
            hb_vt_size(vt, &r, &c);
        if (vt && (r != o->last_rows || c != o->last_cols)) {
            uint8_t b[4];
            o->last_rows = r;
            o->last_cols = c;
            b[0] = (uint8_t)(r >> 8); b[1] = (uint8_t)r;
            b[2] = (uint8_t)(c >> 8); b[3] = (uint8_t)c;
            own_send(o, HB_FR_RESIZE, 0, o->k_out, b, 4);
        }
        {
            uint8_t *pl = malloc(8 + len);
            if (pl) {
                put64(pl, ++o->dseq);
                memcpy(pl + 8, bytes, len);
                own_send(o, HB_FR_DELTA, 0, o->k_out, pl, 8 + len);
                free(pl);
            }
        }
    }
    pthread_mutex_unlock(&o->mu);
    own_unref(o);
}

static void own_grant(hb_share_owner *o, part *p)
{
    cJSON *j = cJSON_CreateObject();
    char *txt;
    if (!j)
        return;
    /* collab → 生成/保留 K_in；view → 丢弃 */
    if (p->perm == HB_SHARE_COLLAB && !p->k_in) {
        p->k_in = sodium_malloc(32);
        if (p->k_in)
            randombytes_buf(p->k_in, 32);
    }
    if (p->perm != HB_SHARE_COLLAB && p->k_in) {
        sodium_memzero(p->k_in, 32);
        sodium_free(p->k_in);
        p->k_in = NULL;
    }
    cJSON_AddStringToObject(j, "perm",
                            p->perm == HB_SHARE_COLLAB ? "collab" : "view");
    if (p->k_in) {
        /* k_in = seal(VK, K_in[p], "hb:share:kin:v1"||0||id||0||p(4BE)) 的 b64 */
        size_t al = strlen(AD_KIN) + 1 + strlen(o->share_id) + 1 + 4;
        uint8_t *ad = malloc(al), *sealed;
        size_t sl;
        if (!ad) {
            cJSON_Delete(j);
            return;
        }
        memcpy(ad, AD_KIN, strlen(AD_KIN));
        ad[strlen(AD_KIN)] = 0;
        memcpy(ad + strlen(AD_KIN) + 1, o->share_id, strlen(o->share_id));
        ad[strlen(AD_KIN) + 1 + strlen(o->share_id)] = 0;
        put32(ad + al - 4, p->id);
        sealed = hb_sync_seal(o->vk, p->k_in, 32, ad, al, &sl);
        free(ad);
        if (sealed) {
            char *b64 = b64_enc(sealed, sl);
            sodium_free(sealed);
            if (b64) {
                cJSON_AddStringToObject(j, "k_in", b64);
                free(b64);
            }
        }
    }
    txt = cJSON_PrintUnformatted(j);
    own_send(o, HB_FR_GRANT, p->id, o->k_out, (const uint8_t *)txt,
             txt ? strlen(txt) : 0);
    free(txt);
    cJSON_Delete(j);
}

static void own_remove(hb_share_owner *o, int idx)
{
    part_wipe(&o->parts[idx]);
    if (idx + 1 < o->nparts)
        memmove(&o->parts[idx], &o->parts[idx + 1],
                (size_t)(o->nparts - idx - 1) * sizeof *o->parts);
    o->nparts--;
}

/* guest PAKE 失败：pake_fail + kick（§6 step4 "踢出该 guest"）。
 * 恒在 o->mu 持锁下调用。 */
static void own_pake_fail(hb_share_owner *o, part *p)
{
    char t[64];
    snprintf(t, sizeof t, "{\"t\":\"pake_fail\",\"id\":%u}", p->id);
    own_text(o, t);
    snprintf(t, sizeof t, "{\"t\":\"kick\",\"id\":%u}", p->id);
    own_text(o, t);
    outq_ev(&o->q, HB_SH_EV_PAKE_FAIL, p->id, 0, NULL, NULL, NULL);
}

static void own_pake1(hb_share_owner *o, part *p,
                      const uint8_t *pl, size_t n)
{
    hb_cpace *cp;
    uint8_t tag[32], frame[64];
    int i;
    bool busy = false;

    if (n != 48)
        return;
    /* 同时只允许 1 个未完成 guest PAKE：踢掉新来者且不计失败（见 §6 注释） */
    for (i = 0; i < o->nparts; i++) {
        part *q = &o->parts[i];
        if (q != p && q->role == 'g' && q->pg == 1) {
            busy = true;
            break;
        }
    }
    if (busy || p->pg != 0) {
        char t[64];
        snprintf(t, sizeof t, "{\"t\":\"kick\",\"id\":%u}", p->id);
        own_text(o, t);
        return;
    }
    memcpy(p->nonce_g, pl, 16);
    memcpy(p->Ya, pl + 16, 32);
    cp = hb__cpace_new(o->share_id, p->nonce_g, o->code, o->link_secret, NULL);
    if (!cp) {
        own_pake_fail(o, p);
        return;
    }
    p->isk = sodium_malloc(64);
    if (!p->isk ||
        hb__cpace_isk(cp, p->Ya, p->Ya, cp->Y, p->isk) != HB_OK) {
        hb__cpace_free(cp);
        if (p->isk) { sodium_free(p->isk); p->isk = NULL; }
        own_pake_fail(o, p);
        return;
    }
    memcpy(frame, cp->Y, 32);
    cpace_tag(p->isk, "owner", tag);
    memcpy(frame + 32, tag, 32);
    hb__cpace_free(cp);        /* yb/K 随 cpace 一起擦除 */
    own_send(o, HB_FR_PAKE2, p->id, NULL, frame, 64);
    sodium_memzero(tag, sizeof tag);
    p->pg = 1;
    p->pg_deadline = hb_now_ms() + PAKE_TIMEOUT_MS;
}

static void own_pake3(hb_share_owner *o, part *p,
                      const uint8_t *pl, size_t n)
{
    uint8_t tag[32], kp[32];
    if (p->pg != 1 || !p->isk || n != 32) {
        if (n)
            own_pake_fail(o, p);
        return;
    }
    cpace_tag(p->isk, "guest", tag);
    if (sodium_memcmp(tag, pl, 32) != 0) {
        sodium_memzero(tag, sizeof tag);
        own_pake_fail(o, p);
        return;
    }
    sodium_memzero(tag, sizeof tag);
    hb__share_k_pake(p->isk, kp);
    sodium_memzero(p->isk, 64);
    sodium_free(p->isk);
    p->isk = NULL;
    p->k_pake = sodium_malloc(32);
    if (p->k_pake)
        memcpy(p->k_pake, kp, 32);
    sodium_memzero(kp, sizeof kp);
    if (!p->k_pake) {
        own_pake_fail(o, p);
        return;
    }
    p->pg = 2;
    p->perm = HB_SHARE_VIEW;              /* guest 永远只读 */
    /* KEY：ISK 派生 K_pake 封装的 K_out */
    own_send(o, HB_FR_KEY, p->id, p->k_pake, o->k_out, 32);
    {
        char t[64];
        snprintf(t, sizeof t, "{\"t\":\"pake_ok\",\"id\":%u}", p->id);
        own_text(o, t);
    }
    outq_ev(&o->q, HB_SH_EV_PAKE_OK, p->id, 0, NULL, NULL, NULL);
    own_presence(o);
    own_want_snap_locked(o, p->id);
}

static void own_join(hb_share_owner *o, part *p,
                     const uint8_t *pt, size_t n)
{
    cJSON *j = json_parse((const char *)pt, n);
    const char *dev, *nm;
    if (!j)
        return;
    dev = json_str(j, "device");
    nm = json_str(j, "name");
    free(p->device);
    free(p->name);
    p->device = dev ? xstrndup(dev, strlen(dev)) : NULL;
    p->name = nm ? xstrndup(nm, strlen(nm)) : NULL;
    cJSON_Delete(j);
    p->perm = o->mode;
    own_grant(o, p);
    outq_ev(&o->q, HB_SH_EV_JOINED, p->id, p->perm,
            p->name, p->device, NULL);
    own_presence(o);
    own_want_snap_locked(o, p->id);
}

static void own_input(hb_share_owner *o, part *p,
                      const uint8_t *sealed, size_t n, const uint8_t *ad,
                      size_t ad_len)
{
    uint8_t *pt;
    size_t pl = 0;
    static const char RO[] = "只读模式，输入未生效";

    if (p->perm == HB_SHARE_COLLAB && p->k_in) {
        pt = hb_sync_open(p->k_in, sealed, n, ad, ad_len, &pl);
        if (pt) {
            hb_session_write(o->sess, (const char *)pt, pl);
            sodium_memzero(pt, pl);
            sodium_free(pt);
            return;
        }
    }
    /* perm 不符、k_in 未发放或解密失败（含用 K_out 伪造）→ 统一拒收并 NOTICE */
    own_send(o, HB_FR_NOTICE, p->id, o->k_out, (const uint8_t *)RO,
             sizeof RO - 1);
    outq_ev(&o->q, HB_SH_EV_INPUT_REJECTED, p->id, 0, NULL, NULL, RO);
}

/* ------------------------------------------------------------------ */

hb_share_owner *hb_share_owner_new(const char *share_id, const uint8_t vk[32],
                                   int mode)
{
    hb_share_owner *o;
    if (hb_core_init() != 0 || !share_id || !vk)
        return NULL;
    o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->share_id = xstrndup(share_id, strlen(share_id));
    o->vk = sodium_malloc(32);
    o->k_out = sodium_malloc(32);
    if (!o->share_id || !o->vk || !o->k_out || !outq_init(&o->q)) {
        free(o->share_id);
        if (o->vk) sodium_free(o->vk);
        if (o->k_out) sodium_free(o->k_out);
        free(o);
        return NULL;
    }
    memcpy(o->vk, vk, 32);
    randombytes_buf(o->k_out, 32);
    snprintf(o->code, sizeof o->code, "%06u",
             randombytes_uniform(1000000));
    randombytes_buf(o->link_secret, sizeof o->link_secret);
    o->mode = mode;
    o->refs = 1;
    o->next_bcast = -1;
    pthread_mutex_init(&o->mu, NULL);
    return o;
}

char *hb_share_owner_meta(hb_share_owner *o, const char *title,
                          const char *host, const char *owner_device)
{
    cJSON *j;
    char *txt, *k_b64, *out = NULL;
    uint8_t *sealed;
    size_t sl, al;
    uint8_t *ad;

    if (!o)
        return NULL;
    j = cJSON_CreateObject();
    if (!j)
        return NULL;
    cJSON_AddStringToObject(j, "title", title ? title : "");
    cJSON_AddStringToObject(j, "host", host ? host : "");
    cJSON_AddStringToObject(j, "mode",
                            o->mode == HB_SHARE_COLLAB ? "collab" : "view");
    k_b64 = b64_enc(o->k_out, 32);
    cJSON_AddStringToObject(j, "k_out", k_b64 ? k_b64 : "");
    cJSON_AddStringToObject(j, "owner_device",
                            owner_device ? owner_device : "");
    cJSON_AddNumberToObject(j, "started", (double)hb_now_ms());
    txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    free(k_b64);
    if (!txt)
        return NULL;

    al = strlen(AD_META) + 1 + strlen(o->share_id);
    ad = malloc(al);
    if (!ad) {
        free(txt);
        return NULL;
    }
    memcpy(ad, AD_META, strlen(AD_META));
    ad[strlen(AD_META)] = 0;
    memcpy(ad + strlen(AD_META) + 1, o->share_id, strlen(o->share_id));
    sealed = hb_sync_seal(o->vk, (const uint8_t *)txt, strlen(txt),
                          ad, al, &sl);
    free(ad);
    free(txt);
    if (sealed) {
        out = b64_enc(sealed, sl);
        sodium_free(sealed);
    }
    return out;
}

char *hb_share_owner_link(hb_share_owner *o, const char *server_url)
{
    char *s_b64, *l_b64, *out;
    if (!o || !server_url)
        return NULL;
    s_b64 = b64_enc((const uint8_t *)server_url, strlen(server_url));
    l_b64 = b64_enc(o->link_secret, sizeof o->link_secret);
    if (!s_b64 || !l_b64) {
        free(s_b64);
        free(l_b64);
        return NULL;
    }
    {
        int need = snprintf(NULL, 0, "harbor://share/%s?s=%s#%s",
                            o->share_id, s_b64, l_b64);
        out = malloc((size_t)need + 1);
        if (out)
            snprintf(out, (size_t)need + 1, "harbor://share/%s?s=%s#%s",
                     o->share_id, s_b64, l_b64);
    }
    free(s_b64);
    free(l_b64);
    return out;
}

const char *hb_share_owner_code(hb_share_owner *o)
{
    return o ? o->code : NULL;
}

const char *hb_share_owner_id(hb_share_owner *o)
{
    return o ? o->share_id : NULL;
}

void hb_share_owner_attach(hb_share_owner *o, hb_session *sess)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->mu);
    if (o->sess && o->tap_set)
        hb_session_set_tap(o->sess, NULL, NULL);
    o->sess = sess;
    o->tap_set = false;
    if (sess) {
        int r = 0, c = 0;
        hb_vt *vt = hb_session_vt(sess);
        if (vt)
            hb_vt_size(vt, &r, &c);
        o->last_rows = r;
        o->last_cols = c;
        hb_session_set_tap(sess, own_tap, o);
        o->tap_set = true;
    }
    pthread_mutex_unlock(&o->mu);
}

int hb_share_owner_fd(hb_share_owner *o) { return o ? o->q.r : -1; }

bool hb_share_owner_next_frame(hb_share_owner *o, hb_share_frame *out)
{
    bool r;
    if (!o)
        return false;
    pthread_mutex_lock(&o->mu);
    r = outq_pop_frame(&o->q, out);
    pthread_mutex_unlock(&o->mu);
    return r;
}

bool hb_share_owner_next_event(hb_share_owner *o, hb_share_event *ev)
{
    bool r;
    if (!o)
        return false;
    pthread_mutex_lock(&o->mu);
    r = outq_pop_event(&o->q, ev);
    pthread_mutex_unlock(&o->mu);
    return r;
}

int64_t hb_share_owner_next_ms(hb_share_owner *o)
{
    int64_t nx = -1;
    int i;
    if (!o)
        return -1;
    pthread_mutex_lock(&o->mu);
    if (o->next_bcast >= 0)
        nx = o->next_bcast;
    for (i = 0; i < o->nparts; i++) {
        part *p = &o->parts[i];
        if (p->pg == 1 &&
            (nx < 0 || p->pg_deadline < nx))
            nx = p->pg_deadline;
    }
    pthread_mutex_unlock(&o->mu);
    return nx;
}

void hb_share_owner_tick(hb_share_owner *o)
{
    int64_t now = hb_now_ms();
    int i;
    if (!o)
        return;
    pthread_mutex_lock(&o->mu);
    for (i = 0; i < o->nparts; i++) {
        part *p = &o->parts[i];
        if (p->pg == 1 && p->pg_deadline <= now) {
            p->pg = 0;            /* 先标记，避免重入再计一次 */
            own_pake_fail(o, p);
        }
    }
    if (o->next_bcast >= 0 && o->next_bcast <= now) {
        if (own_granted(o) > 0) {
            own_want_snap_locked(o, 0);
            o->next_bcast = now + SNAP_BCAST_MS;
        } else {
            o->next_bcast = -1;
        }
    }
    pthread_mutex_unlock(&o->mu);
}

void hb_share_owner_set_mode(hb_share_owner *o, int mode)
{
    int i;
    if (!o)
        return;
    pthread_mutex_lock(&o->mu);
    if (o->mode != mode) {
        o->mode = mode;
        for (i = 0; i < o->nparts; i++) {
            part *p = &o->parts[i];
            if (p->role == 'm' && p->perm >= 0 && !p->left) {
                p->perm = mode;
                own_grant(o, p);    /* 降级时丢弃 K_in */
            }
        }
        own_presence(o);
    }
    pthread_mutex_unlock(&o->mu);
}

void hb_share_owner_kick(hb_share_owner *o, uint32_t participant_id)
{
    int i;
    if (!o)
        return;
    pthread_mutex_lock(&o->mu);
    for (i = 0; i < o->nparts; i++) {
        if (o->parts[i].id == participant_id) {
            char t[64];
            snprintf(t, sizeof t, "{\"t\":\"kick\",\"id\":%u}",
                     participant_id);
            own_text(o, t);
            own_remove(o, i);
            own_presence(o);
            break;
        }
    }
    pthread_mutex_unlock(&o->mu);
}

void hb_share_owner_regen_guest(hb_share_owner *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->mu);
    snprintf(o->code, sizeof o->code, "%06u",
             randombytes_uniform(1000000));
    randombytes_buf(o->link_secret, sizeof o->link_secret);
    pthread_mutex_unlock(&o->mu);
}

int hb_share_owner_mode(hb_share_owner *o) { return o ? o->mode : -1; }

int hb_share_owner_count(hb_share_owner *o)
{
    int n;
    if (!o)
        return 0;
    pthread_mutex_lock(&o->mu);
    n = o->nparts;
    pthread_mutex_unlock(&o->mu);
    return n;
}

void hb_share_owner_free(hb_share_owner *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&o->mu);
    o->dead = true;
    if (o->sess && o->tap_set)
        hb_session_set_tap(o->sess, NULL, NULL);
    o->sess = NULL;
    o->tap_set = false;
    pthread_mutex_unlock(&o->mu);
    own_unref(o);
}

/* --------------------------- 入口分发 --------------------------- */

static void own_frame(hb_share_owner *o, const uint8_t *data, size_t n)
{
    uint8_t type;
    uint32_t to, from;
    uint64_t seq;
    part *p;
    uint8_t *ad, *pt = NULL;
    size_t ad_len, pl = 0;

    if (n < HB_FR_HDRLEN)
        return;
    type = data[0];
    to = get32(data + 1);
    from = get32(data + 5);
    seq = get64(data + 9);
    (void)to;

    pthread_mutex_lock(&o->mu);
    p = own_find(o, from);
    if (!p || p->left) {
        pthread_mutex_unlock(&o->mu);
        return;
    }
    if (seq <= p->last_seq) {         /* 防重放：seq 必须严格递增 */
        pthread_mutex_unlock(&o->mu);
        return;
    }
    p->last_seq = seq;
    n -= HB_FR_HDRLEN;
    data += HB_FR_HDRLEN;

    switch (type) {
    case HB_FR_JOIN:
        if (p->role != 'm' || p->perm >= 0)
            break;
        ad = hb__share_ad(o->share_id, data - HB_FR_HDRLEN, &ad_len);
        if (ad) {
            pt = hb_sync_open(o->k_out, data, n, ad, ad_len, &pl);
            free(ad);
        }
        if (pt) {
            own_join(o, p, pt, pl);
            sodium_free(pt);
        }
        break;
    case HB_FR_SNAP_REQ:
        ad = hb__share_ad(o->share_id, data - HB_FR_HDRLEN, &ad_len);
        if (ad) {
            pt = hb_sync_open(o->k_out, data, n, ad, ad_len, &pl);
            free(ad);
        }
        if (pt) {
            sodium_free(pt);
            /* 限流：每参与者 2 s 至多一次定向快照 */
            if (p->perm >= 0 &&
                hb_now_ms() - p->last_snap_ms >= SNAP_REQ_RATE_MS)
                own_want_snap_locked(o, p->id);
        }
        break;
    case HB_FR_INPUT:
        ad = hb__share_ad(o->share_id, data - HB_FR_HDRLEN, &ad_len);
        if (ad) {
            own_input(o, p, data, n, ad, ad_len);
            free(ad);
        }
        break;
    case HB_FR_PAKE1:
        if (p->role == 'g')
            own_pake1(o, p, data, n);
        break;
    case HB_FR_PAKE3:
        if (p->role == 'g')
            own_pake3(o, p, data, n);
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&o->mu);
}

static void own_text_in(hb_share_owner *o, const char *data, size_t n)
{
    cJSON *j = json_parse(data, n);
    const char *t;
    int id;
    part *p;
    int i;

    if (!j)
        return;
    t = json_str(j, "t");
    id = json_int(j, "id", 0);
    pthread_mutex_lock(&o->mu);
    if (t && !strcmp(t, "joined")) {
        const char *role = json_str(j, "role");
        const char *dev = json_str(j, "device");
        if (o->nparts < MAX_PARTS) {
            p = &o->parts[o->nparts++];
            memset(p, 0, sizeof *p);
            p->id = (uint32_t)id;
            p->role = (role && role[0] == 'g') ? 'g' : 'm';
            p->perm = -1;
            p->device = dev ? xstrndup(dev, strlen(dev)) : NULL;
            if (o->next_bcast < 0)
                o->next_bcast = hb_now_ms() + SNAP_BCAST_MS;
        }
    } else if (t && !strcmp(t, "left")) {
        for (i = 0; i < o->nparts; i++) {
            if (o->parts[i].id == (uint32_t)id) {
                uint32_t pid = o->parts[i].id;
                bool pake_pending = (o->parts[i].pg == 1);
                own_remove(o, i);
                if (pake_pending) {
                    /* guest PAKE3 前断开 → 计失败（§6 step4） */
                    char tt[64];
                    snprintf(tt, sizeof tt,
                             "{\"t\":\"pake_fail\",\"id\":%u}", pid);
                    own_text(o, tt);
                    outq_ev(&o->q, HB_SH_EV_PAKE_FAIL, pid, 0,
                            NULL, NULL, NULL);
                }
                outq_ev(&o->q, HB_SH_EV_LEFT, pid, 0, NULL, NULL, NULL);
                own_presence(o);
                break;
            }
        }
    }
    pthread_mutex_unlock(&o->mu);
    cJSON_Delete(j);
}

void hb_share_owner_incoming(hb_share_owner *o, const uint8_t *data,
                             size_t len, bool is_text)
{
    if (!o || !data)
        return;
    if (is_text)
        own_text_in(o, (const char *)data, len);
    else
        own_frame(o, data, len);
}

/* ============================ 参与者引擎 ============================ */

struct hb_share_peer {
    pthread_mutex_t mu;
    char     *share_id, *server_url, *device, *name, *title;
    uint8_t  *vk;              /* sodium 32（member） */
    uint8_t  *k_out;           /* sodium 32 */
    uint8_t  *k_in;            /* sodium 32 */
    int       perm;
    bool      member;
    hb_vt    *vt;

    uint64_t  seq;             /* 自己的发送 seq */
    uint64_t  owner_seq;       /* 最后收到的 owner seq */
    uint64_t  base_dseq;
    bool      have_base, gap;
    int64_t   last_req_ms;

    uint32_t  self_id;
    bool      ended;
    bool      want_join;         /* hello 到达后再发 JOIN（from 须为真实 id） */

    /* guest CPace */
    hb_cpace *pk;
    uint8_t  *isk;             /* sodium 64 */
    uint8_t  *k_pake;          /* sodium 32：PAKE3 已发、等 KEY */
    int       pg;              /* 1 pake1 已发 / 2 pake3 已发 / 3 完成 */

    outq      q;
};

static void peer_send(hb_share_peer *p, uint8_t type,
                      const uint8_t *key, const uint8_t *pt, size_t n)
{
    hb_share_frame f;
    f = hb__share_mk_frame(p->share_id, type, 1, p->self_id, ++p->seq, key, pt, n);
    outq_push(&p->q, &f);
}

static hb_share_peer *peer_alloc(void)
{
    hb_share_peer *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    pthread_mutex_init(&p->mu, NULL);
    p->vt = hb_vt_new(24, 80, 1000);
    if (!p->vt || !outq_init(&p->q)) {
        if (p->vt)
            hb_vt_free(p->vt);
        pthread_mutex_destroy(&p->mu);
        free(p);
        return NULL;
    }
    p->perm = HB_SHARE_VIEW;
    p->last_req_ms = -10000;
    return p;
}

hb_share_peer *hb_share_peer_new_member(const char *share_id,
                                        const uint8_t vk[32],
                                        const char *meta_b64,
                                        const char *device, const char *name,
                                        hb_err *err)
{
    hb_share_peer *p;
    uint8_t *blob, *pt = NULL, *ad;
    size_t bl = 0, pl = 0, al;
    cJSON *j = NULL;
    const char *kb;
    uint8_t *kbin;
    size_t kbl = 0;

    if (err) *err = HB_ERR_INVALID;
    if (hb_core_init() != 0 || !share_id || !vk || !meta_b64)
        return NULL;
    blob = b64_dec(meta_b64, strlen(meta_b64), &bl);
    if (!blob)
        return NULL;
    al = strlen(AD_META) + 1 + strlen(share_id);
    ad = malloc(al);
    memcpy(ad, AD_META, strlen(AD_META));
    ad[strlen(AD_META)] = 0;
    memcpy(ad + strlen(AD_META) + 1, share_id, strlen(share_id));
    pt = hb_sync_open(vk, blob, bl, ad, al, &pl);
    free(ad);
    free(blob);
    if (!pt)
        return NULL;
    j = json_parse((const char *)pt, pl);
    if (!j) {
        sodium_free(pt);
        return NULL;
    }
    kb = json_str(j, "k_out");
    kbin = kb ? b64_dec(kb, strlen(kb), &kbl) : NULL;
    if (!kbin || kbl != 32) {
        free(kbin);
        cJSON_Delete(j);
        sodium_free(pt);
        return NULL;
    }
    p = peer_alloc();
    if (!p) {
        free(kbin);
        cJSON_Delete(j);
        sodium_free(pt);
        if (err) *err = HB_ERR_NOMEM;
        return NULL;
    }
    p->member = true;
    p->share_id = xstrndup(share_id, strlen(share_id));
    p->device = xstrndup(device ? device : "", strlen(device ? device : ""));
    p->name = xstrndup(name ? name : "", strlen(name ? name : ""));
    {
        const char *t = json_str(j, "title");
        if (t)
            p->title = xstrndup(t, strlen(t));
    }
    p->vk = sodium_malloc(32);
    p->k_out = sodium_malloc(32);
    if (p->vk && p->k_out) {
        memcpy(p->vk, vk, 32);
        memcpy(p->k_out, kbin, 32);
    }
    free(kbin);
    cJSON_Delete(j);
    sodium_memzero(pt, pl);
    sodium_free(pt);
    if (!p->vk || !p->k_out) {
        hb_share_peer_free(p);
        if (err) *err = HB_ERR_NOMEM;
        return NULL;
    }
    /* JOIN 必须等 Relay 的 hello（from 进 AD，须为 Relay 分配的真实 id） */
    p->want_join = true;
    if (err) *err = HB_OK;
    return p;
}

hb_share_peer *hb_share_peer_new_guest(const char *link, const char *code,
                                       hb_err *err)
{
    hb_share_peer *p;
    const char *id0, *id1, *q0, *h0, *s0;
    size_t idl;
    char sid[80];
    uint8_t nonce_g[16], lsec[16];
    size_t nl = 0;

    if (err) *err = HB_ERR_INVALID;
    if (hb_core_init() != 0 || !link || !code || strlen(code) != 6)
        return NULL;
    /* harbor://share/<id>?s=<b64(server)>#<b64(link_secret)> */
    if (strncmp(link, "harbor://share/", 15) != 0)
        return NULL;
    id0 = link + 15;
    q0 = strchr(id0, '?');
    h0 = strchr(id0, '#');
    if (!q0 || !h0 || h0 < q0)
        return NULL;
    id1 = q0;
    idl = (size_t)(id1 - id0);
    if (idl == 0 || idl >= sizeof sid)
        return NULL;
    memcpy(sid, id0, idl);
    sid[idl] = 0;
    s0 = q0 + 1;
    if (strncmp(s0, "s=", 2) != 0)
        return NULL;
    s0 += 2;
    {
        size_t svlen = (size_t)(h0 - s0);
        uint8_t *sv = b64_dec(s0, svlen, &nl);
        uint8_t *ls;
        size_t ll = 0;
        if (!sv)
            return NULL;
        ls = b64_dec(h0 + 1, strlen(h0 + 1), &ll);
        if (!ls || ll != 16) {
            free(sv);
            free(ls);
            return NULL;
        }
        memcpy(lsec, ls, 16);
        free(ls);
        p = peer_alloc();
        if (!p) {
            free(sv);
            return NULL;
        }
        p->share_id = xstrndup(sid, idl);
        p->server_url = xstrndup((const char *)sv, nl);
        free(sv);
    }
    randombytes_buf(nonce_g, 16);
    p->pk = hb__cpace_new(p->share_id, nonce_g, code, lsec, NULL);
    sodium_memzero(lsec, sizeof lsec);
    if (!p->pk || !p->share_id || !p->server_url) {
        hb_share_peer_free(p);
        return NULL;
    }
    p->member = false;
    p->pg = 1;
    /* PAKE1 = nonce_g || Ya（明文） */
    {
        uint8_t pl[48];
        memcpy(pl, nonce_g, 16);
        memcpy(pl + 16, p->pk->Y, 32);
        peer_send(p, HB_FR_PAKE1, NULL, pl, 48);
    }
    if (err) *err = HB_OK;
    return p;
}

const char *hb_share_peer_share_id(hb_share_peer *p)
{
    return p ? p->share_id : NULL;
}

const char *hb_share_peer_server(hb_share_peer *p)
{
    return p ? p->server_url : NULL;
}

const char *hb_share_peer_title(hb_share_peer *p)
{
    return p ? p->title : NULL;
}

uint32_t hb_share_peer_self_id(hb_share_peer *p)
{
    return p ? p->self_id : 0;
}

hb_vt *hb_share_peer_vt(hb_share_peer *p)
{
    return p ? p->vt : NULL;
}

int hb_share_peer_perm(hb_share_peer *p)
{
    return p ? p->perm : HB_SHARE_VIEW;
}

int hb_share_peer_fd(hb_share_peer *p) { return p ? p->q.r : -1; }

bool hb_share_peer_next_frame(hb_share_peer *p, hb_share_frame *out)
{
    bool r;
    if (!p)
        return false;
    pthread_mutex_lock(&p->mu);
    r = outq_pop_frame(&p->q, out);
    pthread_mutex_unlock(&p->mu);
    return r;
}

bool hb_share_peer_next_event(hb_share_peer *p, hb_share_event *ev)
{
    bool r;
    if (!p)
        return false;
    pthread_mutex_lock(&p->mu);
    r = outq_pop_event(&p->q, ev);
    pthread_mutex_unlock(&p->mu);
    return r;
}

int64_t hb_share_peer_next_ms(hb_share_peer *p)
{
    /* SNAP_REQ 限流由到达驱动，无需定时器；保留接口便于平台统一。 */
    (void)p;
    return -1;
}

void hb_share_peer_tick(hb_share_peer *p) { (void)p; }

hb_err hb_share_peer_input(hb_share_peer *p, const char *bytes, size_t len)
{
    hb_err e = HB_OK;
    if (!p || !bytes)
        return HB_ERR_INVALID;
    pthread_mutex_lock(&p->mu);
    if (p->ended || !p->self_id ||
        p->perm != HB_SHARE_COLLAB || !p->k_in) {
        e = HB_ERR_INVALID;
    } else {
        peer_send(p, HB_FR_INPUT, p->k_in, (const uint8_t *)bytes, len);
    }
    pthread_mutex_unlock(&p->mu);
    return e;
}

/* --------------------------- peer 入帧 --------------------------- */

static void peer_snap_req(hb_share_peer *p)
{
    int64_t now = hb_now_ms();
    if (now - p->last_req_ms < SNAP_REQ_RATE_MS || !p->k_out)
        return;
    p->last_req_ms = now;
    peer_send(p, HB_FR_SNAP_REQ, p->k_out, NULL, 0);
}

static void peer_snapshot(hb_share_peer *p, const uint8_t *pt, size_t n)
{
    int rows, cols, alt;
    uint64_t d;
    if (n < 13)
        return;
    rows = (pt[0] << 8) | pt[1];
    cols = (pt[2] << 8) | pt[3];
    alt = pt[4];
    d = get64(pt + 5);
    (void)alt;
    if (rows > 0 && cols > 0) {
        int r0 = 0, c0 = 0;
        hb_vt_size(p->vt, &r0, &c0);
        if (r0 != rows || c0 != cols)
            hb_vt_resize(p->vt, rows, cols);
    }
    /* 快照语义是重建全新终端：复位后重放，否则旧屏内容会被
     * scrollback 段挤进本端 scrollback 而逐次累积。 */
    hb_vt_hard_reset(p->vt);
    hb_vt_feed(p->vt, (const char *)pt + 13, n - 13);
    p->base_dseq = d;
    p->have_base = true;
    p->gap = false;
    outq_ev(&p->q, HB_SH_EV_SNAPSHOT, 0, (rows << 16) | cols, NULL, NULL,
            NULL);
    outq_ev(&p->q, HB_SH_EV_RESIZE, 0, (rows << 16) | cols, NULL, NULL,
            NULL);
}

static void peer_delta(hb_share_peer *p, const uint8_t *pt, size_t n)
{
    uint64_t d;
    if (n < 8 || !p->have_base)
        return;
    d = get64(pt);
    if (p->gap) {                  /* 断档中丢弃全部 DELTA，每 2 s 催一次 */
        peer_snap_req(p);
        return;
    }
    if (d <= p->base_dseq)
        return;
    if (d != p->base_dseq + 1) {
        p->gap = true;
        peer_snap_req(p);
        return;
    }
    hb_vt_feed(p->vt, (const char *)pt + 8, n - 8);
    p->base_dseq = d;
}

static void peer_frame(hb_share_peer *p, const uint8_t *data, size_t n)
{
    uint8_t type;
    uint32_t from;
    uint64_t seq;
    uint8_t *ad, *pt = NULL;
    size_t ad_len, pl = 0;
    const uint8_t *key = NULL;

    if (n < HB_FR_HDRLEN)
        return;
    type = data[0];
    from = get32(data + 5);
    seq = get64(data + 9);
    if (from != 1)                 /* Relay 只转发 owner→participant */
        return;
    pthread_mutex_lock(&p->mu);
    if (p->ended) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (seq <= p->owner_seq) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    p->owner_seq = seq;
    data += HB_FR_HDRLEN;
    n -= HB_FR_HDRLEN;

    switch (type) {
    case HB_FR_SNAPSHOT:
    case HB_FR_DELTA:
    case HB_FR_RESIZE:
    case HB_FR_PRESENCE:
    case HB_FR_NOTICE:
    case HB_FR_GRANT:
        key = p->k_out;
        break;
    case HB_FR_KEY:
        key = p->k_pake;
        break;
    case HB_FR_PAKE2:
        break;                     /* 明文 */
    default:
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (type == HB_FR_PAKE2) {
        if (p->pg == 1 && p->pk && n == 64) {
            const uint8_t *Yb = data, *tag = data + 32;
            uint8_t myY[32];
            memcpy(myY, p->pk->Y, 32);
            p->isk = sodium_malloc(64);
            if (p->isk &&
                hb__cpace_isk(p->pk, Yb, myY, Yb, p->isk) == HB_OK) {
                uint8_t tg[32];
                cpace_tag(p->isk, "owner", tg);
                if (sodium_memcmp(tg, tag, 32) == 0) {
                    uint8_t tg2[32];
                    cpace_tag(p->isk, "guest", tg2);
                    peer_send(p, HB_FR_PAKE3, NULL, tg2, 32);
                    sodium_memzero(tg2, sizeof tg2);
                    /* K_pake 立即派生并保留到 KEY 到达；ISK 用完即擦 */
                    p->k_pake = sodium_malloc(32);
                    if (p->k_pake)
                        hb__share_k_pake(p->isk, p->k_pake);
                    sodium_memzero(p->isk, 64);
                    sodium_free(p->isk);
                    p->isk = NULL;
                    p->pg = 2;
                    hb__cpace_free(p->pk);
                    p->pk = NULL;
                } else {
                    /* 配对码错误 → 本地失败 */
                    sodium_memzero(p->isk, 64);
                    sodium_free(p->isk);
                    p->isk = NULL;
                    p->ended = true;
                    outq_ev(&p->q, HB_SH_EV_PAKE_FAIL, 0, 0,
                            NULL, NULL, "配对码错误");
                    outq_ev(&p->q, HB_SH_EV_ENDED, 0, 0,
                            NULL, NULL, "pake_fail");
                }
                sodium_memzero(tg, sizeof tg);
            }
        }
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (!key) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    ad = hb__share_ad(p->share_id, data - HB_FR_HDRLEN, &ad_len);
    if (ad) {
        pt = hb_sync_open(key, data, n, ad, ad_len, &pl);
        free(ad);
    }
    if (!pt) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    switch (type) {
    case HB_FR_SNAPSHOT:
        if (p->k_out)      /* KEY 未到时 k_out NULL → 已被上面挡下 */
            peer_snapshot(p, pt, pl);
        break;
    case HB_FR_DELTA:
        peer_delta(p, pt, pl);
        break;
    case HB_FR_RESIZE:
        if (pl == 4) {
            int r = (pt[0] << 8) | pt[1], c = (pt[2] << 8) | pt[3];
            hb_vt_resize(p->vt, r, c);
            outq_ev(&p->q, HB_SH_EV_RESIZE, 0, (r << 16) | c,
                    NULL, NULL, NULL);
        }
        break;
    case HB_FR_PRESENCE: {
        char *t = xstrndup((const char *)pt, pl);
        outq_ev(&p->q, HB_SH_EV_PRESENCE, 0, 0, NULL, NULL, t);
        free(t);
        break;
    }
    case HB_FR_NOTICE: {
        char *t = xstrndup((const char *)pt, pl);
        outq_ev(&p->q, HB_SH_EV_NOTICE, 0, 0, NULL, NULL, t);
        free(t);
        break;
    }
    case HB_FR_GRANT: {
        cJSON *j = json_parse((const char *)pt, pl);
        const char *pm, *kb;
        if (j) {
            pm = json_str(j, "perm");
            kb = json_str(j, "k_in");
            if (pm && !strcmp(pm, "collab")) {
                uint8_t *kb64, *kin = NULL, *kad;
                size_t kl = 0, kinl = 0, kal;
                p->perm = HB_SHARE_COLLAB;
                kb64 = kb ? b64_dec(kb, strlen(kb), &kl) : NULL;
                if (p->vk && kb64) {
                    kal = strlen(AD_KIN) + 1 + strlen(p->share_id) + 1 + 4;
                    kad = malloc(kal);
                    memcpy(kad, AD_KIN, strlen(AD_KIN));
                    kad[strlen(AD_KIN)] = 0;
                    memcpy(kad + strlen(AD_KIN) + 1, p->share_id,
                           strlen(p->share_id));
                    kad[strlen(AD_KIN) + 1 + strlen(p->share_id)] = 0;
                    put32(kad + kal - 4, p->self_id);
                    kin = hb_sync_open(p->vk, kb64, kl, kad, kal, &kinl);
                    free(kad);
                }
                free(kb64);
                if (kin && kinl == 32) {
                    if (p->k_in) {
                        sodium_memzero(p->k_in, 32);
                        sodium_free(p->k_in);
                    }
                    p->k_in = sodium_malloc(32);
                    if (p->k_in)
                        memcpy(p->k_in, kin, 32);
                    sodium_memzero(kin, kinl);
                    sodium_free(kin);
                } else if (kin)
                    sodium_free(kin);
            } else {
                p->perm = HB_SHARE_VIEW;
                if (p->k_in) {
                    sodium_memzero(p->k_in, 32);
                    sodium_free(p->k_in);
                    p->k_in = NULL;
                }
            }
            outq_ev(&p->q, HB_SH_EV_PERM, 0, p->perm, NULL, NULL, NULL);
            cJSON_Delete(j);
        }
        break;
    }
    case HB_FR_KEY: {
        /* K_out 由 K_pake 解密；guest 进入正常流 */
        if (pl == 32 && !p->k_out) {
            p->k_out = sodium_malloc(32);
            if (p->k_out)
                memcpy(p->k_out, pt, 32);
            p->pg = 3;
            outq_ev(&p->q, HB_SH_EV_PAKE_OK, p->self_id, 0,
                    NULL, NULL, NULL);
        }
        break;
    }
    }
    sodium_free(pt);
    pthread_mutex_unlock(&p->mu);
}

static void peer_text_in(hb_share_peer *p, const char *data, size_t n)
{
    cJSON *j = json_parse(data, n);
    const char *t;
    if (!j)
        return;
    t = json_str(j, "t");
    pthread_mutex_lock(&p->mu);
    if (t && !strcmp(t, "hello")) {
        p->self_id = (uint32_t)json_int(j, "id", 0);
        if (p->want_join && p->self_id) {
            cJSON *jj = cJSON_CreateObject();
            char *txt;
            p->want_join = false;
            cJSON_AddStringToObject(jj, "device",
                                    p->device ? p->device : "");
            cJSON_AddStringToObject(jj, "name",
                                    p->name ? p->name : "");
            txt = cJSON_PrintUnformatted(jj);
            cJSON_Delete(jj);
            if (txt) {
                peer_send(p, HB_FR_JOIN, p->k_out,
                          (const uint8_t *)txt, strlen(txt));
                free(txt);
            }
        }
    } else if (t && !strcmp(t, "ended")) {
        const char *r = json_str(j, "reason");
        p->ended = true;
        outq_ev(&p->q, HB_SH_EV_ENDED, 0, 0, NULL, NULL,
                r ? r : "ended");
    } else if (t && !strcmp(t, "locked")) {
        p->ended = true;
        outq_ev(&p->q, HB_SH_EV_LOCKED, 0, 0, NULL, NULL, NULL);
        outq_ev(&p->q, HB_SH_EV_ENDED, 0, 0, NULL, NULL, "locked");
    }
    pthread_mutex_unlock(&p->mu);
    cJSON_Delete(j);
}

void hb_share_peer_incoming(hb_share_peer *p, const uint8_t *data,
                            size_t len, bool is_text)
{
    if (!p || !data)
        return;
    if (is_text)
        peer_text_in(p, (const char *)data, len);
    else
        peer_frame(p, data, len);
}

void hb_share_peer_free(hb_share_peer *p)
{
    if (!p)
        return;
    if (p->vk) {
        sodium_memzero(p->vk, 32);
        sodium_free(p->vk);
    }
    if (p->k_out) {
        sodium_memzero(p->k_out, 32);
        sodium_free(p->k_out);
    }
    if (p->k_in) {
        sodium_memzero(p->k_in, 32);
        sodium_free(p->k_in);
    }
    if (p->isk) {
        sodium_memzero(p->isk, 64);
        sodium_free(p->isk);
    }
    if (p->k_pake) {
        sodium_memzero(p->k_pake, 32);
        sodium_free(p->k_pake);
    }
    hb__cpace_free(p->pk);
    if (p->vt)
        hb_vt_free(p->vt);
    free(p->share_id);
    free(p->server_url);
    free(p->device);
    free(p->name);
    free(p->title);
    outq_free(&p->q);
    pthread_mutex_destroy(&p->mu);
    free(p);
}
