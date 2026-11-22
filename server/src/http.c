#include "srv.h"

#include <string.h>

#define HB_SRV_NOT_FOUND 1

/* ============================ IP 限流 ============================ */

typedef struct {
    int64_t start;
    int     count;
} IpWin;

bool hb_rate_limited(hb_srv *srv, SoupServerMessage *msg)
{
    GSocketAddress *ra = soup_server_message_get_remote_address(msg);
    g_autofree char *ip = NULL;
    IpWin *w;
    int64_t now = hbu_now_ms();
    if (ra && G_IS_INET_SOCKET_ADDRESS(ra))
        ip = g_inet_address_to_string(
            g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)));
    if (!ip) ip = g_strdup("unknown");
    w = g_hash_table_lookup(srv->ip_hits, ip);
    if (!w) {
        w = g_new0(IpWin, 1);
        w->start = now;
        g_hash_table_insert(srv->ip_hits, g_strdup(ip), w);
    }
    if (now - w->start > 60000) {
        w->start = now;
        w->count = 0;
    }
    if (++w->count > srv->ip_limit) {
        int64_t ra_s = (w->start + 60000 - now + 999) / 1000;
        g_autofree char *ex =
            g_strdup_printf("\"retry_after\":%lld", (long long)ra_s);
        hbu_error(msg, 429, "rate_limited",
                  "too many requests", ex);
        return true;
    }
    return false;
}

/* ============================ Bearer 认证 ============================ */

bool hb_auth_check(hb_srv *srv, SoupServerMessage *msg,
                   char **account_out, char **device_out)
{
    SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
    const char *auth = soup_message_headers_get_one(h, "Authorization");
    size_t tn = 0;
    uint8_t *tok;
    g_autofree char *hash = NULL, *dev = NULL, *acct = NULL;
    bool revoked = false, wipe = false;

    if (!auth || !g_str_has_prefix(auth, "Bearer "))
        goto unauthorized;
    tok = hbu_b64_decode(auth + 7, &tn);
    if (!tok || tn == 0) {
        g_free(tok);
        goto unauthorized;
    }
    hash = hbu_sha256_hex(tok, tn);
    sodium_memzero(tok, tn);
    g_free(tok);
    if (hbd_token_lookup(srv->db, hash, &dev, &acct) != 0)
        goto unauthorized;
    /* token 行在吊销后保留为墓碑：命中 → device_revoked + wipe */
    if (hbd_device_by_id(srv->db, dev, NULL, &revoked, &wipe) != 0)
        goto unauthorized;
    if (revoked) {
        hbu_error(msg, SOUP_STATUS_UNAUTHORIZED, "device_revoked",
                  "device revoked", wipe ? "\"wipe\":true" : "\"wipe\":false");
        return false;
    }
    hbd_device_touch(srv->db, dev, hbu_now_ms());
    *account_out = g_steal_pointer(&acct);
    *device_out = g_steal_pointer(&dev);
    return true;
unauthorized:
    hbu_error(msg, SOUP_STATUS_UNAUTHORIZED, "unauthorized",
              "invalid token", NULL);
    return false;
}

/* ============================ 账号锁定 ============================ */

static bool acct_locked(hb_srv *srv, SoupServerMessage *msg, HbAcct *a)
{
    int64_t now = hbu_now_ms();
    (void)srv;
    if (a->locked_until > now) {
        int64_t ra = (a->locked_until - now + 999) / 1000;
        g_autofree char *ex =
            g_strdup_printf("\"retry_after\":%lld", (long long)ra);
        hbu_error(msg, 429, "rate_limited",
                  "account locked", ex);
        return true;
    }
    return false;
}

static void auth_failed(hb_srv *srv, HbAcct *a)
{
    a->fail_count++;
    if (a->fail_count >= HB_SRV_FAIL_MAX) {
        int64_t d = srv->lock_base_ms << (a->fail_count - HB_SRV_FAIL_MAX);
        if (d > HB_SRV_LOCK_MAX_MS || d <= 0) d = HB_SRV_LOCK_MAX_MS;
        a->locked_until = hbu_now_ms() + d;
    }
    hbd_acct_set_lock(srv->db, a->id, a->fail_count, a->locked_until);
    hb_log(srv, 2, "auth failure account=%s fail_count=%d", a->id,
           a->fail_count);
}

/* ============================ Argon2 工作线程 ============================ */

typedef struct HashJob HashJob;
struct HashJob {
    hb_srv           *srv;
    SoupServerMessage *msg;
    GMainContext     *mc;
    int               mode;
    uint8_t           s1[64];
    size_t            n1;
    uint8_t           s2[64];
    size_t            n2;
    char             *stored;   /* verify 用库里 hash */
    bool              ok;
    char              h1[crypto_pwhash_STRBYTES];
    char              h2[crypto_pwhash_STRBYTES];
    void            (*cont)(HashJob *);
    void             *data;
    GDestroyNotify    data_free;
};

enum {
    H_VERIFY,          /* s1 vs stored → ok */
    H_HASH,            /* s1 → h1 */
    H_HASH2,           /* s1 → h1, s2 → h2 */
    H_VERIFY_THEN_HASH /* s1 vs stored → ok；ok 则 s2 → h2 */
};

static gboolean hash_done(gpointer p)
{
    HashJob *j = p;
    j->cont(j);
    return G_SOURCE_REMOVE;
}

static void hash_worker(gpointer p, gpointer u)
{
    HashJob *j = p;
    (void)u;
    switch (j->mode) {
    case H_VERIFY:
        j->ok = crypto_pwhash_str_verify(j->stored, (const char *)j->s1,
                                         j->n1) == 0;
        break;
    case H_HASH:
        j->ok = crypto_pwhash_str(j->h1, (const char *)j->s1, j->n1,
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                  crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
        break;
    case H_HASH2:
        j->ok = crypto_pwhash_str(j->h1, (const char *)j->s1, j->n1,
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                  crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0 &&
                crypto_pwhash_str(j->h2, (const char *)j->s2, j->n2,
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                  crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
        break;
    case H_VERIFY_THEN_HASH:
        j->ok = crypto_pwhash_str_verify(j->stored, (const char *)j->s1,
                                         j->n1) == 0;
        if (j->ok)
            j->ok = crypto_pwhash_str(
                        j->h2, (const char *)j->s2, j->n2,
                        crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
        break;
    }
    sodium_memzero(j->s1, sizeof j->s1);
    sodium_memzero(j->s2, sizeof j->s2);
    g_atomic_int_add(&j->srv->hash_pending, -1);
    g_main_context_invoke_full(j->mc, G_PRIORITY_DEFAULT, hash_done, j,
                               NULL);
}

static void hash_go(HashJob *j)
{
    /* 有界队列：排队+运行中超过 --hash-queue 即 429，防内存耗尽 DoS */
    if (g_atomic_int_add(&j->srv->hash_pending, 1) >=
        j->srv->hash_queue) {
        g_atomic_int_add(&j->srv->hash_pending, -1);
        hbu_error(j->msg, 429, "rate_limited", "server busy",
                  "\"retry_after\":1");
        if (j->data_free && j->data) j->data_free(j->data);
        g_free(j->stored);
        g_free(j);
        return;
    }
    soup_server_message_pause(j->msg);
    j->mc = g_main_context_ref_thread_default();
    g_thread_pool_push(j->srv->hash_pool, j, NULL);
}

static void hash_job_free(HashJob *j)
{
    soup_server_message_unpause(j->msg);
    g_main_context_unref(j->mc);
    if (j->data_free && j->data) j->data_free(j->data);
    g_free(j->stored);
    g_free(j);
}

static HashJob *job_new(hb_srv *srv, SoupServerMessage *msg, int mode)
{
    HashJob *j = g_new0(HashJob, 1);
    j->srv = srv;
    j->msg = msg;
    j->mode = mode;
    return j;
}

/* ============================ helpers ============================ */

/* 建 device + token；返回 b64url token（调用方 g_free），*dev_out = device id。 */
static char *new_session(hb_srv *srv, const char *acct, const char *name,
                         const char *platform, char **dev_out)
{
    uint8_t raw[32];
    char *dev = g_uuid_string_random();
    g_autofree char *hash;
    char *tok;
    randombytes_buf(raw, sizeof raw);
    hash = hbu_sha256_hex(raw, sizeof raw);
    hbd_device_add(srv->db, dev, acct, name, platform);
    hbd_token_add(srv->db, hash, dev, acct);
    *dev_out = dev;
    tok = hbu_b64_encode(raw, sizeof raw);
    sodium_memzero(raw, sizeof raw);
    return tok;
}

static char *json_take(JsonBuilder *b)
{
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    char *s;
    json_generator_set_root(g, root);
    s = json_generator_to_data(g, NULL);
    json_node_free(root);
    g_object_unref(g);
    return s;
}

/* {account_id, device_id, token, ...} 成功响应；extra_cb 追加字段。 */
static void respond_session(SoupServerMessage *msg, const char *acct,
                            const char *dev, const char *tok,
                            void (*extra)(JsonBuilder *, void *), void *d)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autofree char *s;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "account_id");
    json_builder_add_string_value(b, acct);
    json_builder_set_member_name(b, "device_id");
    json_builder_add_string_value(b, dev);
    json_builder_set_member_name(b, "token");
    json_builder_add_string_value(b, tok);
    if (extra) extra(b, d);
    json_builder_end_object(b);
    s = json_take(b);
    hbu_respond_json(msg, SOUP_STATUS_OK, s);
}

static bool body_str_b64(JsonObject *o, const char *key, uint8_t *out,
                         size_t want)
{
    const char *s = hbu_jstr(o, key);
    size_t n;
    uint8_t *d;
    if (!s) return false;
    d = hbu_b64_decode(s, &n);
    if (!d || n != want) {
        g_free(d);
        return false;
    }
    memcpy(out, d, want);
    sodium_memzero(d, want);
    g_free(d);
    return true;
}

/* 解析 device:{name,platform}（缺省空字符串）。out 均 malloc'd。 */
static void parse_device(JsonObject *o, char **name, char **platform)
{
    JsonNode *n = json_object_get_member(o, "device");
    JsonObject *d =
        n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
    *name = g_strdup(d ? hbu_jstr(d, "name") : NULL);
    *platform = g_strdup(d ? hbu_jstr(d, "platform") : NULL);
    if (!*name) *name = g_strdup("");
    if (!*platform) *platform = g_strdup("");
}

/* 解析 kdf:{salt,ops,mem}：salt 解出 16B。成功 true。 */
static bool parse_kdf(JsonObject *o, char **salt_b64, int64_t *ops,
                      int64_t *mem)
{
    JsonNode *n = json_object_get_member(o, "kdf");
    JsonObject *k =
        n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
    const char *s;
    size_t sn;
    uint8_t *sd;
    bool ok1, ok2;
    if (!k || !(s = hbu_jstr(k, "salt"))) return false;
    sd = hbu_b64_decode(s, &sn);
    if (!sd || sn != 16) {
        g_free(sd);
        return false;
    }
    g_free(sd);
    *ops = hbu_jint(k, "ops", 0, &ok1);
    *mem = hbu_jint(k, "mem", 0, &ok2);
    if (!ok1 || !ok2 || *ops <= 0 || *mem <= 0) return false;
    *salt_b64 = g_strdup(s);
    return true;
}

/* ============================ §6.1 handlers ============================ */

static void h_prelogin(hb_srv *srv, SoupServerMessage *msg, const char *path,
                       GHashTable *query)
{
    JsonObject *o;
    g_autofree char *email = NULL;
    HbAcct *a = NULL;
    const char *salt;
    int64_t ops, mem;
    g_autofree char *fake_salt = NULL;
    (void)path; (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (hb_rate_limited(srv, msg)) return;
    o = hbu_body_json(msg);
    email = hbu_email_norm(hbu_jstr(o, "email"));
    if (!*email) {
        hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                  "missing email", NULL);
        goto out;
    }
    if (hbd_acct_by_email(srv->db, email, &a) == 0) {
        salt = a->kdf_salt;
        ops = a->kdf_ops;
        mem = a->kdf_mem;
    } else {
        /* 未知邮箱：确定性伪造参数，不泄露账号存在性 */
        uint8_t mac[32];
        uint8_t salt16[16];
        hbu_hmac_sha256(srv->secret, 32, (const uint8_t *)email,
                        strlen(email), mac);
        memcpy(salt16, mac, 16);
        fake_salt = hbu_b64_encode(salt16, 16);
        sodium_memzero(salt16, 16);
        salt = fake_salt;
        ops = crypto_pwhash_OPSLIMIT_MODERATE;
        mem = crypto_pwhash_MEMLIMIT_MODERATE;
    }
    {
        g_autofree char *s = g_strdup_printf(
            "{\"kdf\":{\"salt\":\"%s\",\"ops\":%lld,\"mem\":%lld}}",
            salt, (long long)ops, (long long)mem);
        hbu_respond_json(msg, SOUP_STATUS_OK, s);
    }
out:
    hbd_acct_free(a);
    if (o) json_object_unref(o);
}

/* ---- register ---- */

typedef struct {
    char *email, *kdf_salt;
    int64_t kdf_ops, kdf_mem;
    char *wvk_pw, *wvk_rc;
    char *dname, *dplat;
} RegCtx;

static void reg_ctx_free(RegCtx *c)
{
    g_free(c->email); g_free(c->kdf_salt); g_free(c->wvk_pw);
    g_free(c->wvk_rc); g_free(c->dname); g_free(c->dplat);
    g_free(c);
}

static void register_done(HashJob *j)
{
    RegCtx *c = j->data;
    if (!j->ok) {
        hbu_error(j->msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, "server_error",
                  "hash failed", NULL);
        goto out;
    }
    {
        HbAcct a = {0};
        char *dev, *tok;
        g_autofree char *aid = g_uuid_string_random();
        a.id = aid;
        a.email = c->email;
        a.auth_hash = j->h1;
        a.kdf_salt = c->kdf_salt;
        a.kdf_ops = c->kdf_ops;
        a.kdf_mem = c->kdf_mem;
        a.wrapped_vk_pw = c->wvk_pw;
        a.rc_hash = j->h2;
        a.wrapped_vk_rc = c->wvk_rc;
        a.created = hbu_now_ms();
        hbd_acct_insert(j->srv->db, &a);
        tok = new_session(j->srv, a.id, c->dname, c->dplat, &dev);
        respond_session(j->msg, a.id, dev, tok, NULL, NULL);
        hb_log(j->srv, 2, "register account=%s", a.id);
        sodium_memzero(tok, strlen(tok));
        g_free(tok);
        g_free(dev);
    }
out:
    hash_job_free(j);
}

static void h_register(hb_srv *srv, SoupServerMessage *msg, const char *path,
                       GHashTable *query)
{
    JsonObject *o;
    RegCtx *c = NULL;
    HashJob *j;
    uint8_t auth_key[32], rc_auth[32];
    (void)path; (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (hb_rate_limited(srv, msg)) return;
    o = hbu_body_json(msg);
    c = g_new0(RegCtx, 1);
    if (!o || !body_str_b64(o, "auth_key", auth_key, 32) ||
        !body_str_b64(o, "rc_auth", rc_auth, 32) ||
        !parse_kdf(o, &c->kdf_salt, &c->kdf_ops, &c->kdf_mem) ||
        !(c->wvk_pw = g_strdup(hbu_jstr(o, "wrapped_vk_pw"))) ||
        !(c->wvk_rc = g_strdup(hbu_jstr(o, "wrapped_vk_rc"))))
        goto bad;
    {
        size_t n;
        uint8_t *d = hbu_b64_decode(c->wvk_pw, &n);
        if (!d || n == 0) { g_free(d); goto bad; }
        g_free(d);
        d = hbu_b64_decode(c->wvk_rc, &n);
        if (!d || n == 0) { g_free(d); goto bad; }
        g_free(d);
    }
    c->email = hbu_email_norm(hbu_jstr(o, "email"));
    if (!*c->email) goto bad;
    parse_device(o, &c->dname, &c->dplat);
    {
        HbAcct *a = NULL;
        if (hbd_acct_by_email(srv->db, c->email, &a) == 0) {
            hbd_acct_free(a);
            hbu_error(msg, SOUP_STATUS_CONFLICT, "email_taken",
                      "email already registered", NULL);
            goto done;
        }
    }
    j = job_new(srv, msg, H_HASH2);
    memcpy(j->s1, auth_key, 32); j->n1 = 32;
    memcpy(j->s2, rc_auth, 32);  j->n2 = 32;
    j->cont = register_done;
    j->data = c;
    j->data_free = (GDestroyNotify)reg_ctx_free;
    sodium_memzero(auth_key, 32);
    sodium_memzero(rc_auth, 32);
    if (o) json_object_unref(o);
    hash_go(j);
    return;
bad:
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed register request", NULL);
done:
    sodium_memzero(auth_key, 32);
    sodium_memzero(rc_auth, 32);
    reg_ctx_free(c);
    if (o) json_object_unref(o);
}

/* ---- login ---- */

typedef struct {
    HbAcct *acct;       /* NULL = 邮箱不存在（仍按 dummy 校验） */
    char   *totp;
    char   *dname, *dplat;
} LoginCtx;

static void login_ctx_free(LoginCtx *c)
{
    hbd_acct_free(c->acct);
    g_free(c->totp); g_free(c->dname); g_free(c->dplat);
    g_free(c);
}

static void login_extra(JsonBuilder *b, void *d)
{
    HbAcct *a = d;
    json_builder_set_member_name(b, "wrapped_vk_pw");
    json_builder_add_string_value(b, a->wrapped_vk_pw);
    json_builder_set_member_name(b, "totp_enabled");
    json_builder_add_boolean_value(b, a->totp_enabled);
}

static void login_done(HashJob *j)
{
    LoginCtx *c = j->data;
    hb_srv *srv = j->srv;
    if (!c->acct || !j->ok) {
        /* 不区分账号是否存在 */
        if (c->acct) auth_failed(srv, c->acct);
        hbu_error(j->msg, SOUP_STATUS_UNAUTHORIZED, "invalid_credentials",
                  "invalid credentials", NULL);
        goto out;
    }
    if (c->acct->totp_enabled) {
        int64_t step;
        if (!c->totp) {
            /* 口令对、缺 TOTP：totp_required，不计失败 */
            hbu_error(j->msg, SOUP_STATUS_UNAUTHORIZED, "totp_required",
                      "totp code required", NULL);
            goto out;
        }
        if (!hbu_totp_check(c->acct->totp_secret, 20, c->totp, hbu_now_ms(),
                            c->acct->totp_last_step, &step)) {
            auth_failed(srv, c->acct);
            hbu_error(j->msg, SOUP_STATUS_UNAUTHORIZED, "totp_invalid",
                      "invalid totp code", NULL);
            goto out;
        }
        hbd_acct_set_totp(srv->db, c->acct->id, c->acct->totp_secret,
                          c->acct->totp_pending, true, step);
    }
    hbd_acct_set_lock(srv->db, c->acct->id, 0, 0);
    {
        char *dev, *tok;
        tok = new_session(srv, c->acct->id, c->dname, c->dplat, &dev);
        respond_session(j->msg, c->acct->id, dev, tok, login_extra, c->acct);
        hb_log(srv, 2, "login account=%s", c->acct->id);
        sodium_memzero(tok, strlen(tok));
        g_free(tok);
        g_free(dev);
    }
out:
    hash_job_free(j);
}

static void h_login(hb_srv *srv, SoupServerMessage *msg, const char *path,
                    GHashTable *query)
{
    JsonObject *o;
    LoginCtx *c;
    HashJob *j;
    uint8_t auth_key[32];
    (void)path; (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (hb_rate_limited(srv, msg)) return;
    o = hbu_body_json(msg);
    c = g_new0(LoginCtx, 1);
    if (!o || !body_str_b64(o, "auth_key", auth_key, 32))
        goto bad;
    {
        g_autofree char *email = hbu_email_norm(hbu_jstr(o, "email"));
        if (!*email) goto bad;
        hbd_acct_by_email(srv->db, email, &c->acct); /* 不存在 → NULL */
    }
    c->totp = g_strdup(hbu_jstr(o, "totp"));
    parse_device(o, &c->dname, &c->dplat);
    if (c->acct && acct_locked(srv, msg, c->acct)) {
        login_ctx_free(c);
        if (o) json_object_unref(o);
        return;
    }
    j = job_new(srv, msg, H_VERIFY);
    memcpy(j->s1, auth_key, 32); j->n1 = 32;
    /* 未知邮箱也走真实校验，耗时一致，不泄露账号存在性 */
    j->stored = g_strdup(c->acct ? c->acct->auth_hash : srv->dummy_hash);
    j->cont = login_done;
    j->data = c;
    j->data_free = (GDestroyNotify)login_ctx_free;
    sodium_memzero(auth_key, 32);
    if (o) json_object_unref(o);
    hash_go(j);
    return;
bad:
    sodium_memzero(auth_key, 32);
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed login request", NULL);
    login_ctx_free(c);
    if (o) json_object_unref(o);
}

/* ---- recover ---- */

typedef struct {
    HbAcct *acct;
} RcStartCtx;

static void rc_start_ctx_free(RcStartCtx *c)
{
    hbd_acct_free(c->acct);
    g_free(c);
}

static void rc_start_done(HashJob *j)
{
    RcStartCtx *c = j->data;
    hb_srv *srv = j->srv;
    if (!c->acct || !j->ok) {
        if (c->acct) auth_failed(srv, c->acct);
        hbu_error(j->msg, SOUP_STATUS_UNAUTHORIZED, "invalid_credentials",
                  "invalid credentials", NULL);
        goto out;
    }
    hbd_acct_set_lock(srv->db, c->acct->id, 0, 0);
    {
        uint8_t raw[32];
        g_autofree char *hash, *tok;
        randombytes_buf(raw, 32);
        hash = hbu_sha256_hex(raw, 32);
        hbd_recovery_add(srv->db, hash, c->acct->id,
                         hbu_now_ms() + 600000); /* 10 min */
        tok = hbu_b64_encode(raw, 32);
        sodium_memzero(raw, 32);
        {
            g_autofree char *s = g_strdup_printf(
                "{\"recovery_token\":\"%s\",\"wrapped_vk_rc\":\"%s\"}",
                tok, c->acct->wrapped_vk_rc);
            hbu_respond_json(j->msg, SOUP_STATUS_OK, s);
        }
        sodium_memzero(tok, strlen(tok));
    }
    hb_log(srv, 2, "recover/start account=%s", c->acct->id);
out:
    hash_job_free(j);
}

typedef struct {
    char *acct_id;
    char *rec_hash;
    char *kdf_salt;
    int64_t kdf_ops, kdf_mem;
    char *wvk_pw;
    char *dname, *dplat;
} RcFinishCtx;

static void rc_finish_ctx_free(RcFinishCtx *c)
{
    g_free(c->acct_id); g_free(c->rec_hash); g_free(c->kdf_salt);
    g_free(c->wvk_pw); g_free(c->dname); g_free(c->dplat);
    g_free(c);
}

static void rc_finish_done(HashJob *j)
{
    RcFinishCtx *c = j->data;
    hb_srv *srv = j->srv;
    if (!j->ok) {
        hbu_error(j->msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, "server_error",
                  "hash failed", NULL);
        goto out;
    }
    hbd_acct_set_auth(srv->db, c->acct_id, j->h1, c->kdf_salt, c->kdf_ops,
                      c->kdf_mem, c->wvk_pw);
    hbd_recovery_del(srv->db, c->rec_hash);
    {
        char *dev, *tok;
        tok = new_session(srv, c->acct_id, c->dname, c->dplat, &dev);
        respond_session(j->msg, c->acct_id, dev, tok, NULL, NULL);
        hb_log(srv, 2, "recover/finish account=%s", c->acct_id);
        sodium_memzero(tok, strlen(tok));
        g_free(tok);
        g_free(dev);
    }
out:
    hash_job_free(j);
}

static void h_recover(hb_srv *srv, SoupServerMessage *msg, const char *path,
                      GHashTable *query)
{
    JsonObject *o;
    (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (hb_rate_limited(srv, msg)) return;
    o = hbu_body_json(msg);

    if (!strcmp(path, "/v1/recover/start")) {
        uint8_t rc_auth[32];
        g_autofree char *email = NULL;
        RcStartCtx *c;
        HashJob *j;
        if (!o || !body_str_b64(o, "rc_auth", rc_auth, 32))
            goto bad;
        email = hbu_email_norm(hbu_jstr(o, "email"));
        if (!*email) goto bad;
        c = g_new0(RcStartCtx, 1);
        hbd_acct_by_email(srv->db, email, &c->acct);
        if (c->acct && acct_locked(srv, msg, c->acct)) {
            hbd_acct_free(c->acct);
            g_free(c);
            json_object_unref(o);
            return;
        }
        j = job_new(srv, msg, H_VERIFY);
        memcpy(j->s1, rc_auth, 32); j->n1 = 32;
        j->stored = g_strdup(c->acct ? c->acct->rc_hash : srv->dummy_hash);
        j->cont = rc_start_done;
        j->data = c;
        j->data_free = (GDestroyNotify)rc_start_ctx_free;
        sodium_memzero(rc_auth, 32);
        json_object_unref(o);
        hash_go(j);
        return;
    }
    if (!strcmp(path, "/v1/recover/finish")) {
        uint8_t auth_key[32], rec[64];
        size_t rn = 0;
        uint8_t *recraw;
        RcFinishCtx *c;
        HashJob *j;
        if (!o) goto bad;
        c = g_new0(RcFinishCtx, 1);
        recraw = hbu_b64_decode(hbu_jstr(o, "recovery_token"), &rn);
        if (!recraw || rn == 0 ||
            !body_str_b64(o, "auth_key", auth_key, 32) ||
            !parse_kdf(o, &c->kdf_salt, &c->kdf_ops, &c->kdf_mem) ||
            !(c->wvk_pw = g_strdup(hbu_jstr(o, "wrapped_vk_pw")))) {
            g_free(recraw);
            sodium_memzero(auth_key, 32);
            goto bad_free;
        }
        (void)rec;
        c->rec_hash = hbu_sha256_hex(recraw, rn);
        sodium_memzero(recraw, rn);
        g_free(recraw);
        if (hbd_recovery_lookup(srv->db, c->rec_hash, &c->acct_id) != 0) {
            sodium_memzero(auth_key, 32);
            rc_finish_ctx_free(c);
            hbu_error(msg, SOUP_STATUS_UNAUTHORIZED, "invalid_credentials",
                      "invalid credentials", NULL);
            goto out;
        }
        parse_device(o, &c->dname, &c->dplat);
        j = job_new(srv, msg, H_HASH);
        memcpy(j->s1, auth_key, 32); j->n1 = 32;
        j->cont = rc_finish_done;
        j->data = c;
        j->data_free = (GDestroyNotify)rc_finish_ctx_free;
        sodium_memzero(auth_key, 32);
        json_object_unref(o);
        hash_go(j);
        return;
    bad_free:
        rc_finish_ctx_free(c);
        goto bad;
    }
    hbu_error(msg, SOUP_STATUS_NOT_FOUND, "bad_request", "unknown path",
              NULL);
    goto out;
bad:
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed recover request", NULL);
out:
    if (o) json_object_unref(o);
}

/* ============================ §6.2 handlers ============================ */

/* ---- POST /password ---- */

typedef struct {
    char *acct_id;
    char *kdf_salt;
    int64_t kdf_ops, kdf_mem;
    char *wvk_pw;
} PwCtx;

static void pw_ctx_free(PwCtx *c)
{
    g_free(c->acct_id); g_free(c->kdf_salt); g_free(c->wvk_pw);
    g_free(c);
}

static void password_done(HashJob *j)
{
    PwCtx *c = j->data;
    if (!j->ok) {
        hbu_error(j->msg, SOUP_STATUS_UNAUTHORIZED, "invalid_credentials",
                  "invalid credentials", NULL);
        goto out;
    }
    hbd_acct_set_auth(j->srv->db, c->acct_id, j->h2, c->kdf_salt, c->kdf_ops,
                      c->kdf_mem, c->wvk_pw);
    hbu_respond_json(j->msg, SOUP_STATUS_OK, "{}");
    hb_log(j->srv, 2, "password changed account=%s", c->acct_id);
out:
    hash_job_free(j);
}

static void h_password(hb_srv *srv, SoupServerMessage *msg, const char *path,
                       GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    JsonObject *o;
    PwCtx *c;
    HashJob *j;
    HbAcct *a = NULL;
    uint8_t old_k[32], new_k[32];
    (void)path; (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;
    o = hbu_body_json(msg);
    c = g_new0(PwCtx, 1);
    c->acct_id = g_steal_pointer(&acct);
    if (!o || !body_str_b64(o, "auth_key_old", old_k, 32) ||
        !body_str_b64(o, "auth_key", new_k, 32) ||
        !parse_kdf(o, &c->kdf_salt, &c->kdf_ops, &c->kdf_mem) ||
        !(c->wvk_pw = g_strdup(hbu_jstr(o, "wrapped_vk_pw"))) ||
        hbd_acct_by_id(srv->db, c->acct_id, &a) != 0)
        goto bad;
    j = job_new(srv, msg, H_VERIFY_THEN_HASH);
    memcpy(j->s1, old_k, 32); j->n1 = 32;
    memcpy(j->s2, new_k, 32); j->n2 = 32;
    j->stored = g_strdup(a->auth_hash);
    j->cont = password_done;
    j->data = c;
    j->data_free = (GDestroyNotify)pw_ctx_free;
    hbd_acct_free(a);
    sodium_memzero(old_k, 32);
    sodium_memzero(new_k, 32);
    json_object_unref(o);
    hash_go(j);
    return;
bad:
    sodium_memzero(old_k, 32);
    sodium_memzero(new_k, 32);
    hbd_acct_free(a);
    pw_ctx_free(c);
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed password request", NULL);
    if (o) json_object_unref(o);
}

/* ---- POST /recovery：恢复码轮换 ---- */

typedef struct {
    char *acct_id;
    char *wvk_rc;
} RrotCtx;

static void rrot_ctx_free(RrotCtx *c)
{
    g_free(c->acct_id); g_free(c->wvk_rc); g_free(c);
}

static void recovery_done(HashJob *j)
{
    RrotCtx *c = j->data;
    hb_srv *srv = j->srv;
    if (!j->ok) {
        /* auth_key 错 → 计入账号失败限流 */
        HbAcct *a = NULL;
        if (hbd_acct_by_id(srv->db, c->acct_id, &a) == 0) {
            auth_failed(srv, a);
            hbd_acct_free(a);
        }
        hbu_error(j->msg, SOUP_STATUS_UNAUTHORIZED, "invalid_credentials",
                  "invalid credentials", NULL);
        goto out;
    }
    hbd_acct_set_rc(srv->db, c->acct_id, j->h2, c->wvk_rc);
    hbu_respond_json(j->msg, SOUP_STATUS_OK, "{}");
    hb_log(srv, 2, "recovery rotated account=%s", c->acct_id);
out:
    hash_job_free(j);
}

static void h_recovery(hb_srv *srv, SoupServerMessage *msg, const char *path,
                       GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    JsonObject *o;
    RrotCtx *c;
    HashJob *j;
    HbAcct *a = NULL;
    uint8_t auth_key[32], rc_auth[32];
    (void)path; (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;
    o = hbu_body_json(msg);
    c = g_new0(RrotCtx, 1);
    c->acct_id = g_steal_pointer(&acct);
    if (!o || !body_str_b64(o, "auth_key", auth_key, 32) ||
        !body_str_b64(o, "rc_auth", rc_auth, 32) ||
        !(c->wvk_rc = g_strdup(hbu_jstr(o, "wrapped_vk_rc"))) ||
        hbd_acct_by_id(srv->db, c->acct_id, &a) != 0)
        goto bad;
    if (acct_locked(srv, msg, a)) goto bad_locked;
    j = job_new(srv, msg, H_VERIFY_THEN_HASH);
    memcpy(j->s1, auth_key, 32); j->n1 = 32;
    memcpy(j->s2, rc_auth, 32);  j->n2 = 32;
    j->stored = g_strdup(a->auth_hash);
    j->cont = recovery_done;
    j->data = c;
    j->data_free = (GDestroyNotify)rrot_ctx_free;
    hbd_acct_free(a);
    sodium_memzero(auth_key, 32);
    sodium_memzero(rc_auth, 32);
    json_object_unref(o);
    hash_go(j);
    return;
bad:
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed recovery request", NULL);
bad_locked:
    sodium_memzero(auth_key, 32);
    sodium_memzero(rc_auth, 32);
    hbd_acct_free(a);
    rrot_ctx_free(c);
    if (o) json_object_unref(o);
}

/* ---- POST /logout ---- */

static void h_logout(hb_srv *srv, SoupServerMessage *msg, const char *path,
                     GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    (void)path; (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;
    /* 吊销当前 token/设备（保留墓碑），WS 收到 revoked 并关闭 */
    hbd_device_revoke(srv->db, acct, dev, false);
    hb_ws_notify_revoked(srv, dev, false);
    hb_log(srv, 2, "logout account=%s", acct);
    hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
}

/* ---- /devices ---- */

static void h_devices(hb_srv *srv, SoupServerMessage *msg, const char *path,
                      GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    const char *method = soup_server_message_get_method(msg);
    (void)query;
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;

    if (!strcmp(path, "/v1/devices") && !strcmp(method, SOUP_METHOD_GET)) {
        GArray *ds = hbd_devices_list(srv->db, acct);
        g_autoptr(JsonBuilder) b = json_builder_new();
        g_autofree char *s;
        guint i;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "devices");
        json_builder_begin_array(b);
        for (i = 0; i < ds->len; i++) {
            HbDevice *d = &g_array_index(ds, HbDevice, i);
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "id");
            json_builder_add_string_value(b, d->id);
            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, d->name ? d->name : "");
            json_builder_set_member_name(b, "platform");
            json_builder_add_string_value(b, d->platform ? d->platform : "");
            json_builder_set_member_name(b, "created");
            json_builder_add_int_value(b, d->created);
            json_builder_set_member_name(b, "last_seen");
            json_builder_add_int_value(b, d->last_seen);
            json_builder_set_member_name(b, "current");
            json_builder_add_boolean_value(b, !strcmp(d->id, dev));
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_end_object(b);
        hbd_devices_free(ds);
        s = json_take(b);
        hbu_respond_json(msg, SOUP_STATUS_OK, s);
        return;
    }

    /* POST /v1/devices/{id}/revoke */
    if (g_str_has_prefix(path, "/v1/devices/") &&
        g_str_has_suffix(path, "/revoke") &&
        !strcmp(method, SOUP_METHOD_POST)) {
        g_autofree char *id =
            g_strndup(path + strlen("/v1/devices/"),
                      strlen(path) - strlen("/v1/devices/") -
                          strlen("/revoke"));
        JsonObject *o = hbu_body_json(msg);
        bool wipe = false;
        if (!*id || strchr(id, '/')) {
            hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                      "bad device id", NULL);
            goto rd_out;
        }
        if (!strcmp(id, dev)) {
            hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                      "cannot revoke current device (use /logout)", NULL);
            goto rd_out;
        }
        if (o) {
            JsonNode *n = json_object_get_member(o, "wipe");
            if (n && JSON_NODE_HOLDS_VALUE(n) &&
                json_node_get_value_type(n) == G_TYPE_BOOLEAN)
                wipe = json_node_get_boolean(n);
        }
        if (hbd_device_revoke(srv->db, acct, id, wipe) != 0) {
            hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                      "unknown device", NULL);
            goto rd_out;
        }
        hb_ws_notify_revoked(srv, id, wipe);
        hb_log(srv, 2, "device revoked account=%s device=%s", acct, id);
        hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
    rd_out:
        if (o) json_object_unref(o);
        return;
    }
    hbu_error(msg, SOUP_STATUS_NOT_FOUND, "bad_request", "unknown path",
              NULL);
}

/* ---- /totp ---- */

static void h_totp(hb_srv *srv, SoupServerMessage *msg, const char *path,
                   GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    HbAcct *a = NULL;
    JsonObject *o;
    (void)query;
    if (strcmp(soup_server_message_get_method(msg), SOUP_METHOD_POST)) {
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
        return;
    }
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;
    o = hbu_body_json(msg);

    if (!strcmp(path, "/v1/totp/setup")) {
        uint8_t sec[20];
        g_autofree char *b32 = NULL, *esc_email = NULL, *s = NULL;
        randombytes_buf(sec, sizeof sec);
        if (hbd_acct_by_id(srv->db, acct, &a) != 0) goto bad;
        /* 存为待启用；已启用状态不变 */
        hbd_acct_set_totp(srv->db, acct, sec, true, a->totp_enabled, -1);
        b32 = hbu_base32_encode(sec, sizeof sec);
        sodium_memzero(sec, sizeof sec);
        esc_email = g_uri_escape_string(a->email, NULL, FALSE);
        s = g_strdup_printf(
            "{\"secret\":\"%s\",\"uri\":\"otpauth://totp/Harbor:%s"
            "?secret=%s&issuer=Harbor\"}",
            b32, esc_email, b32);
        hbu_respond_json(msg, SOUP_STATUS_OK, s);
        goto out;
    }

    if (!strcmp(path, "/v1/totp/enable") ||
        !strcmp(path, "/v1/totp/disable")) {
        bool enable = strstr(path, "enable") != NULL;
        const char *code;
        int64_t step;
        if (hbd_acct_by_id(srv->db, acct, &a) != 0) goto bad;
        code = hbu_jstr(o, "code");
        if (!a->totp_pending || !code ||
            !hbu_totp_check(a->totp_secret, 20, code, hbu_now_ms(),
                            a->totp_last_step, &step)) {
            auth_failed(srv, a);
            hbu_error(msg, SOUP_STATUS_UNAUTHORIZED, "totp_invalid",
                      "invalid totp code", NULL);
            goto out;
        }
        if (enable)
            hbd_acct_set_totp(srv->db, acct, a->totp_secret, true, true,
                              step);
        else
            hbd_acct_set_totp(srv->db, acct, NULL, false, false, -1);
        hbu_respond_json(msg, SOUP_STATUS_OK, "{}");
        goto out;
    }

    hbu_error(msg, SOUP_STATUS_NOT_FOUND, "bad_request", "unknown path",
              NULL);
    goto out;
bad:
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed totp request", NULL);
out:
    hbd_acct_free(a);
    if (o) json_object_unref(o);
}

/* ---- /objects ---- */

static void h_objects_get(hb_srv *srv, SoupServerMessage *msg,
                          GHashTable *query, const char *acct)
{
    const char *since_s = query ? g_hash_table_lookup(query, "since") : NULL;
    const char *limit_s = query ? g_hash_table_lookup(query, "limit") : NULL;
    int64_t since = 0, limit = HB_SRV_PULL_MAX;
    GArray *rows;
    bool more;
    int64_t next;
    g_autoptr(JsonBuilder) b = NULL;
    g_autofree char *s = NULL;
    guint i, n;

    if (since_s) {
        char *end;
        since = g_ascii_strtoll(since_s, &end, 10);
        if (*end || since < 0) {
            hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                      "bad since", NULL);
            return;
        }
    }
    if (limit_s) {
        char *end;
        limit = g_ascii_strtoll(limit_s, &end, 10);
        if (*end || limit < 1 || limit > HB_SRV_PULL_MAX) {
            hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
                      "bad limit", NULL);
            return;
        }
    }
    rows = hbd_objects_since(srv->db, acct, since, limit + 1);
    more = rows->len > (guint)limit;
    n = more ? (guint)limit : rows->len;
    next = n ? g_array_index(rows, HbObject, n - 1).seq : since;

    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "objects");
    json_builder_begin_array(b);
    for (i = 0; i < n; i++) {
        HbObject *o = &g_array_index(rows, HbObject, i);
        g_autofree char *b64 = hbu_b64_encode(o->blob, o->blob_n);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id");
        json_builder_add_string_value(b, o->id);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, o->type);
        json_builder_set_member_name(b, "seq");
        json_builder_add_int_value(b, o->seq);
        json_builder_set_member_name(b, "blob");
        json_builder_add_string_value(b, b64);
        json_builder_set_member_name(b, "updated_at");
        json_builder_add_int_value(b, o->updated_at);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "next");
    json_builder_add_int_value(b, next);
    json_builder_set_member_name(b, "more");
    json_builder_add_boolean_value(b, more);
    json_builder_end_object(b);
    hbd_objects_free(rows);
    s = json_take(b);
    hbu_respond_json(msg, SOUP_STATUS_OK, s);
}

typedef struct {
    char    *id, *type;
    int64_t  base_seq;
    uint8_t *blob;
    size_t   blob_n;
} Change;

static void h_objects_post(hb_srv *srv, SoupServerMessage *msg,
                           const char *acct)
{
    JsonObject *o = hbu_body_json(msg);
    JsonNode *chn = o ? json_object_get_member(o, "changes") : NULL;
    JsonArray *arr =
        chn && JSON_NODE_HOLDS_ARRAY(chn) ? json_node_get_array(chn) : NULL;
    Change *chs = NULL;
    GArray *accepted = NULL;
    guint n, i;
    g_autoptr(JsonBuilder) b = NULL;
    g_autofree char *s = NULL;

    if (!arr || (n = json_array_get_length(arr)) == 0 ||
        n > HB_SRV_PUSH_MAX)
        goto bad;
    chs = g_new0(Change, n);
    for (i = 0; i < n; i++) {
        JsonNode *in = json_array_get_element(arr, i);
        JsonObject *io =
            in && JSON_NODE_HOLDS_OBJECT(in) ? json_node_get_object(in)
                                             : NULL;
        const char *id, *type, *blob;
        bool okb;
        if (!io || !hbu_valid_id((id = hbu_jstr(io, "id"))) ||
            !hbu_valid_type((type = hbu_jstr(io, "type"))) ||
            !(blob = hbu_jstr(io, "blob")))
            goto bad;
        chs[i].id = g_strdup(id);
        chs[i].type = g_strdup(type);
        chs[i].base_seq = hbu_jint(io, "base_seq", -1, &okb);
        if (!okb || chs[i].base_seq < 0) goto bad;
        chs[i].blob = hbu_b64_decode(blob, &chs[i].blob_n);
        if (!chs[i].blob || chs[i].blob_n == 0 ||
            chs[i].blob_n > HB_SRV_BLOB_MAX)
            goto bad;
    }
    /* 整批一个事务（逐条 CAS，per-item 结果；冲突项不写库）。 */
    if (sqlite3_exec(srv->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) !=
        SQLITE_OK)
        goto bad;
    accepted = g_array_new(FALSE, TRUE, sizeof(int64_t));
    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "results");
    json_builder_begin_array(b);
    for (i = 0; i < n; i++) {
        int64_t cur = hbd_object_seq(srv->db, acct, chs[i].id);
        int64_t eff = cur < 0 ? 0 : cur;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id");
        json_builder_add_string_value(b, chs[i].id);
        if (eff == chs[i].base_seq) {
            int64_t seq = hbd_seq_next(srv->db, acct);
            hbd_object_put(srv->db, acct, chs[i].id, chs[i].type, seq,
                           chs[i].blob, chs[i].blob_n, hbu_now_ms());
            g_array_append_val(accepted, seq);
            json_builder_set_member_name(b, "ok");
            json_builder_add_boolean_value(b, true);
            json_builder_set_member_name(b, "seq");
            json_builder_add_int_value(b, seq);
        } else {
            HbObject co = {0};
            json_builder_set_member_name(b, "ok");
            json_builder_add_boolean_value(b, false);
            json_builder_set_member_name(b, "conflict");
            json_builder_begin_object(b);
            if (cur >= 0 &&
                hbd_object_get(srv->db, acct, chs[i].id, &co) == 0) {
                g_autofree char *b64 = hbu_b64_encode(co.blob, co.blob_n);
                json_builder_set_member_name(b, "seq");
                json_builder_add_int_value(b, co.seq);
                json_builder_set_member_name(b, "blob");
                json_builder_add_string_value(b, b64);
                json_builder_set_member_name(b, "type");
                json_builder_add_string_value(b, co.type);
                g_free(co.id); g_free(co.type); g_free(co.blob);
            } else {
                /* 对象不存在且 base_seq != 0：协议不变量被破坏
                 * （服务端不删对象），回 seq:0 冲突。 */
                json_builder_set_member_name(b, "seq");
                json_builder_add_int_value(b, 0);
            }
            json_builder_end_object(b);
        }
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    if (sqlite3_exec(srv->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(srv->db, "ROLLBACK", NULL, NULL, NULL);
        goto bad;
    }
    /* 每条已接受的变更广播 changed（含发起者自己的 socket） */
    for (i = 0; i < accepted->len; i++)
        hb_ws_notify_changed(srv, acct,
                             g_array_index(accepted, int64_t, i));
    g_array_free(accepted, TRUE);
    s = json_take(b);
    for (i = 0; i < n; i++) {
        g_free(chs[i].id); g_free(chs[i].type); g_free(chs[i].blob);
    }
    g_free(chs);
    json_object_unref(o);
    hbu_respond_json(msg, SOUP_STATUS_OK, s);
    return;
bad:
    if (accepted) g_array_free(accepted, TRUE);
    if (chs)
        for (i = 0; i < n; i++) {
            g_free(chs[i].id); g_free(chs[i].type); g_free(chs[i].blob);
        }
    g_free(chs);
    hbu_error(msg, SOUP_STATUS_BAD_REQUEST, "bad_request",
              "malformed objects request", NULL);
    if (o) json_object_unref(o);
}

static void h_objects(hb_srv *srv, SoupServerMessage *msg, const char *path,
                      GHashTable *query)
{
    g_autofree char *acct = NULL, *dev = NULL;
    const char *method = soup_server_message_get_method(msg);
    (void)path;
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;
    if (!strcmp(method, SOUP_METHOD_GET))
        h_objects_get(srv, msg, query, acct);
    else if (!strcmp(method, SOUP_METHOD_POST))
        h_objects_post(srv, msg, acct);
    else
        soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
                                       NULL);
}

/* ============================ WebSocket ============================ */

static gboolean ws_ping_check(gpointer p)
{
    WsClient *c = p;
    int64_t now = hbu_now_ms();
    if (now - c->last_pong > 65000) {
        /* 连续 2 个 ping 未回 → 断开 */
        soup_websocket_connection_close(c->conn, 1000, NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void ws_gone(WsClient *c)
{
    c->dead = true;
    g_ptr_array_remove(c->srv->sockets, c); /* free via element free func */
}

static void on_ws_pong(SoupWebsocketConnection *conn, GBytes *data,
                       gpointer u)
{
    WsClient *c = u;
    (void)conn; (void)data;
    c->last_pong = hbu_now_ms();
}

static void on_ws_closed(SoupWebsocketConnection *conn, gpointer u)
{
    (void)conn;
    ws_gone(u);
}

static void on_ws_error(SoupWebsocketConnection *conn, GError *err,
                        gpointer u)
{
    (void)conn; (void)err;
    ws_gone(u);
}

void hb_ws_client_free(WsClient *c)
{
    if (c->ping_src) g_source_remove(c->ping_src);
    g_object_unref(c->conn);
    g_free(c->account_id);
    g_free(c->device_id);
    g_free(c);
}

void hb_ws_notify_changed(hb_srv *srv, const char *account, int64_t seq)
{
    guint i;
    g_autofree char *s = g_strdup_printf("{\"t\":\"changed\",\"seq\":%lld}",
                                         (long long)seq);
    for (i = 0; i < srv->sockets->len; i++) {
        WsClient *c = g_ptr_array_index(srv->sockets, i);
        if (!strcmp(c->account_id, account))
            soup_websocket_connection_send_text(c->conn, s);
    }
}

static gboolean ws_delayed_close(gpointer p)
{
    soup_websocket_connection_close(p, 1000, NULL);
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

void hb_ws_notify_shares(hb_srv *srv, const char *account)
{
    guint i;
    for (i = 0; i < srv->sockets->len; i++) {
        WsClient *c = g_ptr_array_index(srv->sockets, i);
        if (!strcmp(c->account_id, account))
            soup_websocket_connection_send_text(c->conn,
                                                "{\"t\":\"shares\"}");
    }
}

void hb_ws_notify_revoked(hb_srv *srv, const char *device_id, bool wipe)
{
    guint i;
    g_autofree char *s =
        g_strdup_printf("{\"t\":\"revoked\",\"wipe\":%s}",
                        wipe ? "true" : "false");
    for (i = srv->sockets->len; i > 0; i--) {
        WsClient *c = g_ptr_array_index(srv->sockets, i - 1);
        if (!strcmp(c->device_id, device_id)) {
            soup_websocket_connection_send_text(c->conn, s);
            /* 稍后再关，保证消息先出队 */
            g_timeout_add(300, ws_delayed_close, g_object_ref(c->conn));
        }
    }
}

/* WS 升级前认证（early handler）：失败 → 401 常规响应（升级不继续）；
 * 通过 → 返回不响应，升级交给 add_websocket_handler 完成。 */
static void ws_early(SoupServer *server, SoupServerMessage *msg,
                     const char *path, GHashTable *query, gpointer u)
{
    hb_srv *srv = u;
    char *acct = NULL, *dev = NULL;
    (void)server; (void)path; (void)query;
    if (!hb_auth_check(srv, msg, &acct, &dev)) return;
    /* 认证通过：把身份挂在 msg 上给升级后的回调用 */
    g_object_set_data_full(G_OBJECT(msg), "hb-acct", acct, g_free);
    g_object_set_data_full(G_OBJECT(msg), "hb-dev", dev, g_free);
}

static void ws_connected(SoupServer *server, SoupServerMessage *msg,
                         const char *path, SoupWebsocketConnection *conn,
                         gpointer u)
{
    hb_srv *srv = u;
    WsClient *c;
    (void)server; (void)path;
    c = g_new0(WsClient, 1);
    c->srv = srv;
    c->conn = g_object_ref(conn);
    c->account_id =
        g_strdup(g_object_get_data(G_OBJECT(msg), "hb-acct"));
    c->device_id = g_strdup(g_object_get_data(G_OBJECT(msg), "hb-dev"));
    c->last_pong = hbu_now_ms();
    soup_websocket_connection_set_keepalive_interval(conn, 30);
    g_signal_connect(conn, "pong", G_CALLBACK(on_ws_pong), c);
    g_signal_connect(conn, "closed", G_CALLBACK(on_ws_closed), c);
    g_signal_connect(conn, "error", G_CALLBACK(on_ws_error), c);
    c->ping_src = g_timeout_add_seconds(30, ws_ping_check, c);
    g_ptr_array_add(srv->sockets, c);
    hb_log(srv, 2, "ws open account=%s", c->account_id);
}

/* ============================ 路由 ============================ */

typedef void (*SrvHandler)(hb_srv *, SoupServerMessage *, const char *,
                           GHashTable *);

typedef struct {
    hb_srv    *srv;
    SrvHandler h;
} RouteDef;

static void route(SoupServer *server, SoupServerMessage *msg,
                  const char *path, GHashTable *query, gpointer u)
{
    RouteDef *r = u;
    (void)server;
    r->h(r->srv, msg, path, query);
}

void http_attach(hb_srv *srv)
{
    GError *err = NULL;
    /* 进程生命周期内的路由表 */
    static const struct {
        const char *path;
        SrvHandler  h;
    } routes[] = {
        { "/v1/prelogin", h_prelogin },
        { "/v1/register", h_register },
        { "/v1/login", h_login },
        { "/v1/recover", h_recover },
        { "/v1/password", h_password },
        { "/v1/recovery", h_recovery },
        { "/v1/logout", h_logout },
        { "/v1/devices", h_devices },
        { "/v1/totp", h_totp },
        { "/v1/objects", h_objects },
        { "/v1/shares", h_shares },
    };
    static RouteDef defs[G_N_ELEMENTS(routes)];
    size_t i;
    for (i = 0; i < G_N_ELEMENTS(routes); i++) {
        defs[i].srv = srv;
        defs[i].h = routes[i].h;
        soup_server_add_handler(srv->soup, routes[i].path, route,
                                &defs[i], NULL);
    }
    srv->hash_pool = g_thread_pool_new(hash_worker, NULL,
                                       srv->hash_threads, TRUE, &err);
    if (!srv->hash_pool) {
        g_printerr("hash pool: %s\n", err->message);
        exit(1);
    }
    soup_server_add_early_handler(srv->soup, "/v1/ws", ws_early, srv,
                                  NULL);
    soup_server_add_websocket_handler(srv->soup, "/v1/ws", NULL, NULL,
                                      ws_connected, srv, NULL);
    relay_attach(srv);
}

