#include "hb-watch-ui.h"
#include "hb-window.h"
#include "hb-icons.h"
#include "hb-dialogs.h"
#include "hb-notify.h"

#include <regex.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glib/gi18n.h>

/* ============================ settings ============================ */

static hb_obj *watch_settings(HbStore *st)
{
    hb_obj *o = NULL;
    if (hb_vault_settings(hb_store_vault(st), &o) == HB_OK)
        return o;
    return NULL;
}

/* ============================ 规则解析 ============================ */

/* 单行解析："name: re" | "!name: re"；re 用引擎同款 POSIX ERE+ICASE
 * 预编译验证。成功返回 TRUE 并填 *r（字符串深拷贝）。 */
static gboolean parse_rule_line(char *line, hb_watch_rule *r)
{
    gboolean is_err = FALSE;
    char *colon, *name, *re;
    regex_t rx;

    line = g_strstrip(line);
    if (!*line || *line == '#')
        return FALSE;              /* 调用方区分"跳过"与"错误"：空/注释行
                                      在进入本函数前先判。 */
    if (*line == '!') {
        is_err = TRUE;
        line = g_strstrip(line + 1);
    }
    colon = strchr(line, ':');
    if (!colon)
        return FALSE;
    *colon = 0;
    name = g_strstrip(line);
    re   = g_strstrip(colon + 1);
    if (!*name || !*re)
        return FALSE;
    if (regcomp(&rx, re, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
        return FALSE;
    regfree(&rx);
    r->name  = g_strdup(name);
    r->ok_re = is_err ? NULL : g_strdup(re);
    r->err_re = is_err ? g_strdup(re) : NULL;
    return TRUE;
}

static gboolean rule_line_skip(const char *l)
{
    while (*l == ' ' || *l == '\t') l++;
    return !*l || *l == '#';
}

hb_watch_rule *hb_watch_rules_parse_text(const char *text, size_t *out_n,
                                         int *err_line)
{
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(hb_watch_rule));
    size_t n;
    int i, bad = 0;

    for (i = 0; lines[i] && !bad; i++) {
        hb_watch_rule r = {0};
        if (rule_line_skip(lines[i]))
            continue;
        if (!parse_rule_line(lines[i], &r))
            bad = i + 1;
        else
            g_array_append_val(arr, r);
    }
    g_strfreev(lines);
    n = arr->len;
    if (bad) {
        hb_watch_rules_free((hb_watch_rule *)arr->data, n);
        g_array_free(arr, TRUE);
        if (err_line) *err_line = bad;
        return NULL;
    }
    *out_n = n;
    if (err_line) *err_line = 0;
    return (hb_watch_rule *)g_array_free(arr, FALSE);
}

void hb_watch_rules_free(hb_watch_rule *r, size_t n)
{
    size_t i;
    if (!r) return;
    for (i = 0; i < n; i++) {
        g_free((gpointer)r[i].name);
        g_free((gpointer)r[i].ok_re);
        g_free((gpointer)r[i].err_re);
    }
    g_free(r);
}

/* strv 逐行解析并追加到 arr；坏行跳过（对话框保存前已校验）。 */
static void rules_append_strv(GArray *arr, const char *const *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char *dup = g_strdup(v[i]);
        hb_watch_rule r = {0};
        if (!rule_line_skip(dup) && parse_rule_line(dup, &r))
            g_array_append_val(arr, r);
        g_free(dup);
    }
}

hb_watch_rule *hb_watch_rules_merged(HbStore *st, const hb_obj *host,
                                     size_t *out_n)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(hb_watch_rule));
    hb_obj *o = watch_settings(st);
    const char **v;
    size_t n;
    hb_watch_rule *res;

    *out_n = 0;
    if (o) {
        n = hb_obj_get_strv(o, "watch_rules", &v);
        rules_append_strv(arr, v, n);
        hb_obj_free(o);
    }
    if (host) {
        n = hb_obj_get_strv(host, "watch_rules", &v);
        rules_append_strv(arr, v, n);
    }
    *out_n = arr->len;
    res = (hb_watch_rule *)g_array_free(arr, arr->len == 0);
    return res;
}

/* ============================ cfg / apply ============================ */

void hb_watch_cfg_build(HbStore *st, const hb_obj *host,
                        unsigned signals, hb_watch_cfg *out,
                        hb_watch_rule **rules_out, size_t *nrules_out)
{
    hb_obj *o;

    hb_watch_cfg_defaults(out);
    out->signals = signals;
    o = watch_settings(st);
    if (o) {
        out->min_duration_ms =
            (int)hb_obj_get_int(o, "watch_min_duration_s", 5) * 1000;
        out->idle_active_ms =
            (int)hb_obj_get_int(o, "watch_idle_n", 10) * 1000;
        out->idle_quiet_ms =
            (int)hb_obj_get_int(o, "watch_idle_m", 5) * 1000;
        hb_obj_free(o);
    }
    *rules_out = hb_watch_rules_merged(st, host, nrules_out);
    out->rules  = *rules_out;
    out->nrules = *nrules_out;
}

void hb_watch_apply(hb_session *sess, HbStore *st, const char *host_id,
                    gboolean on, unsigned signals)
{
    hb_obj *host = NULL;
    hb_watch_cfg c;
    hb_watch_rule *rules = NULL;
    size_t nr = 0;

    if (!sess)
        return;
    if (!on) {
        hb_session_set_watch(sess, NULL);
        return;
    }
    if (host_id)
        hb_vault_get(hb_store_vault(st), host_id, &host);
    hb_watch_cfg_build(st, host, signals, &c, &rules, &nr);
    hb_session_set_watch(sess, &c);
    hb_watch_rules_free(rules, nr);
    if (host)
        hb_obj_free(host);
}

/* ============================ 路由 ============================ */

/* "HH:MM-HH:MM"（可跨零点；空=关闭）→ 当前是否处于免打扰窗口。 */
static gboolean in_dnd_window(const char *spec)
{
    int sh, sm, eh, em, now, s, e;
    GDateTime *dt;
    if (!spec || !*spec)
        return FALSE;
    if (sscanf(spec, "%d:%d-%d:%d", &sh, &sm, &eh, &em) != 4)
        return FALSE;
    s = sh * 60 + sm;
    e = eh * 60 + em;
    if (s == e)
        return FALSE;
    dt = g_date_time_new_now_local();
    now = g_date_time_get_hour(dt) * 60 + g_date_time_get_minute(dt);
    g_date_time_unref(dt);
    if (s < e)
        return now >= s && now < e;
    return now >= s || now < e;      /* 跨零点 */
}

static char *fmt_duration(int64_t ms)
{
    if (ms < 0)
        return g_strdup("");
    if (ms < 60000)
        return g_strdup_printf(_("耗时 %.1fs"), ms / 1000.0);
    return g_strdup_printf(_("耗时 %dm%02ds"), (int)(ms / 60000),
                           (int)((ms % 60000) / 1000));
}

typedef struct {
    HbWindow *win;
    char     *id;
    guint     token;
} DotClear;

static void dot_clear_free(DotClear *d)
{
    if (d->win)
        g_object_remove_weak_pointer(G_OBJECT(d->win),
                                     (gpointer)&d->win);
    g_free(d->id);
    g_free(d);
}

static gboolean dot_clear_cb(gpointer ud)
{
    DotClear *d = ud;
    if (d->win)
        hb_window_clear_watch_dot(d->win, d->id, d->token);
    return G_SOURCE_REMOVE;
}

void hb_watch_route(GtkWidget *page, const char *tab_id,
                    const char *host_label, const hb_watch_hit *hit)
{
    HbStore *st = hb_store_default();
    hb_obj *o;
    HbWindow *win = NULL;
    GtkRoot *root;
    gboolean termnotify, is_bell, focused, host_only, fdnd, snd;
    const char *kind, *dot, *dnd;
    char *title = NULL, *body = NULL, *dur = NULL;
    const char *host = host_label ? host_label : "?";

    if (!hit || !tab_id)
        return;
    termnotify = hit->signal == HB_WATCH_SIG_TERMNOTIFY;
    is_bell = termnotify && hit->rule && !strcmp(hit->rule, "BEL");

    o = watch_settings(st);
    host_only = o && !g_strcmp0(hb_obj_get_str(o, "notify_body"), "host");
    fdnd = !o || hb_obj_get_bool(o, "watch_focus_dnd", true);
    snd  = o && hb_obj_get_bool(o, "watch_sound", false);
    dnd  = o ? hb_obj_get_str(o, "watch_dnd") : NULL;

    /* ---- 分类 + 文案 ---- */
    if (termnotify)
        kind = is_bell ? "ok" : "info";
    else
        kind = hit->ok ? "ok" : "err";

    if (host_only) {
        /* 隐私模式：正文只带主机名，不携带命令/输出行 */
        title = g_strdup_printf("%s · %s",
            termnotify ? _("终端通知")
                       : (hit->ok ? _("会话任务完成") : _("任务失败")), host);
        body = g_strdup_printf(_("%s 上的任务%s"), host,
            termnotify ? _("有新通知") : (hit->ok ? _("已完成") : _("失败")));
    } else if (termnotify) {
        const char *tt = hit->title && *hit->title ? hit->title
                                                   : _("终端通知");
        title = g_strdup_printf("%s · %s", tt, host);
        body = g_strdup(hit->line ? hit->line : "");
    } else {
        GString *b = g_string_new(NULL);
        title = g_strdup_printf("%s · %s",
            hit->ok ? _("会话任务完成") : _("任务失败"), host);
        g_string_append(b, hit->command && *hit->command ? hit->command
                          : (hit->rule ? hit->rule : ""));
        if (!hit->ok && hit->exit_code >= 0)
            g_string_append_printf(b, " · exit %d", hit->exit_code);
        dur = fmt_duration(hit->duration_ms);
        if (*dur)
            g_string_append_printf(b, " · %s", dur);
        g_string_append(b, _(" — 可以进行下一步了"));
        body = g_string_free(b, FALSE);
    }

    /* ---- 通知中心 ---- */
    hb_notify_post(kind, title, body, tab_id);

    /* ---- tab 角标 ---- */
    root = gtk_widget_get_root(page);
    if (root && HB_IS_WINDOW(root))
        win = HB_WINDOW(root);
    dot = termnotify ? (is_bell ? "ok" : "pending")
                     : (hit->ok ? "ok" : "err");
    if (win) {
        const char *active = hb_window_active_tab_id(win);
        focused = gtk_window_is_active(GTK_WINDOW(win)) &&
                  active && !strcmp(active, tab_id);
        if (focused) {
            /* 正在看的 tab：短暂点亮 4s 即清 */
            DotClear *d = g_new0(DotClear, 1);
            d->win = win;
            g_object_add_weak_pointer(G_OBJECT(win), (gpointer)&d->win);
            d->id = g_strdup(tab_id);
            d->token = hb_window_set_watch_dot(win, tab_id, dot);
            g_timeout_add_full(G_PRIORITY_DEFAULT, 4000, dot_clear_cb,
                               d, (GDestroyNotify)dot_clear_free);
        } else {
            /* 非活动 tab：角标保持到用户切过去（refresh_active 清） */
            hb_window_set_watch_dot(win, tab_id, dot);
        }
    } else {
        focused = FALSE;
    }

    /* ---- 系统通知 ----
     * 当前窗口活动且聚焦该 tab + focus_dnd → 不弹；
     * 全局 DND 窗口内 → 不弹。 */
    if (!((focused && fdnd) || in_dnd_window(dnd))) {
        GApplication *app = g_application_get_default();
        if (app) {
            GNotification *n = g_notification_new(title);
            char *nid = g_strdup_printf("watch-%s", tab_id);
            g_notification_set_body(n, body);
            g_notification_set_default_action_and_target(
                n, "app.show-tab", "s", tab_id);
            if (!hit->ok)
                g_notification_set_priority(n,
                    G_NOTIFICATION_PRIORITY_HIGH);
            g_application_send_notification(app, nid, n);
            g_free(nid);
            g_object_unref(n);
        }
    }
    if (snd)
        gdk_display_beep(gdk_display_get_default());

    g_free(title);
    g_free(body);
    g_free(dur);
    if (o) hb_obj_free(o);
}

/* ============================ 文案 ============================ */

char *hb_watch_signal_summary(unsigned signals)
{
    GString *s = g_string_new(_("监控: "));
    gboolean first = TRUE;
    struct { unsigned bit; const char *name; } map[] = {
        { HB_WATCH_SIG_EOF,       "EOF" },
        { HB_WATCH_SIG_OSC133,    "OSC133" },
        { HB_WATCH_SIG_RULES,     _("AI 预设") },
        { HB_WATCH_SIG_IDLE,      _("空闲") },
        { HB_WATCH_SIG_TERMNOTIFY,_("通知") },
    };
    int i;
    for (i = 0; i < 5; i++)
        if (signals & map[i].bit) {
            if (!first) g_string_append(s, " + ");
            g_string_append(s, map[i].name);
            first = FALSE;
        }
    if (first)
        g_string_append(s, _("关"));
    return g_string_free(s, FALSE);
}

/* ============================ 对话框 ============================ */

typedef struct {
    HbWatchUiOps   ops;
    AdwDialog     *dlg;
    GtkWidget     *rules_err;
    GtkTextBuffer *rules_buf;
    int            refs;           /* 子对话框（测试器/安装器/SSH 确认）
                                      可能比主对话框活得久 → 引用计数 */
} WatchDlg;

static WatchDlg *wd_ref(WatchDlg *d)   { d->refs++; return d; }
static void      wd_unref(WatchDlg *d)
{
    if (--d->refs > 0)
        return;
    if (d->ops.page)
        g_object_remove_weak_pointer(G_OBJECT(d->ops.page),
                                     (gpointer)&d->ops.page);
    if (d->ops.apply_ud && G_IS_OBJECT(d->ops.apply_ud))
        g_object_remove_weak_pointer(G_OBJECT(d->ops.apply_ud),
                                     (gpointer)&d->ops.apply_ud);
    g_free((gpointer)d->ops.host_id);
    g_free(d);
}

static void wd_apply(WatchDlg *d)
{
    if (d->ops.apply && d->ops.page)
        d->ops.apply(d->ops.apply_ud, d->ops.on, d->ops.signals);
}

/* settings 字段写回（保存即生效；store "changed" 会让各页自行重读） */
static void set_save_bool(HbStore *st, const char *key, gboolean v)
{
    hb_obj *o = watch_settings(st);
    if (!o) return;
    hb_obj_set_bool(o, key, v);
    hb_vault_put(hb_store_vault(st), o);
    hb_obj_free(o);
}

static void set_save_str(HbStore *st, const char *key, const char *v)
{
    hb_obj *o = watch_settings(st);
    if (!o) return;
    hb_obj_set_str(o, key, v);
    hb_vault_put(hb_store_vault(st), o);
    hb_obj_free(o);
}

/* ---------- 行构造：标题+副标题+开关 ---------- */

static GtkWidget *switch_row(const char *title, const char *sub,
                             GtkWidget **sw_out)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    GtkWidget *sw = gtk_switch_new();

    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(col), t);
    if (sub) {
        GtkWidget *s = gtk_label_new(sub);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_box_append(GTK_BOX(col), s);
    }
    gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(row), col);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), sw);
    gtk_widget_add_css_class(row, "check-row");
    if (sw_out) *sw_out = sw;
    return row;
}

/* ---------- 开关回调 ---------- */

static void on_master_sw(GtkSwitch *sw, GParamSpec *ps, WatchDlg *d)
{
    (void)ps;
    d->ops.on = gtk_switch_get_active(sw);
    wd_apply(d);
}

static void on_sig_sw(GtkSwitch *sw, GParamSpec *ps, WatchDlg *d)
{
    unsigned bit = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(sw), "bit"));
    (void)ps;
    if (gtk_switch_get_active(sw))
        d->ops.signals |= bit;
    else
        d->ops.signals &= ~bit;
    wd_apply(d);
}

static void on_focus_sw(GtkSwitch *sw, GParamSpec *ps, WatchDlg *d)
{
    (void)ps;
    set_save_bool(d->ops.store, "watch_focus_dnd",
                  gtk_switch_get_active(sw));
}

static void on_priv_sw(GtkSwitch *sw, GParamSpec *ps, WatchDlg *d)
{
    (void)ps;
    set_save_str(d->ops.store, "notify_body",
                 gtk_switch_get_active(sw) ? "host" : "full");
}

/* 规则 textarea：合法 → 存 strv 并重挂引擎；非法 → 行号报错不存 */
static void on_rules_changed(GtkTextBuffer *buf, WatchDlg *d)
{
    GtkTextIter a, b;
    char *text, **lines;
    size_t n = 0;
    int err_line = 0;
    hb_watch_rule *parsed;
    GPtrArray *keep;
    int i;

    gtk_text_buffer_get_bounds(buf, &a, &b);
    text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    parsed = hb_watch_rules_parse_text(text, &n, &err_line);
    if (!parsed) {
        char *m = g_strdup_printf(_("第 %d 行不是合法的 \"名称: 正则\"，"
                                  "或正则无法编译 —— 未保存"), err_line);
        gtk_label_set_label(GTK_LABEL(d->rules_err), m);
        gtk_widget_set_visible(d->rules_err, TRUE);
        g_free(m);
        g_free(text);
        return;
    }
    hb_watch_rules_free(parsed, n);
    gtk_widget_set_visible(d->rules_err, FALSE);

    /* 存回原样行（含注释），只过滤空行 */
    keep = g_ptr_array_new();
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i]; i++) {
        char *l = g_strstrip(lines[i]);
        if (*l)
            g_ptr_array_add(keep, l);
    }
    g_ptr_array_add(keep, NULL);
    {
        hb_obj *o = watch_settings(d->ops.store);
        if (o) {
            hb_obj_set_strv(o, "watch_rules",
                            (const char *const *)keep->pdata,
                            keep->len - 1);
            hb_vault_put(hb_store_vault(d->ops.store), o);
            hb_obj_free(o);
        }
    }
    g_ptr_array_free(keep, TRUE);
    g_strfreev(lines);
    g_free(text);
    wd_apply(d);          /* 规则变了 → 重挂引擎 */
}

/* ---------- 规则测试器 ---------- */

static void on_tester_run(GtkButton *b, gpointer ud);
static void on_installer(GtkButton *b, gpointer ud);

typedef struct {
    WatchDlg      *wd;
    GtkTextBuffer *buf;
    GtkWidget     *result;
    GtkWidget     *hint;
} TesterDlg;

static void tester_free(TesterDlg *t)
{
    wd_unref(t->wd);
    g_free(t);
}

static char *buffer_text(GtkTextBuffer *buf)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    return gtk_text_buffer_get_text(buf, &a, &b, FALSE);
}

static void on_tester_run(GtkButton *b, gpointer ud)
{
    TesterDlg *t = ud;
    WatchDlg *d = t->wd;
    char *text;
    hb_obj *host = NULL;
    hb_watch_cfg c;
    hb_watch_rule *rules = NULL;
    size_t nr = 0;
    hb_watch_hit hit;
    int lines;

    (void)b;
    if (!d || !d->ops.page)
        return;
    memset(&hit, 0, sizeof hit);
    text = buffer_text(t->buf);
    lines = 0;
    {
        char *p = text;
        while (*p) if (*p++ == '\n') lines++;
    }
    gtk_widget_set_visible(t->hint, lines < 2);

    if (d->ops.host_id)
        hb_vault_get(hb_store_vault(d->ops.store), d->ops.host_id, &host);
    hb_watch_cfg_build(d->ops.store, host, d->ops.signals,
                       &c, &rules, &nr);
    if (hb_watch_test_text(&c, text, &hit)) {
        char *m = g_strdup_printf(_("命中：%s (%s) — %s"),
            hit.rule ? hit.rule : "?",
            hit.ok ? "ok" : "err",
            hit.line ? hit.line : "");
        gtk_label_set_label(GTK_LABEL(t->result), m);
        g_free(m);
        hb_watch_hit_clear(&hit);
    } else {
        gtk_label_set_label(GTK_LABEL(t->result), _("未命中"));
    }
    hb_watch_rules_free(rules, nr);
    if (host) hb_obj_free(host);
    g_free(text);
}

static void open_tester(GtkButton *b, WatchDlg *d)
{
    AdwDialog *dlg;
    GtkWidget *body, *foot, *sw, *tv, *run, *f;
    TesterDlg *t;
    (void)b;

    dlg = hb_modal_new(_("规则测试器"), &body, &foot);
    t = g_new0(TesterDlg, 1);
    t->wd = wd_ref(d);

    {
        GtkWidget *h = gtk_label_new(
            _("粘贴一段终端输出样例，用当前信号与规则跑一遍引擎。"));
        gtk_label_set_xalign(GTK_LABEL(h), 0);
        gtk_label_set_wrap(GTK_LABEL(h), TRUE);
        gtk_widget_add_css_class(h, "hb-dim");
        gtk_box_append(GTK_BOX(body), h);
    }

    tv = gtk_text_view_new();
    t->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
    sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(sw), 140);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    gtk_widget_add_css_class(sw, "codebox");
    gtk_box_append(GTK_BOX(body), sw);

    t->hint = gtk_label_new(_("样例不足 2 行，可能不具代表性"));
    gtk_widget_add_css_class(t->hint, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(t->hint), 0);
    gtk_widget_set_visible(t->hint, FALSE);
    gtk_box_append(GTK_BOX(body), t->hint);

    t->result = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(t->result), 0);
    gtk_label_set_wrap(GTK_LABEL(t->result), TRUE);
    gtk_widget_add_css_class(t->result, "mono");
    gtk_box_append(GTK_BOX(body), t->result);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    run = gtk_button_new_with_label(_("测试"));
    gtk_widget_add_css_class(run, "hb-btn");
    gtk_widget_add_css_class(run, "primary");
    g_signal_connect(run, "clicked", G_CALLBACK(on_tester_run), t);
    gtk_box_append(GTK_BOX(f), run);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect_swapped(dlg, "closed",
                             G_CALLBACK(tester_free), t);
    adw_dialog_present(dlg, d->ops.page);
}

/* ---------- shell 集成安装器 ---------- */

typedef struct {
    WatchDlg   *wd;
    GtkDropDown *dd;
    GtkWidget  *preview;
    GtkWidget  *err;
} InstDlg;

static void inst_free(InstDlg *d)
{
    wd_unref(d->wd);
    g_free(d);
}

static const char *const INST_SHELLS[] = { "bash", "zsh", "fish" };

static const char *inst_sel_shell(InstDlg *d)
{
    guint i = gtk_drop_down_get_selected(d->dd);
    return i < 3 ? INST_SHELLS[i] : "bash";
}

/* 本地 rc 路径/追加行 */
static char *local_rc_line(const char *shell)
{
    if (!strcmp(shell, "fish"))
        return g_strdup(
            "test -r ~/.config/harbor/shell/harbor.fish; and "
            "source ~/.config/harbor/shell/harbor.fish"
            "  # Harbor shell integration");
    return g_strdup_printf(
        "[ -r ~/.config/harbor/shell/harbor.%s ] && "
        ". ~/.config/harbor/shell/harbor.%s  # Harbor shell integration",
        shell, shell);
}

static char *local_rc_path(const char *shell)
{
    if (!strcmp(shell, "fish"))
        return g_build_filename(g_get_home_dir(),
            ".config", "fish", "conf.d", "harbor.fish", NULL);
    return g_build_filename(g_get_home_dir(),
        !strcmp(shell, "zsh") ? ".zshrc" : ".bashrc", NULL);
}

static char *shell_script_path(const char *shell)
{
    return g_strdup_printf("~/.config/harbor/shell/harbor.%s", shell);
}

/* 远端命令：heredoc 写脚本文件 + 守卫式 rc 追加（单行序列）。 */
static char *remote_cmd(const char *shell)
{
    const char *script = hb_shell_integration_script(shell);
    GString *g = g_string_new(NULL);
    char *sp = shell_script_path(shell);
    char *rc = NULL, *rcline = NULL;

    g_string_append_printf(g,
        "mkdir -p ~/.config/harbor/shell && "
        "cat > %s <<'HARBOR_SI_EOF'\n%sHARBOR_SI_EOF\n",
        sp, script);
    if (!strcmp(shell, "fish")) {
        /* fish：conf.d 文件直接写（幂等覆盖） */
        g_string_append(g,
            "mkdir -p ~/.config/fish/conf.d && printf '%s\\n' "
            "'test -r ~/.config/harbor/shell/harbor.fish; and "
            "source ~/.config/harbor/shell/harbor.fish"
            "  # Harbor shell integration' "
            "> ~/.config/fish/conf.d/harbor.fish");
    } else {
        rc = g_strdup_printf("~/.%src", shell);
        rcline = local_rc_line(shell);
        g_string_append_printf(g,
            "grep -q 'Harbor shell integration' %s 2>/dev/null || "
            "echo '%s' >> %s", rc, rcline, rc);
        g_free(rc);
        g_free(rcline);
    }
    g_free(sp);
    return g_string_free(g, FALSE);
}

static void inst_refresh_preview(InstDlg *d)
{
    const char *sh = inst_sel_shell(d);
    char *txt;

    if (d->wd->ops.is_ssh) {
        char *cmd = remote_cmd(sh);
        txt = g_strdup_printf(_("远端将执行：\n%s"), cmd);
        g_free(cmd);
    } else {
        char *sp = shell_script_path(sh);
        char *rp = local_rc_path(sh);
        char *rl = local_rc_line(sh);
        txt = g_strdup_printf(
            _("将写入 %s 并向 %s 追加：\n%s"),
            sp, rp, rl);
        g_free(sp); g_free(rp); g_free(rl);
    }
    gtk_label_set_label(GTK_LABEL(d->preview), txt);
    g_free(txt);
}

static void on_inst_shell_sel(GtkDropDown *dd, GParamSpec *ps, InstDlg *d)
{
    (void)dd; (void)ps;
    inst_refresh_preview(d);
}

static void inst_err(InstDlg *d, const char *msg)
{
    gtk_label_set_label(GTK_LABEL(d->err), msg);
    gtk_widget_set_visible(d->err, msg != NULL);
}

/* 本地安装：写脚本文件 + rc 守卫追加。 */
static gboolean install_local(InstDlg *d, const char *shell, char **err)
{
    const char *script = hb_shell_integration_script(shell);
    char *dir, *sp, *rp, *rl, *content;
    gboolean ok = TRUE;
    (void)d;

    dir = g_build_filename(g_get_home_dir(), ".config", "harbor",
                           "shell", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        *err = g_strdup_printf(_("无法创建 %s"), dir);
        g_free(dir);
        return FALSE;
    }
    sp = g_strdup_printf("%s/harbor.%s", dir, shell);
    if (!g_file_set_contents(sp, script, -1, NULL)) {
        *err = g_strdup_printf(_("无法写入 %s"), sp);
        ok = FALSE;
        goto out;
    }
    chmod(sp, 0644);

    rp = local_rc_path(shell);
    rl = local_rc_line(shell);
    if (!strcmp(shell, "fish")) {
        /* conf.d/harbor.fish 直接写（幂等覆盖） */
        char *pd = g_path_get_dirname(rp);
        g_mkdir_with_parents(pd, 0755);
        content = g_strdup_printf("%s\n", rl);
        if (!g_file_set_contents(rp, content, -1, NULL)) {
            *err = g_strdup_printf(_("无法写入 %s"), rp);
            ok = FALSE;
        }
        g_free(content);
        g_free(pd);
    } else {
        /* 标记注释已存在则跳过 */
        gchar *old = NULL;
        gsize olen = 0;
        gboolean have = FALSE;
        if (g_file_get_contents(rp, &old, &olen, NULL))
            have = strstr(old, "Harbor shell integration") != NULL;
        if (!have) {
            content = g_strdup_printf("%s%s\n",
                                      old ? old : "",
                                      rl);
            if (!g_file_set_contents(rp, content, -1, NULL)) {
                *err = g_strdup_printf(_("无法写入 %s"), rp);
                ok = FALSE;
            }
            g_free(content);
        }
        g_free(old);
    }
    g_free(rp);
    g_free(rl);
out:
    g_free(dir);
    g_free(sp);
    return ok;
}

typedef struct {
    WatchDlg *wd;
    char     *cmd;
} SshInst;

static void ssh_inst_free(SshInst *s)
{
    wd_unref(s->wd);
    g_free(s->cmd);
    g_free(s);
}

static void on_ssh_inst_confirm(gboolean yes, gpointer ud)
{
    SshInst *s = ud;
    hb_session *sess;
    char *nl;
    if (!yes || !s->wd->ops.page) {
        ssh_inst_free(s);
        return;
    }
    sess = s->wd->ops.session
        ? s->wd->ops.session(s->wd->ops.session_ud) : NULL;
    if (!sess) {
        hb_toast(s->wd->ops.page, _("会话不可用，无法写入远端"));
        ssh_inst_free(s);
        return;
    }
    nl = g_strdup_printf("%s\n", s->cmd);
    hb_session_write(sess, nl, strlen(nl));
    g_free(nl);
    hb_toast(s->wd->ops.page,
             _("已发送到远端 · 重新打开 shell 后生效"));
    ssh_inst_free(s);
}

static void on_inst_run(GtkButton *b, gpointer ud)
{
    InstDlg *d = ud;
    const char *sh = inst_sel_shell(d);
    (void)b;
    if (!d->wd || !d->wd->ops.page)
        return;
    if (d->wd->ops.is_ssh) {
        /* SSH：先显式确认再把命令写进会话 */
        SshInst *s = g_new0(SshInst, 1);
        char *mk, *esc;
        s->wd = wd_ref(d->wd);
        s->cmd = remote_cmd(sh);
        esc = g_markup_escape_text(s->cmd, -1);
        mk = g_strdup_printf(
            _("将在远端执行以下命令，请确认远端处于 shell 提示符：\n\n"
            "<tt>%s</tt>"), esc);
        g_free(esc);
        hb_dialog_confirm(d->wd->ops.page, _("安装 shell 集成"),
                          mk, _("执行"), FALSE,
                          on_ssh_inst_confirm, s);
        g_free(mk);
    } else {
        char *err = NULL;
        if (install_local(d, sh, &err)) {
            inst_err(d, NULL);
            hb_toast(d->wd->ops.page,
                     _("已安装 · 重新打开 shell 后生效"));
        } else {
            inst_err(d, err ? err : _("安装失败"));
            g_free(err);
        }
    }
}

static void on_installer(GtkButton *b, gpointer ud)
{
    WatchDlg *d = ud;
    AdwDialog *dlg;
    GtkWidget *body, *foot, *f, *dd, *run;
    InstDlg *in;
    GtkStringList *mdl;
    const char *sh = "bash";
    guint sel = 0, i;
    (void)b;
    if (!d)
        return;

    /* 默认 shell：Local 取 $SHELL（在支持列表里），SSH 固定 bash */
    if (!d->ops.is_ssh) {
        const char *env = g_getenv("SHELL");
        if (env) {
            const char *base = g_path_get_basename(env);
            for (i = 0; i < 3; i++)
                if (!strcmp(base, INST_SHELLS[i])) {
                    sh = INST_SHELLS[i];
                    break;
                }
        }
    }
    for (i = 0; i < 3; i++)
        if (!strcmp(sh, INST_SHELLS[i])) sel = i;

    in = g_new0(InstDlg, 1);
    in->wd = wd_ref(d);

    dlg = hb_modal_new(_("安装 shell 集成"), &body, &foot);
    {
        GtkWidget *h = gtk_label_new(
            d->ops.is_ssh
            ? _("OSC 133 标记让监控拿到精确的命令边界与退出码。"
              "将在远端 shell rc 中注入集成脚本（需确认远端在提示符下）。")
            : _("OSC 133 标记让监控拿到精确的命令边界与退出码。"
              "脚本写入 ~/.config/harbor/shell 并在 rc 中追加一行加载。"));
        gtk_label_set_xalign(GTK_LABEL(h), 0);
        gtk_label_set_wrap(GTK_LABEL(h), TRUE);
        gtk_widget_add_css_class(h, "hb-dim");
        gtk_box_append(GTK_BOX(body), h);
    }

    mdl = gtk_string_list_new(INST_SHELLS);
    dd = gtk_drop_down_new(G_LIST_MODEL(mdl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), sel);
    gtk_widget_set_halign(dd, GTK_ALIGN_START);
    g_signal_connect(dd, "notify::selected",
                     G_CALLBACK(on_inst_shell_sel), in);
    in->dd = GTK_DROP_DOWN(dd);
    gtk_box_append(GTK_BOX(body), dd);

    in->preview = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(in->preview), 0);
    gtk_label_set_wrap(GTK_LABEL(in->preview), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(in->preview), PANGO_WRAP_CHAR);
    gtk_label_set_selectable(GTK_LABEL(in->preview), TRUE);
    gtk_widget_add_css_class(in->preview, "mono");
    gtk_widget_add_css_class(in->preview, "hb-dim");
    gtk_box_append(GTK_BOX(body), in->preview);

    in->err = gtk_label_new("");
    gtk_widget_add_css_class(in->err, "hb-red");
    gtk_label_set_xalign(GTK_LABEL(in->err), 0);
    gtk_label_set_wrap(GTK_LABEL(in->err), TRUE);
    gtk_widget_set_visible(in->err, FALSE);
    gtk_box_append(GTK_BOX(body), in->err);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    run = gtk_button_new_with_label(_("安装"));
    gtk_widget_add_css_class(run, "hb-btn");
    gtk_widget_add_css_class(run, "primary");
    g_signal_connect(run, "clicked", G_CALLBACK(on_inst_run), in);
    /* SSH 且当前无会话 → 禁按 */
    if (d->ops.is_ssh) {
        hb_session *sess = d->ops.session
            ? d->ops.session(d->ops.session_ud) : NULL;
        gtk_widget_set_sensitive(run, sess != NULL);
    }
    gtk_box_append(GTK_BOX(f), run);
    gtk_box_append(GTK_BOX(foot), f);

    inst_refresh_preview(in);
    g_signal_connect_swapped(dlg, "closed",
                             G_CALLBACK(inst_free), in);
    adw_dialog_present(dlg, d->ops.page);
}

/* ---------- 主对话框 ---------- */

void hb_watch_ui_open(const HbWatchUiOps *ops)
{
    static const struct {
        unsigned bit; const char *title; const char *sub;
    } rows[] = {
        { HB_WATCH_SIG_EOF,       N_("会话/进程结束（EOF）"),   N_("最强信号") },
        { HB_WATCH_SIG_OSC133,    N_("OSC 133 shell 集成"),
                                  N_("含退出码，可区分成败") },
        { HB_WATCH_SIG_RULES,     N_("AI CLI 输出预设"),
                                  N_("aider / 编译 / pytest / 下载等") },
        { HB_WATCH_SIG_IDLE,      N_("空闲启发式（输出≥10s 后静默≥5s）"),
                                  N_("兜底信号，置信度低") },
        { HB_WATCH_SIG_TERMNOTIFY,N_("终端通知（BEL · OSC 9/777）"),
                                  N_("应用主动请求的通知") },
    };
    WatchDlg *d;
    AdwDialog *dlg;
    GtkWidget *body, *foot, *f, *sp, *btn;
    hb_obj *o;
    int i;
    char *title;

    d = g_new0(WatchDlg, 1);
    d->refs = 1;
    d->ops = *ops;
    d->ops.host_id = g_strdup(ops->host_id);
    g_object_add_weak_pointer(G_OBJECT(ops->page),
                              (gpointer)&d->ops.page);
    if (ops->apply_ud && G_IS_OBJECT(ops->apply_ud))
        g_object_add_weak_pointer(G_OBJECT(ops->apply_ud),
                                  (gpointer)&d->ops.apply_ud);

    title = g_strdup_printf(_("会话监控 · %s"),
                            ops->is_ssh ? "SSH" : _("Local"));
    dlg = hb_modal_new(title, &body, &foot);
    g_free(title);
    d->dlg = dlg;

    /* 启用开关 */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *t = gtk_label_new(NULL);
        GtkWidget *s = gtk_label_new(
            _("命中完成信号 → Tab 角标 + 通知中心 + 系统通知"));
        GtkWidget *sw = gtk_switch_new();
        gtk_label_set_markup(GTK_LABEL(t), _("<b>启用完成监控</b>"));
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(col), t);
        gtk_box_append(GTK_BOX(col), s);
        gtk_box_append(GTK_BOX(row), col);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), sw);
        gtk_widget_add_css_class(row, "check-row");
        gtk_switch_set_active(GTK_SWITCH(sw), d->ops.on);
        g_signal_connect(sw, "notify::active",
                         G_CALLBACK(on_master_sw), d);
        gtk_box_append(GTK_BOX(body), row);
    }

    /* 信号开关组 */
    {
        GtkWidget *lb = gtk_label_new(_("完成信号（任一命中即提醒）"));
        gtk_label_set_xalign(GTK_LABEL(lb), 0);
        gtk_widget_add_css_class(lb, "hb-form-label");
        gtk_box_append(GTK_BOX(body), lb);
    }
    for (i = 0; i < 5; i++) {
        GtkWidget *sw;
        GtkWidget *r = switch_row(_(rows[i].title), _(rows[i].sub), &sw);
        gtk_switch_set_active(GTK_SWITCH(sw),
                              (d->ops.signals & rows[i].bit) != 0);
        g_object_set_data(G_OBJECT(sw), "bit",
                          GUINT_TO_POINTER(rows[i].bit));
        g_signal_connect(sw, "notify::active",
                         G_CALLBACK(on_sig_sw), d);
        gtk_box_append(GTK_BOX(body), r);
    }

    /* 自定义规则 */
    {
        GtkWidget *lb = gtk_label_new(
            _("自定义规则（正则，命中后需安静片刻确认）"));
        GtkWidget *sw = gtk_scrolled_window_new();
        GtkWidget *tv = gtk_text_view_new();
        GtkWidget *hint = gtk_label_new(
            _("每行一条：name: 正则 = 成功 · !name: 正则 = 失败 · # 开头为注释"));
        const char **v = NULL;
        size_t n = 0;
        GString *cur = g_string_new(NULL);
        size_t k;

        gtk_label_set_xalign(GTK_LABEL(lb), 0);
        gtk_widget_add_css_class(lb, "hb-form-label");
        gtk_box_append(GTK_BOX(body), lb);

        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
        d->rules_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
        o = watch_settings(d->ops.store);
        if (o) {
            n = hb_obj_get_strv(o, "watch_rules", &v);
            for (k = 0; k < n; k++) {
                g_string_append(cur, v[k]);
                g_string_append_c(cur, '\n');
            }
            hb_obj_free(o);
        }
        gtk_text_buffer_set_text(d->rules_buf, cur->str, -1);
        g_string_free(cur, TRUE);

        gtk_scrolled_window_set_min_content_height(
            GTK_SCROLLED_WINDOW(sw), 64);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
        gtk_widget_add_css_class(sw, "codebox");
        gtk_box_append(GTK_BOX(body), sw);

        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_widget_add_css_class(hint, "hb-dim");
        gtk_box_append(GTK_BOX(body), hint);

        d->rules_err = gtk_label_new("");
        gtk_widget_add_css_class(d->rules_err, "hb-red");
        gtk_label_set_xalign(GTK_LABEL(d->rules_err), 0);
        gtk_widget_set_visible(d->rules_err, FALSE);
        gtk_box_append(GTK_BOX(body), d->rules_err);

        g_signal_connect(d->rules_buf, "changed",
                         G_CALLBACK(on_rules_changed), d);
    }

    /* 全局开关 */
    {
        GtkWidget *sw1, *sw2;
        gboolean fdnd = TRUE, host_only = FALSE;
        o = watch_settings(d->ops.store);
        if (o) {
            fdnd = hb_obj_get_bool(o, "watch_focus_dnd", true);
            host_only = !g_strcmp0(hb_obj_get_str(o, "notify_body"),
                                   "host");
            hb_obj_free(o);
        }
        gtk_box_append(GTK_BOX(body),
            switch_row(_("聚焦本 Tab 时免打扰"),
                       _("仅更新角标，不弹系统通知"), &sw1));
        gtk_switch_set_active(GTK_SWITCH(sw1), fdnd);
        g_signal_connect(sw1, "notify::active",
                         G_CALLBACK(on_focus_sw), d);
        gtk_box_append(GTK_BOX(body),
            switch_row(_("通知正文仅显示主机名"),
                       _("隐藏命令与输出内容，防敏感外显"), &sw2));
        gtk_switch_set_active(GTK_SWITCH(sw2), host_only);
        g_signal_connect(sw2, "notify::active",
                         G_CALLBACK(on_priv_sw), d);
    }

    /* footer */
    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_hexpand(f, TRUE);
    btn = gtk_button_new_with_label(_("规则测试器"));
    gtk_widget_add_css_class(btn, "hb-btn");
    g_signal_connect(btn, "clicked", G_CALLBACK(open_tester), d);
    gtk_box_append(GTK_BOX(f), btn);
    btn = gtk_button_new_with_label(_("安装 shell 集成…"));
    gtk_widget_add_css_class(btn, "hb-btn");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_installer), d);
    gtk_box_append(GTK_BOX(f), btn);
    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(f), sp);
    btn = gtk_button_new_with_label(_("完成"));
    gtk_widget_add_css_class(btn, "hb-btn");
    gtk_widget_add_css_class(btn, "primary");
    g_signal_connect_swapped(btn, "clicked",
                             G_CALLBACK(adw_dialog_close), dlg);
    gtk_box_append(GTK_BOX(f), btn);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect_swapped(dlg, "closed", G_CALLBACK(wd_unref), d);
    adw_dialog_present(dlg, ops->page);
}
