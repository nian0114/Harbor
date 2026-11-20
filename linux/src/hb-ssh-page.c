#include "hb-ssh-page.h"
#include "hb-terminal.h"
#include "hb-find-bar.h"
#include "hb-term-theme.h"
#include "hb-icons.h"
#include "hb-notify.h"
#include "hb-dialogs.h"
#include "hb-window.h"
#include "hb-watch-ui.h"

#include <harbor/ssh.h>
#include <harbor/keys.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ==================================================================
 * SSH 连接页：连接进度（节点+日志）→ 原地切换终端 → 断线自动重连。
 *
 * 事件流（来自 HbTerminal 的 ev_cb，见 hb_terminal_set_event_cb）：
 *   STAGE      连接阶段日志行 → 进度节点 + 日志列表
 *   HOSTKEY    TOFU / 密钥变更对话框 → hostkey_reply
 *   PROMPT     密码/私钥口令对话框 → prompt_reply
 *   CONNECTED  切到终端视图
 *   OSINFO     探测到的远端 OS → 更新 host.os + 头像
 *   ERROR      连接期 → 错误卡；已连接 → 断线条 + 指数退避重连
 *   EXIT       干净退出 → dead 状态 + 重连按钮
 * ================================================================== */

struct _HbSshPage {
    GtkBox parent;

    HbStore    *store;

    /* 所在 tab（app 在 add_tab 后 attach，用于改标题/图标/dot） */
    HbWindow   *win;
    char       *tab_id;

    /* 主机信息（深拷贝的显示字段；完整对象每次连接重读） */
    char       *host_id;
    char       *hname;
    char       *haddr;
    char       *huser;
    int         hport;
    char       *os_key;         /* tab 头像键 */

    /* 会话 */
    hb_session *sess;
    gboolean    was_connected;  /* 曾经 CONNECTED（区分连接期/运行期错误） */
    gboolean    connecting;     /* 当前处于连接流程（含重连尝试） */
    gboolean    dead;           /* 干净 EXIT 后的终态 */
    gboolean    awaiting_reply; /* 对话框挂起中（关闭 tab 先应答再释放） */

    /* ---- connect 视图 ---- */
    GtkWidget  *stack;
    GtkWidget  *node[3];        /* link / shield / terminal */
    GtkWidget  *bar[2];
    GtkWidget  *log_box;
    GtkWidget  *log_scroll;
    GtkWidget  *hide_btn;
    gboolean    hide_log;
    int         cur_stage;
    GtkWidget  *err_card;       /* 连接期错误卡片（NULL=无） */
    GtkWidget  *conn_foot;      /* connect 视图底部按钮行（错误卡显示时隐藏） */
    AdwDialog  *open_dialog;    /* 当前挂起的模态（TOFU/changed/密码） */

    /* ---- 终端视图 ---- */
    GtkWidget  *term_view;
    GtkWidget  *term;
    GtkWidget  *overlay;
    GtkWidget  *tt_label;
    GtkWidget  *tt_icon;
    GtkWidget  *reconn_btn;
    GtkWidget  *watch_btn;
    GtkWidget  *find_bar;
    GtkWidget  *st_conn;
    GtkWidget  *st_sb;
    GtkWidget  *st_watch;
    GtkWidget  *dead_lbl;
    GtkWidget  *strip;          /* 断线重连条 */
    GtkWidget  *strip_label;
    GtkWidget  *buf_hint;       /* "输入将在重连后发送" */

    /* FR-13 完成监控（会话级状态，重连后重挂到新 session） */
    gboolean    watch_on;
    unsigned    watch_signals;

    /* 自动重连 */
    guint       backoff_src;
    int         backoff_k;      /* 第 k 次重连（0=未在退避） */
    int         countdown;      /* 距下次重连的秒数 */
    GString    *inbuf;          /* 断线期本地缓冲（≤4KiB） */

    /* vault 日志 */
    int64_t     log_id;
    gint64      t0;             /* 连接开始 ts（µs） */

    /* 待保存的"记住"凭据（CONNECTED 后写入） */
    char       *pend_secret;    /* malloc 文本；写完即清 */
    char       *pend_key_id;    /* passphrase → key/host id；密码 → host id */
    gboolean    pend_is_pass;

    /* PROMPT 归属 + 首次使用补元数据 + 使用前确认 */
    char      **hop_ids;        /* hop 下标 → host 对象 id（jumps + target） */
    char      **conn_key_ids;   /* 本次连接用到的 key 对象 id（strv） */
    char       *prompt_key_id;  /* 当前 PROMPT 归属的 key 对象 id（无=NULL） */
    char       *fill_key_id;    /* 已提示口令的 key 对象（CONNECTED 补元数据） */
    char       *fill_pass;      /* 该口令副本（用后抹掉） */

    /* HOSTKEY 事件挂起期间的暂存（对话框 cb 用） */
    char       *pending_line;
    char       *pending_host;
    char       *pending_keyfp;

    /* 重连：从死会话偷出的 vt，供下一次 start_connect 复用 */
    hb_vt      *reuse_vt;
    gboolean    was_reconnect;
    gboolean    reconnected_feed;   /* 连上后注入"重新连接成功"提示 */

    GtkWidget  *connect_box;
    gulong      changed_handler;

    char       *theme_id;
    int         sb_lines;
};

G_DEFINE_TYPE(HbSshPage, hb_ssh_page, GTK_TYPE_BOX)

enum { SIG_EDIT_HOST, N_SIGS };
static guint sigs[N_SIGS];

extern char *hb_opt_theme;      /* main.c dev 选项 */

static void start_connect(HbSshPage *p, gboolean is_reconnect);
static void free_session(HbSshPage *p);

/* ------------------------- 工具 ------------------------- */

static HbWindow *page_win(HbSshPage *p)
{
    if (p->win) return p->win;
    GtkRoot *r = gtk_widget_get_root(GTK_WIDGET(p));
    return HB_IS_WINDOW(r) ? HB_WINDOW(r) : NULL;
}

static void tab_title(HbSshPage *p, const char *t)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id) hb_window_set_tab_title(w, p->tab_id, t);
}
static void tab_icon(HbSshPage *p, const char *ic)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id) hb_window_set_tab_icon(w, p->tab_id, ic);
}
static void tab_dot(HbSshPage *p, const char *dot)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id) hb_window_set_tab_dot(w, p->tab_id, dot);
}
static void tab_close(HbSshPage *p)
{
    HbWindow *w = page_win(p);
    if (w && p->tab_id) hb_window_close_tab(w, p->tab_id);
}

static const char *os_avatar(const char *os)
{
    /* host.os → 头像/tab 图标键 */
    if (!os || !*os || !strcmp(os, "auto")) return "asterisk";
    return hb_os_avatar_key(os);
}

/* ------------------------- 终端状态栏 ------------------------- */

static void set_status(HbSshPage *p, const char *txt, const char *cls)
{
    gtk_label_set_label(GTK_LABEL(p->st_conn), txt);
    gtk_widget_remove_css_class(p->st_conn, "hb-term-ok");
    gtk_widget_remove_css_class(p->st_conn, "hb-term-ended");
    gtk_widget_remove_css_class(p->st_conn, "hb-term-warn");
    if (cls) gtk_widget_add_css_class(p->st_conn, cls);
}

/* ------------------------- 连接进度节点 ------------------------- */

/* stage 0-1 → node0, 2 → node1, 3-4 → node2 */
static int node_of(int stage)
{
    if (stage <= 1) return 0;
    if (stage == 2) return 1;
    return 2;
}

static void set_node(GtkWidget *n, const char *state)
{
    gtk_widget_remove_css_class(n, "pending");
    gtk_widget_remove_css_class(n, "active");
    gtk_widget_remove_css_class(n, "spin");
    gtk_widget_remove_css_class(n, "done");
    if (state) gtk_widget_add_css_class(n, state);
}

static void update_nodes(HbSshPage *p)
{
    int cur = node_of(p->cur_stage), i;
    for (i = 0; i < 3; i++) {
        if (i < cur)       set_node(p->node[i], "done");
        else if (i == cur) set_node(p->node[i], "active spin");
        else               set_node(p->node[i], "pending");
    }
    for (i = 0; i < 2; i++) {
        gtk_widget_remove_css_class(p->bar[i], "pending");
        gtk_widget_remove_css_class(p->bar[i], "done");
        gtk_widget_add_css_class(p->bar[i],
                               i < cur ? "done" : "pending");
    }
}

static void add_log_line(HbSshPage *p, const char *text, gboolean err)
{
    GtkWidget *row, *l;
    GtkWidget *v;
    if (!p->log_box) return;
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "connect-log-line");
    gtk_box_append(GTK_BOX(row),
        hb_icon_new(err ? "x" : "user", 13));
    l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    if (err) gtk_widget_add_css_class(l, "hb-red");
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(p->log_box), row);
    /* 滚到底 */
    v = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(p->log_scroll));
    if (v) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(p->log_scroll));
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
    }
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

/* 抹零释放（密码/passphrase/凭据副本统一走它） */
static void wipe_free(char *s)
{
    if (!s) return;
    for (char *q = s; *q; q++) *q = 0;
    free(s);
}

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
static void collect_known(HbSshPage *p, ConnCtx *c)
{
    hb_obj **objs = NULL;
    size_t n = 0, i, k = 0;
    objs = hb_store_list(p->store, "known", &n);
    c->known = calloc(n + 1, sizeof(char *));
    for (i = 0; i < n; i++) {
        const char *line = hb_obj_get_str(objs[i], "line");
        if (line && *line)
            c->known[k++] = strdup(line);
    }
    c->known[k] = NULL;
    c->o.known_lines = (const char *const *)c->known;
    c->o.nknown = k;
    if (objs) hb_obj_list_free(objs, n);
}

/* 单个 host 对象 → hop；secret 进 ctx->secrets（调用方清零释放）。
 * hop 的字符串字段指入 h —— h 必须活到 hb_session_new_ssh 返回。 */
static void host_to_hop(HbSshPage *p, ConnCtx *c, hb_obj *h, hb_ssh_hop *hop)
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
        hop->password = hb_vault_get_secret(hb_store_vault(p->store),
                                            hb_obj_id(h), "password");
        ctx_sec_add(c, (char *)hop->password);
    } else if (!strcmp(auth, "key") && kid && *kid) {
        hop->auth = HB_SSH_AUTH_KEY;
        hop->privkey = hb_vault_get_secret(hb_store_vault(p->store),
                                           kid, "private");
        hop->passphrase = hb_vault_get_secret(hb_store_vault(p->store),
                                              kid, "passphrase");
        ctx_sec_add(c, (char *)hop->privkey);
        ctx_sec_add(c, (char *)hop->passphrase);
    } else if (!strcmp(auth, "key")) {
        hop->auth = HB_SSH_AUTH_KEY;
        hop->identity_file = hb_obj_get_str(h, "identity_file");
        /* 主机级 "passphrase" secret：identity_file 的"记住口令" */
        hop->passphrase = hb_vault_get_secret(hb_store_vault(p->store),
                                            hb_obj_id(h), "passphrase");
        ctx_sec_add(c, (char *)hop->passphrase);
    } else if (!strcmp(auth, "agent")) {
        hop->auth = HB_SSH_AUTH_AGENT;
    } else if (!strcmp(auth, "none")) {
        hop->auth = HB_SSH_AUTH_NONE;
    } else {
        hop->auth = HB_SSH_AUTH_AUTO;
        hop->identity_file = hb_obj_get_str(h, "identity_file");
        hop->passphrase = hb_vault_get_secret(hb_store_vault(p->store),
                                            hb_obj_id(h), "passphrase");
        ctx_sec_add(c, (char *)hop->passphrase);
    }
}

/* 组装本次连接的 hb_ssh_opts（target + jumps + known + 杂项）。
 * target/jump 的字符串指入各自的 host 对象；target host 对象在
 * start_connect 里存活到 new_ssh 返回，jump hosts 挂在 ctx->jhosts 上。 */
static ConnCtx *build_ctx(HbSshPage *p, hb_obj **target_host_out)
{
    ConnCtx *c = calloc(1, sizeof *c);
    hb_obj *h = NULL;
    const char **jump_ids = NULL;
    size_t nj = 0, i;

    if (!p->host_id ||
        hb_vault_get(hb_store_vault(p->store), p->host_id, &h) != HB_OK || !h)
        goto out;

    host_to_hop(p, c, h, &c->o.target);

    /* jumps：host.jump 是 strv 的 host id，外→内
     * （get_strv 返回的数组要 free，元素是借用的 valuestring） */
    nj = hb_obj_get_strv(h, "jump", &jump_ids);
    c->nhops = nj + 1;
    c->hop_ids = calloc(c->nhops + 1, sizeof(char *));
    c->key_ids = calloc(c->nhops + 1, sizeof(char *));
    c->hop_ids[nj] = strdup(hb_obj_id(h));
    {
        const char *tk = hb_obj_get_str(h, "key_id");
        c->key_ids[nj] = tk && *tk ? strdup(tk) : NULL;
    }
    if (nj) {
        c->jumps = calloc(nj, sizeof(hb_ssh_hop));
        c->jhosts = calloc(nj + 1, sizeof(hb_obj *));
        for (i = 0; i < nj; i++) {
            hb_obj *jh = NULL;
            if (hb_vault_get(hb_store_vault(p->store), jump_ids[i],
                             &jh) != HB_OK || !jh) {
                c->missing_jump = strdup(jump_ids[i]);
                break;
            }
            c->hop_ids[i] = strdup(hb_obj_id(jh));
            {
                const char *jk = hb_obj_get_str(jh, "key_id");
                c->key_ids[i] = jk && *jk ? strdup(jk) : NULL;
            }
            host_to_hop(p, c, jh, &c->jumps[i]);
            c->jhosts[i] = jh;   /* 保活到 ctx 释放 */
        }
        if (c->missing_jump)
            nj = 0;
        free(jump_ids);
        c->o.jumps = c->jumps;
        c->o.njumps = nj;
    }

    collect_known(p, c);

    c->o.connect_timeout_s = 15;
    /* keepalive 缺省 30s（拔线 3 间隔≈90s 判死）；显式 0 = 关闭 */
    {
        int64_t ka = hb_obj_get_int(h, "keepalive", -1);
        c->o.keepalive_s = ka < 0 ? 30 : (int)ka;
    }
    c->o.term = "xterm-256color";
    {
        const char *os = hb_obj_get_str(h, "os");
        c->o.detect_os = (!os || !*os || !strcmp(os, "auto"));
    }
out:
    *target_host_out = h;       /* 调用方持有至 new_ssh 后释放 */
    return c;
}

/* ------------------------- vault 日志 ------------------------- */

static void log_start(HbSshPage *p)
{
    hb_log l = {0};
    l.user = p->huser;
    l.host = p->hname;
    l.host_id = p->host_id;
    l.proto = "SSH";
    l.result = "connecting";
    l.duration_s = -1;
    l.saved = false;
    hb_vault_log_add(hb_store_vault(p->store), &l, &p->log_id);
    p->t0 = g_get_monotonic_time();
}

static void log_finish(HbSshPage *p, const char *result, const char *reason)
{
    gint64 dur;
    if (p->log_id <= 0) return;
    dur = (g_get_monotonic_time() - p->t0) / G_USEC_PER_SEC;
    hb_vault_log_finish(hb_store_vault(p->store), p->log_id,
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

static void save_known(HbSshPage *p, const char *host_port,
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
        char *c = strrchr(host, ':');
        if (c) { *c = 0; port = atoi(c + 1); }
    }
    /* fingerprint = text2 里 "keytype SHA256:xx" 的第 2 段 */
    fp = g_strdup(keytype_fp ? keytype_fp : "");
    {
        char *sp = strchr(fp, ' ');
        if (sp) memmove(fp, sp + 1, strlen(sp));
    }

    /* 已存在 host+port 的 known 对象 → 更新；否则新建 */
    objs = hb_store_list(p->store, "known", &n);
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
    hb_vault_put(hb_store_vault(p->store), o);
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

/* ------------------------- OSINFO ------------------------- */

static void on_osinfo(HbSshPage *p, const char *id, const char *pretty)
{
    const char *norm;
    hb_obj *h = NULL;
    (void)pretty;
    if (!id || !*id) return;
    if (!strcmp(id, "ubuntu"))       norm = "ubuntu";
    else if (!strcmp(id, "debian"))  norm = "debian";
    else if (!strcmp(id, "fedora"))  norm = "fedora";
    else                             norm = "generic";
    /* 仅在 auto/未设置时保存 */
    if (p->host_id &&
        hb_vault_get(hb_store_vault(p->store), p->host_id, &h) == HB_OK && h) {
        const char *os = hb_obj_get_str(h, "os");
        if (!os || !*os || !strcmp(os, "auto")) {
            hb_obj_set_str(h, "os", norm);
            hb_vault_put(hb_store_vault(p->store), h);
        }
        hb_obj_free(h);
    }
    g_free(p->os_key);
    p->os_key = g_strdup(norm);
    tab_icon(p, os_avatar(norm));
    if (p->tt_icon)
        hb_icon_set_name(HB_ICON(p->tt_icon), os_avatar(norm));
}

/* ------------------------- 断线重连 ------------------------- */

static const int backoff_tab[] = { 1, 2, 4, 8, 16, 30 };
static void reconnect_now(HbSshPage *p);

static void stop_backoff(HbSshPage *p)
{
    if (p->backoff_src) {
        g_source_remove(p->backoff_src);
        p->backoff_src = 0;
    }
    gtk_widget_set_visible(p->strip, FALSE);
    gtk_widget_set_visible(p->buf_hint, FALSE);
}

static void strip_set(HbSshPage *p, const char *txt)
{
    gtk_label_set_label(GTK_LABEL(p->strip_label), txt);
}

static gboolean backoff_tick(gpointer ud)
{
    HbSshPage *p = ud;
    char *t;
    if (--p->countdown > 0) {
        t = g_strdup_printf("连接已断开 · %d 秒后重连（第 %d 次）",
                            p->countdown, p->backoff_k);
        strip_set(p, t);
        g_free(t);
        return G_SOURCE_CONTINUE;
    }
    p->backoff_src = 0;
    reconnect_now(p);
    return G_SOURCE_REMOVE;
}

static void start_backoff(HbSshPage *p)
{
    int idx = p->backoff_k;     /* 第 k 次（即将进行的是 k+1） */
    int d = idx < 6 ? backoff_tab[idx] : 30;
    char *t;
    p->backoff_k++;
    p->countdown = d;
    t = g_strdup_printf("连接已断开 · %d 秒后重连（第 %d 次）", d, p->backoff_k);
    strip_set(p, t);
    g_free(t);
    gtk_widget_set_visible(p->strip, TRUE);
    set_status(p, "● 连接已断开", "hb-term-warn");
    tab_dot(p, "err");
    if (p->backoff_src) g_source_remove(p->backoff_src);
    p->backoff_src = g_timeout_add_seconds(1, backoff_tick, p);
}

static void on_reconn_now(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    if (p->backoff_src) { g_source_remove(p->backoff_src); p->backoff_src = 0; }
    reconnect_now(p);
}
static void on_reconn_cancel(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    stop_backoff(p);
    p->backoff_k = 0;
    p->dead = TRUE;
    set_status(p, "● 会话已结束", "hb-term-ended");
    gtk_widget_set_visible(p->dead_lbl, TRUE);
    gtk_widget_set_visible(p->reconn_btn, TRUE);
    g_string_set_size(p->inbuf, 0);
}

/* 会话已死（断线/退出）时的用户输入 → 本地缓冲（≤4KiB），
 * 重连成功后 flush。dead-input 信号只在会话死时才发出，所以无条件缓冲。 */
static void on_dead_input(HbTerminal *t, const char *utf8, gpointer ud)
{
    HbSshPage *p = ud;
    (void)t;
    if (p->inbuf->len + strlen(utf8) > 4096) return;   /* 满则丢 */
    g_string_append(p->inbuf, utf8);
    gtk_widget_set_visible(p->buf_hint, TRUE);
}

/* ------------------------- 会话生命周期 ------------------------- */

/* 偷出旧 vt、喂入重连提示行、起新会话（reuse_vt），重绑终端。 */
static void reconnect_now(HbSshPage *p)
{
    static const char *mark = "\r\n\e[2m[重新连接…]\e[0m\r\n";

    if (p->backoff_src) {
        g_source_remove(p->backoff_src);
        p->backoff_src = 0;
    }
    if (p->sess && !hb_session_alive(p->sess)) {
        p->reuse_vt = hb_session_steal_vt(p->sess);
        if (p->reuse_vt)
            hb_vt_feed(p->reuse_vt, mark, strlen(mark));
    }
    if (p->sess) {
        hb_terminal_set_session(HB_TERMINAL(p->term), NULL);
        hb_session_close(p->sess);
        hb_session_free(p->sess);
        p->sess = NULL;
    }
    strip_set(p, "正在重连…");
    gtk_widget_set_visible(p->strip, TRUE);
    start_connect(p, TRUE);
}

/* 干净 EXIT / 断线 EXIT 都会走这里 */
static void on_exited(HbTerminal *t, int code, gpointer ud)
{
    HbSshPage *p = ud;
    (void)t; (void)code;
    /* EXIT 在 ERROR(DISCONNECTED) 之后到达；若断线退避已启动则忽略 */
    if (p->backoff_k > 0 || p->backoff_src)
        return;
    if (!p->was_connected) {
        wipe_free(p->pend_secret);
        p->pend_secret = NULL;
        g_clear_pointer(&p->pend_key_id, g_free);
    }
    p->dead = TRUE;
    set_status(p, "● 会话已结束", "hb-term-ended");
    tab_dot(p, NULL);
    gtk_widget_set_visible(p->dead_lbl, TRUE);
    gtk_widget_set_visible(p->reconn_btn, TRUE);
    log_finish(p, p->was_connected ? "ok" : "cancelled", NULL);
    p->log_id = 0;   /* 归零避免二次 finish */
}

static void on_title(HbTerminal *t, const char *title, gpointer ud)
{
    HbSshPage *p = ud;
    char *f;
    (void)t;
    if (title && *title)
        f = g_strdup_printf("ssh %s@%s · %s", p->huser, p->hname, title);
    else
        f = g_strdup_printf("ssh %s@%s", p->huser, p->hname);
    gtk_label_set_label(GTK_LABEL(p->tt_label), f);
    g_free(f);
}

static void on_notify(HbTerminal *t, const char *title, const char *body,
                      gpointer ud)
{
    HbSshPage *p = ud;
    (void)t;
    /* 监控开着时 OSC 9/777 由 watch 的 TERMNOTIFY hit 路由，
     * 不在这里重复投递 */
    if (p->watch_on)
        return;
    if (title) {
        /* notify_body=host：正文只带主机名，不回显 OSC 内容 */
        hb_obj *o = NULL;
        gboolean host_only = FALSE;
        if (hb_vault_settings(hb_store_vault(p->store), &o) == HB_OK && o) {
            host_only = !g_strcmp0(hb_obj_get_str(o, "notify_body"),
                                   "host");
            hb_obj_free(o);
        }
        if (host_only) {
            char *tt = g_strdup_printf("终端通知 · %s", p->hname);
            char *bb = g_strdup_printf("%s 上的任务有新通知", p->hname);
            hb_notify_post("info", tt, bb, NULL);
            g_free(tt); g_free(bb);
        } else {
            hb_notify_post("info", title, body ? body : "", NULL);
        }
    }
}

/* ------------------------------ 监控 ------------------------------ */

static void watch_refresh_ui(HbSshPage *p)
{
    char *t;
    GtkWidget *bx;

    if (!p->watch_btn)
        return;
    bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(bx), hb_icon_new("eye", 13));
    gtk_box_append(GTK_BOX(bx),
        gtk_label_new(p->watch_on ? "监控 开" : "监控 关"));
    gtk_button_set_child(GTK_BUTTON(p->watch_btn), bx);
    if (p->watch_on) {
        gtk_widget_add_css_class(p->watch_btn, "on");
        t = hb_watch_signal_summary(p->watch_signals);
    } else {
        gtk_widget_remove_css_class(p->watch_btn, "on");
        t = NULL;
    }
    if (p->st_watch)
        gtk_label_set_label(GTK_LABEL(p->st_watch),
                            t ? t : "监控: 关");
    g_free(t);
}

static void watch_apply_session(HbSshPage *p)
{
    if (p->sess)
        hb_watch_apply(p->sess, p->store, p->host_id,
                       p->watch_on, p->watch_signals);
}

static void watch_set(gpointer ud, gboolean on, unsigned signals)
{
    HbSshPage *p = ud;
    p->watch_on = on;
    p->watch_signals = signals;
    watch_refresh_ui(p);
    watch_apply_session(p);
}

static hb_session *watch_get_sess(gpointer ud)
{
    HbSshPage *p = ud;
    return p->sess;
}

static void on_find_btn(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    hb_ssh_page_find_open(p);
}

static void on_watch_btn(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    HbWatchUiOps ops = {0};
    (void)b;
    ops.page = GTK_WIDGET(p);
    ops.store = p->store;
    ops.host_id = p->host_id;
    ops.is_ssh = TRUE;
    ops.on = p->watch_on;
    ops.signals = p->watch_signals;
    ops.apply = watch_set;
    ops.apply_ud = p;
    ops.session = watch_get_sess;
    ops.session_ud = p;
    hb_watch_ui_open(&ops);
}

/* ------------------------- 事件分发（ev_cb） ------------------------- */

static void on_session_ev(HbTerminal *t, const hb_sess_event *ev, gpointer ud);

/* dev 环境变量（仅 debug 构建生效；release 里编译掉） */
#ifndef NDEBUG
static gboolean dev_accept_hk(void)
{ return g_getenv("HARBOR_DEV_ACCEPT_HOSTKEY") != NULL; }
static gboolean dev_cancel_prompt(void)
{ return g_getenv("HARBOR_DEV_CANCEL_PROMPT") != NULL; }
#endif

/* open_dialog 跟踪：weak-pointer 让对话框被销毁（未发 closed 也
 * 会被 finalize）时指针自动置 NULL，不留悬垂引用 —— start_connect
 * / free_session 里的 force_close 才永远不会打到死对象。 */
static void set_open_dialog(HbSshPage *p, AdwDialog *d)
{
    if (p->open_dialog)
        g_object_remove_weak_pointer(G_OBJECT(p->open_dialog),
                                     (gpointer)&p->open_dialog);
    p->open_dialog = d;
    if (d)
        g_object_add_weak_pointer(G_OBJECT(d),
                                  (gpointer)&p->open_dialog);
}

/* TOFU / hostkey-changed 应答。
 * 对话框弹出期间对页面 g_object_ref —— tab 中途关闭时回调仍拿到
 * 有效对象（dispose 已应答并置 sess=NULL）。 */
static void on_tofu_done(gboolean accept, gpointer ud)
{
    HbSshPage *p = ud;
    set_open_dialog(p, NULL);
    p->awaiting_reply = FALSE;
    if (accept && p->pending_line)
        save_known(p, p->pending_host, p->pending_keyfp,
                   p->pending_line, FALSE);
    hb_session_hostkey_reply(p->sess, accept);
    g_clear_pointer(&p->pending_line, g_free);
    g_clear_pointer(&p->pending_host, g_free);
    g_clear_pointer(&p->pending_keyfp, g_free);
    g_object_unref(p);
}

static void on_hk_changed_done(gboolean accept, gpointer ud)
{
    HbSshPage *p = ud;
    set_open_dialog(p, NULL);
    p->awaiting_reply = FALSE;
    if (accept && p->pending_line)
        save_known(p, p->pending_host, p->pending_keyfp,
                   p->pending_line, TRUE);
    hb_session_hostkey_reply(p->sess, accept);
    g_clear_pointer(&p->pending_line, g_free);
    g_clear_pointer(&p->pending_host, g_free);
    g_clear_pointer(&p->pending_keyfp, g_free);
    g_object_unref(p);
}

static void on_pw_done(const char *secret, gboolean remember, gpointer ud)
{
    HbSshPage *p = ud;
    set_open_dialog(p, NULL);
    p->awaiting_reply = FALSE;
    if (secret && remember && p->pend_key_id) {
        /* 暂存到 CONNECTED 后写 vault */
        wipe_free(p->pend_secret);
        p->pend_secret = g_strdup(secret);
    }
    /* 不管记不记住，都暂存该 passphrase：连上后给公钥未知的
     * 加密导入 key 回填元数据（FR-02 首次使用补 public/fingerprint） */
    if (secret && p->pend_is_pass && p->prompt_key_id) {
        g_free(p->fill_key_id);
        p->fill_key_id = g_strdup(p->prompt_key_id);
        wipe_free(p->fill_pass);
        p->fill_pass = g_strdup(secret);
    }
    hb_session_prompt_reply(p->sess, secret);
    g_object_unref(p);
}

static void save_pending_secret(HbSshPage *p)
{
    if (!p->pend_secret || !p->pend_key_id) return;
    hb_vault_set_secret(hb_store_vault(p->store), p->pend_key_id,
                        p->pend_is_pass ? "passphrase" : "password",
                        p->pend_secret);
    /* 抹掉明文副本 */
    wipe_free(p->pend_secret);
    p->pend_secret = NULL;
    g_clear_pointer(&p->pend_key_id, g_free);
}

/* 加密导入的 key（PEM/PKCS8）第一次成功连接后：public/fingerprint 还
 * 是空的 —— 用刚输入（或已记住）的 passphrase 重新 inspect 补齐。 */
static void fill_key_metadata(HbSshPage *p)
{
    hb_vault *v = hb_store_vault(p->store);
    size_t i;
    if (!p->conn_key_ids) return;
    for (i = 0; p->conn_key_ids[i]; i++) {
        const char *kid = p->conn_key_ids[i];
        hb_obj *k = NULL;
        const char *pub, *pass = NULL;
        char *pass_sec = NULL, *priv = NULL;
        hb_key_info info;
        if (!kid || !*kid) continue;
        if (hb_vault_get(v, kid, &k) != HB_OK || !k) continue;
        pub = hb_obj_get_str(k, "public");
        if (pub && *pub) { hb_obj_free(k); continue; }   /* 已有 → 跳过 */
        if (p->fill_key_id && !strcmp(p->fill_key_id, kid) &&
            p->fill_pass)
            pass = p->fill_pass;          /* 借用，无拷贝 */
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

/* CONNECTED：切终端 + 记日志 + 存凭据 + last_connected + startup snippet */
static void on_connected(HbSshPage *p)
{
    hb_obj *h = NULL;
    char *t;

    p->was_connected = TRUE;
    p->connecting = FALSE;
    p->backoff_k = 0;
    stop_backoff(p);
    p->dead = FALSE;

    /* "ok"·进行中：duration=-1；EXIT/关 tab 时再写真实时长 */
    if (p->log_id > 0)
        hb_vault_log_finish(hb_store_vault(p->store), p->log_id,
                            "ok", NULL, -1);

    save_pending_secret(p);
    fill_key_metadata(p);
    wipe_free(p->fill_pass);
    p->fill_pass = NULL;
    g_clear_pointer(&p->fill_key_id, g_free);
    g_clear_pointer(&p->prompt_key_id, g_free);
    if (p->conn_key_ids) {
        size_t i;
        for (i = 0; p->conn_key_ids[i]; i++) free(p->conn_key_ids[i]);
        g_clear_pointer(&p->conn_key_ids, free);
    }

    /* last_connected */
    if (p->host_id &&
        hb_vault_get(hb_store_vault(p->store), p->host_id, &h) == HB_OK && h) {
        hb_obj_set_int(h, "last_connected", g_get_real_time() / 1000);
        /* startup_snippet */
        {
            const char *snid = hb_obj_get_str(h, "startup_snippet");
            if (snid && *snid) {
                hb_obj *sn = NULL;
                if (hb_vault_get(hb_store_vault(p->store), snid, &sn) == HB_OK
                    && sn) {
                    const char *body = hb_obj_get_str(sn, "body");
                    if (body && *body) {
                        char *line = g_strconcat(body, "\n", NULL);
                        hb_session_write(p->sess, line, strlen(line));
                        g_free(line);
                    }
                    hb_obj_free(sn);
                }
            }
        }
        hb_vault_put(hb_store_vault(p->store), h);
        hb_obj_free(h);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "term");
    set_status(p, "● 已连接", "hb-term-ok");
    tab_dot(p, "ok");
    gtk_widget_set_visible(p->reconn_btn, FALSE);
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_widget_set_visible(p->buf_hint, FALSE);

    /* 工具栏标题 */
    t = g_strdup_printf("ssh %s@%s", p->huser, p->hname);
    gtk_label_set_label(GTK_LABEL(p->tt_label), t);
    g_free(t);
    tab_title(p, p->hname);
    tab_icon(p, os_avatar(p->os_key));

    /* 重连成功 → 注入提示行 + flush 缓冲输入 */
    if (p->reconnected_feed) {
        hb_vt *v = hb_session_vt(p->sess);
        static const char *l1 = "\e[2m[重新连接成功]\e[0m\r\n";
        static const char *l2 =
            "\e[33m提示: 远端会话已丢失，建议在 tmux 内工作以便 attach 恢复\e[0m\r\n";
        if (v) {
            hb_vt_lock(v);
            hb_vt_feed(v, l1, strlen(l1));
            hb_vt_feed(v, l2, strlen(l2));
            hb_vt_unlock(v);
        }
        p->reconnected_feed = FALSE;
    }
    if (p->inbuf->len) {
        hb_session_write(p->sess, p->inbuf->str, p->inbuf->len);
        g_string_set_size(p->inbuf, 0);
    }
    gtk_widget_grab_focus(p->term);
}

static void on_errcard_close(GtkButton *b, gpointer ud)
{ (void)b; tab_close((HbSshPage *)ud); }
static void on_errcard_retry(GtkButton *b, gpointer ud)
{ (void)b; start_connect((HbSshPage *)ud, FALSE); }
static void on_errcard_edit(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    g_signal_emit(p, sigs[SIG_EDIT_HOST], 0, p->host_id);
}

/* 连接期错误 → 错误卡片（插在日志区下方） */
static void show_error_card(HbSshPage *p, const char *msg, const char *detail,
                            int code)
{
    GtkWidget *card, *title, *exp, *foot, *dl;
    char *markup;

    if (p->err_card) {
        gtk_widget_unparent(p->err_card);
        p->err_card = NULL;
    }
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card, "connect-errcard");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 430, -1);

    markup = g_markup_printf_escaped("<b>%s</b>",
                                     msg ? msg : "连接失败");
    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(card), title);

    if (code == HB_SSH_ERR_HOSTKEY_CHANGED) {
        GtkWidget *w = gtk_label_new(
            "为安全起见已阻断连接 —— 主机密钥与本地记录不符。");
        gtk_widget_add_css_class(w, "hb-red");
        gtk_label_set_wrap(GTK_LABEL(w), TRUE);
        gtk_label_set_xalign(GTK_LABEL(w), 0);
        gtk_box_append(GTK_BOX(card), w);
    }
    if (detail && *detail) {
        exp = gtk_expander_new("技术详情");
        dl = gtk_label_new(detail);
        gtk_label_set_wrap(GTK_LABEL(dl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(dl), 0);
        gtk_expander_set_child(GTK_EXPANDER(exp), dl);
        gtk_widget_add_css_class(exp, "hb-dim");
        gtk_box_append(GTK_BOX(card), exp);
    }

    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    {
        GtkWidget *c = gtk_button_new_with_label("Close");
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect(c, "clicked", G_CALLBACK(on_errcard_close), p);
        gtk_box_append(GTK_BOX(foot), c);
    }
    if (p->host_id) {
        GtkWidget *e = gtk_button_new_with_label("编辑主机");
        gtk_widget_add_css_class(e, "hb-btn");
        g_signal_connect(e, "clicked", G_CALLBACK(on_errcard_edit), p);
        gtk_box_append(GTK_BOX(foot), e);
    }
    {
        GtkWidget *r = gtk_button_new_with_label("重试");
        gtk_widget_add_css_class(r, "hb-btn");
        gtk_widget_add_css_class(r, "primary");
        g_signal_connect(r, "clicked", G_CALLBACK(on_errcard_retry), p);
        gtk_box_append(GTK_BOX(foot), r);
    }
    gtk_box_append(GTK_BOX(card), foot);

    /* 插到日志区后（connect 视图内 footer 之前） */
    gtk_box_insert_child_after(GTK_BOX(p->connect_box),
                               card, p->log_scroll);
    p->err_card = card;
    gtk_widget_set_visible(p->hide_btn, FALSE);
    if (p->conn_foot)
        gtk_widget_set_visible(p->conn_foot, FALSE);
}

static void on_session_ev(HbTerminal *t, const hb_sess_event *ev, gpointer ud)
{
    HbSshPage *p = ud;
    (void)t;
    switch (ev->type) {
    case HB_SESS_EV_STAGE:
        add_log_line(p, ev->text ? ev->text : "", FALSE);
        if (ev->code > p->cur_stage) {
            p->cur_stage = ev->code;
            update_nodes(p);
        }
        break;

    case HB_SESS_EV_HOSTKEY: {
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
        p->awaiting_reply = TRUE;
        g_free(p->pending_host);  p->pending_host  = g_strdup(hp);
        g_free(p->pending_keyfp); p->pending_keyfp = g_strdup(ev->text2);
        g_free(p->pending_line);  p->pending_line  = g_strdup(line);
#ifndef NDEBUG
        if (dev_accept_hk()) {
            if (line) save_known(p, hp, ev->text2, line, ev->code == 1);
            p->awaiting_reply = FALSE;
            hb_session_hostkey_reply(p->sess, TRUE);
            g_free(art); g_free(kt);
            break;
        }
#endif
        g_object_ref(p);   /* 对话框存活期间保活页面（done cb 里 unref） */
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
                    const char *c = strrchr(s, ':');
                    if (c) {
                        g_snprintf(host, sizeof host, "%.*s",
                                   (int)(c - s), s);
                        port = atoi(c + 1);
                    } else g_strlcpy(host, s, sizeof host);
                }
                {
                    hb_obj **ob; size_t n, i;
                    g_free(oldfp);
                    oldfp = g_strdup("(无本地记录)");
                    ob = hb_store_list(p->store, "known", &n);
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
            set_open_dialog(p,
                hb_dialog_hostkey_changed(GTK_WINDOW(page_win(p)), hp,
                    kt ? kt : "", oldfp,
                    ev->text2 ? (strchr(ev->text2,' ') ?
                                 strchr(ev->text2,' ')+1 : ev->text2) : "",
                    on_hk_changed_done, p));
            g_free(oldfp);
        } else {
            set_open_dialog(p,
                hb_dialog_tofu(GTK_WINDOW(page_win(p)), hp,
                    kt ? kt : "",
                    ev->text2 ? (strchr(ev->text2,' ') ?
                                 strchr(ev->text2,' ')+1 : ev->text2) : "",
                    art ? art : "", on_tofu_done, p));
        }
        g_free(art); g_free(kt);
        break;
    }

    case HB_SESS_EV_PROMPT: {
        gboolean is_pass = (ev->code == 1);
        const char *hop_host = p->host_id;
        p->awaiting_reply = TRUE;
        g_free(p->pend_key_id);
        p->pend_key_id = NULL;
        g_free(p->prompt_key_id);
        p->prompt_key_id = NULL;
        p->pend_is_pass = is_pass;
        /* 归属到具体 hop：text2 是 hop 下标（jumps 0..n-1，njumps=target）；
         * ProxyJump 时提示可能来自某跳板，不能总记到目标主机。 */
        if (ev->text2 && p->hop_ids) {
            long idx = strtol(ev->text2, NULL, 10);
            size_t n = 0;
            while (p->hop_ids[n]) n++;
            if (idx >= 0 && (size_t)idx < n && p->hop_ids[idx])
                hop_host = p->hop_ids[idx];
        }
        /* 记住到哪个对象：passphrase → 该 hop 的 key_id
         *（identity_file 则该 hop 主机自身）；密码 → 该 hop 主机 */
        {
            hb_obj *h = NULL;
            if (hop_host && hb_vault_get(
                    hb_store_vault(p->store), hop_host, &h) == HB_OK && h) {
                const char *kid = hb_obj_get_str(h, "key_id");
                if (is_pass && kid && *kid) {
                    p->pend_key_id = g_strdup(kid);
                    p->prompt_key_id = g_strdup(kid);
                } else {
                    p->pend_key_id = g_strdup(hop_host);
                }
                hb_obj_free(h);
            } else p->pend_key_id = g_strdup(hop_host);
        }
#ifndef NDEBUG
        if (dev_cancel_prompt()) {
            p->awaiting_reply = FALSE;
            hb_session_prompt_reply(p->sess, NULL);
            break;
        }
#endif
        g_object_ref(p);   /* 同 HOSTKEY：done cb 里 unref */
        set_open_dialog(p,
            hb_dialog_password(GTK_WINDOW(page_win(p)),
                ev->text ? ev->text : "Password:", is_pass,
                TRUE /* show remember */, on_pw_done, p));
        break;
    }

    case HB_SESS_EV_CONNECTED:
        on_connected(p);
        break;

    case HB_SESS_EV_OSINFO:
        on_osinfo(p, ev->text, ev->text2);
        break;

    case HB_SESS_EV_ERROR:
        add_log_line(p, ev->text ? ev->text : "连接失败", TRUE);
        /* 连接没成功 → 记住的凭据不再有用，立刻抹掉明文 */
        wipe_free(p->pend_secret);
        p->pend_secret = NULL;
        g_clear_pointer(&p->pend_key_id, g_free);
        if (p->was_connected) {
            /* 已连接后掉线 / 重连尝试失败：
             * 凭据/密钥类错误 → 停重试（需用户介入），错误卡；
             * 其余 → 继续指数退避。 */
            switch (ev->code) {
            case HB_SSH_ERR_CANCELLED:
            case HB_SSH_ERR_AUTH:
            case HB_SSH_ERR_HOSTKEY_CHANGED:
            case HB_SSH_ERR_HOSTKEY_REJECTED:
                p->connecting = FALSE;
                stop_backoff(p);
                p->backoff_k = 0;
                log_finish(p, hb_ssh_err_name(ev->code),
                           ev->text ? ev->text : "error");
                p->log_id = 0;
                gtk_stack_set_visible_child_name(GTK_STACK(p->stack),
                                                 "connect");
                show_error_card(p, ev->text, ev->text2, ev->code);
                break;
            default:
                start_backoff(p);
                break;
            }
        } else {
            /* 首次连接期失败 → 错误卡 */
            p->connecting = FALSE;
            log_finish(p, hb_ssh_err_name(ev->code),
                       ev->text ? ev->text : "error");
            p->log_id = 0;
            gtk_stack_set_visible_child_name(GTK_STACK(p->stack),
                                             "connect");
            show_error_card(p, ev->text, ev->text2, ev->code);
        }
        break;

    case HB_SESS_EV_WATCH:
        if (ev->watch && p->tab_id)
            hb_watch_route(GTK_WIDGET(p), p->tab_id, p->hname,
                           ev->watch);
        break;

    default:
        break;
    }
}

/* ------------------------- start_connect ------------------------- */

static void free_session(HbSshPage *p)
{
    /* 先强关挂起的模态 —— 它的 done cb 会同步跑（accept=FALSE），
     * 把应答发给当前这个将死的 sess；不关的话对话框会活到新会话，
     * 之后的点击会应答错会话。 */
    if (p->open_dialog) {
        AdwDialog *d = p->open_dialog;
        set_open_dialog(p, NULL);
        adw_dialog_force_close(d);
    }
    if (!p->sess) return;
    if (p->awaiting_reply) {
        /* 让阻塞中的应答先返回，避免线程挂住导致 free 卡住 */
        hb_session_prompt_reply(p->sess, NULL);
        hb_session_hostkey_reply(p->sess, FALSE);
        p->awaiting_reply = FALSE;
    }
    hb_terminal_set_session(HB_TERMINAL(p->term), NULL);
    hb_session_close(p->sess);
    hb_session_free(p->sess);
    p->sess = NULL;
}

/* 使用前确认（FR-02 验收 2 的 Linux 形态）：收集目标 + 各跳板中
 * auth=key 且 key.confirm=true 的密钥；一个都不读 secret。 */
static GPtrArray *collect_confirm_keys(HbSshPage *p)
{
    hb_vault *v = hb_store_vault(p->store);
    hb_obj *h = NULL;
    const char **jump = NULL;
    size_t nj = 0, i;
    GPtrArray *out = NULL;

    if (!p->host_id ||
        hb_vault_get(v, p->host_id, &h) != HB_OK || !h)
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
    HbSshPage *p;
    GPtrArray *lines;   /* 待确认密钥描述行 */
} KeyConfirmCtx;

static void continue_connect(HbSshPage *p);

static void on_key_confirm(gboolean ok, gpointer ud)
{
    KeyConfirmCtx *cc = ud;
    HbSshPage *p = cc->p;

    set_open_dialog(p, NULL);
    g_ptr_array_free(cc->lines, TRUE);
    g_free(cc);
    if (!gtk_widget_get_root(GTK_WIDGET(p))) {   /* tab 已关 */
        g_object_unref(p);
        return;
    }
    if (ok) {
        continue_connect(p);
    } else {
        /* 拒绝 → 不建会话（secret 从未被读）。
         * reuse_vt 不动：它属于下一次成功连接，不由本次拒绝销毁。 */
        p->connecting = FALSE;
        gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");
        show_error_card(p, "已拒绝使用密钥，连接已取消", NULL, 0);
        log_finish(p, "cancelled", "key confirm denied");
        p->log_id = 0;
    }
    g_object_unref(p);
}

/* 真正起会话（确认通过或无需确认后进入）：偷 reuse_vt、杀旧会话、
 * build_ctx、new_ssh。 */
static void continue_connect(HbSshPage *p)
{
    ConnCtx *c;
    hb_vt *reuse;
    hb_obj *thost = NULL;

    /* 重连：reconnect_now 已把旧会话的 vt 偷出来挂在 p->reuse_vt；
     * 普通 start_over 则没有。 */
    reuse = p->reuse_vt;
    p->reuse_vt = NULL;
    if (p->sess) {
        hb_terminal_set_session(HB_TERMINAL(p->term), NULL);
        hb_session_close(p->sess);
        hb_session_free(p->sess);
        p->sess = NULL;
    }

    /* 上一次的 hop 归属表作废 */
    if (p->hop_ids) {
        size_t i;
        for (i = 0; p->hop_ids[i]; i++) free(p->hop_ids[i]);
        g_clear_pointer(&p->hop_ids, free);
    }
    if (p->conn_key_ids) {
        size_t i;
        for (i = 0; p->conn_key_ids[i]; i++) free(p->conn_key_ids[i]);
        g_clear_pointer(&p->conn_key_ids, free);
    }

    c = build_ctx(p, &thost);
    /* hop 归属表 + 本次连接用到的 key：转给页面（ctx 释放前搬出） */
    p->hop_ids = c ? c->hop_ids : NULL;
    if (c) c->hop_ids = NULL;
    {
        size_t i, nk = 0;
        if (c && c->key_ids) {
            for (i = 0; i <= c->nhops; i++)
                if (c->key_ids[i]) nk++;
            p->conn_key_ids = calloc(nk + 1, sizeof(char *));
            nk = 0;
            for (i = 0; i <= c->nhops; i++)
                if (c->key_ids[i]) {
                    p->conn_key_ids[nk++] = c->key_ids[i];
                    c->key_ids[i] = NULL;
                }
        }
    }
    if (c && c->missing_jump) {
        char *m = g_strdup_printf("跳板主机不存在或已删除（%s）",
                                  c->missing_jump);
        p->connecting = FALSE;
        gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");
        show_error_card(p, m, NULL, 0);
        g_free(m);
        log_finish(p, "error", "jump host missing");
        p->log_id = 0;
        if (reuse) hb_vt_free(reuse);
        conn_ctx_free(c);
        if (thost) hb_obj_free(thost);
        return;
    }
    if (!c || !c->o.target.host || !*c->o.target.host) {
        p->connecting = FALSE;
        log_finish(p, "error", "host object missing");
        p->log_id = 0;
        show_error_card(p, "无法读取主机信息", NULL, 0);
        if (reuse) hb_vt_free(reuse);
        conn_ctx_free(c);
        if (thost) hb_obj_free(thost);
        return;
    }
    c->o.reuse_vt = reuse;
    /* new_ssh 深拷贝全部 opts —— target/jump host 对象与 secret 至此可释 */
    p->sess = hb_session_new_ssh(&c->o, 24, 80, p->sb_lines);
    conn_ctx_free(c);
    if (thost) hb_obj_free(thost);
    if (!p->sess) {
        p->connecting = FALSE;
        log_finish(p, "error", "session alloc failed");
        p->log_id = 0;
        show_error_card(p, "无法创建 SSH 会话",
                        "主机配置缺少必填字段（如用户名）", 0);
        if (reuse) hb_vt_free(reuse);
        return;
    }
    hb_terminal_set_session(HB_TERMINAL(p->term), p->sess);
    watch_apply_session(p);       /* 监控状态随（重）连重挂 */
    set_status(p, "● 连接中…", "hb-term-warn");
}

static void start_connect(HbSshPage *p, gboolean is_reconnect)
{
    /* 先强关上一次尝试挂起的模态（密钥确认/TOFU/口令）：其 cb 会按
     * "拒绝/取消"路径收尾旧尝试，保证任何时刻最多一个挂起对话框。 */
    if (p->open_dialog) {
        AdwDialog *d = p->open_dialog;
        set_open_dialog(p, NULL);
        adw_dialog_force_close(d);
    }
    if (p->err_card) {        /* 上次尝试的错误卡（含重连路径） */
        gtk_widget_unparent(p->err_card);
        p->err_card = NULL;
    }
    p->connecting = TRUE;
    p->was_reconnect = is_reconnect;
    p->reconnected_feed = is_reconnect;
    if (!is_reconnect) {
        p->was_connected = FALSE;
        p->cur_stage = -1;
        update_nodes(p);
        gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");
        /* 清日志 */
        {
            GtkWidget *ch;
            while ((ch = gtk_widget_get_first_child(p->log_box)))
                gtk_widget_unparent(ch);
        }
        gtk_widget_set_visible(p->hide_btn, TRUE);
        if (p->conn_foot)
            gtk_widget_set_visible(p->conn_foot, TRUE);
    }
    tab_dot(p, NULL);
    log_start(p);

    /* 使用确认：在读任何 secret 之前（build_ctx 会 get_secret） */
    {
        GPtrArray *cf = collect_confirm_keys(p);
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
            cc->p = p;
            cc->lines = cf;
            g_object_ref(p);   /* 对话框期间保活页面 */
            /* 挂 open_dialog：start_connect/free_session 会强关它
             *（cb 按拒绝路径收尾旧尝试），绝不叠加第二个确认框。 */
            set_open_dialog(p,
                hb_dialog_confirm(GTK_WIDGET(p), "使用密钥",
                                  mk->str, "允许使用", FALSE,
                                  on_key_confirm, cc));
            g_string_free(mk, TRUE);
            return;
        }
        if (cf) g_ptr_array_free(cf, TRUE);
    }
    continue_connect(p);
}

/* ------------------------- 设置/主题 ------------------------- */

static void apply_settings(HbSshPage *p)
{
    hb_obj *o = NULL;
    const char *theme_id = "harbor";
    const char *family = "Monospace";
    double px = 13, lh = 1.5;
    gboolean blink = TRUE, osc52 = FALSE;

    if (hb_vault_settings(hb_store_vault(p->store), &o) == HB_OK && o) {
        theme_id = hb_obj_get_str(o, "theme");
        family = hb_obj_get_str(o, "font_family");
        px = (double)hb_obj_get_int(o, "font_size", 13);
        lh = hb_obj_get_double(o, "line_height", 1.5);
        blink = hb_obj_get_bool(o, "cursor_blink", true);
        osc52 = hb_obj_get_bool(o, "osc52", false);
        p->sb_lines = (int)hb_obj_get_int(o, "scrollback", 10000);
    }
    if (!theme_id || !*theme_id) theme_id = "harbor";
    if (!family || !*family) family = "Monospace";
    if (hb_opt_theme) theme_id = hb_opt_theme;

    /* 主题类加在 term_view 上（.hb-term-view.theme-* 选择器），
     * 连接进度页保持浅色不受主题影响 */
    if (p->theme_id)
        gtk_widget_remove_css_class(p->term_view, p->theme_id);
    g_free(p->theme_id);
    p->theme_id = g_strdup_printf("theme-%s", theme_id);
    gtk_widget_add_css_class(p->term_view, p->theme_id);

    hb_terminal_set_theme(HB_TERMINAL(p->term), hb_term_theme_get(theme_id));
    hb_terminal_set_font(HB_TERMINAL(p->term), family, px, lh);
    hb_terminal_set_cursor_blink(HB_TERMINAL(p->term), blink);
    hb_terminal_set_osc52(HB_TERMINAL(p->term), osc52);

    {
        char raw[32], num[40], *txt; int i = 0, j = 0, n;
        g_snprintf(raw, sizeof raw, "%d", p->sb_lines);
        n = (int)strlen(raw);
        while (i < n) {
            if (i > 0 && (n - i) % 3 == 0) num[j++] = ',';
            num[j++] = raw[i++];
        }
        num[j] = 0;
        txt = g_strdup_printf("scrollback %s 行", num);
        gtk_label_set_label(GTK_LABEL(p->st_sb), txt);
        g_free(txt);
    }
    if (o) hb_obj_free(o);
}

static void on_store_changed(HbStore *st, const char *type, const char *id,
                             gpointer ud)
{
    HbSshPage *p = ud;
    (void)st; (void)id;
    if (!strcmp(type, "settings")) apply_settings(p);
    else if (!strcmp(type, "known") && !p->connecting) {
        /* known_hosts 被外部改了（如同步）—— 下次连接自动生效，无需动作 */
    }
}

/* ------------------------- 视图构建 ------------------------- */

static void on_hide_log(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    p->hide_log = !p->hide_log;
    gtk_button_set_label(b, p->hide_log ? "Show logs" : "Hide logs");
    gtk_widget_set_visible(p->log_scroll, !p->hide_log);
}

static void on_start_over(GtkButton *b, gpointer ud)
{ (void)b; start_connect((HbSshPage *)ud, FALSE); }
static void on_close_tab(GtkButton *b, gpointer ud)
{ (void)b; tab_close((HbSshPage *)ud); }
static void on_reconnect_btn(GtkButton *b, gpointer ud)
{
    HbSshPage *p = ud;
    (void)b;
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_widget_set_visible(p->reconn_btn, FALSE);
    p->dead = FALSE;
    /* 终端视图内原地重连（不跳回连接页） */
    p->was_connected = TRUE;
    reconnect_now(p);
}

static GtkWidget *build_connect_view(HbSshPage *p)
{
    GtkWidget *wrap, *head, *av, *txtcol, *nm, *sub, *track, *foot;
    int i;
    static const char *node_icons[] = { "link", "shield", "terminal" };
    char *t;

    wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrap, "connect-wrap");
    gtk_widget_set_vexpand(wrap, TRUE);

    /* head：avatar + name + "SSH host:port" + hide-logs */
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(head, "connect-head");
    av = hb_os_avatar_new(p->os_key ? p->os_key : "generic", 38);
    gtk_box_append(GTK_BOX(head), av);
    txtcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    nm = gtk_label_new(p->hname);
    gtk_widget_add_css_class(nm, "connect-name");
    gtk_label_set_xalign(GTK_LABEL(nm), 0);
    gtk_box_append(GTK_BOX(txtcol), nm);
    t = g_strdup_printf("SSH %s:%d", p->haddr, p->hport);
    sub = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(sub, "connect-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_append(GTK_BOX(txtcol), sub);
    gtk_box_append(GTK_BOX(head), txtcol);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(head), sp);
    }
    p->hide_btn = gtk_button_new_with_label("Hide logs");
    gtk_widget_add_css_class(p->hide_btn, "hb-btn");
    gtk_widget_add_css_class(p->hide_btn, "sm");
    gtk_widget_set_valign(p->hide_btn, GTK_ALIGN_CENTER);
    g_signal_connect(p->hide_btn, "clicked", G_CALLBACK(on_hide_log), p);
    gtk_box_append(GTK_BOX(head), p->hide_btn);
    gtk_box_append(GTK_BOX(wrap), head);

    /* progress track：3 节点 + 2 连线 */
    track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(track, "progress-track");
    gtk_widget_set_halign(track, GTK_ALIGN_CENTER);
    for (i = 0; i < 3; i++) {
        p->node[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(p->node[i], "node");
        gtk_widget_add_css_class(p->node[i], "pending");
        gtk_box_append(GTK_BOX(p->node[i]),
                       hb_icon_new(node_icons[i], 12));
        gtk_widget_set_size_request(p->node[i], 22, 22);
        gtk_box_append(GTK_BOX(track), p->node[i]);
        if (i < 2) {
            p->bar[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(p->bar[i], "bar");
            gtk_widget_add_css_class(p->bar[i], "pending");
            gtk_widget_set_size_request(p->bar[i], -1, 3);
            gtk_widget_set_hexpand(p->bar[i], TRUE);
            gtk_widget_set_valign(p->bar[i], GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(track), p->bar[i]);
        }
    }
    gtk_widget_set_size_request(track, 430, -1);
    gtk_box_append(GTK_BOX(wrap), track);

    /* 日志区 */
    p->log_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(p->log_scroll, "connect-log");
    gtk_widget_set_size_request(p->log_scroll, 430, 220);
    gtk_widget_set_halign(p->log_scroll, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->log_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p->log_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(p->log_box, 16);
    gtk_widget_set_margin_end(p->log_box, 16);
    gtk_widget_set_margin_top(p->log_box, 14);
    gtk_widget_set_margin_bottom(p->log_box, 14);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p->log_scroll),
                                  p->log_box);
    gtk_box_append(GTK_BOX(wrap), p->log_scroll);

    /* footer（错误卡显示时隐藏 —— 卡片自带 Close/重试） */
    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    p->conn_foot = foot;
    gtk_widget_set_halign(foot, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(foot, 18);
    {
        GtkWidget *c = gtk_button_new_with_label("Close");
        gtk_widget_add_css_class(c, "hb-btn");
        g_signal_connect(c, "clicked", G_CALLBACK(on_close_tab), p);
        gtk_box_append(GTK_BOX(foot), c);
    }
    {
        GtkWidget *r = gtk_button_new_with_label("Start over");
        gtk_widget_add_css_class(r, "hb-btn");
        gtk_widget_add_css_class(r, "primary");
        g_signal_connect(r, "clicked", G_CALLBACK(on_start_over), p);
        gtk_box_append(GTK_BOX(foot), r);
    }
    gtk_box_append(GTK_BOX(wrap), foot);
    return wrap;
}

static GtkWidget *build_term_view(HbSshPage *p)
{
    GtkWidget *box, *bar, *mono, *sb, *strip;
    char *t;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "hb-term-view");

    /* 工具栏 */
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "hb-term-toolbar");
    p->tt_icon = hb_icon_new(os_avatar(p->os_key), 15);
    gtk_widget_add_css_class(p->tt_icon, "hb-term-tt");
    gtk_box_append(GTK_BOX(bar), p->tt_icon);
    t = g_strdup_printf("ssh %s@%s", p->huser, p->hname);
    p->tt_label = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(p->tt_label, "hb-term-tt");
    gtk_label_set_xalign(GTK_LABEL(p->tt_label), 0);
    gtk_box_append(GTK_BOX(bar), p->tt_label);
    {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(bar), sp);
    }
    mono = hb_mono_icon_new(">_");
    gtk_widget_set_visible(mono, FALSE);
    gtk_box_append(GTK_BOX(bar), mono);
    {
        GtkWidget *fb = gtk_button_new();
        gtk_widget_add_css_class(fb, "tbtn");
        gtk_widget_set_tooltip_text(fb, "在 scrollback 中搜索 (Ctrl+Shift+F)");
        gtk_button_set_child(GTK_BUTTON(fb), hb_icon_new("search", 13));
        g_signal_connect(fb, "clicked",
                         G_CALLBACK(on_find_btn), p);
        gtk_box_append(GTK_BOX(bar), fb);
    }
    p->watch_btn = gtk_button_new();
    gtk_widget_add_css_class(p->watch_btn, "tbtn");
    g_signal_connect(p->watch_btn, "clicked",
                     G_CALLBACK(on_watch_btn), p);
    gtk_box_append(GTK_BOX(bar), p->watch_btn);
    p->reconn_btn = gtk_button_new();
    gtk_widget_add_css_class(p->reconn_btn, "tbtn");
    {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_box_append(GTK_BOX(r), hb_icon_new("refresh", 13));
        gtk_box_append(GTK_BOX(r), gtk_label_new("重连"));
        gtk_button_set_child(GTK_BUTTON(p->reconn_btn), r);
    }
    g_signal_connect(p->reconn_btn, "clicked",
                     G_CALLBACK(on_reconnect_btn), p);
    gtk_widget_set_visible(p->reconn_btn, FALSE);
    gtk_box_append(GTK_BOX(bar), p->reconn_btn);
    gtk_box_append(GTK_BOX(box), bar);

    /* scrollback 查找条（FR-20） */
    p->find_bar = hb_find_bar_new();
    gtk_box_append(GTK_BOX(box), p->find_bar);

    /* 断线重连条 */
    strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(strip, "hb-disc-strip");
    p->strip_label = gtk_label_new("连接已断开");
    gtk_label_set_xalign(GTK_LABEL(p->strip_label), 0);
    gtk_widget_set_hexpand(p->strip_label, TRUE);
    gtk_box_append(GTK_BOX(strip), p->strip_label);
    {
        GtkWidget *now = gtk_button_new_with_label("立即重连");
        gtk_widget_add_css_class(now, "hb-btn");
        gtk_widget_add_css_class(now, "sm");
        g_signal_connect(now, "clicked", G_CALLBACK(on_reconn_now), p);
        gtk_box_append(GTK_BOX(strip), now);
        GtkWidget *cancel = gtk_button_new_with_label("取消");
        gtk_widget_add_css_class(cancel, "hb-btn");
        gtk_widget_add_css_class(cancel, "sm");
        g_signal_connect(cancel, "clicked", G_CALLBACK(on_reconn_cancel), p);
        gtk_box_append(GTK_BOX(strip), cancel);
    }
    gtk_widget_set_visible(strip, FALSE);
    p->strip = strip;
    gtk_box_append(GTK_BOX(box), strip);

    /* overlay：terminal + scrollbar + dead 提示 */
    p->overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(p->overlay, TRUE);
    p->term = hb_terminal_new();
    gtk_widget_set_vexpand(p->term, TRUE);
    gtk_widget_set_hexpand(p->term, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(p->overlay), p->term);
    sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
        hb_terminal_scroll_adjustment(HB_TERMINAL(p->term)));
    gtk_widget_add_css_class(sb, "hb-term-sb");
    gtk_widget_set_halign(sb, GTK_ALIGN_END);
    gtk_widget_set_valign(sb, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), sb);
    p->dead_lbl = gtk_label_new("[连接已关闭]");
    gtk_widget_add_css_class(p->dead_lbl, "hb-term-dead");
    gtk_widget_set_halign(p->dead_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->dead_lbl, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(p->dead_lbl, 24);
    gtk_widget_set_visible(p->dead_lbl, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->dead_lbl);
    gtk_box_append(GTK_BOX(box), p->overlay);

    hb_find_bar_bind(HB_FIND_BAR(p->find_bar), HB_TERMINAL(p->term));

    /* 缓冲输入提示 */
    p->buf_hint = gtk_label_new("输入将在重连后发送");
    gtk_widget_add_css_class(p->buf_hint, "hb-disc-strip");
    gtk_label_set_xalign(GTK_LABEL(p->buf_hint), 0);
    gtk_widget_set_visible(p->buf_hint, FALSE);
    gtk_box_append(GTK_BOX(box), p->buf_hint);

    /* 状态栏 */
    {
        GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_add_css_class(st, "hb-term-status");
        p->st_conn = gtk_label_new("● 连接中…");
        gtk_widget_add_css_class(p->st_conn, "hb-term-warn");
        gtk_box_append(GTK_BOX(st), p->st_conn);
        GtkWidget *tt = gtk_label_new("xterm-256color");
        gtk_widget_add_css_class(tt, "hb-term-muted");
        gtk_box_append(GTK_BOX(st), tt);
        GtkWidget *enc = gtk_label_new("UTF-8");
        gtk_widget_add_css_class(enc, "hb-term-muted");
        gtk_box_append(GTK_BOX(st), enc);
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp, TRUE);
        gtk_box_append(GTK_BOX(st), sp);
        p->st_watch = gtk_label_new("监控: 关");
        gtk_widget_add_css_class(p->st_watch, "hb-term-muted");
        gtk_box_append(GTK_BOX(st), p->st_watch);
        p->st_sb = gtk_label_new("");
        gtk_box_append(GTK_BOX(st), p->st_sb);
        gtk_box_append(GTK_BOX(box), st);
    }
    return box;
}

/* ------------------------- 生命周期 ------------------------- */

static void hb_ssh_page_dispose(GObject *o)
{
    HbSshPage *p = HB_SSH_PAGE(o);
    if (p->backoff_src) {
        g_source_remove(p->backoff_src);
        p->backoff_src = 0;
    }
    free_session(p);
    if (p->log_id > 0) {
        /* 关 tab 时若还连着/连着过：给日志一个收尾 */
        hb_vault_log_finish(hb_store_vault(p->store), p->log_id,
                            p->was_connected ? "ok" : "cancelled",
                            NULL,
                            (g_get_monotonic_time() - p->t0) / G_USEC_PER_SEC);
        p->log_id = 0;
    }
    if (p->reuse_vt) {
        hb_vt_free(p->reuse_vt);
        p->reuse_vt = NULL;
    }
    if (p->changed_handler && p->store)
        g_signal_handler_disconnect(p->store, p->changed_handler);
    p->changed_handler = 0;
    G_OBJECT_CLASS(hb_ssh_page_parent_class)->dispose(o);
}

static void hb_ssh_page_finalize(GObject *o)
{
    HbSshPage *p = HB_SSH_PAGE(o);
    g_free(p->tab_id);
    g_free(p->host_id);
    g_free(p->hname);
    g_free(p->haddr);
    g_free(p->huser);
    g_free(p->os_key);
    g_free(p->theme_id);
    wipe_free(p->pend_secret);
    g_free(p->pend_key_id);
    if (p->hop_ids) {
        size_t i;
        for (i = 0; p->hop_ids[i]; i++) free(p->hop_ids[i]);
        free(p->hop_ids);
    }
    if (p->conn_key_ids) {
        size_t i;
        for (i = 0; p->conn_key_ids[i]; i++) free(p->conn_key_ids[i]);
        free(p->conn_key_ids);
    }
    g_free(p->prompt_key_id);
    g_free(p->fill_key_id);
    wipe_free(p->fill_pass);
    g_free(p->pending_line);
    g_free(p->pending_host);
    g_free(p->pending_keyfp);
    if (p->inbuf) g_string_free(p->inbuf, TRUE);
    G_OBJECT_CLASS(hb_ssh_page_parent_class)->finalize(o);
}

static void hb_ssh_page_class_init(HbSshPageClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_ssh_page_dispose;
    oc->finalize = hb_ssh_page_finalize;
    sigs[SIG_EDIT_HOST] = g_signal_new("edit-host", HB_TYPE_SSH_PAGE,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void hb_ssh_page_init(HbSshPage *p)
{
    p->sb_lines = 10000;
    p->inbuf = g_string_new(NULL);
    p->log_id = -1;
    p->cur_stage = -1;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(p),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(GTK_WIDGET(p), "hb-ssh-page");

    p->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(p->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(p->stack, TRUE);
    gtk_box_append(GTK_BOX(p), p->stack);
}

GtkWidget *hb_ssh_page_new(HbStore *st, const hb_obj *host)
{
    HbSshPage *p = g_object_new(HB_TYPE_SSH_PAGE, NULL);
    const char *os;

    p->store = st;
    p->host_id = g_strdup(hb_obj_id(host));
    p->hname = g_strdup(hb_obj_get_str(host, "name") ?: "host");
    p->haddr = g_strdup(hb_obj_get_str(host, "address") ?: "");
    p->huser = g_strdup(hb_obj_get_str(host, "user") ?: g_get_user_name());
    p->hport = (int)hb_obj_get_int(host, "port", 22);
    if (p->hport <= 0) p->hport = 22;
    os = hb_obj_get_str(host, "os");
    p->os_key = g_strdup(os && *os && strcmp(os,"auto") ? os : "generic");

    /* 终端先建（连接视图也用它消费事件） */
    p->term_view = build_term_view(p);
    p->connect_box = build_connect_view(p);
    gtk_stack_add_named(GTK_STACK(p->stack), p->connect_box, "connect");
    gtk_stack_add_named(GTK_STACK(p->stack), p->term_view, "term");
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "connect");

    apply_settings(p);

    /* 监控默认：SSH 默认开（settings.watch_default_ssh），
     * 信号掩码取引擎默认（EOF|133|RULES|TERMNOTIFY） */
    {
        hb_obj *o = NULL;
        hb_watch_cfg dc;
        hb_watch_cfg_defaults(&dc);
        p->watch_signals = dc.signals;
        if (hb_vault_settings(hb_store_vault(st), &o) == HB_OK && o) {
            p->watch_on = hb_obj_get_bool(o, "watch_default_ssh", true);
            if (hb_obj_get_bool(o, "watch_idle", false))
                p->watch_signals |= HB_WATCH_SIG_IDLE;
            hb_obj_free(o);
        }
    }
    watch_refresh_ui(p);

    hb_terminal_set_event_cb(HB_TERMINAL(p->term), on_session_ev, p);
    g_signal_connect(p->term, "title-changed",
                     G_CALLBACK(on_title), p);
    g_signal_connect(p->term, "exited", G_CALLBACK(on_exited), p);
    g_signal_connect(p->term, "dead-input", G_CALLBACK(on_dead_input), p);
    g_signal_connect(p->term, "notify-event", G_CALLBACK(on_notify), p);

    p->changed_handler = g_signal_connect(st, "changed",
        G_CALLBACK(on_store_changed), p);

    update_nodes(p);
    start_connect(p, FALSE);
    return GTK_WIDGET(p);
}

/* tab 关联（add_tab 之后调用；OSINFO/标题更新要用） */
void hb_ssh_page_attach_tab(HbSshPage *p, GtkWidget *win, const char *tab_id)
{
    p->win = HB_WINDOW(win);
    g_free(p->tab_id);
    p->tab_id = g_strdup(tab_id);
    hb_window_set_tab_host(p->win, tab_id, p->host_id);
}

/* 供 dev 调试：像用户输入一样把文本写进会话 */
void hb_ssh_page_feed_input(HbSshPage *p, const char *text)
{
    if (p->sess && text)
        hb_session_write(p->sess, text, strlen(text));
}

hb_session *hb_ssh_page_session(HbSshPage *p)
{
    return p->sess;
}

void hb_ssh_page_find_open(HbSshPage *p)
{
    /* 连接页/错误卡时没有终端 —— 仅 term 视图可用 */
    if (p->find_bar)
        hb_find_bar_open(HB_FIND_BAR(p->find_bar));
}
