#include "srv.h"

#include <string.h>
#include <ctype.h>

/* ============================ base64url ============================ */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

uint8_t *hbu_b64_decode(const char *s, size_t *n_out)
{
    size_t n, i, outn;
    uint8_t *out;
    if (!s) return NULL;
    n = strlen(s);
    if (n == 0 || n % 4 == 1) return NULL;
    outn = (n / 4) * 3;
    switch (n % 4) { case 2: outn += 1; break; case 3: outn += 2; break; }
    out = g_malloc(outn);
    for (i = 0; i < n; i += 4) {
        int v0 = b64_val(s[i]);
        int v1 = i + 1 < n ? b64_val(s[i + 1]) : -1;
        int v2 = i + 2 < n ? b64_val(s[i + 2]) : 0;
        int v3 = i + 3 < n ? b64_val(s[i + 3]) : 0;
        uint32_t w;
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            g_free(out);
            return NULL;
        }
        w = (uint32_t)(v0 << 18 | v1 << 12 | v2 << 6 | v3);
        size_t o = (i / 4) * 3;
        size_t rem = n - i;
        if (o < outn) out[o] = w >> 16;
        if (rem > 2 && o + 1 < outn) out[o + 1] = w >> 8;
        if (rem > 3 && o + 2 < outn) out[o + 2] = w;
    }
    *n_out = outn;
    return out;
}

char *hbu_b64_encode(const uint8_t *d, size_t n)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i, outn = (n / 3) * 4 + (n % 3 ? n % 3 + 1 : 0);
    char *out = g_malloc(outn + 1), *p = out;
    for (i = 0; i + 3 <= n; i += 3) {
        uint32_t w = d[i] << 16 | d[i + 1] << 8 | d[i + 2];
        *p++ = T[w >> 18]; *p++ = T[w >> 12 & 63];
        *p++ = T[w >> 6 & 63]; *p++ = T[w & 63];
    }
    if (n % 3 == 1) {
        uint32_t w = d[i] << 16;
        *p++ = T[w >> 18]; *p++ = T[w >> 12 & 63];
    } else if (n % 3 == 2) {
        uint32_t w = d[i] << 16 | d[i + 1] << 8;
        *p++ = T[w >> 18]; *p++ = T[w >> 12 & 63]; *p++ = T[w >> 6 & 63];
    }
    *p = 0;
    return out;
}

/* ============================ misc ============================ */

char *hbu_sha256_hex(const uint8_t *d, size_t n)
{
    uint8_t h[crypto_hash_sha256_BYTES];
    char *out = g_malloc(65);
    crypto_hash_sha256(h, d, n);
    sodium_bin2hex(out, 65, h, sizeof h);
    return out;
}

void hbu_hmac_sha256(const uint8_t *key, size_t kn, const uint8_t *data,
                     size_t dn, uint8_t out[32])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, kn);
    g_hmac_update(h, data, dn);
    gsize len = 32;
    g_hmac_get_digest(h, out, &len);
    g_hmac_unref(h);
}

char *hbu_email_norm(const char *email)
{
    char *s = g_strdup(email ? email : "");
    g_strstrip(s);
    char *low = g_ascii_strdown(s, -1);
    g_free(s);
    return low;
}

char *hbu_base32_encode(const uint8_t *d, size_t n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    size_t outn = (n * 8 + 4) / 5;
    char *out = g_malloc(outn + 1), *p = out;
    uint64_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | d[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            *p++ = T[(acc >> bits) & 31];
        }
    }
    if (bits) *p++ = T[(acc << (5 - bits)) & 31];
    *p = 0;
    return out;
}

uint32_t hbu_totp(const uint8_t *secret, size_t n, int64_t step)
{
    uint8_t ctr[8], digest[20];
    GHmac *h;
    gsize len = 20;
    int off;
    uint32_t code;
    for (int i = 7; i >= 0; i--) ctr[i] = step >> ((7 - i) * 8);
    h = g_hmac_new(G_CHECKSUM_SHA1, secret, n);
    g_hmac_update(h, ctr, 8);
    g_hmac_get_digest(h, digest, &len);
    g_hmac_unref(h);
    off = digest[19] & 0xf;
    code = ((uint32_t)(digest[off] & 0x7f) << 24) |
           ((uint32_t)digest[off + 1] << 16) |
           ((uint32_t)digest[off + 2] << 8) | digest[off + 3];
    return code % 1000000;
}

bool hbu_totp_check(const uint8_t *secret, size_t n, const char *code,
                    int64_t now_ms, int64_t last_used, int64_t *used_step)
{
    int64_t step = now_ms / 30000;
    long want;
    if (!code || strlen(code) != 6) return false;
    for (const char *c = code; *c; c++)
        if (!isdigit((unsigned char)*c)) return false;
    want = strtol(code, NULL, 10);
    for (int64_t s = step - 1; s <= step + 1; s++) {
        if (s <= last_used) continue;          /* 防窗口内重放 */
        if (hbu_totp(secret, n, s) == (uint32_t)want) {
            *used_step = s;
            return true;
        }
    }
    return false;
}

int64_t hbu_now_ms(void)
{
    return g_get_real_time() / 1000;
}

bool hbu_valid_id(const char *id)
{
    size_t n;
    if (!id || !(n = strlen(id)) || n > HB_SRV_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '-') return false;
    }
    return true;
}

bool hbu_valid_type(const char *t)
{
    size_t n;
    if (!t || !(n = strlen(t)) || n > HB_SRV_TYPE_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!(t[i] >= 'a' && t[i] <= 'z') && t[i] != '_') return false;
    return true;
}

/* ============================ JSON helpers ============================ */

JsonObject *hbu_body_json(SoupServerMessage *msg)
{
    GBytes *body =
        soup_message_body_flatten(soup_server_message_get_request_body(msg));
    gsize bn;
    gconstpointer bd = g_bytes_get_data(body, &bn);
    JsonParser *p = json_parser_new();
    JsonObject *o = NULL;
    if (bd && json_parser_load_from_data(p, bd, bn, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            o = json_object_ref(json_node_get_object(root));
    }
    g_object_unref(p);
    g_bytes_unref(body);
    return o;
}

const char *hbu_jstr(JsonObject *o, const char *key)
{
    JsonNode *n;
    if (!o || !(n = json_object_get_member(o, key))) return NULL;
    if (!JSON_NODE_HOLDS_VALUE(n) ||
        json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

int64_t hbu_jint(JsonObject *o, const char *key, int64_t fallback, bool *ok)
{
    JsonNode *n;
    if (ok) *ok = false;
    if (!o || !(n = json_object_get_member(o, key))) return fallback;
    if (!JSON_NODE_HOLDS_VALUE(n) ||
        json_node_get_value_type(n) != G_TYPE_INT64)
        return fallback;
    if (ok) *ok = true;
    return json_node_get_int(n);
}

void hbu_respond_json(SoupServerMessage *msg, guint status, const char *json)
{
    soup_server_message_set_status(msg, status, NULL);
    soup_message_body_append(soup_server_message_get_response_body(msg),
                             SOUP_MEMORY_COPY, json, strlen(json));
    soup_message_headers_set_content_type(
        soup_server_message_get_response_headers(msg), "application/json",
        NULL);
}

void hbu_error(SoupServerMessage *msg, guint status, const char *code,
               const char *message, const char *extra_json)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autofree char *s;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "error");
    json_builder_add_string_value(b, code);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
    if (extra_json) {
        JsonParser *p = json_parser_new();
        g_autofree char *wrapped = g_strdup_printf("{%s}", extra_json);
        if (json_parser_load_from_data(p, wrapped, -1, NULL)) {
            JsonObject *eo =
                json_node_get_object(json_parser_get_root(p));
            GList *members = json_object_get_members(eo);
            for (GList *l = members; l; l = l->next) {
                json_builder_set_member_name(b, l->data);
                json_builder_add_value(
                    b, json_node_copy(
                           json_object_get_member(eo, l->data)));
            }
            g_list_free(members);
        }
        g_object_unref(p);
    }
    json_builder_end_object(b);
    {
        JsonGenerator *g = json_generator_new();
        JsonNode *root = json_builder_get_root(b);
        json_generator_set_root(g, root);
        s = json_generator_to_data(g, NULL);
        json_node_free(root);
        g_object_unref(g);
    }
    hbu_respond_json(msg, status, s);
}
