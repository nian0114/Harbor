#include "hb-conn.h"
#include "hb-dialogs.h"
#include "hb-forward-mgr.h"

#include <harbor/ssh.h>
#include <harbor/keys.h>
#include <string.h>
#include <stdlib.h>

/* ==================================================================
 * HbConnCtl — SSH/SFTP 连接流程控制器（从 hb-ssh-page 抽出的安全流）。
 *
 * 拥有：key-confirm → opts 组装 → new_ssh/new_sftp → TOFU/变更/
 * 口令对话框 → CONNECTED 后的凭据落库 / key 元数据补齐 /
 * last_connected / vault 日志。
 * 不拥有：连接视图渲染、终端、重连退避 —— 全部经信号交给 owner。
 * ================================================================== */

struct _HbConnCtl {
    GObject parent;

    HbStore     *store;
    GtkWidget   *dialog_parent;    /* 弱引用（weak pointer） */
    HbConnMode   mode;

    char        *host_id;
    char        *hname;            /* 日志/对话框显示用 */
    char        *huser;

    hb_session  *sess;
    gboolean     was_connected;    /* 曾经 CONNECTED（区分连接期/运行期错误） */
    gboolean     awaiting_reply;   /* 对话框挂起中 */

    AdwDialog   *open_dialog;      /* 当前挂起的模态（weak pointer） */

    /* vault 日志 */
    int64_t      log_id;
    gint64       t0;               /* 连接开始 ts（µs） */

    /* 待保存的"记住"凭据（CONNECTED 后写入） */
    char        *pend_secret;      /* malloc 文本；写完即清 */
    char        *pend_key_id;      /* passphrase → key/host id；密码 → host id */
    gboolean     pend_is_pass;

    /* PROMPT 归属 + 首次使用补元数据 */
    char       **hop_ids;          /* hop 下标 → host 对象 id（jumps + target） */
    char       **conn_key_ids;     /* 本次连接用到的 key 对象 id（strv） */
    char        *prompt_key_id;    /* 当前 PROMPT 归属的 key 对象 id（无=NULL） */
    char        *fill_key_id;      /* 已提示口令的 key 对象（CONNECTED 补元数据） */
    char        *fill_pass;        /* 该口令副本（用后抹掉） */

    /* HOSTKEY 事件挂起期间的暂存（对话框 cb 用） */
    char        *pending_line;
    char        *pending_host;
    char        *pending_keyfp;

    /* 重连 vt：start 传入后由 ctl 持有；key-confirm 被拒绝时不销毁
     *（属于下一次成功连接 —— 与原页面字段语义一致），continue_connect
     * 消费。仅 SHELL。 */
    hb_vt       *reuse_vt;

    /* FORWARD 模式：转发规则（字符串深拷贝，rule 字段指入本对象） */
    hb_pf_rule   rule;
    char        *rule_bind, *rule_dest;
};

G_DEFINE_TYPE(HbConnCtl, hb_conn_ctl, G_TYPE_OBJECT)

enum {
    SIG_STAGE, SIG_SESSION, SIG_CONNECTED, SIG_FAILED,
    SIG_DISCONN, SIG_EXITED, SIG_OSINFO, N_SIGS
};
static guint sigs[N_SIGS];

static void continue_connect(HbConnCtl *c, int rows, int cols);
static int  ctl_scrollback(HbConnCtl *c);

/* dev 环境变量（仅 debug 构建生效；release 里编译掉） */
#ifndef NDEBUG
static gboolean dev_accept_hk(void)
{ return g_getenv("HARBOR_DEV_ACCEPT_HOSTKEY") != NULL; }
static gboolean dev_cancel_prompt(void)
{ return g_getenv("HARBOR_DEV_CANCEL_PROMPT") != NULL; }
#endif

/* 抹零释放（密码/passphrase/凭据副本统一走它） */
static void wipe_free(char *s)
{
    if (!s) return;
    for (char *q = s; *q; q++) *q = 0;
    free(s);
}

/* ------------------------- host → ssh opts ------------------------- */

/* 一次连接所需的全部拥有内存：opts 里的指针都指向这里，
 * 整个 ctx 在 hb_session_new_ssh（深拷贝）返回后释放。 */
typedef struct {
    hb_ssh_opts  o;
    hb_ssh_hop  *jumps;        /* njumps 个 */
    hb_obj    **jhosts;        /* jump host 对象（hop 字段指入，保持存活） */
    char      **secrets;       /* 全部 strdup 的 secret（target+jumps） */
    size_t      nsec, capsec;
    char      **known;         /* known_lines 深拷贝 */
    char      **hop_ids;       /* strdup：jumps + target 的 host 对象 id */
    char      **key_ids;       /* strdup：用到的 key 对象 id（可能含 NULL 跳） */
    size_t      nhops;
    char       *missing_jump;  /* jump id 不在 vault → 连接前报错 */
} ConnCtx;

static void conn_ctx_free(ConnCtx *c)
{
    size_t i;
    if (!c) return;
    for (i = 0; c->known && c->known[i]; i++) free(c->known[i]);
    free(c->known);
    for (i = 0; i < c->nsec; i++) hb_secret_free(c->secrets[i]);
    free(c->secrets);
    for (i = 0; c->jhosts && c->jhosts[i]; i++) hb_obj_free(c->jhosts[i]);
    free(c->jhosts);
    free(c->jumps);
    if (c->hop_ids) {
        for (i = 0; i <= c->nhops; i++) free(c->hop_ids[i]);
        free(c->hop_ids);
    }
    if (c->key_ids) {
        for (i = 0; i <= c->nhops; i++) free(c->key_ids[i]);
        free(c->key_ids);
    }
    free(c->missing_jump);
    free(c);
}

static void ctx_sec_add(ConnCtx *c, char *s)
{
    if (!s) return;
    if (c->nsec == c->capsec) {
        c->capsec = c->capsec ? c->capsec * 2 : 8;
        c->secrets = realloc(c->secrets, c->capsec * sizeof(char *));
    }
    c->secrets[c->nsec++] = s;
}

/* 从 vault 收集全部非 tombstone known 对象的 line → ctx->known */
static void collect_known(HbConnCtl *c, ConnCtx *x)
{
    hb_obj **objs = NULL;
    size_t n = 0, i, k = 0;
    objs = hb_store_list(c->store, "known", &n);
    x->known = calloc(n + 1, sizeof(char *));
    for (i = 0; i < n; i++) {
        const char *line = hb_obj_get_str(objs[i], "line");
        if (line && *line)
            x->known[k++] = strdup(line);
    }
    x->known[k] = NULL;
    x->o.known_lines = (const char *const *)x->known;
    x->o.nknown = k;
    if (objs) hb_obj_list_free(objs, n);
}

/* 单个 host 对象 → hop；secret 进 ctx->secrets（调用方清零释放）。
 * hop 的字符串字段指入 h —— h 必须活到 hb_session_new_ssh 返回。 */
static void host_to_hop(HbConnCtl *c, ConnCtx *x, hb_obj *h, hb_ssh_hop *hop)
{
    const char *auth, *kid;
    memset(hop, 0, sizeof *hop);
    hop->host = hb_obj_get_str(h, "address");
    hop->port = (int)hb_obj_get_int(h, "port", 22);
    if (hop->port <= 0) hop->port = 22;
    hop->user = hb_obj_get_str(h, "user");
    if (!hop->user || !*hop->user)
        hop->user = g_get_user_name();   /* 与 ssh(1) 一致：未填用户名用本机用户 */
    hop->label = hb_obj_get_str(h, "name");
    hop->auth = HB_SSH_AUTH_AUTO;

    auth = hb_obj_get_str(h, "auth");
    kid  = hb_obj_get_str(h, "key_id");
    if (!auth) auth = "auto";

    if (!strcmp(auth, "password")) {
        hop->auth = HB_SSH_AUTH_PASSWORD;
        hop->password = hb_vault_get_secret(hb_store_vault(c->store),
                                            hb_obj_id(h), "password");
        ctx_sec_add(x, (char *)hop->password);
    } else if (!strcmp(auth, "key") && kid && *kid) {
        hop->auth = HB_SSH_AUTH_KEY;
        hop->privkey = hb_vault_get_secret(hb_store_vault(c->store),
                                           kid, "private");
        hop->passphrase = hb_vault_get_secret(hb_store_vault(c->store),
                                              kid, "passphrase");
        ctx_sec_add(x, (char *)hop->privkey);
        ctx_sec_add(x, (char *)hop->passphrase);
    } else if (!strcmp(auth, "key")) {
        hop->auth = HB_SSH_AUTH_KEY;
        hop->identity_file = hb_obj_get_str(h, "identity_file");
        /* 主机级 "passphrase" secret：identity_file 的"记住口令" */
        hop->passphrase = hb_vault_get_secret(hb_store_vault(c->store),
                                            hb_obj_id(h), "passphrase");
        ctx_sec_add(x, (char *)hop->passphrase);
    } else if (!strcmp(auth, "agent")) {
        hop->auth = HB_SSH_AUTH_AGENT;
    } else if (!strcmp(auth, "none")) {
        hop->auth = HB_SSH_AUTH_NONE;
    } else {
        hop->auth = HB_SSH_AUTH_AUTO;
        hop->identity_file = hb_obj_get_str(h, "identity_file");
        hop->passphrase = hb_vault_get_secret(hb_store_vault(c->store),
                                            hb_obj_id(h), "passphrase");
        ctx_sec_add(x, (char *)hop->passphrase);
    }
}

/* 组装本次连接的 hb_ssh_opts（target + jumps + known + 杂项）。
 * target/jump 的字符串指入各自的 host 对象；target host 对象在
 * start_connect 里存活到 new_ssh 返回，jump hosts 挂在 ctx->jhosts 上。 */
static ConnCtx *build_ctx(HbConnCtl *c, hb_obj **target_host_out)
{
    ConnCtx *x = calloc(1, sizeof *x);
    hb_obj *h = NULL;
    const char **jump_ids = NULL;
    size_t nj = 0, i;

    if (!c->host_id ||
        hb_vault_get(hb_store_vault(c->store), c->host_id, &h) != HB_OK || !h)
        goto out;

    host_to_hop(c, x, h, &x->o.target);

    /* jumps：host.jump 是 strv 的 host id，外→内
     * （get_strv 返回的数组要 free，元素是借用的 valuestring） */
    nj = hb_obj_get_strv(h, "jump", &jump_ids);
    x->nhops = nj + 1;
    x->hop_ids = calloc(x->nhops + 1, sizeof(char *));
    x->key_ids = calloc(x->nhops + 1, sizeof(char *));
    x->hop_ids[nj] = strdup(hb_obj_id(h));
    {
        const char *tk = hb_obj_get_str(h, "key_id");
        x->key_ids[nj] = tk && *tk ? strdup(tk) : NULL;
    }
    if (nj) {
        x->jumps = calloc(nj, sizeof(hb_ssh_hop));
        x->jhosts = calloc(nj + 1, sizeof(hb_obj *));
        for (i = 0; i < nj; i++) {
            hb_obj *jh = NULL;
            if (hb_vault_get(hb_store_vault(c->store), jump_ids[i],
                             &jh) != HB_OK || !jh) {
                x->missing_jump = strdup(jump_ids[i]);
                break;
            }
            x->hop_ids[i] = strdup(hb_obj_id(jh));
            {
                const char *jk = hb_obj_get_str(jh, "key_id");
                x->key_ids[i] = jk && *jk ? strdup(jk) : NULL;
            }
            host_to_hop(c, x, jh, &x->jumps[i]);
            x->jhosts[i] = jh;   /* 保活到 ctx 释放 */
        }
        if (x->missing_jump)
            nj = 0;
        free(jump_ids);
        x->o.jumps = x->jumps;
        x->o.njumps = nj;
    }

    collect_known(c, x);

    x->o.connect_timeout_s = 15;
    /* keepalive 缺省 30s（拔线 3 间隔≈90s 判死）；显式 0 = 关闭 */
    {
        int64_t ka = hb_obj_get_int(h, "keepalive", -1);
        x->o.keepalive_s = ka < 0 ? 30 : (int)ka;
    }
    x->o.term = "xterm-256color";
    {
        const char *os = hb_obj_get_str(h, "os");
        x->o.detect_os = (!os || !*os || !strcmp(os, "auto"));
    }
out:
    *target_host_out = h;       /* 调用方持有至 new_ssh 后释放 */
    return x;
}

/* ------------------------- vault 日志 ------------------------- */

static void log_start(HbConnCtl *c)
{
    hb_log l = {0};
    l.user = c->huser;
    l.host = c->hname;
    l.host_id = c->host_id;
    l.proto = c->mode == HB_CONN_SFTP    ? "SFTP" :
              c->mode == HB_CONN_FORWARD ? "SSH/Forward" : "SSH";
    l.result = "connecting";
    l.duration_s = -1;
    l.saved = false;
    hb_vault_log_add(hb_store_vault(c->store), &l, &c->log_id);
    c->t0 = g_get_monotonic_time();
}

static void log_finish(HbConnCtl *c, const char *result, const char *reason)
{
    gint64 dur;
    if (c->log_id <= 0) return;
    dur = (g_get_monotonic_time() - c->t0) / G_USEC_PER_SEC;
    hb_vault_log_finish(hb_store_vault(c->store), c->log_id,
                        result, reason, dur);
}

/* ------------------------- known_hosts 写入 ------------------------- */

/* 解析 HOSTKEY 事件 extra：第1行=known_hosts 行，其余=randomart。
 * 返回新分配 randomart；line 通过 out 返回借用指针。 */
static char *parse_extra(const char *extra, const char **line_out)
{
    const char *nl = extra ? strchr(extra, '\n') : NULL;
    if (!extra) { if (line_out) *line_out = NULL; return NULL; }
    if (!nl) {
        if (line_out) *line_out = extra;
        return NULL;
    }
    if (line_out) *line_out = extra;   /* 首行（借用，调用方截断到 \n） */
    return g_strdup(nl + 1);
}

static void save_known(HbConnCtl *c, const char *host_port,
                       const char *keytype_fp, const char *line,
                       gboolean update_only)
{
    /* line 形如 "[h]:p keytype b64" 或 "h keytype b64" */
    char *ldup, *full_line, *sp1, *sp2, *keytype, *b64, *fp, *host, *end;
    int port = 22;
    hb_obj **objs; size_t n, i;
    hb_obj *o = NULL;

    if (!line) return;
    ldup = g_strdup(line);
    /* 截到行首 \n（extra 里 line 后还有 randomart） */
    end = strchr(ldup, '\n'); if (end) *end = 0;
    full_line = g_strdup(ldup);   /* 整行另存一份，下面拆分会破坏 ldup */
    sp1 = strchr(ldup, ' ');
    if (!sp1) { g_free(full_line); g_free(ldup); return; }
    *sp1 = 0;
    sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) { g_free(full_line); g_free(ldup); return; }
    *sp2 = 0;
    host = ldup;
    keytype = sp1 + 1;
    b64 = sp2 + 1;
    /* "[h]:p" → host + port */
    if (*host == '[') {
        char *rb = strchr(host, ']');
        if (rb) {
            *rb = 0;
            host++;
            if (rb[1] == ':') port = atoi(rb + 2);
        }
    } else {
        char *cc = strrchr(host, ':');
        if (cc) { *cc = 0; port = atoi(cc + 1); }
    }
    /* fingerprint = text2 里 "keytype SHA256:xx" 的第 2 段 */
    fp = g_strdup(keytype_fp ? keytype_fp : "");
    {
        char *sp = strchr(fp, ' ');
        if (sp) memmove(fp, sp + 1, strlen(sp));
    }

    /* 已存在 host+port 的 known 对象 → 更新；否则新建 */
    objs = hb_store_list(c->store, "known", &n);
    for (i = 0; i < n; i++) {
        const char *oh = hb_obj_get_str(objs[i], "host");
        int op = (int)hb_obj_get_int(objs[i], "port", 22);
        if (oh && !strcmp(oh, host) && op == port) {
            o = objs[i]; /* 接管 */
            break;
        }
    }
    if (!o) o = hb_obj_new("known");
    hb_obj_set_str(o, "host", host);
    hb_obj_set_int(o, "port", port);
    hb_obj_set_str(o, "keytype", keytype);
    hb_obj_set_str(o, "key", b64);
    hb_obj_set_str(o, "fingerprint", fp);
    hb_obj_set_int(o, "added", g_get_real_time() / 1000);
    hb_obj_set_str(o, "line", full_line);
    hb_vault_put(hb_store_vault(c->store), o);
    /* o 可能来自 objs[] —— 从数组里摘掉再整体释放 */
    if (objs) {
        for (i = 0; i < n; i++)
            if (objs[i] == o) objs[i] = NULL;
        hb_obj_list_free(objs, n);
    }
    hb_obj_free(o);
    (void)host_port; (void)update_only;
    g_free(full_line);
    g_free(fp);
    g_free(ldup);
}

/* ------------------------- open_dialog 跟踪 ------------------------- */

static GtkWindow *ctl_win(HbConnCtl *c)
{
    GtkRoot *r;
    if (!c->dialog_parent) return NULL;
    r = gtk_widget_get_root(c->dialog_parent);
    return GTK_IS_WINDOW(r) ? GTK_WINDOW(r) : NULL;
}

/* weak-pointer 让对话框被销毁（未发 closed 也会被 finalize）时指针
 * 自动置 NULL，不留悬垂引用 —— start/cancel 里的 force_close 才永远
 * 不会打到死对象。 */
static void set_open_dialog(HbConnCtl *c, AdwDialog *d)
{
    if (c->open_dialog)
        g_object_remove_weak_pointer(G_OBJECT(c->open_dialog),
                                     (gpointer)&c->open_dialog);
    c->open_dialog = d;
    if (d)
        g_object_add_weak_pointer(G_OBJECT(d),
                                  (gpointer)&c->open_dialog);
}

/* 强关挂起的模态 —— 它的 done cb 会同步跑（accept=FALSE），把应答
 * 发给当前这个将死的 sess；不关的话对话框会活到新会话，之后的点击
 * 会应答错会话。 */
static void force_close_dialog(HbConnCtl *c)
{
    if (c->open_dialog) {
        AdwDialog *d = c->open_dialog;
        set_open_dialog(c, NULL);
        adw_dialog_force_close(d);
    }
}

/* ------------------------- 凭据/元数据 ------------------------- */

static void save_pending_secret(HbConnCtl *c)
{
    if (!c->pend_secret || !c->pend_key_id) return;
    hb_vault_set_secret(hb_store_vault(c->store), c->pend_key_id,
                        c->pend_is_pass ? "passphrase" : "password",
                        c->pend_secret);
    /* 抹掉明文副本 */
    wipe_free(c->pend_secret);
    c->pend_secret = NULL;
    g_clear_pointer(&c->pend_key_id, g_free);
}

/* 加密导入的 key（PEM/PKCS8）第一次成功连接后：public/fingerprint 还
 * 是空的 —— 用刚输入（或已记住）的 passphrase 重新 inspect 补齐。 */
static void fill_key_metadata(HbConnCtl *c)
{
    hb_vault *v = hb_store_vault(c->store);
    size_t i;
    if (!c->conn_key_ids) return;
    for (i = 0; c->conn_key_ids[i]; i++) {
        const char *kid = c->conn_key_ids[i];
        hb_obj *k = NULL;
        const char *pub, *pass = NULL;
        char *pass_sec = NULL, *priv = NULL;
        hb_key_info info;
        if (!kid || !*kid) continue;
        if (hb_vault_get(v, kid, &k) != HB_OK || !k) continue;
        pub = hb_obj_get_str(k, "public");
        if (pub && *pub) { hb_obj_free(k); continue; }   /* 已有 → 跳过 */
        if (c->fill_key_id && !strcmp(c->fill_key_id, kid) &&
            c->fill_pass)
            pass = c->fill_pass;          /* 借用，无拷贝 */
        else
            pass = pass_sec = hb_vault_get_secret(v, kid, "passphrase");
        if (pass) {
            priv = hb_vault_get_secret(v, kid, "private");
            if (priv &&
                hb_key_inspect((const unsigned char *)priv,
                               strlen(priv), pass, &info) == HB_OK &&
                info.public_line) {
                hb_obj_set_str(k, "public", info.public_line);
                if (info.fingerprint)
                    hb_obj_set_str(k, "fingerprint", info.fingerprint);
                if (info.kind)
                    hb_obj_set_str(k, "kind", info.kind);
                if (info.bits)
                    hb_obj_set_int(k, "bits", info.bits);
                hb_vault_put(v, k);
                hb_key_info_clear(&info);
            }
        }
        if (pass_sec) hb_secret_free(pass_sec);
        if (priv) hb_secret_free(priv);
        hb_obj_free(k);
    }
}

static void ctl_strv_free(char ***sv)
{
    size_t i;
    if (!*sv) return;
    for (i = 0; (*sv)[i]; i++) free((*sv)[i]);
    g_clear_pointer(sv, free);
}

/* ------------------------- 对话框 cb ------------------------- */

/* TOFU / hostkey-changed 应答。
 * 对话框弹出期间对 ctl g_object_ref —— owner 中途销毁时回调仍拿到
 * 有效对象（dispose 已应答并置 sess=NULL）。 */
static void on_tofu_done(gboolean accept, gpointer ud)
{
    HbConnCtl *c = ud;
    set_open_dialog(c, NULL);
    c->awaiting_reply = FALSE;
    if (accept && c->pending_line)
        save_known(c, c->pending_host, c->pending_keyfp,
                   c->pending_line, FALSE);
    hb_session_hostkey_reply(c->sess, accept);
    g_clear_pointer(&c->pending_line, g_free);
    g_clear_pointer(&c->pending_host, g_free);
    g_clear_pointer(&c->pending_keyfp, g_free);
    g_object_unref(c);
}

static void on_hk_changed_done(gboolean accept, gpointer ud)
{
    HbConnCtl *c = ud;
    set_open_dialog(c, NULL);
    c->awaiting_reply = FALSE;
    if (accept && c->pending_line)
        save_known(c, c->pending_host, c->pending_keyfp,
                   c->pending_line, TRUE);
    hb_session_hostkey_reply(c->sess, accept);
    g_clear_pointer(&c->pending_line, g_free);
    g_clear_pointer(&c->pending_host, g_free);
    g_clear_pointer(&c->pending_keyfp, g_free);
    g_object_unref(c);
}

static void on_pw_done(const char *secret, gboolean remember, gpointer ud)
{
    HbConnCtl *c = ud;
    set_open_dialog(c, NULL);
    c->awaiting_reply = FALSE;
    if (secret && remember && c->pend_key_id) {
        /* 暂存到 CONNECTED 后写 vault */
        wipe_free(c->pend_secret);
        c->pend_secret = g_strdup(secret);
    }
    /* 不管记不记住，都暂存该 passphrase：连上后给公钥未知的
     * 加密导入 key 回填元数据（FR-02 首次使用补 public/fingerprint） */
    if (secret && c->pend_is_pass && c->prompt_key_id) {
        g_free(c->fill_key_id);
        c->fill_key_id = g_strdup(c->prompt_key_id);
        wipe_free(c->fill_pass);
        c->fill_pass = g_strdup(secret);
    }
    hb_session_prompt_reply(c->sess, secret);
    g_object_unref(c);
}

/* 使用前确认（FR-02 验收 2 的 Linux 形态）：收集目标 + 各跳板中
 * auth=key 且 key.confirm=true 的密钥；一个都不读 secret。 */
static GPtrArray *collect_confirm_keys(HbConnCtl *c)
{
    hb_vault *v = hb_store_vault(c->store);
    hb_obj *h = NULL;
    const char **jump = NULL;
    size_t nj = 0, i;
    GPtrArray *out = NULL;

    if (!c->host_id ||
        hb_vault_get(v, c->host_id, &h) != HB_OK || !h)
        return NULL;

    /* hops[0..nj-1]=jump hosts, [nj]=target —— 逐跳检查 */
    nj = hb_obj_get_strv(h, "jump", &jump);
    for (i = 0; i <= nj; i++) {
        hb_obj *hh = NULL, *k = NULL;
        const char *auth, *kid;
        if (i < nj) {
            if (hb_vault_get(v, jump[i], &hh) != HB_OK || !hh)
                continue;      /* missing jump 由 build_ctx 报错 */
        } else {
            hh = h;
        }
        auth = hb_obj_get_str(hh, "auth");
        kid = hb_obj_get_str(hh, "key_id");
        if (auth && !strcmp(auth, "key") && kid && *kid &&
            hb_vault_get(v, kid, &k) == HB_OK && k &&
            hb_obj_get_bool(k, "confirm", false)) {
            const char *kn = hb_obj_get_str(k, "name");
            const char *hu = hb_obj_get_str(hh, "user");
            const char *hn = hb_obj_get_str(hh, "name");
            if (!out)
                out = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(out,
                g_strdup_printf("%s → %s@%s",
                                kn && *kn ? kn : "key",
                                hu && *hu ? hu : "?",
                                hn && *hn ? hn : "?"));
        }
        if (k) hb_obj_free(k);
        if (i < nj && hh) hb_obj_free(hh);
    }
    free(jump);
    hb_obj_free(h);
    return out;
}

typedef struct {
    HbConnCtl *c;
    int        rows, cols;
    GPtrArray *lines;   /* 待确认密钥描述行 */
} KeyConfirmCtx;

static void on_key_confirm(gboolean ok, gpointer ud)
{
    KeyConfirmCtx *cc = ud;
    HbConnCtl *c = cc->c;
    int rows = cc->rows, cols = cc->cols;

    set_open_dialog(c, NULL);
    g_ptr_array_free(cc->lines, TRUE);
    g_free(cc);
    if (!c->dialog_parent ||
        !gtk_widget_get_root(c->dialog_parent)) {   /* owner 已销毁 */
        g_object_unref(c);
        return;
    }
    if (ok) {
        continue_connect(c, rows, cols);
    } else {
        /* 拒绝 → 不建会话（secret 从未被读）。
         * c->reuse_vt 不动：它属于下一次成功连接，不由本次拒绝销毁。 */
        g_signal_emit(c, sigs[SIG_FAILED], 0, 0,
                      "已拒绝使用密钥，连接已取消", NULL);
        log_finish(c, "cancelled", "key confirm denied");
        c->log_id = 0;
    }
    g_object_unref(c);
}

/* ------------------------- continue_connect ------------------------- */

/* 真正起会话（确认通过或无需确认后进入）：杀旧会话、build_ctx、
 * new_ssh/new_sftp。 */
static void continue_connect(HbConnCtl *c, int rows, int cols)
{
    ConnCtx *x;
    hb_vt *reuse;
    hb_obj *thost = NULL;

    /* 重连：owner 已把旧会话的 vt 偷出来交给 start，挂在 c->reuse_vt；
     * 普通 start 则没有。此处消费（接管所有权）。 */
    reuse = c->reuse_vt;
    c->reuse_vt = NULL;

    if (c->sess) {
        hb_session_close(c->sess);
        hb_session_free(c->sess);
        c->sess = NULL;
    }

    /* 上一次的 hop 归属表作废 */
    ctl_strv_free(&c->hop_ids);
    ctl_strv_free(&c->conn_key_ids);

    x = build_ctx(c, &thost);
    /* hop 归属表 + 本次连接用到的 key：转给 ctl（ctx 释放前搬出） */
    c->hop_ids = x ? x->hop_ids : NULL;
    if (x) x->hop_ids = NULL;
    {
        size_t i, nk = 0;
        if (x && x->key_ids) {
            for (i = 0; i <= x->nhops; i++)
                if (x->key_ids[i]) nk++;
            c->conn_key_ids = calloc(nk + 1, sizeof(char *));
            nk = 0;
            for (i = 0; i <= x->nhops; i++)
                if (x->key_ids[i]) {
                    c->conn_key_ids[nk++] = x->key_ids[i];
                    x->key_ids[i] = NULL;
                }
        }
    }
    if (x && x->missing_jump) {
        char *m = g_strdup_printf("跳板主机不存在或已删除（%s）",
                                  x->missing_jump);
        g_signal_emit(c, sigs[SIG_FAILED], 0, 0, m, NULL);
        g_free(m);
        log_finish(c, "error", "jump host missing");
        c->log_id = 0;
        if (reuse) hb_vt_free(reuse);
        conn_ctx_free(x);
        if (thost) hb_obj_free(thost);
        return;
    }
    if (!x || !x->o.target.host || !*x->o.target.host) {
        log_finish(c, "error", "host object missing");
        c->log_id = 0;
        g_signal_emit(c, sigs[SIG_FAILED], 0, 0,
                      "无法读取主机信息", NULL);
        if (reuse) hb_vt_free(reuse);
        conn_ctx_free(x);
        if (thost) hb_obj_free(thost);
        return;
    }
    /* new_ssh 深拷贝全部 opts —— target/jump host 对象与 secret 至此可释 */
    if (c->mode == HB_CONN_SFTP) {
        c->sess = hb_session_new_sftp(&x->o);
        if (reuse) hb_vt_free(reuse);   /* SFTP 无 vt，入参作废 */
    } else if (c->mode == HB_CONN_FORWARD) {
        char *ferr = NULL;
        if (reuse) hb_vt_free(reuse);   /* FORWARD 无 vt，入参作废 */
        /* 断开检测靠 keepalive：host 显式关掉时也兜底 30s。 */
        if (x->o.keepalive_s <= 0)
            x->o.keepalive_s = 30;
        c->sess = hb_session_new_forward(&x->o, &c->rule, NULL, &ferr);
        if (!c->sess) {
            /* 绑定冲突等（code=0：不可重试，由 owner 置 FAILED） */
            char *t = g_strdup_printf("无法监听 %s:%d",
                c->rule.bind_addr && *c->rule.bind_addr
                    ? c->rule.bind_addr : "127.0.0.1",
                c->rule.bind_port);
            log_finish(c, "error", ferr ? ferr : "bind failed");
            c->log_id = 0;
            g_signal_emit(c, sigs[SIG_FAILED], 0, 0, t, ferr);
            g_free(t);
            free(ferr);
            conn_ctx_free(x);
            if (thost) hb_obj_free(thost);
            return;
        }
    } else {
        x->o.reuse_vt = reuse;
        c->sess = hb_session_new_ssh(&x->o, rows, cols,
                                     ctl_scrollback(c));
    }
    conn_ctx_free(x);
    if (thost) hb_obj_free(thost);
    if (!c->sess) {
        log_finish(c, "error", "session alloc failed");
        c->log_id = 0;
        g_signal_emit(c, sigs[SIG_FAILED], 0, 0,
                      "无法创建 SSH 会话",
                      "主机配置缺少必填字段（如用户名）");
        if (reuse) hb_vt_free(reuse);
        return;
    }
    g_signal_emit(c, sigs[SIG_SESSION], 0, c->sess);
}

/* settings.scrollback（与页面 apply_settings 同源的取值） */
static int ctl_scrollback(HbConnCtl *c)
{
    hb_obj *o = NULL;
    int sb = 10000;
    if (hb_vault_settings(hb_store_vault(c->store), &o) == HB_OK && o) {
        sb = (int)hb_obj_get_int(o, "scrollback", 10000);
        hb_obj_free(o);
    }
    return sb;
}

/* ------------------------- 对外 API ------------------------- */

HbConnCtl *hb_conn_ctl_new(HbStore *st, GtkWidget *dialog_parent,
                           HbConnMode mode)
{
    HbConnCtl *c = g_object_new(HB_TYPE_CONN_CTL, NULL);
    c->store = st;
    c->mode = mode;
    c->dialog_parent = dialog_parent;
    if (dialog_parent)
        g_object_add_weak_pointer(G_OBJECT(dialog_parent),
                                  (gpointer)&c->dialog_parent);
    return c;
}

void hb_conn_ctl_start(HbConnCtl *c, const char *host_id,
                       hb_vt *reuse_vt, int rows, int cols)
{
    hb_obj *h = NULL;

    /* 先强关上一次尝试挂起的模态（密钥确认/TOFU/口令）：其 cb 会按
     * "拒绝/取消"路径收尾旧尝试，保证任何时刻最多一个挂起对话框。 */
    force_close_dialog(c);
    /* 上一个 session（重连时的死会话）先收掉 —— 同原 reconnect_now
     * 的顺序：确认对话框弹出前旧会话已死。 */
    if (c->sess) {
        if (c->awaiting_reply) {
            hb_session_prompt_reply(c->sess, NULL);
            hb_session_hostkey_reply(c->sess, FALSE);
            c->awaiting_reply = FALSE;
        }
        hb_session_close(c->sess);
        hb_session_free(c->sess);
        c->sess = NULL;
    }

    if (reuse_vt) {
        if (c->reuse_vt) hb_vt_free(c->reuse_vt);   /* 不应发生，兜底 */
        c->reuse_vt = reuse_vt;
    }

    g_free(c->host_id);
    c->host_id = g_strdup(host_id);
    /* 调用方本次没交 vt = 非重连开始（含拒绝后重试）：复位连接态。
     * 存留的 c->reuse_vt 仍会在 continue_connect 里被消费 ——
     * 与原"start_over 也会吃掉偷来的 vt"一致。 */
    if (!reuse_vt)
        c->was_connected = FALSE;

    /* 日志行显示字段（host 对象此刻的值） */
    g_free(c->hname);
    g_free(c->huser);
    c->hname = NULL;
    c->huser = NULL;
    if (c->host_id &&
        hb_vault_get(hb_store_vault(c->store), c->host_id, &h) == HB_OK && h) {
        const char *nm = hb_obj_get_str(h, "name");
        const char *un = hb_obj_get_str(h, "user");
        c->hname = g_strdup(nm && *nm ? nm : "host");
        c->huser = g_strdup(un && *un ? un : g_get_user_name());
        hb_obj_free(h);
    }

    log_start(c);

    /* 使用确认：在读任何 secret 之前（build_ctx 会 get_secret） */
    {
        GPtrArray *cf = collect_confirm_keys(c);
        if (cf && cf->len) {
            KeyConfirmCtx *cc = g_new0(KeyConfirmCtx, 1);
            GString *mk = g_string_new(NULL);
            size_t i;
            g_string_append(mk, "以下密钥开启了<b>使用前确认</b>：\n");
            for (i = 0; i < cf->len; i++) {
                char *e = g_markup_escape_text(
                    (char *)g_ptr_array_index(cf, i), -1);
                g_string_append_printf(mk, "  • %s\n", e);
                g_free(e);
            }
            g_string_append(mk, "允许使用这些密钥登录吗？");
            cc->c = c;
            cc->rows = rows;
            cc->cols = cols;
            cc->lines = cf;
            g_object_ref(c);   /* 对话框期间保活 */
            set_open_dialog(c,
                hb_dialog_confirm(c->dialog_parent, "使用密钥",
                                  mk->str, "允许使用", FALSE,
                                  on_key_confirm, cc));
            g_string_free(mk, TRUE);
            return;
        }
        if (cf) g_ptr_array_free(cf, TRUE);
    }
    continue_connect(c, rows, cols);
}

void hb_conn_ctl_cancel(HbConnCtl *c)
{
    force_close_dialog(c);
    if (!c->sess) return;
    if (c->awaiting_reply) {
        /* 让阻塞中的应答先返回，避免线程挂住导致 free 卡住 */
        hb_session_prompt_reply(c->sess, NULL);
        hb_session_hostkey_reply(c->sess, FALSE);
        c->awaiting_reply = FALSE;
    }
    hb_session_close(c->sess);
    hb_session_free(c->sess);
    c->sess = NULL;
}

hb_session *hb_conn_ctl_session(HbConnCtl *c)
{
    return c->sess;
}

void hb_conn_ctl_set_forward(HbConnCtl *c, const hb_pf_rule *rule)
{
    if (!rule) return;
    c->rule = *rule;
    g_free(c->rule_bind);
    g_free(c->rule_dest);
    c->rule_bind = g_strdup(rule->bind_addr);
    c->rule_dest = g_strdup(rule->dest_addr);
    c->rule.bind_addr = c->rule_bind;
    c->rule.dest_addr = c->rule_dest;
}

/* ------------------------- 事件处理 ------------------------- */

static void on_hostkey_ev(HbConnCtl *c, const hb_sess_event *ev)
{
    /* code 0=未知 1=变更；text=host:port text2="keytype FP" extra=line\nart */
    const char *line = NULL;
    char *art = parse_extra(ev->extra, &line);
    char *kt = NULL;
    const char *hp = ev->text;
    /* 拆 text2 出 keytype 用于 tag */
    if (ev->text2) {
        char *sp = strchr(ev->text2, ' ');
        kt = g_strndup(ev->text2, sp ? (gsize)(sp - ev->text2)
                                   : strlen(ev->text2));
    }
    c->awaiting_reply = TRUE;
    g_free(c->pending_host);  c->pending_host  = g_strdup(hp);
    g_free(c->pending_keyfp); c->pending_keyfp = g_strdup(ev->text2);
    g_free(c->pending_line);  c->pending_line  = g_strdup(line);
#ifndef NDEBUG
    if (dev_accept_hk()) {
        if (line) save_known(c, hp, ev->text2, line, ev->code == 1);
        c->awaiting_reply = FALSE;
        hb_session_hostkey_reply(c->sess, TRUE);
        g_free(art); g_free(kt);
        return;
    }
#endif
    g_object_ref(c);   /* 对话框存活期间保活（done cb 里 unref） */
    if (ev->code == 1) {
        /* 需要旧指纹：从 vault 里查 */
        char *oldfp = g_strdup("(未知)");
        {
            char host[256]; int port = 22;
            /* hp 形如 "h:p" 或 "[h]:p" */
            const char *s = hp;
            if (*s == '[') {
                const char *rb = strchr(s, ']');
                if (rb) {
                    g_snprintf(host, sizeof host, "%.*s",
                               (int)(rb - s - 1), s + 1);
                    if (rb[1] == ':') port = atoi(rb + 2);
                }
            } else {
                const char *cc = strrchr(s, ':');
                if (cc) {
                    g_snprintf(host, sizeof host, "%.*s",
                               (int)(cc - s), s);
                    port = atoi(cc + 1);
                } else g_strlcpy(host, s, sizeof host);
            }
            {
                hb_obj **ob; size_t n, i;
                g_free(oldfp);
                oldfp = g_strdup("(无本地记录)");
                ob = hb_store_list(c->store, "known", &n);
                for (i = 0; i < n; i++) {
                    const char *oh = hb_obj_get_str(ob[i], "host");
                    int op = (int)hb_obj_get_int(ob[i], "port", 22);
                    if (oh && !strcmp(oh, host) && op == port) {
                        const char *f = hb_obj_get_str(ob[i],
                                                       "fingerprint");
                        g_free(oldfp);
                        oldfp = g_strdup(f ? f : "(无)");
                        break;
                    }
                }
                for (i = 0; i < n; i++) hb_obj_free(ob[i]);
                free(ob);
            }
        }
        set_open_dialog(c,
            hb_dialog_hostkey_changed(ctl_win(c), hp,
                kt ? kt : "", oldfp,
                ev->text2 ? (strchr(ev->text2,' ') ?
                             strchr(ev->text2,' ')+1 : ev->text2) : "",
                on_hk_changed_done, c));
        g_free(oldfp);
    } else {
        set_open_dialog(c,
            hb_dialog_tofu(ctl_win(c), hp,
                kt ? kt : "",
                ev->text2 ? (strchr(ev->text2,' ') ?
                             strchr(ev->text2,' ')+1 : ev->text2) : "",
                art ? art : "", on_tofu_done, c));
    }
    g_free(art); g_free(kt);
}

static void on_prompt_ev(HbConnCtl *c, const hb_sess_event *ev)
{
    gboolean is_pass = (ev->code == 1);
    const char *hop_host = c->host_id;
    c->awaiting_reply = TRUE;
    g_free(c->pend_key_id);
    c->pend_key_id = NULL;
    g_free(c->prompt_key_id);
    c->prompt_key_id = NULL;
    c->pend_is_pass = is_pass;
    /* 归属到具体 hop：text2 是 hop 下标（jumps 0..n-1，njumps=target）；
     * ProxyJump 时提示可能来自某跳板，不能总记到目标主机。 */
    if (ev->text2 && c->hop_ids) {
        long idx = strtol(ev->text2, NULL, 10);
        size_t n = 0;
        while (c->hop_ids[n]) n++;
        if (idx >= 0 && (size_t)idx < n && c->hop_ids[idx])
            hop_host = c->hop_ids[idx];
    }
    /* 记住到哪个对象：passphrase → 该 hop 的 key_id
     *（identity_file 则该 hop 主机自身）；密码 → 该 hop 主机 */
    {
        hb_obj *h = NULL;
        if (hop_host && hb_vault_get(
                hb_store_vault(c->store), hop_host, &h) == HB_OK && h) {
            const char *kid = hb_obj_get_str(h, "key_id");
            if (is_pass && kid && *kid) {
                c->pend_key_id = g_strdup(kid);
                c->prompt_key_id = g_strdup(kid);
            } else {
                c->pend_key_id = g_strdup(hop_host);
            }
            hb_obj_free(h);
        } else c->pend_key_id = g_strdup(hop_host);
    }
#ifndef NDEBUG
    if (dev_cancel_prompt()) {
        c->awaiting_reply = FALSE;
        hb_session_prompt_reply(c->sess, NULL);
        return;
    }
#endif
    g_object_ref(c);   /* 同 HOSTKEY：done cb 里 unref */
    set_open_dialog(c,
        hb_dialog_password(ctl_win(c),
            ev->text ? ev->text : "Password:", is_pass,
            TRUE /* show remember */, on_pw_done, c));
}

static void on_connected_ev(HbConnCtl *c)
{
    hb_obj *h = NULL;

    c->was_connected = TRUE;

    /* "ok"·进行中：duration=-1；EXIT/销毁时再写真实时长 */
    if (c->log_id > 0)
        hb_vault_log_finish(hb_store_vault(c->store), c->log_id,
                            "ok", NULL, -1);

    save_pending_secret(c);
    fill_key_metadata(c);
    wipe_free(c->fill_pass);
    c->fill_pass = NULL;
    g_clear_pointer(&c->fill_key_id, g_free);
    g_clear_pointer(&c->prompt_key_id, g_free);
    ctl_strv_free(&c->conn_key_ids);

    /* last_connected */
    if (c->host_id &&
        hb_vault_get(hb_store_vault(c->store), c->host_id,
                     &h) == HB_OK && h) {
        hb_obj_set_int(h, "last_connected", g_get_real_time() / 1000);
        hb_vault_put(hb_store_vault(c->store), h);
        hb_obj_free(h);
    }

    {
        /* 信号 handler（autostart 等）可能 cancel/释放 owner：
         * 发射期间给 c 加引用，并先拷出 host_id。 */
        char *hid = g_strdup(c->host_id);
        g_object_ref(c);
        g_signal_emit(c, sigs[SIG_CONNECTED], 0);

        /* FR-06 联动：SHELL/SFTP 连上主机 X → 拉起 via==X 且
         * autostart 的转发规则（FORWARD 自身不触发，避免规则互相牵扯）。 */
        if (c->mode != HB_CONN_FORWARD && hid)
            hb_forward_mgr_autostart(hid);
        g_free(hid);
        g_object_unref(c);
    }
}

static void on_error_ev(HbConnCtl *c, const hb_sess_event *ev)
{
    /* 红色日志行先走 stage（mark='e'，idx=-1 不推进节点） */
    g_signal_emit(c, sigs[SIG_STAGE], 0, -1,
                  ev->text ? ev->text : "连接失败", (gchar)'e');
    /* 连接没成功 → 记住的凭据不再有用，立刻抹掉明文 */
    wipe_free(c->pend_secret);
    c->pend_secret = NULL;
    g_clear_pointer(&c->pend_key_id, g_free);
    if (c->was_connected) {
        /* 已连接后掉线 / 重连尝试失败：
         * 凭据/密钥类错误 → 停重试（需用户介入），错误卡；
         * 其余 → owner 继续指数退避。 */
        switch (ev->code) {
        case HB_SSH_ERR_CANCELLED:
        case HB_SSH_ERR_AUTH:
        case HB_SSH_ERR_HOSTKEY_CHANGED:
        case HB_SSH_ERR_HOSTKEY_REJECTED:
            log_finish(c, hb_ssh_err_name(ev->code),
                       ev->text ? ev->text : "error");
            c->log_id = 0;
            g_signal_emit(c, sigs[SIG_FAILED], 0, ev->code,
                          ev->text, ev->text2);
            break;
        default:
            g_signal_emit(c, sigs[SIG_DISCONN], 0);
            break;
        }
    } else {
        /* 首次连接期失败 → 错误卡 */
        log_finish(c, hb_ssh_err_name(ev->code),
                   ev->text ? ev->text : "error");
        c->log_id = 0;
        g_signal_emit(c, sigs[SIG_FAILED], 0, ev->code,
                      ev->text, ev->text2);
    }
}

gboolean hb_conn_ctl_handle_event(HbConnCtl *c, const hb_sess_event *ev)
{
    switch (ev->type) {
    case HB_SESS_EV_STAGE:
        g_signal_emit(c, sigs[SIG_STAGE], 0, ev->code,
                      ev->text ? ev->text : "", (gchar)0);
        return TRUE;

    case HB_SESS_EV_HOSTKEY:
        on_hostkey_ev(c, ev);
        return TRUE;

    case HB_SESS_EV_PROMPT:
        on_prompt_ev(c, ev);
        return TRUE;

    case HB_SESS_EV_CONNECTED:
        on_connected_ev(c);
        return TRUE;

    case HB_SESS_EV_OSINFO:
        g_signal_emit(c, sigs[SIG_OSINFO], 0, ev->text, ev->text2);
        return TRUE;

    case HB_SESS_EV_ERROR:
        on_error_ev(c, ev);
        return TRUE;

    case HB_SESS_EV_EXIT:
        /* 终端页里 EXIT 被 HbTerminal 消费后以合成事件转发至此；
         * SFTP 面板直接轮询 fd 时会原生到达。 */
        if (!c->was_connected) {
            wipe_free(c->pend_secret);
            c->pend_secret = NULL;
            g_clear_pointer(&c->pend_key_id, g_free);
        }
        log_finish(c, c->was_connected ? "ok" : "cancelled", NULL);
        c->log_id = 0;   /* 归零避免二次 finish */
        g_signal_emit(c, sigs[SIG_EXITED], 0, ev->code);
        return TRUE;

    default:
        return FALSE;
    }
}

/* ------------------------- 生命周期 ------------------------- */

static void hb_conn_ctl_dispose(GObject *o)
{
    HbConnCtl *c = HB_CONN_CTL(o);
    hb_conn_ctl_cancel(c);
    if (c->log_id > 0) {
        /* owner 销毁时若还连着/连着过：给日志一个收尾 */
        hb_vault_log_finish(hb_store_vault(c->store), c->log_id,
                            c->was_connected ? "ok" : "cancelled",
                            NULL,
                            (g_get_monotonic_time() - c->t0) / G_USEC_PER_SEC);
        c->log_id = 0;
    }
    if (c->reuse_vt) {
        hb_vt_free(c->reuse_vt);
        c->reuse_vt = NULL;
    }
    if (c->dialog_parent) {
        g_object_remove_weak_pointer(G_OBJECT(c->dialog_parent),
                                     (gpointer)&c->dialog_parent);
        c->dialog_parent = NULL;
    }
    G_OBJECT_CLASS(hb_conn_ctl_parent_class)->dispose(o);
}

static void hb_conn_ctl_finalize(GObject *o)
{
    HbConnCtl *c = HB_CONN_CTL(o);
    g_free(c->host_id);
    g_free(c->hname);
    g_free(c->huser);
    wipe_free(c->pend_secret);
    g_free(c->pend_key_id);
    ctl_strv_free(&c->hop_ids);
    ctl_strv_free(&c->conn_key_ids);
    g_free(c->prompt_key_id);
    g_free(c->fill_key_id);
    wipe_free(c->fill_pass);
    g_free(c->pending_line);
    g_free(c->pending_host);
    g_free(c->pending_keyfp);
    g_free(c->rule_bind);
    g_free(c->rule_dest);
    G_OBJECT_CLASS(hb_conn_ctl_parent_class)->finalize(o);
}

static void hb_conn_ctl_class_init(HbConnCtlClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_conn_ctl_dispose;
    oc->finalize = hb_conn_ctl_finalize;

    sigs[SIG_STAGE] = g_signal_new("stage", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_CHAR);
    sigs[SIG_SESSION] = g_signal_new("session", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
        G_TYPE_POINTER);
    sigs[SIG_CONNECTED] = g_signal_new("connected", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    sigs[SIG_FAILED] = g_signal_new("failed", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    sigs[SIG_DISCONN] = g_signal_new("disconnected", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    sigs[SIG_EXITED] = g_signal_new("exited", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
        G_TYPE_INT);
    sigs[SIG_OSINFO] = g_signal_new("osinfo", HB_TYPE_CONN_CTL,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
        G_TYPE_STRING, G_TYPE_STRING);
}

static void hb_conn_ctl_init(HbConnCtl *c)
{
    c->log_id = -1;
}
