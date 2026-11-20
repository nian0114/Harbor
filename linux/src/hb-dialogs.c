#include "hb-dialogs.h"
#include "hb-icons.h"
#include "hb-window.h"

/* ============================ 共用骨架 ============================ */

/* 白色模态：AdwDialog 直接承载竖排 box（GtkWindow 不能嵌进 widget）。
 * 布局：header / GtkScrolledWindow(body) / footer —— footer 固定在
 * 滚动区外钉底，内容超高时 body 收缩滚动，按钮永远不会被推出对话框。 */
static GtkWidget *modal_new(const char *title, int width,
                            GtkWidget **body_out, GtkWidget **foot_out)
{
    AdwDialog *dlg;
    GtkWidget *wrap, *hb, *title_l, *close_b, *body, *foot, *sw;

    dlg = adw_dialog_new();
    gtk_widget_add_css_class(GTK_WIDGET(dlg), "hb-modal");
    adw_dialog_set_content_width(dlg, width);
    adw_dialog_set_follows_content_size(dlg, TRUE);

    hb = gtk_header_bar_new();
    gtk_widget_add_css_class(hb, "hb-modal-header");
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), FALSE);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb), "");
    title_l = gtk_label_new(title);
    gtk_widget_add_css_class(title_l, "hb-modal-title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title_l);
    close_b = gtk_button_new();
    gtk_widget_add_css_class(close_b, "hb-modal-x");
    gtk_widget_add_css_class(close_b, "flat");
    gtk_button_set_child(GTK_BUTTON(close_b), hb_icon_new("x", 12));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), close_b);
    g_signal_connect_swapped(close_b, "clicked",
                             G_CALLBACK(adw_dialog_close), dlg);

    body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(body, 20);
    gtk_widget_set_margin_end(body, 20);
    gtk_widget_set_margin_top(body, 14);
    gtk_widget_set_margin_bottom(body, 16);

    sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(sw), 560);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), body);

    foot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(wrap), hb);
    gtk_box_append(GTK_BOX(wrap), sw);
    gtk_box_append(GTK_BOX(wrap), foot);
    adw_dialog_set_child(dlg, wrap);

    if (body_out) *body_out = body;
    if (foot_out) *foot_out = foot;
    return GTK_WIDGET(dlg);
}

static GtkWidget *hint_box(const char *icon, const char *text,
                           const char *cls)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l;
    gtk_widget_add_css_class(b, "hintbox");
    gtk_widget_add_css_class(b, cls);
    gtk_box_append(GTK_BOX(b), hb_icon_new(icon, 16));
    l = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(b), l);
    return b;
}

static void closure_str_free(gpointer data, GClosure *closure)
{ (void)closure; g_free(data); }

static void copy_to_clipboard(GtkButton *btn, gpointer text)
{
    GdkClipboard *cl = gtk_widget_get_clipboard(GTK_WIDGET(btn));
    (void)btn;
    gdk_clipboard_set_text(cl, text);
}

static GtkWidget *code_box(const char *text, gboolean copy_btn)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l;
    gtk_widget_add_css_class(b, "codebox");
    l = gtk_label_new(text);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "mono");
    gtk_widget_set_hexpand(l, TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_CHAR);
    gtk_box_append(GTK_BOX(b), l);
    if (copy_btn) {
        GtkWidget *cb = gtk_button_new_with_label("复制");
        gtk_widget_add_css_class(cb, "hb-btn-small");
        g_signal_connect_data(cb, "clicked",
            G_CALLBACK(copy_to_clipboard), g_strdup(text),
            closure_str_free, 0);
        gtk_box_append(GTK_BOX(b), cb);
    }
    return b;
}

static GtkWidget *footer_new(const char *cancel_txt, const char *ok_txt,
                             const char *ok_css, GtkWidget **ok_out)
{
    GtkWidget *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *c, *o;
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    c = gtk_button_new_with_label(cancel_txt);
    gtk_widget_add_css_class(c, "hb-btn");
    gtk_box_append(GTK_BOX(f), c);
    o = gtk_button_new_with_label(ok_txt);
    gtk_widget_add_css_class(o, "hb-btn");
    gtk_widget_add_css_class(o, ok_css);
    gtk_box_append(GTK_BOX(f), o);
    if (ok_out) *ok_out = o;
    return f;
}

/* ============================ TOFU ============================ */

typedef struct {
    hb_dialog_bool_cb cb;
    gpointer          ud;
    AdwDialog        *dlg;
    gboolean          accepted;
} TofuData;

static void tofu_cancel(GtkButton *b, TofuData *d)
{ (void)b; d->accepted = FALSE; adw_dialog_close(d->dlg); }
static void tofu_ok(GtkButton *b, TofuData *d)
{ (void)b; d->accepted = TRUE;  adw_dialog_close(d->dlg); }

static void tofu_closed(AdwDialog *dlg, TofuData *d)
{
    (void)dlg;
    if (d->cb) d->cb(d->accepted, d->ud);
    g_free(d);
}

AdwDialog *hb_dialog_tofu(GtkWindow *parent, const char *host_port,
                          const char *keytype, const char *fingerprint,
                          const char *randomart,
                          hb_dialog_bool_cb cb, gpointer ud)
{
    GtkWidget *dlg, *body, *foot, *row, *art, *f, *ok, *cancel;
    TofuData *d = g_new0(TofuData, 1);
    d->cb = cb;
    d->ud = ud;

    dlg = modal_new("首次连接 · 验证主机指纹", 470, &body, &foot);
    d->dlg = ADW_DIALOG(dlg);

    gtk_box_append(GTK_BOX(body), hint_box("shield",
        "该主机不在 known_hosts 中。请通过电话 / 控制台等带外渠道核对指纹后再信任，"
        "防止中间人攻击。", "amber"));

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(row, TRUE);
    gtk_box_append(GTK_BOX(row), code_box(host_port, FALSE));
    if (keytype && *keytype) {
        GtkWidget *tag = gtk_label_new(keytype);
        gtk_widget_add_css_class(tag, "hb-tag");
        gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), tag);
    }
    gtk_box_append(GTK_BOX(body), row);

    gtk_box_append(GTK_BOX(body), code_box(fingerprint, TRUE));

    if (randomart && *randomart) {
        art = gtk_label_new(randomart);
        gtk_widget_add_css_class(art, "fingerprint-art");
        gtk_label_set_selectable(GTK_LABEL(art), TRUE);
        gtk_label_set_xalign(GTK_LABEL(art), 0.5);
        gtk_box_append(GTK_BOX(body), art);
    }

    gtk_box_append(GTK_BOX(body), hint_box("check",
        "信任后写入 Known Hosts 并经 E2EE 同步。之后指纹若变更将红色告警并阻断连接。",
        "plain"));

    f = footer_new("取消", "信任并继续", "primary", &ok);
    cancel = gtk_widget_get_first_child(f);
    g_signal_connect(cancel, "clicked", G_CALLBACK(tofu_cancel), d);
    g_signal_connect(ok, "clicked", G_CALLBACK(tofu_ok), d);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(dlg, "closed", G_CALLBACK(tofu_closed), d);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
    return ADW_DIALOG(dlg);
}

/* ======================== hostkey changed ======================== */

typedef struct {
    hb_dialog_bool_cb cb;
    gpointer          ud;
    AdwDialog        *dlg;
    GtkWidget        *ok;
    gboolean          accepted;
} HkData;

static void hk_cancel(GtkButton *b, HkData *d)
{ (void)b; d->accepted = FALSE; adw_dialog_close(d->dlg); }
static void hk_ok(GtkButton *b, HkData *d)
{ (void)b; d->accepted = TRUE;  adw_dialog_close(d->dlg); }
static void hk_check_toggled(GtkCheckButton *ck, HkData *d)
{ gtk_widget_set_sensitive(d->ok, gtk_check_button_get_active(ck)); }

static void hk_closed(AdwDialog *dlg, HkData *d)
{
    (void)dlg;
    if (d->cb) d->cb(d->accepted, d->ud);
    g_free(d);
}

AdwDialog *hb_dialog_hostkey_changed(GtkWindow *parent,
                                     const char *host_port,
                                     const char *keytype,
                                     const char *old_fp, const char *new_fp,
                                     hb_dialog_bool_cb cb, gpointer ud)
{
    GtkWidget *dlg, *body, *foot, *grid, *l, *f, *cancel;
    GtkWidget *check;
    HkData *d = g_new0(HkData, 1);
    d->cb = cb;
    d->ud = ud;

    dlg = modal_new("警告：主机密钥已变更", 500, &body, &foot);
    d->dlg = ADW_DIALOG(dlg);

    gtk_box_append(GTK_BOX(body), hint_box("shield",
        "远程主机发来的密钥与本地记录不一致。这可能是系统重装或配置变更，"
        "也可能是中间人攻击。除非你已带外核实，否则应立即断开。", "red"));

    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_hexpand(row, TRUE);
        gtk_box_append(GTK_BOX(row), code_box(host_port, FALSE));
        if (keytype && *keytype) {
            GtkWidget *tag = gtk_label_new(keytype);
            gtk_widget_add_css_class(tag, "hb-tag");
            gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(row), tag);
        }
        gtk_box_append(GTK_BOX(body), row);
    }

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_hexpand(grid, TRUE);
    l = gtk_label_new("旧指纹"); gtk_widget_add_css_class(l, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), code_box(old_fp, FALSE), 1, 0, 1, 1);
    l = gtk_label_new("新指纹"); gtk_widget_add_css_class(l, "hb-dim");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), code_box(new_fp, FALSE), 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(body), grid);

    check = gtk_check_button_new_with_label("我了解风险");
    gtk_widget_add_css_class(check, "check-row");
    gtk_widget_set_halign(check, GTK_ALIGN_START);
    g_signal_connect(check, "toggled", G_CALLBACK(hk_check_toggled), d);
    gtk_box_append(GTK_BOX(body), check);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("断开连接");
    gtk_widget_add_css_class(cancel, "hb-btn");
    gtk_widget_add_css_class(cancel, "primary");
    g_signal_connect(cancel, "clicked", G_CALLBACK(hk_cancel), d);
    gtk_box_append(GTK_BOX(f), cancel);
    d->ok = gtk_button_new_with_label("我已核实，更新指纹并继续");
    gtk_widget_add_css_class(d->ok, "hb-btn");
    gtk_widget_add_css_class(d->ok, "danger");
    gtk_widget_set_sensitive(d->ok, FALSE);
    g_signal_connect(d->ok, "clicked", G_CALLBACK(hk_ok), d);
    gtk_box_append(GTK_BOX(f), d->ok);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(dlg, "closed", G_CALLBACK(hk_closed), d);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
    /* FR-09 acc2：Enter/默认动作必须是「断开连接」而非更新指纹 —
     * 焦点落在 cancel 上（ok 仍需勾选后才可用），Esc/关闭也走
     * accepted=FALSE 路径。 */
    gtk_widget_grab_focus(cancel);
    return ADW_DIALOG(dlg);
}

/* ============================ password ============================ */

typedef struct {
    hb_dialog_secret_cb cb;
    gpointer            ud;
    AdwDialog          *dlg;
    GtkWidget          *entry;
    GtkWidget          *remember;
    gboolean            done;
} PwData;

static void pw_deliver(PwData *d, const char *secret)
{
    if (d->done) return;
    d->done = TRUE;
    /* 复制给 cb，然后把 entry 抹掉（FR-09：不在内存里多留） */
    if (secret) {
        char *copy = g_strdup(secret);
        gboolean rem = d->remember &&
            gtk_check_button_get_active(GTK_CHECK_BUTTON(d->remember));
        gtk_editable_set_text(GTK_EDITABLE(d->entry), "");
        if (d->cb) d->cb(copy, rem, d->ud);
        g_free(copy);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(d->entry), "");
        if (d->cb) d->cb(NULL, FALSE, d->ud);
    }
}

static void pw_cancel(GtkButton *b, PwData *d)
{ (void)b; adw_dialog_close(d->dlg); }
static void pw_ok(GtkButton *b, PwData *d)
{
    (void)b;
    pw_deliver(d, gtk_editable_get_text(GTK_EDITABLE(d->entry)));
    adw_dialog_close(d->dlg);
}
static void pw_activate(GtkEditable *e, PwData *d)
{ (void)e; pw_ok(NULL, d); }

static void pw_closed(AdwDialog *dlg, PwData *d)
{
    (void)dlg;
    pw_deliver(d, NULL);   /* × 或 Esc = 取消 */
    g_free(d);
}

AdwDialog *hb_dialog_password(GtkWindow *parent, const char *prompt,
                              gboolean is_passphrase,
                              gboolean show_remember,
                              hb_dialog_secret_cb cb, gpointer ud)
{
    GtkWidget *dlg, *body, *foot, *f, *ok, *cancel;
    PwData *d = g_new0(PwData, 1);
    d->cb = cb;
    d->ud = ud;

    dlg = modal_new(is_passphrase ? "私钥密码" : "身份验证", 420, &body, &foot);
    d->dlg = ADW_DIALOG(dlg);

    {
        GtkWidget *l = gtk_label_new(prompt);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(body), l);
    }

    d->entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(d->entry), TRUE);
    g_signal_connect(d->entry, "activate", G_CALLBACK(pw_activate), d);
    gtk_box_append(GTK_BOX(body), d->entry);

    if (show_remember) {
        d->remember = gtk_check_button_new_with_label("记住（加密保存）");
        gtk_widget_add_css_class(d->remember, "check-row");
        gtk_widget_set_halign(d->remember, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(body), d->remember);
    }

    f = footer_new("取消", "连接", "primary", &ok);
    cancel = gtk_widget_get_first_child(f);
    g_signal_connect(cancel, "clicked", G_CALLBACK(pw_cancel), d);
    g_signal_connect(ok, "clicked", G_CALLBACK(pw_ok), d);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(dlg, "closed", G_CALLBACK(pw_closed), d);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
    gtk_widget_grab_focus(d->entry);
    return ADW_DIALOG(dlg);
}

/* ============================= toast ============================= */

static HbWindow *find_win(GtkWidget *w)
{
    GtkRoot *r = gtk_widget_get_root(w);
    return HB_IS_WINDOW(r) ? HB_WINDOW(r) : NULL;
}

void hb_toast(GtkWidget *any_widget, const char *text)
{
    HbWindow *w = find_win(any_widget);
    if (w) hb_window_toast(w, text);
}

void hb_toast_undo(GtkWidget *any_widget, const char *text,
                   hb_toast_undo_cb undo_cb, gpointer ud)
{
    HbWindow *w = find_win(any_widget);
    if (w) hb_window_toast_undo(w, text, (GCallback)undo_cb, ud);
}

void hb_toast_undo_destroy(GtkWidget *any_widget, const char *text,
                           hb_toast_undo_cb undo_cb,
                           GDestroyNotify destroy, gpointer ud)
{
    HbWindow *w = find_win(any_widget);
    if (w)
        hb_window_toast_action2(w, text, "撤销", 9,
                                (GCallback)undo_cb, destroy, ud);
}

/* ============================ 通用模态 ============================ */

AdwDialog *hb_modal_new(const char *title,
                        GtkWidget **body_out, GtkWidget **foot_out)
{
    return ADW_DIALOG(modal_new(title, 520, body_out, foot_out));
}

/* ------------------------------ confirm ------------------------------ */

typedef struct {
    hb_dialog_bool_cb cb;
    gpointer          ud;
    AdwDialog        *dlg;
    gboolean          accepted;
} CfmData;

static void cfm_cancel(GtkButton *b, CfmData *d)
{ (void)b; d->accepted = FALSE; adw_dialog_close(d->dlg); }
static void cfm_ok(GtkButton *b, CfmData *d)
{ (void)b; d->accepted = TRUE;  adw_dialog_close(d->dlg); }
static void cfm_closed(AdwDialog *dlg, CfmData *d)
{
    (void)dlg;
    if (d->cb) d->cb(d->accepted, d->ud);
    g_free(d);
}

/* 对话框的承载 widget：parent 尚未挂进窗口层级时（如新 tab 建页即刻
 * 连接 —— hb_ssh_page_new 在入树前跑 start_connect），退回应用活动
 * 窗口；否则 adw_dialog_present(NULL) 会创建一个独立顶层窗，无 WM
 * 时落在 (0,0) 被主窗遮住、不可见却吃掉所有输入。 */
static GtkWidget *dialog_parent(GtkWidget *parent)
{
    GtkRoot *r = parent ? gtk_widget_get_root(parent) : NULL;
    GApplication *app;
    GtkWindow *w;
    if (GTK_IS_WIDGET(r))
        return GTK_WIDGET(r);
    app = g_application_get_default();
    if (GTK_IS_APPLICATION(app) &&
        (w = gtk_application_get_active_window(GTK_APPLICATION(app))))
        return GTK_WIDGET(w);
    return NULL;
}

AdwDialog *hb_dialog_confirm(GtkWidget *parent, const char *title,
                             const char *markup, const char *ok_label,
                             gboolean danger,
                             hb_dialog_bool_cb cb, gpointer ud)
{
    GtkWidget *dlg, *body, *foot, *msg, *f, *ok, *cancel;
    CfmData *d = g_new0(CfmData, 1);
    d->cb = cb;
    d->ud = ud;

    dlg = modal_new(title, 420, &body, &foot);
    d->dlg = ADW_DIALOG(dlg);

    msg = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(msg), markup ? markup : "");
    gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
    gtk_label_set_xalign(GTK_LABEL(msg), 0);
    gtk_widget_add_css_class(msg, "hb-modal-msg");
    gtk_box_append(GTK_BOX(body), msg);

    f = footer_new("取消", ok_label, danger ? "danger" : "primary", &ok);
    cancel = gtk_widget_get_first_child(f);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cfm_cancel), d);
    g_signal_connect(ok, "clicked", G_CALLBACK(cfm_ok), d);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(dlg, "closed", G_CALLBACK(cfm_closed), d);
    adw_dialog_present(ADW_DIALOG(dlg), dialog_parent(parent));
    return ADW_DIALOG(dlg);
}

/* 只读纯文本信息框（导入警告等）：文本不解析 Pango markup，
 * 任何文件/vault 内容都能安全显示。 */
void hb_dialog_text(GtkWidget *parent, const char *title, const char *text)
{
    GtkWidget *dlg, *body, *foot, *msg, *f, *ok;

    dlg = modal_new(title, 420, &body, &foot);

    msg = gtk_label_new(text ? text : "");
    gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
    gtk_label_set_xalign(GTK_LABEL(msg), 0);
    gtk_label_set_selectable(GTK_LABEL(msg), TRUE);
    gtk_widget_add_css_class(msg, "hb-modal-msg");
    gtk_box_append(GTK_BOX(body), msg);

    f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f, "hb-modal-footer");
    gtk_widget_set_halign(f, GTK_ALIGN_END);
    ok = gtk_button_new_with_label("关闭");
    gtk_widget_add_css_class(ok, "hb-btn");
    gtk_widget_add_css_class(ok, "primary");
    gtk_box_append(GTK_BOX(f), ok);
    g_signal_connect_swapped(ok, "clicked",
                             G_CALLBACK(adw_dialog_close), dlg);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    adw_dialog_present(ADW_DIALOG(dlg), dialog_parent(parent));
}

/* ------------------------------ prompt ------------------------------ */

typedef struct {
    hb_dialog_text_cb cb;
    gpointer          ud;
    AdwDialog        *dlg;
    GtkWidget        *entry;
    char             *result;
} PrmData;

static void prm_deliver(PrmData *d, const char *text)
{
    d->result = g_strdup(text);
    gtk_editable_set_text(GTK_EDITABLE(d->entry), "");
    adw_dialog_close(d->dlg);
}
static void prm_cancel(GtkButton *b, PrmData *d)
{ (void)b; prm_deliver(d, NULL); }
static void prm_ok(GtkButton *b, PrmData *d)
{
    (void)b;
    prm_deliver(d, gtk_editable_get_text(GTK_EDITABLE(d->entry)));
}
static void prm_activate(GtkEntry *e, PrmData *d)
{ (void)e; prm_deliver(d, gtk_editable_get_text(GTK_EDITABLE(d->entry))); }
static void prm_closed(AdwDialog *dlg, PrmData *d)
{
    (void)dlg;
    if (d->cb) d->cb(d->result, d->ud);
    g_free(d->result);
    g_free(d);
}

void hb_dialog_prompt(GtkWidget *parent, const char *title,
                      const char *label, const char *initial,
                      hb_dialog_text_cb cb, gpointer ud)
{
    GtkWidget *dlg, *body, *foot, *l, *f, *ok, *cancel;
    PrmData *d = g_new0(PrmData, 1);
    d->cb = cb;
    d->ud = ud;

    dlg = modal_new(title, 400, &body, &foot);
    d->dlg = ADW_DIALOG(dlg);

    if (label) {
        l = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_add_css_class(l, "hb-form-label");
        gtk_box_append(GTK_BOX(body), l);
    }
    d->entry = gtk_entry_new();
    if (initial) gtk_editable_set_text(GTK_EDITABLE(d->entry), initial);
    g_signal_connect(d->entry, "activate", G_CALLBACK(prm_activate), d);
    gtk_box_append(GTK_BOX(body), d->entry);

    f = footer_new("取消", "确定", "primary", &ok);
    cancel = gtk_widget_get_first_child(f);
    g_signal_connect(cancel, "clicked", G_CALLBACK(prm_cancel), d);
    g_signal_connect(ok, "clicked", G_CALLBACK(prm_ok), d);
    gtk_widget_set_margin_start(f, 20);
    gtk_widget_set_margin_end(f, 20);
    gtk_widget_set_margin_top(f, 4);
    gtk_widget_set_margin_bottom(f, 16);
    gtk_box_append(GTK_BOX(foot), f);

    g_signal_connect(dlg, "closed", G_CALLBACK(prm_closed), d);
    adw_dialog_present(ADW_DIALOG(dlg), dialog_parent(parent));
    gtk_widget_grab_focus(d->entry);
}
