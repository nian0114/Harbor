/* hb-forward-mgr — 端口转发运行管理器（FR-06）
 *
 * 应用级单例：按 vault "forward" 对象 id 管理每条规则的运行会话。
 * 每条运行中的规则持有一个 HB_CONN_FORWARD 的 HbConnCtl（完整复用
 * TOFU / 密钥确认 / 密码对话框 / 跳板 / vault 日志），会话事件 fd
 * 的 GLib 源由本对象挂接：事件先喂 hb_conn_ctl_handle_event，
 * HB_SESS_EV_FORWARD（LISTENING/STATS/WARN）由本对象消费。
 *
 * 状态机：STOPPED → CONNECTING → LISTENING ⇄ RECONNECTING →（不可重试）FAILED
 * 重连退避 1/2/5/10/30/30…s，LISTENING 成功后复位。
 * ================================================================== */

#include "hb-forward-mgr.h"
#include "hb-conn.h"
#include "hb-notify.h"

#include <harbor/ssh.h>
#include <glib-unix.h>
#include <string.h>

struct _HbForwardMgr {
    GObject   parent;
    HbStore  *store;          /* 应用单例（借用） */
    HbWindow *window;         /* weak pointer */
    GHashTable *runs;         /* rule_id -> FwdRun* */
    gulong    store_changed_h;
    gboolean  boot_queued;
};

typedef struct FwdRun FwdRun;
struct FwdRun {
    HbForwardMgr *m;
    char         *id;

    HbConnCtl    *ctl;
    hb_session   *sess;       /* borrowed（ctl 所有） */
    guint         ev_src;
    guint         retry_src;

    hb_fwd_state  state;
    gboolean      want_run;   /* 用户/自动启动意图（停连退避区分用） */

    char         *via;        /* 当前使用的经由主机 id */
    char         *cfg;        /* 配置指纹：编辑检测（重启判断） */

    /* 状态快照 */
    int           bound_port;
    uint64_t      up, down;
    uint32_t      active, total;
    char         *message;

    /* 退避 */
    int           bo_idx;
    gint64        retry_at;   /* monotonic µs */
};

G_DEFINE_TYPE(HbForwardMgr, hb_forward_mgr, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];
static HbForwardMgr *the_mgr;

static const int kBackoff[] = { 1, 2, 5, 10, 30 };
#define N_BACKOFF ((int)(sizeof(kBackoff) / sizeof(kBackoff[0])))

static void run_restart(FwdRun *r, hb_obj *o);
static void fail_permanent(FwdRun *r, const char *title,
                           const char *details);

/* ------------------------------ 工具 ------------------------------ */

static void emit_changed(HbForwardMgr *m, const char *id)
{
    g_signal_emit(m, sigs[SIG_CHANGED], 0, id);
}

/* 配置指纹：编辑重启判断用（不含 enabled/autostart 等非连接字段） */
static char *cfg_string(hb_obj *o)
{
    return g_strdup_printf("%s|%s|%d|%s|%d|%s",
        hb_obj_get_str(o, "kind") ?: "local",
        hb_obj_get_str(o, "bind_addr") ?: "",
        (int)hb_obj_get_int(o, "bind_port", 0),
        hb_obj_get_str(o, "dest_addr") ?: "",
        (int)hb_obj_get_int(o, "dest_port", 0),
        hb_obj_get_str(o, "via") ?: "");
}

static void rule_from_obj(hb_obj *o, hb_pf_rule *rule)
{
    const char *kind = hb_obj_get_str(o, "kind");
    memset(rule, 0, sizeof *rule);
    rule->kind = kind && !strcmp(kind, "remote")  ? HB_PF_REMOTE :
                 kind && !strcmp(kind, "dynamic") ? HB_PF_DYNAMIC :
                                                    HB_PF_LOCAL;
    rule->bind_addr = hb_obj_get_str(o, "bind_addr");
    rule->bind_port = (int)hb_obj_get_int(o, "bind_port", 0);
    rule->dest_addr = hb_obj_get_str(o, "dest_addr");
    rule->dest_port = (int)hb_obj_get_int(o, "dest_port", 0);
}

/* "127.0.0.1:13306 → db.internal:3306"（通知/确认文案用） */
static char *rule_desc(hb_obj *o)
{
    const char *kind = hb_obj_get_str(o, "kind");
    const char *ba = hb_obj_get_str(o, "bind_addr");
    const char *da = hb_obj_get_str(o, "dest_addr");
    int bp = (int)hb_obj_get_int(o, "bind_port", 0);
    int dp = (int)hb_obj_get_int(o, "dest_port", 0);
    char *bind = g_strdup_printf("%s:%d",
        ba && *ba ? ba : "127.0.0.1", bp);
    char *out;
    if (kind && !strcmp(kind, "remote"))
        out = g_strdup_printf("服务器 %s → %s:%d", bind,
                              da && *da ? da : "127.0.0.1", dp);
    else if (kind && !strcmp(kind, "dynamic"))
        out = g_strdup_printf("%s → SOCKS5", bind);
    else
        out = g_strdup_printf("%s → %s:%d", bind,
                              da && *da ? da : "?", dp);
    g_free(bind);
    return out;
}

static gboolean host_exists(HbForwardMgr *m, const char *id)
{
    hb_obj *h = NULL;
    gboolean ok;
    if (!id || !*id)
        return FALSE;
    ok = hb_vault_get(hb_store_vault(m->store), id, &h) == HB_OK &&
         h && !hb_obj_tombstone(h);
    if (h) hb_obj_free(h);
    return ok;
}

static gboolean retryable_code(int code)
{
    switch (code) {
    case HB_SSH_ERR_RESOLVE:
    case HB_SSH_ERR_REFUSED:
    case HB_SSH_ERR_UNREACHABLE:
    case HB_SSH_ERR_TIMEOUT:
    case HB_SSH_ERR_HANDSHAKE:
    case HB_SSH_ERR_CHANNEL:
    case HB_SSH_ERR_DISCONNECTED:
        return TRUE;
    default:
        return FALSE;
    }
}

static void persist_enabled(FwdRun *r, gboolean on)
{
    hb_obj *o = NULL;
    if (hb_vault_get(hb_store_vault(r->m->store), r->id, &o) == HB_OK &&
        o && !hb_obj_tombstone(o)) {
        hb_obj_set_bool(o, "enabled", on);
        hb_vault_put(hb_store_vault(r->m->store), o);
    }
    if (o) hb_obj_free(o);
}

/* ------------------------------ 运行记录 ------------------------------ */

static void run_detach_conn(FwdRun *r)
{
    if (r->ev_src) { g_source_remove(r->ev_src); r->ev_src = 0; }
    r->sess = NULL;
}

static void run_free(FwdRun *r)
{
    if (r->retry_src) g_source_remove(r->retry_src);
    run_detach_conn(r);
    if (r->ctl) {
        /* ctl 可能因 pending 对话框（key-confirm 等）比 run 活得久：
         * 先摘掉指向 r 的信号，再 cancel/unref。 */
        g_signal_handlers_disconnect_by_data(r->ctl, r);
        hb_conn_ctl_cancel(r->ctl);   /* close+free 会话（引擎内 ≤2s） */
        g_object_unref(r->ctl);
    }
    g_free(r->id);
    g_free(r->via);
    g_free(r->cfg);
    g_free(r->message);
    g_free(r);
}

static FwdRun *run_get(HbForwardMgr *m, const char *id, gboolean create)
{
    FwdRun *r = g_hash_table_lookup(m->runs, id);
    if (!r && create) {
        r = g_new0(FwdRun, 1);
        r->m = m;
        r->id = g_strdup(id);
        r->state = HB_FWD_STOPPED;
        g_hash_table_insert(m->runs, r->id, r);
    }
    return r;
}

/* ------------------------------ 重连退避 ------------------------------ */

static gboolean retry_cb(gpointer ud);

static void schedule_retry(FwdRun *r)
{
    int secs = kBackoff[r->bo_idx < N_BACKOFF ? r->bo_idx
                                            : N_BACKOFF - 1];
    if (r->bo_idx < N_BACKOFF)
        r->bo_idx++;
    r->retry_at = g_get_monotonic_time() + (gint64)secs * G_USEC_PER_SEC;
    if (r->retry_src) g_source_remove(r->retry_src);
    r->retry_src = g_timeout_add_seconds((guint)secs, retry_cb, r);
}

static void enter_reconnecting(FwdRun *r)
{
    if (!r->want_run)
        return;
    run_detach_conn(r);
    /* 幂等：failed + exited 成对到来时同一次失败只推进一次退避 */
    if (r->state == HB_FWD_RECONNECTING && r->retry_src)
        return;
    r->state = HB_FWD_RECONNECTING;
    schedule_retry(r);
    emit_changed(r->m, r->id);
}

static gboolean retry_cb(gpointer ud)
{
    FwdRun *r = ud;
    r->retry_src = 0;
    if (!r->want_run)
        return G_SOURCE_REMOVE;
    /* 重试瞬间 via 主机可能已被删除 → 判失败（不可重试） */
    if (!host_exists(r->m, r->via)) {
        fail_permanent(r, "经由主机不存在或已删除", NULL);
        return G_SOURCE_REMOVE;
    }
    r->state = HB_FWD_CONNECTING;
    hb_conn_ctl_start(r->ctl, r->via, NULL, 0, 0);
    emit_changed(r->m, r->id);
    return G_SOURCE_REMOVE;
}

/* ------------------------------ 永久失败 ------------------------------ */

static void notify_failed(FwdRun *r, const char *details)
{
    hb_obj *o = NULL;
    char *desc = NULL, *t;
    if (hb_vault_get(hb_store_vault(r->m->store), r->id, &o) == HB_OK &&
        o && !hb_obj_tombstone(o))
        desc = rule_desc(o);
    if (o) hb_obj_free(o);
    t = g_strdup_printf("转发规则启动失败 · %s",
                        desc ? desc : r->id);
    hb_notify_post("err", t, details && *details ? details : "", NULL);
    g_free(t);
    g_free(desc);
}

/* 不可重试失败：FAILED + enabled=false + 一条通知 */
static void fail_permanent(FwdRun *r, const char *title,
                           const char *details)
{
    if (r->retry_src) { g_source_remove(r->retry_src); r->retry_src = 0; }
    run_detach_conn(r);
    r->want_run = FALSE;
    r->state = HB_FWD_FAILED;
    g_free(r->message);
    r->message = g_strdup(details && *details ? details : title);
    persist_enabled(r, FALSE);
    notify_failed(r, details ? details : title);
    emit_changed(r->m, r->id);
}

/* ------------------------------ ctl 信号 ------------------------------ */

static gboolean run_ev_fd(int fd, GIOCondition cond, gpointer ud)
{
    FwdRun *r = ud;
    hb_sess_event ev;
    (void)fd;
    if (!r->sess)
        return G_SOURCE_REMOVE;
    /* 先尽量取干事件队列再判定 HUP：断开时的 ERROR/EXIT 常与 HUP
     * 同时到达，直接摘源会把 DISCONNECTED 丢掉 → 失去重连。 */
    while (r->sess && hb_session_next_event(r->sess, &ev)) {
        if (hb_conn_ctl_handle_event(r->ctl, &ev))
            ;
        else if (ev.type == HB_SESS_EV_FORWARD && ev.pf) {
            switch (ev.pf->kind) {
            case HB_PF_EV_LISTENING:
                r->state = HB_FWD_LISTENING;
                r->bound_port = ev.pf->bound_port;
                r->bo_idx = 0;            /* 成功 → 退避复位 */
                g_clear_pointer(&r->message, g_free);
                emit_changed(r->m, r->id);
                break;
            case HB_PF_EV_STATS:
                r->up = ev.pf->bytes_up;
                r->down = ev.pf->bytes_down;
                r->active = ev.pf->active;
                r->total = ev.pf->total;
                emit_changed(r->m, r->id);
                break;
            case HB_PF_EV_WARN:
                g_free(r->message);
                r->message = g_strdup(ev.pf->message);
                emit_changed(r->m, r->id);
                break;
            }
        }
        hb_sess_event_clear(&ev);
    }
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
        return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

static void on_ctl_session(HbConnCtl *c, gpointer sp, gpointer ud)
{
    FwdRun *r = ud;
    (void)c; (void)sp;
    r->sess = hb_conn_ctl_session(r->ctl);
    if (!r->sess)
        return;
    if (r->ev_src) g_source_remove(r->ev_src);
    r->ev_src = g_unix_fd_add(hb_session_event_fd(r->sess),
        G_IO_IN | G_IO_ERR | G_IO_HUP, run_ev_fd, r);
}

static void on_ctl_failed(HbConnCtl *c, int code, const char *title,
                          const char *details, gpointer ud)
{
    FwdRun *r = ud;
    (void)c;
    if (!r->want_run)
        return;
    if (retryable_code(code)) {
        enter_reconnecting(r);
        return;
    }
    /* AUTH / HOSTKEY_* / CANCELLED / FORWARD / code 0（绑定冲突、
     * 密钥被拒、跳板缺失…） → 永久失败 */
    fail_permanent(r, title ? title : "启动失败", details);
}

static void on_ctl_disconn(HbConnCtl *c, gpointer ud)
{
    FwdRun *r = ud;
    (void)c;
    enter_reconnecting(r);
}

static void on_ctl_exited(HbConnCtl *c, int code, gpointer ud)
{
    FwdRun *r = ud;
    (void)c; (void)code;
    /* 会话线程结束：已判 FAILED / 用户停止 → 忽略；否则视作断开。 */
    if (r->state == HB_FWD_FAILED || !r->want_run)
        return;
    enter_reconnecting(r);
}

static void ensure_ctl(FwdRun *r)
{
    if (r->ctl)
        return;
    r->ctl = hb_conn_ctl_new(r->m->store,
                             GTK_WIDGET(r->m->window), HB_CONN_FORWARD);
    g_signal_connect(r->ctl, "session",
                     G_CALLBACK(on_ctl_session), r);
    g_signal_connect(r->ctl, "failed",
                     G_CALLBACK(on_ctl_failed), r);
    g_signal_connect(r->ctl, "disconnected",
                     G_CALLBACK(on_ctl_disconn), r);
    g_signal_connect(r->ctl, "exited",
                     G_CALLBACK(on_ctl_exited), r);
}

/* ------------------------------ 启停 ------------------------------ */

/* 以当前 vault 对象（调用方已校验非 tombstone）启动；persist 控制是否
 * 把 enabled 写成 TRUE（autostart / 启动恢复不改写用户意图）。 */
static void run_start(HbForwardMgr *m, const char *id, gboolean persist)
{
    FwdRun *r = run_get(m, id, TRUE);
    hb_obj *o = NULL;
    const char *via;
    hb_pf_rule rule;

    if (r->want_run)
        return;
    if (hb_vault_get(hb_store_vault(m->store), id, &o) != HB_OK ||
        !o || hb_obj_tombstone(o)) {
        if (o) hb_obj_free(o);
        return;
    }
    if (!m->window) {   /* 对话框无处挂 → 连接会无声挂死 */
        fail_permanent(r, "窗口未就绪", NULL);
        hb_obj_free(o);
        return;
    }
    via = hb_obj_get_str(o, "via");
    if (!host_exists(m, via)) {
        fail_permanent(r, "经由主机不存在或已删除", NULL);
        hb_obj_free(o);
        return;
    }

    ensure_ctl(r);
    rule_from_obj(o, &rule);
    hb_conn_ctl_set_forward(r->ctl, &rule);
    g_free(r->via);
    r->via = g_strdup(via);
    g_free(r->cfg);
    r->cfg = cfg_string(o);
    r->want_run = TRUE;
    if (persist)
        persist_enabled(r, TRUE);
    /* 状态复位 */
    r->bound_port = 0;
    r->up = r->down = 0;
    r->active = r->total = 0;
    g_clear_pointer(&r->message, g_free);
    r->bo_idx = 0;
    r->state = HB_FWD_CONNECTING;
    /* start 内可能同步 emit failed（绑定冲突 code=0）→ FAILED */
    hb_conn_ctl_start(r->ctl, r->via, NULL, 0, 0);
    emit_changed(m, r->id);
    hb_obj_free(o);
}

static void run_stop(FwdRun *r, gboolean persist)
{
    r->want_run = FALSE;
    if (r->retry_src) { g_source_remove(r->retry_src); r->retry_src = 0; }
    run_detach_conn(r);
    if (r->ctl)
        hb_conn_ctl_cancel(r->ctl);
    r->state = HB_FWD_STOPPED;
    if (persist)
        persist_enabled(r, FALSE);
    emit_changed(r->m, r->id);
}

/* 运行中的规则被编辑 → 新配置重启（store changed 时调用）。 */
static void run_restart(FwdRun *r, hb_obj *o)
{
    const char *via = hb_obj_get_str(o, "via");
    hb_pf_rule rule;

    if (!host_exists(r->m, via)) {
        fail_permanent(r, "经由主机不存在或已删除", NULL);
        return;
    }
    ensure_ctl(r);
    rule_from_obj(o, &rule);
    hb_conn_ctl_set_forward(r->ctl, &rule);
    g_free(r->via);
    r->via = g_strdup(via);
    /* 重启：摘旧会话再开新会话（ctl 自带杀旧会话，但事件源要先摘） */
    if (r->retry_src) { g_source_remove(r->retry_src); r->retry_src = 0; }
    run_detach_conn(r);
    hb_conn_ctl_cancel(r->ctl);
    r->bound_port = 0;
    r->up = r->down = 0;
    r->active = r->total = 0;
    g_clear_pointer(&r->message, g_free);
    r->bo_idx = 0;
    r->state = HB_FWD_CONNECTING;
    hb_conn_ctl_start(r->ctl, r->via, NULL, 0, 0);
    emit_changed(r->m, r->id);
}

/* ------------------------------ vault 变更 ------------------------------ */

static void on_store_changed(HbStore *st, const char *type,
                             const char *id, gpointer ud)
{
    HbForwardMgr *m = ud;
    FwdRun *r;
    hb_obj *o = NULL;
    char *cfg;
    (void)st;

    if (strcmp(type, "forward") || !id)
        return;
    r = g_hash_table_lookup(m->runs, id);
    if (hb_vault_get(hb_store_vault(m->store), id, &o) != HB_OK ||
        !o || hb_obj_tombstone(o)) {
        /* 删除 → 停止并摘掉运行时 */
        if (o) hb_obj_free(o);
        if (r) {
            g_hash_table_remove(m->runs, id);   /* run_free：停止会话 */
            emit_changed(m, id);
        }
        return;
    }
    if (r && r->want_run) {
        cfg = cfg_string(o);
        if (g_strcmp0(cfg, r->cfg)) {
            g_free(r->cfg);
            r->cfg = cfg;
            run_restart(r, o);
        } else {
            g_free(cfg);
        }
    }
    hb_obj_free(o);
}

/* ------------------------------ 对外 API ------------------------------ */

HbForwardMgr *hb_forward_mgr_default(void)
{
    if (!the_mgr)
        the_mgr = g_object_new(HB_TYPE_FORWARD_MGR, NULL);
    return the_mgr;
}

static gboolean boot_idle(gpointer ud)
{
    HbForwardMgr *m = ud;
    hb_forward_mgr_start_all(m);
    return G_SOURCE_REMOVE;
}

static void on_window_gone(gpointer data, GObject *dead)
{
    HbForwardMgr *m = data;
    (void)dead;
    m->window = NULL;
    /* 应用退出兜底：摘下全部会话；enabled 保持不动，下次启动恢复 */
    hb_forward_mgr_stop_all(m);
}

void hb_forward_mgr_set_window(HbForwardMgr *m, HbWindow *win)
{
    if (m->window)
        g_object_remove_weak_pointer(G_OBJECT(m->window),
                                     (gpointer)&m->window);
    m->window = win;
    if (win) {
        g_object_add_weak_pointer(G_OBJECT(win),
                                  (gpointer)&m->window);
        g_object_weak_ref(G_OBJECT(win), on_window_gone, m);
        /* 窗口 shown 后一次性 idle：启动全部 enabled=true 规则 */
        if (!m->boot_queued) {
            m->boot_queued = TRUE;
            g_idle_add(boot_idle, m);
        }
    }
}

void hb_forward_mgr_start(HbForwardMgr *m, const char *rule_id)
{
    if (rule_id)
        run_start(m, rule_id, TRUE);
}

void hb_forward_mgr_stop(HbForwardMgr *m, const char *rule_id)
{
    FwdRun *r = rule_id ? g_hash_table_lookup(m->runs, rule_id) : NULL;
    if (r)
        run_stop(r, TRUE);
    else if (rule_id) {
        /* 未运行也要把意图写回去（开关来回拨） */
        hb_obj *o = NULL;
        if (hb_vault_get(hb_store_vault(m->store), rule_id,
                         &o) == HB_OK && o && !hb_obj_tombstone(o)) {
            hb_obj_set_bool(o, "enabled", FALSE);
            hb_vault_put(hb_store_vault(m->store), o);
        }
        if (o) hb_obj_free(o);
    }
}

void hb_forward_mgr_start_all(HbForwardMgr *m)
{
    hb_obj **objs;
    size_t n = 0, i;
    objs = hb_store_list(m->store, "forward", &n);
    for (i = 0; objs && i < n; i++)
        if (hb_obj_get_bool(objs[i], "enabled", FALSE))
            run_start(m, hb_obj_id(objs[i]), FALSE);
    if (objs) hb_obj_list_free(objs, n);
}

void hb_forward_mgr_stop_all(HbForwardMgr *m)
{
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, m->runs);
    while (g_hash_table_iter_next(&it, NULL, &v))
        run_stop((FwdRun *)v, FALSE);   /* 不改写 enabled */
}

gboolean hb_forward_mgr_status(HbForwardMgr *m, const char *rule_id,
                               hb_fwd_status *out)
{
    FwdRun *r;
    memset(out, 0, sizeof *out);
    out->state = HB_FWD_STOPPED;
    r = rule_id ? g_hash_table_lookup(m->runs, rule_id) : NULL;
    if (!r)
        return FALSE;
    out->state = r->state;
    out->bound_port = r->bound_port;
    out->bytes_up = r->up;
    out->bytes_down = r->down;
    out->active = r->active;
    out->total = r->total;
    out->message = r->message;
    if (r->state == HB_FWD_RECONNECTING) {
        gint64 left = r->retry_at - g_get_monotonic_time();
        out->retry_in_s = left > 0 ? (int)(left / G_USEC_PER_SEC) + 1 : 0;
    }
    return TRUE;
}

void hb_forward_mgr_autostart(const char *host_id)
{
    HbForwardMgr *m;
    hb_obj **objs;
    size_t n = 0, i;
    if (!host_id)
        return;
    m = hb_forward_mgr_default();
    objs = hb_store_list(m->store, "forward", &n);
    for (i = 0; objs && i < n; i++) {
        const char *via = hb_obj_get_str(objs[i], "via");
        if (via && !strcmp(via, host_id) &&
            hb_obj_get_bool(objs[i], "autostart", FALSE)) {
            FwdRun *r = g_hash_table_lookup(m->runs,
                                            hb_obj_id(objs[i]));
            if (!r || (!r->want_run && r->state != HB_FWD_CONNECTING &&
                       r->state != HB_FWD_RECONNECTING))
                run_start(m, hb_obj_id(objs[i]), FALSE);
        }
    }
    if (objs) hb_obj_list_free(objs, n);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void hb_forward_mgr_dispose(GObject *o)
{
    HbForwardMgr *m = HB_FORWARD_MGR(o);
    if (m->store && m->store_changed_h) {
        g_signal_handler_disconnect(m->store, m->store_changed_h);
        m->store_changed_h = 0;
    }
    if (m->window) {
        g_object_remove_weak_pointer(G_OBJECT(m->window),
                                     (gpointer)&m->window);
        m->window = NULL;
    }
    g_hash_table_destroy(m->runs);   /* run_free 逐个停止会话 */
    m->runs = NULL;
    G_OBJECT_CLASS(hb_forward_mgr_parent_class)->dispose(o);
}

static void hb_forward_mgr_class_init(HbForwardMgrClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_forward_mgr_dispose;
    sigs[SIG_CHANGED] = g_signal_new("changed", HB_TYPE_FORWARD_MGR,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void hb_forward_mgr_init(HbForwardMgr *m)
{
    m->store = hb_store_default();
    m->runs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify)run_free);
    m->store_changed_h = g_signal_connect(m->store, "changed",
        G_CALLBACK(on_store_changed), m);
}
