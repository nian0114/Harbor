/* rd_rdp.c — RDP 后端（FreeRDP 3 client-common）。所有 FreeRDP 调用只在 rd worker
 * 线程发生；UI 线程仅经输入队列/答复通道交互。
 *
 * 证书：Harbor 是信任唯一来源。隧道下总是连 127.0.0.1:<随机端口>，FreeRDP 自带的
 * host:port known_hosts 没法用——VerifyX509Certificate/Verify*CertificateEx 里
 * 自己算 "SHA256:" + DER SHA256 base64 指纹与 opts.cert_fp 比对，接受也一律只
 * 限本次会话（返回 2，从不返回 1 → FreeRDP 永不写它的证书库）。 */

#include "rd_priv.h"

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/drdynvc.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/region.h>
#include <freerdp/input.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/error.h>
#include <winpr/wlog.h>
#include <winpr/synch.h>
#include <winpr/string.h>
#include <winpr/collections.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sodium.h>

/* rdpClientContext 是 FreeRDP 私有布局（client-common 要求 ContextSize >=
 * 其大小），把我们的 rd 指针放进 context 会撞上它——改用 context→rd 静态小映射。 */

static pthread_mutex_t rdp_map_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { const rdpContext *c; hb_rd *rd; } rdp_map[16];

static void rdp_map_put(const rdpContext *c, hb_rd *rd)
{
    pthread_mutex_lock(&rdp_map_mu);
    for (size_t i = 0; i < sizeof rdp_map / sizeof rdp_map[0]; i++)
        if (!rdp_map[i].c) {
            rdp_map[i].c = c;
            rdp_map[i].rd = rd;
            break;
        }
    pthread_mutex_unlock(&rdp_map_mu);
}

static hb_rd *rdp_map_get(const rdpContext *c)
{
    hb_rd *rd = NULL;
    pthread_mutex_lock(&rdp_map_mu);
    for (size_t i = 0; i < sizeof rdp_map / sizeof rdp_map[0]; i++)
        if (rdp_map[i].c == c) {
            rd = rdp_map[i].rd;
            break;
        }
    pthread_mutex_unlock(&rdp_map_mu);
    return rd;
}

static void rdp_map_del(const rdpContext *c)
{
    pthread_mutex_lock(&rdp_map_mu);
    for (size_t i = 0; i < sizeof rdp_map / sizeof rdp_map[0]; i++)
        if (rdp_map[i].c == c)
            rdp_map[i].c = NULL, rdp_map[i].rd = NULL;
    pthread_mutex_unlock(&rdp_map_mu);
}

typedef struct {
    hb_rd *rd;
    freerdp *instance;
    rdpContext *ctx;
    pthread_mutex_t mu;
    CliprdrClientContext *clip;
    DispClientContext *disp;
    char *clip_text;         /* 本地剪贴板缓存 UTF-8，供 ServerFormatDataRequest 用 */
    pthread_mutex_t clip_mu;
    unsigned prev_buttons;
    bool prompt_used;
    bool connected;
    char sec_desc[64];
    char fp[128];            /* 最近看到的指纹 */
    hb_rd_err last_err;
    char last_det[512];
} rd_rdp;

static rd_rdp *bep(hb_rd *rd) { return rd->be; }
static hb_rd *ctx_rd(const rdpContext *c) { return rdp_map_get(c); }

/* ---------- 指纹 ---------- */

/* DER → "SHA256:<base64 no pad>"，写入 buf（>=45 字节） */
static void fp_of_der(const BYTE *der, size_t dlen, char *buf, size_t blen)
{
    unsigned char dg[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(dg, der, dlen);
    char b64[64];
    sodium_bin2base64(b64, sizeof b64, dg, sizeof dg, sodium_base64_VARIANT_ORIGINAL);
    snprintf(buf, blen, "SHA256:%s", b64);
    char *eq = strchr(buf, '=');
    if (eq)
        *eq = 0;
}

/* PEM 首块 → 证书 → 指纹 + subject/issuer 摘要（text2 由调用方组装） */
static bool fp_of_pem(const char *pem, size_t plen, char *fp, size_t flen,
                      char *subj, size_t slen, char *issuer, size_t ilen)
{
    const char *end = strstr(pem, "-----END CERTIFICATE-----");
    if (!end)
        return false;
    size_t n = (size_t)(end - pem) + strlen("-----END CERTIFICATE-----");
    char *block = strndup(pem, n < plen ? n : plen);
    if (!block)
        return false;
    rdpCertificate *cert = freerdp_certificate_new_from_pem(block);
    free(block);
    if (!cert)
        return false;
    BYTE *der = NULL;
    size_t dlen = 0;
    BOOL ok = FALSE;
    der = freerdp_certificate_get_der(cert, &dlen);
    if (der) {
        fp_of_der(der, dlen, fp, flen);
        char *s = freerdp_certificate_get_subject(cert);
        char *i = freerdp_certificate_get_issuer(cert);
        snprintf(subj, slen, "%s", s ? s : "?");
        snprintf(issuer, ilen, "%s", i ? i : "?");
        free(s);
        free(i);
        ok = TRUE;
    }
    freerdp_certificate_free(cert);
    return ok;
}

/* CERT 事件 + 等待答复；true = 本次会话接受 */
static bool cert_prompt(hb_rd *rd, int changed, const char *fp,
                        const char *subj, const char *issuer, const char *host)
{
    rd_rdp *be = bep(rd);
    if (rd->o.cert_fp && strcmp(rd->o.cert_fp, fp) == 0) {
        snprintf(be->fp, sizeof be->fp, "%s", fp);
        return true; /* 与受信指纹一致：直接接受（本次会话） */
    }
    /* Harbor 以 vault 指纹为准：已存指纹且不符即"证书变更"，
     * 与 freerdp 自身 known_hosts 视角的 CHANGED 标志无关（它对
     * 隧道随机端口永远无知，FLAG 恒为 0）。 */
    changed = changed || (rd->o.cert_fp != NULL);
    char sum[640];
    snprintf(sum, sizeof sum, "CN=%s\n颁发者: %s\n主机: %s",
             subj && *subj ? subj : "?", issuer && *issuer ? issuer : "?",
             host ? host : "?");
    hb__rd_arm_cert(rd);
    hb_rd_event ev = { .type = HB_RD_EV_CERT,
                       .code = changed ? 1 : 0,
                       .text = (char *)fp,
                       .text2 = sum };
    hb__rd_push(rd, &ev);
    bool acc = hb__rd_wait_cert(rd);
    if (!acc)
        rd->closing = true; /* 拒绝 → 断开并走 CANCELLED/CERT_REJECTED 映射 */
    return acc;
}

/* ---------- FreeRDP 回调 ---------- */

static int rdp_verify_x509(freerdp *instance, const BYTE *data, size_t length,
                           const char *hostname, UINT16 port, DWORD flags)
{
    hb_rd *rd = ctx_rd(instance->context);
    (void)port;
    char fp[128] = { 0 }, subj[256] = { 0 }, iss[256] = { 0 };
    if (!fp_of_pem((const char *)data, length, fp, sizeof fp, subj, sizeof subj,
                   iss, sizeof iss))
        return 0; /* 解析不了 → 拒绝 */
    rd_rdp *be = bep(rd);
    snprintf(be->fp, sizeof be->fp, "%s", fp);
    const char *host = rd->o.tls_hostname ? rd->o.tls_hostname
                                          : (hostname ? hostname : "?");
    int changed = (flags & VERIFY_CERT_FLAG_CHANGED) ? 1 : 0;
    return cert_prompt(rd, changed, fp, subj, iss, host) ? 2 : 0;
}

static DWORD rdp_verify_cert_ex(freerdp *instance, const char *host, UINT16 port,
                                const char *common_name, const char *subject,
                                const char *issuer, const char *fingerprint,
                                DWORD flags)
{
    hb_rd *rd = ctx_rd(instance->context);
    rd_rdp *be = bep(rd);
    (void)common_name;
    char fp[128] = { 0 }, subj[256], iss[256];
    snprintf(subj, sizeof subj, "%s", subject ? subject : "?");
    snprintf(iss, sizeof iss, "%s", issuer ? issuer : "?");
    if ((flags & VERIFY_CERT_FLAG_FP_IS_PEM) && fingerprint &&
        fp_of_pem(fingerprint, strlen(fingerprint), fp, sizeof fp, NULL, 0, NULL, 0)) {
        /* 正常路径：拿到 PEM，算 DER 指纹 */
    } else if (fingerprint) {
        /* 旧路径：FreeRDP 内部指纹格式（hex），无法换算成 DER 指纹——原样展示 */
        snprintf(fp, sizeof fp, "%s", fingerprint);
    } else
        snprintf(fp, sizeof fp, "?");
    snprintf(be->fp, sizeof be->fp, "%s", fp);
    const char *hn = rd->o.tls_hostname ? rd->o.tls_hostname : (host ? host : "?");
    (void)port;
    int changed = (flags & VERIFY_CERT_FLAG_CHANGED) ? 1 : 0;
    return cert_prompt(rd, changed, fp, subj, iss, hn) ? 2 : 0;
}

static DWORD rdp_verify_changed_ex(freerdp *instance, const char *host, UINT16 port,
                                   const char *common_name, const char *subject,
                                   const char *issuer, const char *new_fingerprint,
                                   const char *old_subject, const char *old_issuer,
                                   const char *old_fingerprint, DWORD flags)
{
    (void)old_subject;
    (void)old_issuer;
    (void)old_fingerprint;
    return rdp_verify_cert_ex(instance, host, port, common_name, subject, issuer,
                              new_fingerprint, flags | VERIFY_CERT_FLAG_CHANGED);
}

static BOOL rdp_authenticate(freerdp *instance, char **username, char **password,
                           char **domain, rdp_auth_reason reason)
{
    hb_rd *rd = ctx_rd(instance->context);
    rd_rdp *be = bep(rd);
    (void)reason;
    if (rd->password) {
        *username = _strdup(rd->o.user ? rd->o.user : "");
        *password = _strdup(rd->password);
        *domain = _strdup(rd->o.domain ? rd->o.domain : "");
        return TRUE;
    }
    if (be->prompt_used) /* 已问过一轮：FreeRDP 再问就是凭据被拒 → 失败 */
        return FALSE;
    hb__rd_arm_prompt(rd);
    hb_rd_event ev = { .type = HB_RD_EV_PROMPT,
                       .code = 1,
                       .text = (char *)"需要 RDP 凭据（用户名 / 密码 / 域）" };
    hb__rd_push(rd, &ev);
    char *u = NULL, *p = NULL, *d = NULL;
    if (!hb__rd_wait_prompt(rd, &u, &p, &d)) {
        free(u);
        free(d);
        return FALSE;
    }
    be->prompt_used = true;
    *username = _strdup(u ? u : "");
    *password = _strdup(p ? p : "");
    *domain = _strdup(d ? d : "");
    free(u);
    free(d);
    if (p)
        sodium_free(p);
    return TRUE;
}

/* cliprdr —— CF_UNICODETEXT only，双向 */

static void clip_send_format_list(rd_rdp *be, BOOL have_text)
{
    if (!be->clip)
        return;
    CLIPRDR_FORMAT fmt = { 0 };
    fmt.formatId = CF_UNICODETEXT;
    CLIPRDR_FORMAT_LIST l = { 0 };
    l.numFormats = have_text ? 1 : 0;
    l.formats = have_text ? &fmt : NULL;
    be->clip->ClientFormatList(be->clip, &l);
}

static UINT clip_monitor_ready(CliprdrClientContext *c,
                               const CLIPRDR_MONITOR_READY *mr)
{
    rd_rdp *be = c->custom;
    (void)mr;
    CLIPRDR_GENERAL_CAPABILITY_SET gs = { 0 };
    CLIPRDR_CAPABILITIES caps = { 0 };
    caps.common.msgType = CB_CLIP_CAPS;
    caps.cCapabilitiesSets = 1;
    caps.capabilitySets = (CLIPRDR_CAPABILITY_SET *)&gs;
    gs.capabilitySetType = CB_CAPSTYPE_GENERAL;
    gs.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    gs.version = CB_CAPS_VERSION_2;
    gs.generalFlags = CB_USE_LONG_FORMAT_NAMES;
    c->ClientCapabilities(c, &caps);
    clip_send_format_list(be, TRUE); /* 声明我们能提供 CF_UNICODETEXT */
    return CHANNEL_RC_OK;
}

static UINT clip_server_format_list(CliprdrClientContext *c,
                                    const CLIPRDR_FORMAT_LIST *fl)
{
    rd_rdp *be = c->custom;
    CLIPRDR_FORMAT_LIST_RESPONSE r = { 0 };
    r.common.msgType = CB_FORMAT_LIST_RESPONSE;
    r.common.msgFlags = CB_RESPONSE_OK;
    c->ClientFormatListResponse(c, &r);
    for (UINT32 i = 0; i < fl->numFormats; i++) {
        if (fl->formats[i].formatId == CF_UNICODETEXT) {
            CLIPRDR_FORMAT_DATA_REQUEST rq = { 0 };
            rq.common.msgType = CB_FORMAT_DATA_REQUEST;
            rq.requestedFormatId = CF_UNICODETEXT;
            c->ClientFormatDataRequest(c, &rq);
            break;
        }
    }
    (void)be;
    return CHANNEL_RC_OK;
}

static UINT clip_server_data_request(CliprdrClientContext *c,
                                     const CLIPRDR_FORMAT_DATA_REQUEST *rq)
{
    rd_rdp *be = c->custom;
    CLIPRDR_FORMAT_DATA_RESPONSE r = { 0 };
    r.common.msgType = CB_FORMAT_DATA_RESPONSE;
    if (rq->requestedFormatId != CF_UNICODETEXT) {
        r.common.msgFlags = CB_RESPONSE_FAIL;
        return c->ClientFormatDataResponse(c, &r);
    }
    pthread_mutex_lock(&be->clip_mu);
    const char *txt = be->clip_text ? be->clip_text : "";
    size_t wlen = 0;
    WCHAR *w = ConvertUtf8ToWCharAlloc(txt, &wlen);
    pthread_mutex_unlock(&be->clip_mu);
    if (!w) {
        r.common.msgFlags = CB_RESPONSE_FAIL;
        return c->ClientFormatDataResponse(c, &r);
    }
    r.common.msgFlags = CB_RESPONSE_OK;
    r.common.dataLen = (UINT32)((wlen + 1) * sizeof(WCHAR));
    r.requestedFormatData = (const BYTE *)w;
    UINT rc = c->ClientFormatDataResponse(c, &r);
    free(w);
    return rc;
}

static UINT clip_client_data_response(CliprdrClientContext *c,
                                      const CLIPRDR_FORMAT_DATA_RESPONSE *r)
{
    rd_rdp *be = c->custom;
    hb_rd *rd = ctx_rd((const rdpContext *)be->ctx);
    if (!(r->common.msgFlags & CB_RESPONSE_OK) || !r->requestedFormatData)
        return CHANNEL_RC_OK;
    /* 服务端数据不保证 NUL 结尾：先补一个 WCHAR NUL */
    size_t n = r->common.dataLen / sizeof(WCHAR);
    WCHAR *w = calloc(n + 1, sizeof(WCHAR));
    if (!w)
        return CHANNEL_RC_OK;
    memcpy(w, r->requestedFormatData, n * sizeof(WCHAR));
    char *u = ConvertWCharToUtf8Alloc(w, NULL);
    free(w);
    if (u) {
        hb__rd_clipboard(rd, u);
        free(u);
    }
    return CHANNEL_RC_OK;
}

/* drdynvc → disp 接口捕获 */

static UINT drdynvc_connected(DrdynvcClientContext *ctx, const char *name,
                              void *pInterface)
{
    rd_rdp *be = ctx->custom;
    if (strcmp(name, "disp") == 0)
        be->disp = (DispClientContext *)pInterface;
    return CHANNEL_RC_OK;
}

/* 显示更新 → FRAME */

static BOOL rdp_end_paint(rdpContext *context)
{
    hb_rd *rd = ctx_rd(context);
    rdpGdi *gdi = context->gdi;
    if (!gdi || !gdi->primary)
        return TRUE;
    HGDI_RGN inv = gdi->primary->hdc->hwnd->invalid;
    if (!inv || gdi->primary->hdc->hwnd->ninvalid < 0)
        return TRUE;
    int x = inv->x, y = inv->y, w = inv->w, h = inv->h;
    if (w > 0 && h > 0) {
        int gw = gdi->width, gh = gdi->height;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > gw) w = gw - x;
        if (y + h > gh) h = gh - y;
        if (w > 0 && h > 0)
            hb__rd_fb_update(rd, gdi->primary_buffer, gdi->stride,
                             gw, gh, x, y, w, h);
    }
    /* 清 invalid 区域 */
    gdi->primary->hdc->hwnd->ninvalid = -1;
    return TRUE;
}

static BOOL rdp_desktop_resize(rdpContext *context)
{
    hb_rd *rd = ctx_rd(context);
    rdpGdi *gdi = context->gdi;
    if (!gdi)
        return TRUE;
    if (gdi->width != rd->fb_w || gdi->height != rd->fb_h)
        hb__rd_resize(rd, gdi->width, gdi->height);
    return TRUE;
}

static BOOL rdp_pre_connect(freerdp *instance)
{
    rdpContext *context = instance->context;
    hb_rd *rd = ctx_rd(context);
    rdpSettings *s = context->settings;

    freerdp_settings_set_uint32(s, FreeRDP_KeyboardLayout, 0x0409);
    freerdp_settings_set_uint32(s, FreeRDP_PerformanceFlags,
                              PERF_DISABLE_WALLPAPER | PERF_DISABLE_FULLWINDOWDRAG |
                                  PERF_DISABLE_MENUANIMATIONS | PERF_DISABLE_THEMING);
    switch (rd->o.quality) {
    case HB_RD_Q_FAST:
        freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 16);
        freerdp_settings_set_bool(s, FreeRDP_AllowFontSmoothing, FALSE);
        freerdp_settings_set_bool(s, FreeRDP_AllowDesktopComposition, FALSE);
        break;
    case HB_RD_Q_HIGH:
        freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32);
        freerdp_settings_set_bool(s, FreeRDP_AllowFontSmoothing, TRUE);
        freerdp_settings_set_bool(s, FreeRDP_AllowDesktopComposition, TRUE);
        freerdp_settings_set_uint32(s, FreeRDP_PerformanceFlags, 0);
        break;
    case HB_RD_Q_BALANCED:
    default:
        freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32);
        freerdp_settings_set_bool(s, FreeRDP_AllowFontSmoothing, TRUE);
        break;
    }
    freerdp_settings_set_bool(s, FreeRDP_FastPathOutput, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_FrameMarkerCommandEnabled, TRUE);

    if (!freerdp_client_load_addins(context->channels, s))
        return FALSE;

    rd_rdp *be = bep(rd);
    be->clip = freerdp_channels_get_static_channel_interface(context->channels,
                                                           "cliprdr");
    if (be->clip) {
        be->clip->custom = be;
        be->clip->MonitorReady = clip_monitor_ready;
        be->clip->ServerFormatList = clip_server_format_list;
        be->clip->ServerFormatDataRequest = clip_server_data_request;
        be->clip->ClientFormatDataResponse = clip_client_data_response;
    }
    DrdynvcClientContext *dvc =
        freerdp_channels_get_static_channel_interface(context->channels, "drdynvc");
    if (dvc) {
        dvc->custom = be;
        dvc->OnChannelConnected = drdynvc_connected;
    }
    hb__rd_stage(rd, "RDP 协商 / 认证…");
    return TRUE;
}

static BOOL rdp_post_connect(freerdp *instance)
{
    rdpContext *context = instance->context;
    hb_rd *rd = ctx_rd(context);
    rd_rdp *be = bep(rd);
    if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
        return FALSE;
    context->update->EndPaint = rdp_end_paint;
    context->update->DesktopResize = rdp_desktop_resize;
    be->connected = true;
    snprintf(be->sec_desc, sizeof be->sec_desc, "NLA · TLS");
    hb__rd_connected(rd, context->gdi->width, context->gdi->height,
                     rd->o.tls_hostname ? rd->o.tls_hostname : rd->o.host,
                     be->sec_desc);
    hb__rd_wipe_password(rd);
    /* 认证后清掉 settings 里的口令副本 */
    freerdp_settings_set_string(context->settings, FreeRDP_Password, NULL);
    return TRUE;
}

static void rdp_post_disconnect(freerdp *instance)
{
    rdpContext *context = instance->context;
    rd_rdp *be = bep(ctx_rd(context));
    gdi_free(instance);
    be->clip = NULL;
    be->disp = NULL;
}

/* entry points */

static BOOL hb_rdp_global_init(void) { return TRUE; }
static void hb_rdp_global_uninit(void) {}

static BOOL hb_rdp_client_new(freerdp *instance, rdpContext *context)
{
    (void)context;
    instance->PreConnect = rdp_pre_connect;
    instance->PostConnect = rdp_post_connect;
    instance->PostDisconnect = rdp_post_disconnect;
    instance->AuthenticateEx = rdp_authenticate;
    instance->VerifyX509Certificate = rdp_verify_x509;
    instance->VerifyCertificateEx = rdp_verify_cert_ex;
    instance->VerifyChangedCertificateEx = rdp_verify_changed_ex;
    return TRUE;
}

static void hb_rdp_client_free(freerdp *instance, rdpContext *context)
{
    (void)instance;
    (void)context;
}

static int hb_rdp_client_start(rdpContext *context)
{
    return freerdp_connect(context->instance) ? 0 : -1;
}

static int hb_rdp_client_stop(rdpContext *context)
{
    freerdp_disconnect(context->instance);
    return 0;
}

/* ---------- 输入发送 ---------- */

/* X11 (evdev) keycode → RDP scancode。FreeRDP 3.11+ 弃用了自带的映射函数，
 * 推荐客户端自带表。索引 = keycode - 8；KBDEXT(0x100) 标记扩展键。 */
static DWORD x11kc_to_rdp_scancode(uint32_t kc)
{
#define S(c) MAKE_RDP_SCANCODE(c, FALSE)
#define SE(c) MAKE_RDP_SCANCODE(c, TRUE)
    static const DWORD tab[] = {
        /* 8 */ 0,
        S(0x01), /* 9 Esc */
        S(0x02), S(0x03), S(0x04), S(0x05), S(0x06), S(0x07), S(0x08),
        S(0x09), S(0x0A), S(0x0B), S(0x0C), S(0x0D), /* 10-21: 1..= */
        S(0x0E), /* 22 BackSpace */
        S(0x0F), /* 23 Tab */
        S(0x10), S(0x11), S(0x12), S(0x13), S(0x14), S(0x15), S(0x16),
        S(0x17), S(0x18), S(0x19), /* 24-33: q..p */
        S(0x1A), S(0x1B),          /* 34-35: [ ] */
        S(0x1C),                   /* 36 Return */
        S(0x1D),                   /* 37 Control_L */
        S(0x1E), S(0x1F), S(0x20), S(0x21), S(0x22), S(0x23), S(0x24),
        S(0x25), S(0x26), S(0x27), S(0x28), /* 38-48: a..' */
        S(0x29),                   /* 49 ` */
        S(0x2A),                   /* 50 Shift_L */
        S(0x2B),                   /* 51 \ */
        S(0x2C), S(0x2D), S(0x2E), S(0x2F), S(0x30), S(0x31), S(0x32),
        S(0x33), S(0x34), S(0x35), /* 52-61: z../ */
        S(0x36),                   /* 62 Shift_R */
        S(0x37),                   /* 63 KP_* */
        S(0x38),                   /* 64 Alt_L */
        S(0x39),                   /* 65 space */
        S(0x3A),                   /* 66 CapsLock */
        S(0x3B), S(0x3C), S(0x3D), S(0x3E), S(0x3F), S(0x40), S(0x41),
        S(0x42), S(0x43), S(0x44), /* 67-76: F1-F10 */
        S(0x45),                   /* 77 NumLock */
        S(0x46),                   /* 78 ScrollLock */
        S(0x47), S(0x48), S(0x49), /* 79-81 KP_7/8/9 */
        S(0x4A),                   /* 82 KP_- */
        S(0x4B), S(0x4C), S(0x4D), /* 83-85 KP_4/5/6 */
        S(0x4E),                   /* 86 KP_+ */
        S(0x4F), S(0x50), S(0x51), /* 87-89 KP_1/2/3 */
        S(0x52),                   /* 90 KP_0 */
        S(0x53),                   /* 91 KP_. */
        0,                         /* 92 */
        0,                         /* 93 */
        S(0x56),                   /* 94 <>| (non-US) */
        S(0x57), S(0x58),          /* 95-96: F11/F12 */
        0, 0, 0, 0, 0,             /* 97-101 */
        SE(0x37),                  /* 102? PrintScreen → 见 107 */
        0,                         /* 103 */
        SE(0x1C),                  /* 104 KP_Enter */
        SE(0x1D),                  /* 105 Control_R */
        SE(0x35),                  /* 106 KP_/ */
        SE(0x37),                  /* 107 PrintScreen */
        SE(0x38),                  /* 108 Alt_R / ISO_Level3 */
        0,                         /* 109 */
        SE(0x47),                  /* 110 Home */
        SE(0x48),                  /* 111 Up */
        SE(0x49),                  /* 112 PageUp */
        SE(0x4B),                  /* 113 Left */
        SE(0x4D),                  /* 114 Right */
        SE(0x4F),                  /* 115 End */
        SE(0x50),                  /* 116 Down */
        SE(0x51),                  /* 117 PageDown */
        SE(0x52),                  /* 118 Insert */
        SE(0x53),                  /* 119 Delete */
        0, 0, 0, 0, 0, 0, 0,       /* 120-126 */
        0,                         /* 127 Pause（特殊序列，略） */
        0, 0, 0, 0,                /* 128-131 */
        S(0x7D),                   /* 132 Yen */
        SE(0x5B),                  /* 133 Super_L */
        SE(0x5C),                  /* 134 Super_R */
        SE(0x5D),                  /* 135 Menu */
    };
#undef S
#undef SE
    if (kc < 8 || kc - 8 >= sizeof tab / sizeof tab[0])
        return 0;
    return tab[kc - 8];
}

static void rdp_send_input(hb_rd *rd, rd_rdp *be, const rd_in *in)
{
    (void)rd;
    rdpContext *context = (rdpContext *)be->ctx;
    if (!context || !context->input)
        return;
    rdpInput *input = context->input;
    switch (in->t) {
    case RD_IN_POINTER: {
        /* 位置变化 → MOVE；按钮变化 → 逐位 down/up；滚轮 → WHEEL 脉冲 */
        freerdp_input_send_mouse_event(input, PTR_FLAGS_MOVE, in->x, in->y);
        static const UINT16 btnf[3] = { PTR_FLAGS_BUTTON1, PTR_FLAGS_BUTTON3,
                                        PTR_FLAGS_BUTTON2 };
        for (int i = 0; i < 3; i++) {
            unsigned bit = 1u << i;
            if ((in->buttons & bit) && !(be->prev_buttons & bit))
                freerdp_input_send_mouse_event(input, PTR_FLAGS_DOWN | btnf[i],
                                               in->x, in->y);
            else if (!(in->buttons & bit) && (be->prev_buttons & bit))
                freerdp_input_send_mouse_event(input, btnf[i], in->x, in->y);
        }
        unsigned chg = in->buttons & ~be->prev_buttons;
        if (chg & 0x08)
            freerdp_input_send_mouse_event(input, PTR_FLAGS_WHEEL | 0x78, in->x, in->y);
        if (chg & 0x10)
            freerdp_input_send_mouse_event(input,
                                           PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x78,
                                           in->x, in->y);
        if (chg & 0x20)
            freerdp_input_send_mouse_event(input, PTR_FLAGS_HWHEEL | 0x78, in->x, in->y);
        if (chg & 0x40)
            freerdp_input_send_mouse_event(input,
                                           PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x78,
                                           in->x, in->y);
        be->prev_buttons = in->buttons & 0x7;
        break;
    }
    case RD_IN_KEY: {
        if (in->x11kc) {
            DWORD sc = x11kc_to_rdp_scancode(in->x11kc);
            if (sc)
                freerdp_input_send_keyboard_event_ex(input, in->down, FALSE, sc);
        } else if (in->keysym >= 0x20 && in->keysym <= 0xFFFF) {
            freerdp_input_send_unicode_keyboard_event(
                input, in->down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE,
                (UINT16)in->keysym);
        }
        break;
    }
    case RD_IN_CLIPBOARD: {
        pthread_mutex_lock(&be->clip_mu);
        free(be->clip_text);
        be->clip_text = strdup(in->text);
        pthread_mutex_unlock(&be->clip_mu);
        clip_send_format_list(be, TRUE);
        free(in->text);
        break;
    }
    case RD_IN_SIZE: {
        if (!be->disp)
            break;
        DISPLAY_CONTROL_MONITOR_LAYOUT lay = { 0 };
        lay.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
        lay.Width = (UINT32)in->w;
        lay.Height = (UINT32)in->h;
        lay.Orientation = ORIENTATION_LANDSCAPE;
        lay.PhysicalWidth = (UINT32)(in->w / 96.0 * 25.4);
        lay.PhysicalHeight = (UINT32)(in->h / 96.0 * 25.4);
        lay.DesktopScaleFactor = 100;
        lay.DeviceScaleFactor = 100;
        be->disp->SendMonitorLayout(be->disp, 1, &lay);
        break;
    }
    case RD_IN_QUALITY:
        break; /* RDP 画质改动下次连接生效 */
    }
}

/* ---------- 错误映射 ---------- */

static void rdp_init_log(void)
{
    /* 预期路径（证书询问、Kerberos 探测、登录失败）也会产生 ERROR 日志；
     * 技术细节已通过 ERROR 事件的 text2 上报，这里彻底静音。 */
    WLog_SetLogLevel(WLog_GetRoot(), WLOG_OFF);
}

static hb_rd_err map_last_error(hb_rd *rd, char *det, size_t dlen)
{
    rd_rdp *be = bep(rd);
    UINT32 code = freerdp_get_last_error((rdpContext *)be->ctx);
    const char *name = freerdp_get_last_error_name(code);
    const char *str = freerdp_get_last_error_string(code);
    snprintf(det, dlen, "%s (%s)", str ? str : "?", name ? name : "?");

    if (code == FREERDP_ERROR_SUCCESS)
        return HB_RD_ERR_DISCONNECTED;
    /* 按错误名归类 */
    if (strstr(name, "AUTHENTICATION") || strstr(name, "LOGON") ||
        strstr(name, "ACCESS_DENIED") || strstr(name, "ACCOUNT") ||
        strstr(name, "PASSWORD"))
        return HB_RD_ERR_AUTH;
    if (strstr(name, "CERT") || strstr(name, "TLS") || strstr(name, "SECURITY"))
        return HB_RD_ERR_TLS;
    if (strstr(name, "DNS") || strstr(name, "NAME") || strstr(name, "REFUSED"))
        return HB_RD_ERR_REFUSED;
    if (strstr(name, "TIMEOUT") || strstr(name, "TIMED_OUT"))
        return HB_RD_ERR_TIMEOUT;
    if (strstr(name, "CONNECT"))
        return HB_RD_ERR_REFUSED;
    if (strstr(name, "TRANSPORT"))
        return HB_RD_ERR_DISCONNECTED;
    return HB_RD_ERR_PROTOCOL;
}

/* ---------- worker ---------- */

void hb__rd_rdp_abort(hb_rd *rd)
{
    rd_rdp *be = bep(rd);
    if (!be)
        return;
    pthread_mutex_lock(&be->mu);
    if (be->ctx)
        freerdp_abort_connect_context((rdpContext *)be->ctx);
    pthread_mutex_unlock(&be->mu);
}

void hb__rd_rdp_run(hb_rd *rd)
{
    rd_rdp *be = calloc(1, sizeof *be);
    be->rd = rd;
    rd->be = be;
    pthread_mutex_init(&be->mu, NULL);
    pthread_mutex_init(&be->clip_mu, NULL);

    /* 压住 FreeRDP/WinPR 默认日志噪声（不记口令） */
    static pthread_once_t log_once = PTHREAD_ONCE_INIT;
    pthread_once(&log_once, rdp_init_log);

    hb__rd_stage(rd, "正在连接…");

    RDP_CLIENT_ENTRY_POINTS ep = { 0 };
    ep.Size = sizeof ep;
    ep.Version = RDP_CLIENT_INTERFACE_VERSION;
    ep.GlobalInit = hb_rdp_global_init;
    ep.GlobalUninit = hb_rdp_global_uninit;
    /* client-common 要求 ContextSize 不小于私有 rdpClientContext(~3KB) */
    ep.ContextSize = 4096;
    ep.ClientNew = hb_rdp_client_new;
    ep.ClientFree = hb_rdp_client_free;
    ep.ClientStart = hb_rdp_client_start;
    ep.ClientStop = hb_rdp_client_stop;

    rdpContext *context = freerdp_client_context_new(&ep);
    if (!context) {
        hb__rd_error(rd, HB_RD_ERR_PROTOCOL, "无法创建 FreeRDP 上下文", NULL);
        return;
    }
    rdp_map_put(context, rd);
    be->ctx = context;
    be->instance = context->instance;

    rdpSettings *s = context->settings;
    freerdp_settings_set_string(s, FreeRDP_ServerHostname, rd->o.host);
    freerdp_settings_set_uint32(s, FreeRDP_ServerPort, (UINT32)rd->o.port);
    freerdp_settings_set_string(s, FreeRDP_Username,
                                rd->o.user ? rd->o.user : "");
    if (rd->password)
        freerdp_settings_set_string(s, FreeRDP_Password, rd->password);
    freerdp_settings_set_string(s, FreeRDP_Domain,
                                rd->o.domain ? rd->o.domain : "");
    freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth,
                                rd->o.width > 0 ? (UINT32)rd->o.width : 1024);
    freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight,
                                rd->o.height > 0 ? (UINT32)rd->o.height : 768);
    freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, FALSE);
    freerdp_settings_set_bool(s, FreeRDP_RedirectClipboard, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_SupportDisplayControl, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_AutoReconnectionEnabled, FALSE);
    freerdp_settings_set_bool(s, FreeRDP_AudioPlayback, FALSE);
    freerdp_settings_set_uint32(s, FreeRDP_AuthenticationLevel, 2);
    if (rd->o.freerdp_config_dir)
        freerdp_settings_set_string(s, FreeRDP_ConfigPath,
                                    rd->o.freerdp_config_dir);
    if (rd->o.tls_hostname)
        freerdp_settings_set_string(s, FreeRDP_CertificateName,
                                    rd->o.tls_hostname);
    freerdp_client_add_static_channel(s, 1, (const char *[]){ "cliprdr" });
    freerdp_client_add_dynamic_channel(s, 1, (const char *[]){ "disp" });

    HANDLE wake = CreateFileDescriptorEventA(NULL, FALSE, FALSE,
                                             rd->cmd_r, WINPR_FD_READ);
    int r = freerdp_client_start(context);
    if (r != 0 || !be->connected) {
        char det[512] = { 0 };
        hb_rd_err err;
        if (*be->fp && rd->closing)
            err = HB_RD_ERR_CERT_REJECTED,
            snprintf(det, sizeof det, "certificate rejected");
        else if (rd->closing)
            err = HB_RD_ERR_CANCELLED, snprintf(det, sizeof det, "closed");
        else
            err = map_last_error(rd, det, sizeof det);
        const char *cn =
            err == HB_RD_ERR_AUTH           ? "认证失败：用户名或密码错误"
            : err == HB_RD_ERR_TLS          ? "TLS/加密协商失败"
            : err == HB_RD_ERR_TIMEOUT      ? "连接超时"
            : err == HB_RD_ERR_CERT_REJECTED ? "证书被拒绝"
            : err == HB_RD_ERR_CANCELLED    ? "已取消"
            : err == HB_RD_ERR_REFUSED      ? "无法连接到服务器"
                                            : "RDP 协议错误";
        hb__rd_error(rd, err, cn, det);
        goto out;
    }

    /* 事件循环 */
    while (!hb__rd_closing(rd)) {
        HANDLE handles[32];
        DWORD n = freerdp_get_event_handles(context, handles, 31);
        if (n == (DWORD)-1) {
            if (!hb__rd_closing(rd))
                hb__rd_error(rd, HB_RD_ERR_DISCONNECTED, "连接中断",
                             "freerdp_get_event_handles failed");
            break;
        }
        handles[n] = wake;
        DWORD w = WaitForMultipleObjects(n + 1, handles, FALSE,
                                         hb__rd_in_pending(rd) ? 0 : 500);
        if (w == WAIT_FAILED)
            break;
        if (!freerdp_check_event_handles(context)) {
            if (freerdp_get_last_error(context) == FREERDP_ERROR_SUCCESS ||
                hb__rd_closing(rd))
                break;
            char det[512] = { 0 };
            hb_rd_err e2 = map_last_error(rd, det, sizeof det);
            hb__rd_error(rd, e2,
                         e2 == HB_RD_ERR_AUTH ? "认证失败：用户名或密码错误"
                                              : "连接中断",
                         det);
            break;
        }
        /* 输入 */
        rd_in in[64];
        size_t cnt = hb__rd_in_drain(rd, in, 64);
        for (size_t i = 0; i < cnt; i++)
            rdp_send_input(rd, be, &in[i]);
        hb__rd_in_drain_fd(rd);
    }

out:
    if (be->connected && !hb__rd_closing(rd))
        hb__rd_error(rd, HB_RD_ERR_DISCONNECTED, "连接中断", "session ended");
    freerdp_client_stop(context);
    if (wake)
        CloseHandle(wake);
    pthread_mutex_lock(&be->mu);
    be->ctx = NULL;
    pthread_mutex_unlock(&be->mu);
    rdp_map_del(context);
    freerdp_client_context_free(context);
}

void hb__rd_rdp_destroy(hb_rd *rd)
{
    rd_rdp *be = bep(rd);
    if (!be)
        return;
    pthread_mutex_destroy(&be->mu);
    pthread_mutex_destroy(&be->clip_mu);
    free(be->clip_text);
    free(be);
    rd->be = NULL;
}
