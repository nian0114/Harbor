/* 外观与终端设置页（FR-21 / FR-15）：左列主题+字体等控件，右列真实
 * HbTerminal 预览，底部快捷键只读表（来自 hb-shortcuts 单一表）。
 * 控件即时应用：预览立即更新，settings 对象 ~150ms 防抖写回 vault，
 * 打开的终端页经 HbStore "changed" 自行重挂。 */
#include "hb-settings-view.h"
#include "hb-store.h"
#include "hb-icons.h"
#include "hb-shortcuts.h"
#include "hb-terminal.h"
#include "hb-term-theme.h"
#include "hb-application.h"
#include "hb-login.h"
#include "hb-dialogs.h"

#include <adwaita.h>
#include <sodium.h>
#include <string.h>

#include "harbor/backup.h"

#define SAVE_DEBOUNCE_MS 150

/* 主题卡片顺序/标签；id 对应 hb-term-theme.c THEMES 表 */
static const struct { const char *id, *label; } THEME_CARDS[] = {
    { "harbor",    "Harbor Dark" },
    { "ubuntu",    "Ubuntu"      },
    { "solarized", "Solarized"   },
    { "light",     "Paper Light" },
};
#define N_THEMES (sizeof(THEME_CARDS) / sizeof(THEME_CARDS[0]))

static const int SB_CHOICES[] = { 1000, 5000, 10000, 50000, 100000 };
#define N_SB (sizeof(SB_CHOICES) / sizeof(SB_CHOICES[0]))

/* 预览内容：真实 SGR —— dim(2) / 绿(32) / 蓝(34) 让主题 ANSI 调色板显形 */
static const char PREVIEW_ANSI[] =
    "\033[2mWelcome to Ubuntu 24.04.4 LTS\033[0m\r\n"
    "\033[32mnian@10.88.2.11\033[0m:\033[34m~\033[0m$ \033[1mai-task\033[0m\r\n"
    "\033[2m\xe2\xa0\xb8 Applying edits to src/server.c \xe2\x80\xa6\033[0m\r\n"
    "\033[32m\xe2\x9c\x94 Task completed \xe2\x80\x94 "
    "3 files changed, tests green.\033[0m\r\n"
    "\033[32mnian@10.88.2.11\033[0m:\033[34m~\033[0m$ ";

struct _HbSettingsView {
    GtkBox parent;

    HbStore     *store;
    gulong       changed_handler;
    gboolean     updating;      /* refresh 回填中控件信号不再触发保存 */
    guint        save_src;

    GtkWidget   *cards[N_THEMES];
    GtkWidget   *font_scale, *lh_scale;
    GtkWidget   *font_lbl, *lh_lbl;
    GtkWidget   *family_dd, *sb_dd;
    GtkWidget   *blink_sw, *osc52_sw;
    GtkWidget   *tray_sw, *login_sw;
    GtkWidget   *ui_dd;         /* 外观：浅色/深色/跟随系统 */
    GPtrArray   *sc_rows;       /* ScRow*，快捷键编辑行 */
    GtkWidget   *sftp_spin;
    GtkWidget   *pv_view;       /* .hb-term-view.theme-* 容器 */
    GtkWidget   *pv_term;       /* HbTerminal（预览模式） */
    char        *pv_theme_id;   /* 当前挂在 pv_view 上的 theme-* 类 */
};

G_DEFINE_TYPE(HbSettingsView, hb_settings_view, GTK_TYPE_BOX)

/* ------------------------------------------------------------------ */
/* 主题卡片预览色：一次性生成 .tc-* 规则（每主题 3 条）                    */
/* ------------------------------------------------------------------ */

static void install_card_css(void)
{
    static gboolean done;
    GtkCssProvider *prov;
    GString *s;
    size_t i;

    if (done) return;
    done = TRUE;
    s = g_string_new(NULL);
    for (i = 0; i < N_THEMES; i++) {
        const HbTermTheme *t = hb_term_theme_get(THEME_CARDS[i].id);
        char bg[8], fg[8], dim[8];
        g_snprintf(bg, sizeof bg, "#%02x%02x%02x",
            (int)(t->bg.red * 255 + .5), (int)(t->bg.green * 255 + .5),
            (int)(t->bg.blue * 255 + .5));
        g_snprintf(fg, sizeof fg, "#%02x%02x%02x",
            (int)(t->fg.red * 255 + .5), (int)(t->fg.green * 255 + .5),
            (int)(t->fg.blue * 255 + .5));
        g_snprintf(dim, sizeof dim, "#%02x%02x%02x",
            (int)(t->dim.red * 255 + .5), (int)(t->dim.green * 255 + .5),
            (int)(t->dim.blue * 255 + .5));
        g_string_append_printf(s,
            ".tc-prev-%s { background-color: %s; }\n"
            ".tc-fg-%s { color: %s; }\n"
            ".tc-dim-%s { color: %s; }\n",
            THEME_CARDS[i].id, bg,
            THEME_CARDS[i].id, fg,
            THEME_CARDS[i].id, dim);
    }
    prov = gtk_css_provider_new();
    gtk_css_provider_load_from_string(prov, s->str);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
    g_string_free(s, TRUE);
}

/* ------------------------------------------------------------------ */
/* 当前控件值 → 预览 + 防抖保存                                          */
/* ------------------------------------------------------------------ */

static const char *cur_theme_id(HbSettingsView *v)
{
    size_t i;
    for (i = 0; i < N_THEMES; i++)
        if (gtk_widget_has_css_class(v->cards[i], "on"))
            return THEME_CARDS[i].id;
    return "harbor";
}

static const char *cur_family(HbSettingsView *v)
{
    GtkStringList *m = GTK_STRING_LIST(
        gtk_drop_down_get_model(GTK_DROP_DOWN(v->family_dd)));
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(v->family_dd));
    const char *s = sel == GTK_INVALID_LIST_POSITION ? "Monospace"
        : gtk_string_list_get_string(m, sel);
    return s ? s : "Monospace";
}

static int cur_scrollback(HbSettingsView *v)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(v->sb_dd));
    return sel < N_SB ? SB_CHOICES[sel] : 10000;
}

static void apply_preview(HbSettingsView *v)
{
    const char *tid = cur_theme_id(v);
    char *cls;
    double fs = gtk_range_get_value(GTK_RANGE(v->font_scale));
    double lh = gtk_range_get_value(GTK_RANGE(v->lh_scale));

    if (v->pv_theme_id)
        gtk_widget_remove_css_class(v->pv_view, v->pv_theme_id);
    cls = g_strdup_printf("theme-%s", tid);
    gtk_widget_add_css_class(v->pv_view, cls);
    g_free(v->pv_theme_id);
    v->pv_theme_id = cls;

    hb_terminal_set_theme(HB_TERMINAL(v->pv_term),
                          hb_term_theme_get(tid));
    hb_terminal_set_font(HB_TERMINAL(v->pv_term),
                         cur_family(v), fs, lh);
    hb_terminal_set_cursor_blink(HB_TERMINAL(v->pv_term),
        gtk_switch_get_active(GTK_SWITCH(v->blink_sw)));
    hb_terminal_set_osc52(HB_TERMINAL(v->pv_term),
        gtk_switch_get_active(GTK_SWITCH(v->osc52_sw)));
}

static const char *cur_ui_theme(const HbSettingsView *v);
static void sc_refresh_all(HbSettingsView *v);

static gboolean save_cb(gpointer ud)
{
    HbSettingsView *v = ud;
    hb_obj *o = NULL;

    v->save_src = 0;
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        hb_obj_set_str(o, "theme", cur_theme_id(v));
        hb_obj_set_int(o, "font_size",
            (int64_t)gtk_range_get_value(GTK_RANGE(v->font_scale)));
        hb_obj_set_double(o, "line_height",
            gtk_range_get_value(GTK_RANGE(v->lh_scale)));
        hb_obj_set_str(o, "font_family", cur_family(v));
        hb_obj_set_int(o, "scrollback", cur_scrollback(v));
        hb_obj_set_bool(o, "cursor_blink",
            gtk_switch_get_active(GTK_SWITCH(v->blink_sw)));
        hb_obj_set_bool(o, "osc52",
            gtk_switch_get_active(GTK_SWITCH(v->osc52_sw)));
        hb_obj_set_int(o, "sftp_concurrency",
            (int64_t)gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(v->sftp_spin)));
        hb_obj_set_bool(o, "close_to_tray",
            gtk_switch_get_active(GTK_SWITCH(v->tray_sw)));
        hb_obj_set_bool(o, "start_at_login",
            gtk_switch_get_active(GTK_SWITCH(v->login_sw)));
        hb_obj_set_str(o, "ui_theme", cur_ui_theme(v));
        hb_vault_put(hb_store_vault(v->store), o);
        hb_obj_free(o);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_save(HbSettingsView *v)
{
    if (v->updating)
        return;
    if (!v->save_src)
        v->save_src = g_timeout_add(SAVE_DEBOUNCE_MS, save_cb, v);
}

static const char *const UI_THEME_IDS[] = { "light", "dark", "system" };

static const char *cur_ui_theme(const HbSettingsView *v)
{
    guint s = gtk_drop_down_get_selected(GTK_DROP_DOWN(v->ui_dd));
    return s < G_N_ELEMENTS(UI_THEME_IDS) ? UI_THEME_IDS[s] : "light";
}

static void on_ui_dd(GtkDropDown *dd, GParamSpec *ps, gpointer ud)
{
    HbSettingsView *v = ud;
    (void)dd; (void)ps;
    save_cb(v);                      /* 先落库，apply_theme 读得到新值 */
    hb_application_apply_theme();
}

/* ------------------------------------------------------------------ */
/* 控件回调                                                            */
/* ------------------------------------------------------------------ */

static void on_theme_card(GtkButton *b, gpointer ud)
{
    HbSettingsView *v = ud;
    const char *id = g_object_get_data(G_OBJECT(b), "tid");
    size_t i;
    for (i = 0; i < N_THEMES; i++) {
        if (!strcmp(THEME_CARDS[i].id, id))
            gtk_widget_add_css_class(v->cards[i], "on");
        else
            gtk_widget_remove_css_class(v->cards[i], "on");
    }
    apply_preview(v);
    schedule_save(v);
}

static void on_font_scale(GtkRange *r, gpointer ud)
{
    HbSettingsView *v = ud;
    char s[24];
    g_snprintf(s, sizeof s, "%d px",
               (int)gtk_range_get_value(r));
    gtk_label_set_label(GTK_LABEL(v->font_lbl), s);
    apply_preview(v);
    schedule_save(v);
}

static void on_lh_scale(GtkRange *r, gpointer ud)
{
    HbSettingsView *v = ud;
    char s[16];
    g_snprintf(s, sizeof s, "%.1f", gtk_range_get_value(r));
    gtk_label_set_label(GTK_LABEL(v->lh_lbl), s);
    apply_preview(v);
    schedule_save(v);
}

static void on_dd_changed(GObject *dd, GParamSpec *ps, gpointer ud)
{
    HbSettingsView *v = ud;
    (void)dd; (void)ps;
    apply_preview(v);
    schedule_save(v);
}

static void on_sw_changed(GObject *sw, GParamSpec *ps, gpointer ud)
{
    HbSettingsView *v = ud;
    (void)sw; (void)ps;
    apply_preview(v);
    schedule_save(v);
}

static void on_spin_changed(GtkSpinButton *sp, gpointer ud)
{
    HbSettingsView *v = ud;
    (void)sp;
    schedule_save(v);
}

static void on_reset(GtkButton *b, gpointer ud)
{
    HbSettingsView *v = ud;
    size_t i;
    (void)b;

    v->updating = TRUE;
    for (i = 0; i < N_THEMES; i++) {
        if (!strcmp(THEME_CARDS[i].id, "harbor"))
            gtk_widget_add_css_class(v->cards[i], "on");
        else
            gtk_widget_remove_css_class(v->cards[i], "on");
    }
    gtk_range_set_value(GTK_RANGE(v->font_scale), 13);
    gtk_range_set_value(GTK_RANGE(v->lh_scale), 1.5);
    gtk_label_set_label(GTK_LABEL(v->font_lbl), "13 px");
    gtk_label_set_label(GTK_LABEL(v->lh_lbl), "1.5");
    /* 字体族 → Monospace（建列表时保证它存在）；scrollback → 10000 */
    {
        GtkStringList *m = GTK_STRING_LIST(
            gtk_drop_down_get_model(GTK_DROP_DOWN(v->family_dd)));
        guint n = g_list_model_get_n_items(G_LIST_MODEL(m)), k;
        for (k = 0; k < n; k++) {
            if (!strcmp(gtk_string_list_get_string(m, k), "Monospace")) {
                gtk_drop_down_set_selected(
                    GTK_DROP_DOWN(v->family_dd), k);
                break;
            }
        }
    }
    for (i = 0; i < N_SB; i++)
        if (SB_CHOICES[i] == 10000)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(v->sb_dd),
                                       (guint)i);
    gtk_switch_set_active(GTK_SWITCH(v->blink_sw), TRUE);
    gtk_switch_set_active(GTK_SWITCH(v->osc52_sw), FALSE);
    v->updating = FALSE;
    sc_refresh_all(v);

    apply_preview(v);
    schedule_save(v);
}

/* ------------------------------------------------------------------ */
/* settings 回填（含外部改动）                                            */
/* ------------------------------------------------------------------ */

static void refresh(HbSettingsView *v)
{
    hb_obj *o = NULL;
    const char *theme = "harbor", *family = "Monospace";
    const char *uit = "light";
    double fs = 13, lh = 1.5;
    int sb = 10000;
    gboolean blink = TRUE, osc52 = FALSE;
    gboolean ctt = TRUE, sal = FALSE;
    size_t i;

    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        theme = hb_obj_get_str(o, "theme");
        family = hb_obj_get_str(o, "font_family");
        fs = (double)hb_obj_get_int(o, "font_size", 13);
        lh = hb_obj_get_double(o, "line_height", 1.5);
        sb = (int)hb_obj_get_int(o, "scrollback", 10000);
        blink = hb_obj_get_bool(o, "cursor_blink", true);
        osc52 = hb_obj_get_bool(o, "osc52", false);
        ctt = hb_obj_get_bool(o, "close_to_tray", true);
        sal = hb_obj_get_bool(o, "start_at_login", false);
        uit = hb_obj_get_str(o, "ui_theme");
    }
    if (!uit || !*uit) uit = "light";
    if (!theme || !*theme) theme = "harbor";
    if (!family || !*family) family = "Monospace";

    v->updating = TRUE;
    for (i = 0; i < N_THEMES; i++) {
        if (!strcmp(THEME_CARDS[i].id, theme))
            gtk_widget_add_css_class(v->cards[i], "on");
        else
            gtk_widget_remove_css_class(v->cards[i], "on");
    }
    gtk_range_set_value(GTK_RANGE(v->font_scale), CLAMP(fs, 10, 20));
    gtk_range_set_value(GTK_RANGE(v->lh_scale), CLAMP(lh, 1.2, 2.0));
    {
        char s[24];
        g_snprintf(s, sizeof s, "%d px", (int)fs);
        gtk_label_set_label(GTK_LABEL(v->font_lbl), s);
        g_snprintf(s, sizeof s, "%.1f", lh);
        gtk_label_set_label(GTK_LABEL(v->lh_lbl), s);
    }
    {
        GtkStringList *m = GTK_STRING_LIST(
            gtk_drop_down_get_model(GTK_DROP_DOWN(v->family_dd)));
        guint n = g_list_model_get_n_items(G_LIST_MODEL(m)), k;
        for (k = 0; k < n; k++) {
            if (!strcmp(gtk_string_list_get_string(m, k), family)) {
                gtk_drop_down_set_selected(
                    GTK_DROP_DOWN(v->family_dd), k);
                break;
            }
        }
        /* 字体已卸载 → 退回 Monospace */
        if (k == n) {
            for (k = 0; k < n; k++)
                if (!strcmp(gtk_string_list_get_string(m, k),
                            "Monospace")) {
                    gtk_drop_down_set_selected(
                        GTK_DROP_DOWN(v->family_dd), k);
                    break;
                }
        }
    }
    for (i = 0; i < N_SB; i++)
        if (SB_CHOICES[i] == sb)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(v->sb_dd),
                                       (guint)i);
    gtk_switch_set_active(GTK_SWITCH(v->blink_sw), blink);
    gtk_switch_set_active(GTK_SWITCH(v->osc52_sw), osc52);
    gtk_switch_set_active(GTK_SWITCH(v->tray_sw), ctt);
    gtk_switch_set_active(GTK_SWITCH(v->login_sw), sal);
    {
        guint sel = 0;
        for (i = 0; i < G_N_ELEMENTS(UI_THEME_IDS); i++)
            if (!strcmp(UI_THEME_IDS[i], uit)) sel = (guint)i;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(v->ui_dd), sel);
    }
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(v->sftp_spin),
        CLAMP((double)hb_obj_get_int(o, "sftp_concurrency", 3), 1, 8));
    v->updating = FALSE;
    sc_refresh_all(v);

    apply_preview(v);
    if (o) hb_obj_free(o);
}

static void on_store_changed(HbStore *st, const char *type,
                             const char *id, gpointer ud)
{
    (void)st; (void)id;
    if (!strcmp(type, "settings"))
        refresh(HB_SETTINGS_VIEW(ud));
}

/* ------------------------------------------------------------------ */
/* 构建                                                                */
/* ------------------------------------------------------------------ */

static GtkWidget *panel_head(const char *icon, const char *title,
                             const char *sub)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *s;
    GtkWidget *hd = gtk_label_new(NULL);
    char *mk = g_strdup_printf("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(hd), mk);
    g_free(mk);
    gtk_widget_add_css_class(hd, "set-h4");
    gtk_label_set_xalign(GTK_LABEL(hd), 0);
    gtk_box_append(GTK_BOX(h), hb_icon_new(icon, 15));
    gtk_box_append(GTK_BOX(h), hd);
    s = gtk_label_new(sub);
    gtk_widget_add_css_class(s, "p-sub");
    gtk_label_set_xalign(GTK_LABEL(s), 0);
    gtk_label_set_wrap(GTK_LABEL(s), TRUE);
    {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(col), h);
        gtk_box_append(GTK_BOX(col), s);
        return col;
    }
}

static GtkWidget *theme_card_new(const char *id, const char *label,
                                 HbSettingsView *v)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *prev = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *l1, *l2, *name;
    char *cls;

    gtk_widget_add_css_class(btn, "theme-card");
    gtk_widget_set_hexpand(btn, TRUE);

    cls = g_strdup_printf("tc-prev-%s", id);
    gtk_widget_add_css_class(prev, "theme-prev");
    gtk_widget_add_css_class(prev, cls);
    g_free(cls);
    l1 = gtk_label_new("nian@host:~$ ai-task");
    cls = g_strdup_printf("tc-fg-%s", id);
    gtk_widget_add_css_class(l1, cls);
    g_free(cls);
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    gtk_box_append(GTK_BOX(prev), l1);
    l2 = gtk_label_new("✔ Task completed (6.1s)");
    cls = g_strdup_printf("tc-dim-%s", id);
    gtk_widget_add_css_class(l2, cls);
    g_free(cls);
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_box_append(GTK_BOX(prev), l2);
    gtk_box_append(GTK_BOX(col), prev);

    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *ck;
        name = gtk_label_new(label);
        gtk_widget_add_css_class(name, "tc-name");
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(row), name);
        ck = hb_icon_new("check", 13);
        gtk_widget_add_css_class(ck, "tc-check");
        gtk_box_append(GTK_BOX(row), ck);
        gtk_box_append(GTK_BOX(col), row);
    }
    gtk_button_set_child(GTK_BUTTON(btn), col);
    g_object_set_data(G_OBJECT(btn), "tid", (gpointer)id);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_theme_card), v);
    return btn;
}

static GtkWidget *slider_row(const char *title, GtkWidget **scale_out,
                             GtkWidget **val_out, double lo, double hi,
                             double step, GCallback cb,
                             HbSettingsView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *t = gtk_label_new(title);
    GtkWidget *val = gtk_label_new("");
    GtkWidget *sc = gtk_scale_new_with_range(
        GTK_ORIENTATION_HORIZONTAL, lo, hi, step);

    gtk_widget_add_css_class(box, "slider-row");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_widget_add_css_class(t, "sl-title");
    gtk_box_append(GTK_BOX(row), t);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_widget_add_css_class(val, "sl-val");
    gtk_box_append(GTK_BOX(row), val);
    gtk_box_append(GTK_BOX(box), row);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_widget_set_hexpand(sc, TRUE);
    g_signal_connect(sc, "value-changed", cb, v);
    gtk_box_append(GTK_BOX(box), sc);
    *scale_out = sc;
    *val_out = val;
    return box;
}

static GtkWidget *switch_row(const char *title, const char *sub,
                             GtkWidget **sw_out, GCallback cb,
                             HbSettingsView *v)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    GtkWidget *sw = gtk_switch_new();

    gtk_widget_add_css_class(row, "check-row");
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
    if (cb)
        g_signal_connect(sw, "notify::active", cb, v);
    gtk_box_append(GTK_BOX(row), sw);
    *sw_out = sw;
    return row;
}

static GtkWidget *build_preview(HbSettingsView *v)
{
    GtkWidget *view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *mono, *tt, *sp, *pill, *ov, *sb, *ok, *mt;

    /* 与真实终端页同 chrome：.hb-term-view.theme-* 挂在容器上 */
    gtk_widget_add_css_class(view, "hb-term-view");
    gtk_widget_add_css_class(view, "pref-preview");

    gtk_widget_add_css_class(tb, "hb-term-toolbar");
    mono = hb_mono_icon_new(">_");
    gtk_widget_add_css_class(mono, "hb-term-tt");
    gtk_box_append(GTK_BOX(tb), mono);
    tt = gtk_label_new("ssh nian@10.88.2.11");
    gtk_widget_add_css_class(tt, "hb-term-tt");
    gtk_label_set_xalign(GTK_LABEL(tt), 0);
    gtk_box_append(GTK_BOX(tb), tt);
    sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(tb), sp);
    pill = gtk_button_new();
    gtk_widget_add_css_class(pill, "tbtn");
    gtk_widget_set_sensitive(pill, FALSE);
    {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_append(GTK_BOX(r), hb_icon_new("eye", 12));
        gtk_box_append(GTK_BOX(r), gtk_label_new("监控 开"));
        gtk_button_set_child(GTK_BUTTON(pill), r);
    }
    gtk_box_append(GTK_BOX(tb), pill);
    gtk_box_append(GTK_BOX(view), tb);

    ov = gtk_overlay_new();
    gtk_widget_set_vexpand(ov, TRUE);
    v->pv_term = hb_terminal_new();
    gtk_widget_set_vexpand(v->pv_term, TRUE);
    gtk_widget_set_hexpand(v->pv_term, TRUE);
    gtk_widget_set_focusable(v->pv_term, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(ov), v->pv_term);
    sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
        hb_terminal_scroll_adjustment(HB_TERMINAL(v->pv_term)));
    gtk_widget_add_css_class(sb, "hb-term-sb");
    gtk_widget_set_halign(sb, GTK_ALIGN_END);
    gtk_widget_set_valign(sb, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), sb);
    gtk_box_append(GTK_BOX(view), ov);

    gtk_widget_add_css_class(st, "hb-term-status");
    ok = gtk_label_new("● 已连接");
    gtk_widget_add_css_class(ok, "hb-term-ok");
    gtk_box_append(GTK_BOX(st), ok);
    mt = gtk_label_new("xterm-256color");
    gtk_widget_add_css_class(mt, "hb-term-muted");
    gtk_box_append(GTK_BOX(st), mt);
    {
        GtkWidget *sp2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp2, TRUE);
        gtk_box_append(GTK_BOX(st), sp2);
    }
    mt = gtk_label_new("scrollback 10,000 行");
    gtk_box_append(GTK_BOX(st), mt);
    gtk_box_append(GTK_BOX(view), st);

    hb_terminal_set_preview(HB_TERMINAL(v->pv_term), PREVIEW_ANSI);
    v->pv_view = view;
    return view;
}

/* ------------------------------------------------------------------ */
/* 快捷键编辑                                                          */
/* ------------------------------------------------------------------ */

typedef struct ScRow_ ScRow;
static void sc_commit(ScRow *r, const char *accel);
static void sc_row_update(ScRow *r);
static void sc_refresh_all(HbSettingsView *v);
static GtkWidget *build_backup_panel(HbSettingsView *v);

struct ScRow_ {
    HbSettingsView *v;
    size_t          idx;
    GtkWidget      *btn;      /* 显示/捕获按键的按钮 */
    GtkWidget      *rst;      /* ↺ 恢复默认 */
    gboolean        capturing;
};

static void sc_row_update(ScRow *r)
{
    hb_obj *o = NULL;
    guint k = 0;
    GdkModifierType m = 0;
    char *lbl;

    if (hb_vault_settings(hb_store_vault(r->v->store), &o) == HB_OK && o)
        hb_shortcuts_effective_row(o, r->idx, &k, &m);
    lbl = hb_shortcut_accel_label(k, m);
    gtk_button_set_label(GTK_BUTTON(r->btn), lbl);
    g_free(lbl);
    hb_obj_free(o);
}

/* 捕获态按键：Esc 取消；Backspace/Delete 禁用；其他→新绑定 */
static gboolean sc_capture_key(GtkEventControllerKey *kc, guint keyval,
                               guint code, GdkModifierType st, gpointer ud)
{
    ScRow *r = ud;
    (void)kc; (void)code;

    if (!r->capturing)
        return FALSE;
    if (keyval == GDK_KEY_Escape) {
        r->capturing = FALSE;
        sc_row_update(r);
        return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete) {
        sc_commit(r, "");
        return TRUE;
    }
    /* 忽略纯修饰键 */
    if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R ||
        keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R ||
        keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
        keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R ||
        keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R)
        return TRUE;
    {
        GdkModifierType m = st & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                  GDK_ALT_MASK);
        guint k = gdk_keyval_to_lower(keyval);
        char *accel = gtk_accelerator_name(k, m);
        sc_commit(r, accel);
        g_free(accel);
    }
    return TRUE;
}

/* 应用新绑定：冲突 → 对方解绑；终端常用 Ctrl+字母 → 警告但允许 */
static void sc_commit(ScRow *r, const char *accel)
{
    HbSettingsView *v = r->v;
    hb_obj *o = NULL;
    guint k = 0;
    GdkModifierType m = 0;
    int conf;

    r->capturing = FALSE;
    if (!hb_shortcut_accel_parse(accel, &k, &m)) {
        hb_toast(GTK_WIDGET(v), "无效的快捷键");
        sc_row_update(r);
        return;
    }
    if (hb_vault_settings(hb_store_vault(v->store), &o) != HB_OK || !o) {
        sc_row_update(r);
        return;
    }
    conf = hb_shortcut_conflict(o, r->idx, k, m);
    if (conf >= 0) {
        const HbShortcut *sc = hb_shortcuts(NULL);
        hb_shortcuts_apply_override(o, (size_t)conf, "");
        {
            char *m2 = g_strdup_printf("与「%s」冲突，已解绑对方",
                                       sc[conf].label);
            hb_toast(GTK_WIDGET(v), m2);
            g_free(m2);
        }
    }
    if (k && m == GDK_CONTROL_MASK &&
        k >= GDK_KEY_a && k <= GDK_KEY_z)
        hb_toast(GTK_WIDGET(v),
                 "警告：Ctrl+字母在终端里常是 shell 控制键");
    hb_shortcuts_apply_override(o, r->idx, accel);
    hb_vault_put(hb_store_vault(v->store), o);
    hb_obj_free(o);
    sc_refresh_all(v);
}

static void sc_reset_row(ScRow *r)
{
    hb_obj *o = NULL;
    if (hb_vault_settings(hb_store_vault(r->v->store), &o) == HB_OK && o) {
        hb_shortcuts_apply_override(o, r->idx, NULL);
        hb_vault_put(hb_store_vault(r->v->store), o);
        hb_obj_free(o);
        sc_refresh_all(r->v);
    }
}

static void on_sc_reset_all(GtkButton *b, gpointer ud)
{
    HbSettingsView *v = ud;
    hb_obj *o = NULL;
    (void)b;
    if (hb_vault_settings(hb_store_vault(v->store), &o) == HB_OK && o) {
        hb_shortcuts_reset_all(o);
        hb_vault_put(hb_store_vault(v->store), o);
        hb_obj_free(o);
        sc_refresh_all(v);
    }
}

static void on_sc_btn(GtkButton *b, gpointer ud)
{
    ScRow *r = ud;
    (void)b;
    r->capturing = TRUE;
    gtk_button_set_label(GTK_BUTTON(r->btn),
                         "按下新的快捷键…（Esc 取消，Backspace 禁用）");
    gtk_widget_grab_focus(r->btn);
}

static void sc_refresh_all(HbSettingsView *v)
{
    if (v->sc_rows)
        for (guint i = 0; i < v->sc_rows->len; i++)
            sc_row_update(g_ptr_array_index(v->sc_rows, i));
}

static GtkWidget *build_shortcuts_panel(HbSettingsView *v)
{
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *grid, *rst_all;
    const HbShortcut *sc;
    size_t n, i;
    int row = 0;

    gtk_widget_add_css_class(panel, "panel");
    gtk_box_append(GTK_BOX(panel),
        panel_head("terminal", "快捷键",
                   "点击按键编辑；Backspace 禁用，↺ 恢复默认。"
                   "Alt/Ctrl+1…9 家族固定。"));
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    sc = hb_shortcuts(&n);
    if (!v->sc_rows)
        v->sc_rows = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < n; i++) {
        GtkWidget *l = gtk_label_new(sc[i].label);
        ScRow *r = g_new0(ScRow, 1);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        r->v = v;
        r->idx = i;
        r->btn = gtk_button_new();
        gtk_widget_add_css_class(r->btn, "kbd");
        gtk_widget_set_halign(r->btn, GTK_ALIGN_END);
        {
            GtkEventController *kc = gtk_event_controller_key_new();
            g_signal_connect(kc, "key-pressed",
                             G_CALLBACK(sc_capture_key), r);
            gtk_widget_add_controller(r->btn, kc);
        }
        if (!hb_shortcut_editable(i)) {
            gtk_widget_set_sensitive(r->btn, FALSE);
            {
                guint k = sc[i].keyval;
                GdkModifierType m = sc[i].mods;
                char *lbl = hb_shortcut_accel_label(k, m);
                /* tab-n 行展示原文案（Alt+1…9） */
                g_free(lbl);
                gtk_button_set_label(GTK_BUTTON(r->btn), sc[i].accel);
            }
        }
        g_signal_connect(r->btn, "clicked",
                         G_CALLBACK(on_sc_btn), r);
        g_ptr_array_add(v->sc_rows, r);
        if (hb_shortcut_editable(i))
            sc_row_update(r);
        gtk_grid_attach(GTK_GRID(grid), l, (int)(row % 2) * 3,
                        row / 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), r->btn,
                        (int)(row % 2) * 3 + 1, row / 2, 1, 1);
        r->rst = gtk_button_new_from_icon_name("view-refresh-symbolic");
        gtk_widget_add_css_class(r->rst, "flat");
        gtk_widget_set_tooltip_text(r->rst, "恢复默认");
        g_signal_connect_swapped(r->rst, "clicked",
                                 G_CALLBACK(sc_reset_row), r);
        gtk_grid_attach(GTK_GRID(grid), r->rst,
                        (int)(row % 2) * 3 + 2, row / 2, 1, 1);
        row++;
    }
    gtk_box_append(GTK_BOX(panel), grid);
    rst_all = gtk_button_new_with_label("全部恢复默认");
    gtk_widget_set_halign(rst_all, GTK_ALIGN_END);
    g_signal_connect(rst_all, "clicked",
                     G_CALLBACK(on_sc_reset_all), v);
    gtk_box_append(GTK_BOX(panel), rst_all);
    return panel;
}

/* start_at_login 开关：立即写/删 user unit；失败则回退开关。 */
static void on_login_sw(GObject *sw, GParamSpec *ps, gpointer ud)
{
    HbSettingsView *v = ud;
    GError *err = NULL;
    gboolean want = gtk_switch_get_active(GTK_SWITCH(sw));
    (void)ps;
    if (v->updating)
        return;
    if (!hb_login_set_enabled(want, &err)) {
        v->updating = TRUE;
        gtk_switch_set_active(GTK_SWITCH(sw), FALSE);
        v->updating = FALSE;
        hb_toast(GTK_WIDGET(v), err ? err->message : "写入自启动项失败");
        g_clear_error(&err);
        return;
    }
    schedule_save(v);
}

static void on_tray_sw(GObject *sw, GParamSpec *ps, gpointer ud)
{
    HbSettingsView *v = ud;
    (void)sw; (void)ps;
    if (!v->updating)
        schedule_save(v);
}

static void hb_settings_view_init(HbSettingsView *v)
{
    GtkWidget *scroll, *outer, *grid, *left, *right;
    GtkWidget *tgrid;
    size_t i;

    install_card_css();
    v->store = hb_store_default();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(v),
                                   GTK_ORIENTATION_VERTICAL);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(v), scroll);

    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(outer, "hb-content");
    gtk_widget_set_margin_top(outer, 14);
    gtk_widget_set_margin_bottom(outer, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), outer);

    /* ---- 两列 ---- */
    grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_set_homogeneous(GTK_BOX(grid), TRUE);
    gtk_box_append(GTK_BOX(outer), grid);

    /* ===== 左：终端主题 ===== */
    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(left, "panel");
    gtk_widget_set_hexpand(left, TRUE);
    gtk_box_append(GTK_BOX(grid), left);
    gtk_box_append(GTK_BOX(left),
        panel_head("palette", "终端主题",
                   "实时应用到所有会话与分屏。"));

    tgrid = gtk_grid_new();
    gtk_widget_add_css_class(tgrid, "theme-grid");
    gtk_grid_set_column_spacing(GTK_GRID(tgrid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(tgrid), 10);
    for (i = 0; i < N_THEMES; i++) {
        v->cards[i] = theme_card_new(THEME_CARDS[i].id,
                                     THEME_CARDS[i].label, v);
        gtk_grid_attach(GTK_GRID(tgrid), v->cards[i],
                        (int)(i % 2), (int)(i / 2), 1, 1);
    }
    gtk_box_append(GTK_BOX(left), tgrid);

    gtk_box_append(GTK_BOX(left),
        slider_row("字号", &v->font_scale, &v->font_lbl,
                   10, 20, 1, G_CALLBACK(on_font_scale), v));
    gtk_box_append(GTK_BOX(left),
        slider_row("行距", &v->lh_scale, &v->lh_lbl,
                   1.2, 2.0, 0.1, G_CALLBACK(on_lh_scale), v));

    /* 字体族：安装的单宽字体；Monospace 总在首位（别名永远有效） */
    {
        GtkStringList *m = gtk_string_list_new(NULL);
        PangoFontFamily **fams = NULL;
        int nf = 0, k;
        pango_context_list_families(
            gtk_widget_get_pango_context(GTK_WIDGET(v)), &fams, &nf);
        gtk_string_list_append(m, "Monospace");
        for (k = 0; k < nf; k++) {
            const char *nm = pango_font_family_get_name(fams[k]);
            if (!pango_font_family_is_monospace(fams[k]) ||
                !strcmp(nm, "Monospace"))
                continue;
            gtk_string_list_append(m, nm);
        }
        g_free(fams);
        v->family_dd = gtk_drop_down_new(G_LIST_MODEL(m), NULL);
        /* gtk_drop_down_new 消耗 m 的引用：不可再 unref，否则
         * dd->model 悬空，拆卸时 sel/filter 链读到已释放对象 */
        g_signal_connect(v->family_dd, "notify::selected",
                         G_CALLBACK(on_dd_changed), v);
    }
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *t = gtk_label_new("字体");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_widget_add_css_class(t, "sl-title");
        gtk_box_append(GTK_BOX(col), t);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(row), col);
        gtk_widget_set_valign(v->family_dd, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), v->family_dd);
        gtk_widget_add_css_class(row, "check-row");
        gtk_box_append(GTK_BOX(left), row);
    }

    /* scrollback */
    {
        GtkStringList *m = gtk_string_list_new(NULL);
        const char *lbls[] = { "1,000 行", "5,000 行", "10,000 行",
                               "50,000 行", "100,000 行" };
        GtkWidget *row, *col, *t, *s;
        for (i = 0; i < N_SB; i++)
            gtk_string_list_append(m, lbls[i]);
        v->sb_dd = gtk_drop_down_new(G_LIST_MODEL(m), NULL);
        /* 同上：_new 已消耗引用 */
        g_signal_connect(v->sb_dd, "notify::selected",
                         G_CALLBACK(on_dd_changed), v);
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        t = gtk_label_new("Scrollback");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_widget_add_css_class(t, "sl-title");
        gtk_box_append(GTK_BOX(col), t);
        s = gtk_label_new("新会话生效");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_box_append(GTK_BOX(col), s);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(row), col);
        gtk_widget_set_valign(v->sb_dd, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), v->sb_dd);
        gtk_widget_add_css_class(row, "check-row");
        gtk_box_append(GTK_BOX(left), row);
    }

    gtk_box_append(GTK_BOX(left),
        switch_row("光标闪烁", NULL, &v->blink_sw,
                   G_CALLBACK(on_sw_changed), v));
    gtk_box_append(GTK_BOX(left),
        switch_row("允许远端写入剪贴板（OSC 52）",
                   "远端程序可改写本机剪贴板，仅在信任的主机上开启",
                   &v->osc52_sw, G_CALLBACK(on_sw_changed), v));

    /* SFTP 并发传输数（1-8，默认 3） */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *t = gtk_label_new("SFTP 并发传输数");
        GtkWidget *s = gtk_label_new("同一主机同时进行的传输（1-8）");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_box_append(GTK_BOX(col), t);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_box_append(GTK_BOX(col), s);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(row), col);
        v->sftp_spin = gtk_spin_button_new_with_range(1, 8, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(v->sftp_spin), 3);
        gtk_widget_set_valign(v->sftp_spin, GTK_ALIGN_CENTER);
        g_signal_connect(v->sftp_spin, "value-changed",
                         G_CALLBACK(on_spin_changed), v);
        gtk_box_append(GTK_BOX(row), v->sftp_spin);
        gtk_widget_add_css_class(row, "check-row");
        gtk_box_append(GTK_BOX(left), row);
    }

    /* Autocomplete：BETA，未实现 → insensitive，不落库 */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *t = gtk_label_new("Autocomplete");
        GtkWidget *beta = gtk_label_new("BETA");
        GtkWidget *s = gtk_label_new("即将推出");
        GtkWidget *sw = gtk_switch_new();
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_box_append(GTK_BOX(trow), t);
        gtk_widget_add_css_class(beta, "beta");
        gtk_widget_set_valign(beta, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(trow), beta);
        gtk_box_append(GTK_BOX(col), trow);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_add_css_class(s, "hb-dim");
        gtk_box_append(GTK_BOX(col), s);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(row), col);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), sw);
        gtk_widget_add_css_class(row, "check-row");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_box_append(GTK_BOX(left), row);
    }

    {
        GtkWidget *rst = gtk_button_new();
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(rst, "hb-btn-small");
        gtk_box_append(GTK_BOX(r), hb_icon_new("refresh", 12));
        gtk_box_append(GTK_BOX(r), gtk_label_new("恢复默认"));
        gtk_button_set_child(GTK_BUTTON(rst), r);
        gtk_widget_set_halign(rst, GTK_ALIGN_START);
        g_signal_connect(rst, "clicked", G_CALLBACK(on_reset), v);
        gtk_box_append(GTK_BOX(left), rst);
    }

    /* ===== 右：实时预览 ===== */
    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(right, "panel");
    gtk_widget_set_hexpand(right, TRUE);
    gtk_box_append(GTK_BOX(grid), right);
    gtk_box_append(GTK_BOX(right),
        panel_head("terminal", "实时预览",
                   "拖动左侧滑杆 / 切换主题，立即生效。"));
    {
        GtkWidget *pv = build_preview(v);
        gtk_widget_set_size_request(pv, -1, 300);
        gtk_box_append(GTK_BOX(right), pv);
    }
    {
        GtkWidget *hint = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *txt;
        gtk_widget_add_css_class(hint, "hintbox");
        gtk_widget_add_css_class(hint, "plain");
        gtk_box_append(GTK_BOX(hint), hb_icon_new("info", 14));
        txt = gtk_label_new(
            "设置保存在本机 Vault；账号同步上线后随 E2EE 同步到其它设备。");
        gtk_label_set_wrap(GTK_LABEL(txt), TRUE);
        gtk_label_set_xalign(GTK_LABEL(txt), 0);
        gtk_box_append(GTK_BOX(hint), txt);
        gtk_box_append(GTK_BOX(right), hint);
    }

    /* ===== 底部：外观 ===== */
    {
        GtkWidget *grp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        GtkWidget *row, *col, *t;
        gtk_widget_add_css_class(grp, "panel");
        gtk_box_append(GTK_BOX(outer), grp);
        gtk_box_append(GTK_BOX(grp),
            panel_head("monitor", "外观",
                       "界面配色主题；终端配色在上方预览区单独选择。"));

        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        t = gtk_label_new("界面主题");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_widget_add_css_class(t, "sl-title");
        gtk_box_append(GTK_BOX(col), t);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(row), col);
        {
            const char *lbls[] = { "浅色", "深色", "跟随系统", NULL };
            v->ui_dd = gtk_drop_down_new_from_strings(
                (const char * const *)lbls);
        }
        gtk_widget_set_valign(v->ui_dd, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), v->ui_dd);
        gtk_widget_add_css_class(row, "check-row");
        gtk_box_append(GTK_BOX(grp), row);
        g_signal_connect(v->ui_dd, "notify::selected",
                         G_CALLBACK(on_ui_dd), v);
    }

    /* ===== 底部：系统托盘与启动 ===== */
    {
        GtkWidget *grp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        GtkWidget *tray_row, *sub;
        gtk_widget_add_css_class(grp, "panel");
        gtk_box_append(GTK_BOX(outer), grp);
        gtk_box_append(GTK_BOX(grp),
            panel_head("window", "系统托盘与启动",
                       "关闭窗口时驻留托盘、登录时自动启动。"));

        tray_row = switch_row("关闭时驻留系统托盘", NULL,
                              &v->tray_sw, G_CALLBACK(on_tray_sw), v);
        gtk_box_append(GTK_BOX(grp), tray_row);
        if (!hb_application_tray_available()) {
            gtk_widget_set_sensitive(tray_row, FALSE);
            sub = gtk_label_new("当前桌面未提供系统托盘");
            gtk_widget_add_css_class(sub, "hb-dim");
            gtk_label_set_xalign(GTK_LABEL(sub), 0);
            gtk_box_append(GTK_BOX(grp), sub);
        }
        gtk_box_append(GTK_BOX(grp),
            switch_row("登录时自动启动",
                       "图形会话启动时以 --background 后台运行",
                       &v->login_sw, G_CALLBACK(on_login_sw), v));
    }

    /* ===== 底部：快捷键 ===== */
    gtk_box_append(GTK_BOX(outer), build_shortcuts_panel(v));

    /* ===== 底部：数据 ===== */
    gtk_box_append(GTK_BOX(outer), build_backup_panel(v));

    v->changed_handler = g_signal_connect(v->store, "changed",
        G_CALLBACK(on_store_changed), v);
    refresh(v);
}


/* ------------------------------------------------------------------ */
/* 数据备份（FR-15）：.harbor-backup 加密导出 / 导入。                  */
/* Argon2id KDF ~1.2s：export 与 preview 放 GTask worker；import 最后  */
/* 一步写 vault 会同步触发 store "changed"，留在主线程（一次性 ~1.5s）。*/
/* ------------------------------------------------------------------ */

typedef struct {
    HbSettingsView *v;
    AdwDialog      *dlg;
    GtkWidget      *pw1;          /* 口令（导出=新密码，导入=解密密码） */
    GtkWidget      *pw2;          /* 确认（仅导出） */
    GtkWidget      *hint;         /* 强度/错误提示 */
    GtkWidget      *ok;           /* 当前对话框主按钮（dev 驱动用） */
    char           *path;         /* 导出目标路径 */
    char           *pw_stash;     /* 关窗后 worker 要用的口令副本 */
    uint8_t        *data;         /* 导入文件字节 */
    size_t          data_len;
    gboolean        busy;         /* 已点确定进入下一阶段 → 关窗不回收 */
} BkUi;

static BkUi *bk_cur;              /* dev 驱动：当前打开的备份对话框 */
static char *bk_dev_out;          /* dev 驱动：导出目标（跳过文件对话框） */

static void bkui_free(BkUi *b)
{
    if (bk_cur == b) bk_cur = NULL;
    if (b->pw_stash) {
        sodium_memzero(b->pw_stash, strlen(b->pw_stash));
        g_clear_pointer(&b->pw_stash, g_free);
    }
    g_clear_pointer(&b->path, g_free);
    if (b->data) {
        sodium_memzero(b->data, b->data_len);
        g_clear_pointer(&b->data, g_free);
    }
    g_free(b);
}

static void bkui_closed(BkUi *b)
{
    if (!b->busy)
        bkui_free(b);
}

static void wipe_pw_entry(GtkWidget *e)
{
    gtk_editable_set_text(GTK_EDITABLE(e), "");
}

/* 口令强度：长度+字符类别，实时提示 */
static const char *pw_strength(const char *pw)
{
    size_t n = pw ? strlen(pw) : 0;
    gboolean lo = FALSE, up = FALSE, dg = FALSE, sym = FALSE;
    const char *p;
    int cls;
    for (p = pw; p && *p; p++) {
        if (*p >= 'a' && *p <= 'z') lo = TRUE;
        else if (*p >= 'A' && *p <= 'Z') up = TRUE;
        else if (*p >= '0' && *p <= '9') dg = TRUE;
        else sym = TRUE;
    }
    cls = lo + up + dg + sym;
    if (n >= 12 && cls >= 3) return "强度：强";
    if (n >= 8)              return "强度：中（建议 12+ 位混合字符）";
    if (n > 0)               return "强度：弱（至少 8 位）";
    return "";
}

static void bk_pw_changed(GtkEditable *e, gpointer ud)
{
    BkUi *b = ud;
    const char *a = gtk_editable_get_text(GTK_EDITABLE(b->pw1));
    const char *c = b->pw2 ? gtk_editable_get_text(GTK_EDITABLE(b->pw2))
                           : NULL;
    char msg[128];

    g_strlcpy(msg, pw_strength(a), sizeof msg);
    if (c && *c && strcmp(a, c))
        g_strlcat(msg, "　·　两次输入不一致", sizeof msg);
    gtk_label_set_label(GTK_LABEL(b->hint), msg);
    (void)e;
}

/* ---- 导出：worker 里跑 KDF+加密+写盘 ---- */
typedef struct {
    hb_vault *vault;
    char     *pw;
    char     *path;
    hb_err    err;
} BkExportJob;

static void bk_export_work(GTask *task, gpointer src, gpointer data,
                           GCancellable *cancellable)
{
    BkExportJob *j = data;
    (void)task;
    uint8_t *out = NULL;
    size_t out_len = 0;
    (void)src; (void)cancellable;

    j->err = hb_backup_export(j->vault, j->pw, &out, &out_len);
    sodium_memzero(j->pw, strlen(j->pw));
    if (j->err == HB_OK && out &&
        !g_file_set_contents(j->path, (const char *)out,
                             (gssize)out_len, NULL))
        j->err = HB_ERR_IO;
    if (out) { sodium_memzero(out, out_len); free(out); }
}

static void bk_export_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    BkUi *b = ud;
    BkExportJob *j = g_task_get_task_data(G_TASK(res));
    (void)src;
    if (j->err == HB_OK)
        hb_toast(GTK_WIDGET(b->v), "加密备份已导出");
    else
        hb_toast(GTK_WIDGET(b->v), "导出失败");
    bkui_free(b);
}

static void bk_export_job_free(BkExportJob *j)
{
    g_free(j->pw);
    g_free(j->path);
    g_free(j);
}

static void bk_export_launch(BkUi *b)
{
    GTask *t;
    BkExportJob *j = g_new0(BkExportJob, 1);
    j->vault = hb_store_vault(b->v->store);
    j->pw = g_strdup(b->pw_stash); /* worker 里 memzero 后释放 */
    j->path = g_strdup(b->path);
    t = g_task_new(NULL, NULL, bk_export_done, b);
    g_task_set_task_data(t, j, (GDestroyNotify)bk_export_job_free);
    g_task_run_in_thread(t, bk_export_work);
    g_object_unref(t);
}

static void bk_save_cb(GObject *src, GAsyncResult *res, gpointer ud)
{
    BkUi *b = ud;
    GFile *gf = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    if (!gf) { bkui_free(b); return; }
    g_free(b->path);
    b->path = g_file_get_path(gf);
    g_object_unref(gf);
    if (!b->path) { bkui_free(b); return; }
    bk_export_launch(b);
}

static void bk_export_go(GtkButton *btn, gpointer ud)
{
    BkUi *b = ud;
    const char *a = gtk_editable_get_text(GTK_EDITABLE(b->pw1));
    const char *c = gtk_editable_get_text(GTK_EDITABLE(b->pw2));
    (void)btn;

    if (!*a) {
        gtk_label_set_label(GTK_LABEL(b->hint), "请输入备份密码");
        return;
    }
    if (strcmp(a, c)) {
        gtk_label_set_label(GTK_LABEL(b->hint), "两次输入不一致");
        return;
    }
    if (strlen(a) < 8)
        hb_toast(GTK_WIDGET(b->v), "密码少于 8 位，仍继续导出");
    b->busy = TRUE;
    /* 关窗后 pw 控件即销毁，先留副本给 worker */
    g_free(b->pw_stash);
    b->pw_stash = g_strdup(a);
    wipe_pw_entry(b->pw1);
    if (b->pw2) wipe_pw_entry(b->pw2);
    if (bk_dev_out) {
        g_free(b->path);
        b->path = g_strdup(bk_dev_out);
        adw_dialog_close(b->dlg);
        bk_export_launch(b);
        return;
    }
    {
        GtkFileDialog *d = gtk_file_dialog_new();
        adw_dialog_close(b->dlg);
        gtk_file_dialog_set_title(d, "保存加密备份");
        gtk_file_dialog_set_initial_name(d, "harbor" HB_BACKUP_EXT);
        gtk_file_dialog_save(d, NULL, NULL, bk_save_cb, b);
        g_object_unref(d);
    }
}

/* ---- 导入：读文件 → 密码 → worker preview → 确认 → import ---- */
typedef struct {
    char    *pw;
    uint8_t *data;
    size_t   data_len;
    hb_err   err;
    hb_backup_stat st;
} BkImportJob;

static void bk_preview_work(GTask *task, gpointer src, gpointer data,
                            GCancellable *cancellable)
{
    BkImportJob *j = data;
    (void)task;
    (void)src; (void)cancellable;
    j->err = hb_backup_preview(j->data, j->data_len, j->pw, &j->st);
}

static void bk_import_do(BkUi *b, const char *pw)
{
    hb_backup_stat st;
    hb_err e = hb_backup_import(hb_store_vault(b->v->store),
                                b->data, b->data_len, pw, &st);
    if (e == HB_OK) {
        char msg[160];
        g_snprintf(msg, sizeof msg,
                   "已导入 %zu 个对象：新建 %zu · 覆盖 %zu",
                   st.total, st.created, st.overwritten);
        hb_toast(GTK_WIDGET(b->v), msg);
        /* settings 可能被覆盖 → 立即重挂主题 */
        hb_application_apply_theme();
    } else {
        hb_toast(GTK_WIDGET(b->v), "口令错误或文件已损坏");
    }
}

static void bk_confirm_cb(gboolean yes, gpointer ud)
{
    BkUi *b = ud;
    if (yes)
        bk_import_do(b, (const char *)b->path);  /* pw 暂存 path */
    if (b->path) sodium_memzero(b->path, strlen(b->path));
    bkui_free(b);
}

static void bk_preview_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    BkUi *b = ud;
    BkImportJob *j = g_task_get_task_data(G_TASK(res));
    (void)src;
    if (j->err != HB_OK) {
        hb_toast(GTK_WIDGET(b->v), "口令错误或文件已损坏");
        if (b->path) sodium_memzero(b->path, strlen(b->path));
        bkui_free(b);
        return;
    }
    {
        char msg[200];
        g_snprintf(msg, sizeof msg,
                   "备份文件校验通过，包含 <b>%zu</b> 个对象"
                   "（含密钥与密文）。导入将按对象覆盖同名数据。",
                   j->st.total);
        b->dlg = hb_dialog_confirm(GTK_WIDGET(b->v),
                                   "导入加密备份", msg,
                                   "导入", FALSE, bk_confirm_cb, b);
    }
}

static void bk_import_job_free(BkImportJob *j)
{
    if (j->pw) {
        sodium_memzero(j->pw, strlen(j->pw));
        g_free(j->pw);
    }
    g_free(j);
}

static void bk_import_go(GtkButton *btn, gpointer ud)
{
    BkUi *b = ud;
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(b->pw1));
    GTask *t;
    BkImportJob *j;
    (void)btn;

    if (!*pw) {
        gtk_label_set_label(GTK_LABEL(b->hint), "请输入备份密码");
        return;
    }
    b->busy = TRUE;
    /* 关窗后 pw 控件即销毁，先留副本（bkui_free 负责 memzero） */
    g_free(b->path);
    b->path = g_strdup(pw);        /* 临时借 path 存明文，confirm 后清 */
    wipe_pw_entry(b->pw1);
    adw_dialog_close(b->dlg);
    j = g_new0(BkImportJob, 1);
    j->pw = g_strdup(b->path);     /* pw 指向已销毁控件，用副本 */
    j->data = b->data;
    j->data_len = b->data_len;
    t = g_task_new(NULL, NULL, bk_preview_done, b);
    g_task_set_task_data(t, j, (GDestroyNotify)bk_import_job_free);
    g_task_run_in_thread(t, bk_preview_work);
    g_object_unref(t);
}

static void bk_import_pw_dialog(BkUi *b)
{
    GtkWidget *body, *foot;
    AdwDialog *d;
    GtkWidget *hint;

    d = hb_modal_new("导入加密备份", &body, &foot);
    b->dlg = d;
    gtk_box_append(GTK_BOX(body),
        gtk_label_new("输入该备份的导出密码："));
    b->pw1 = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(
        GTK_PASSWORD_ENTRY(b->pw1), TRUE);
    gtk_box_append(GTK_BOX(body), b->pw1);
    hint = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "hb-dim");
    gtk_box_append(GTK_BOX(body), hint);
    b->hint = hint;
    {
        GtkWidget *ok = gtk_button_new_with_label("下一步");
        gtk_widget_add_css_class(ok, "primary");
        g_signal_connect(ok, "clicked", G_CALLBACK(bk_import_go), b);
        gtk_box_append(GTK_BOX(foot), ok);
        b->ok = ok;
    }
    g_signal_connect_swapped(d, "closed",
                             G_CALLBACK(bkui_closed), b);
    adw_dialog_present(d, GTK_WIDGET(b->v));
    bk_cur = b;
}

static void bk_file_cb(GObject *src, GAsyncResult *res, gpointer ud)
{
    BkUi *b = ud;
    GFile *gf = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),
                                            res, NULL);
    char *contents = NULL;
    gsize len = 0;

    if (!gf) { bkui_free(b); return; }
    if (!g_file_load_contents(gf, NULL, &contents, &len, NULL, NULL)) {
        g_object_unref(gf);
        hb_toast(GTK_WIDGET(b->v), "无法读取备份文件");
        bkui_free(b);
        return;
    }
    g_object_unref(gf);
    b->data = (uint8_t *)contents;
    b->data_len = len;
    bk_import_pw_dialog(b);
}

static void on_bk_export(GtkButton *btn, gpointer ud)
{
    HbSettingsView *v = ud;
    BkUi *b = g_new0(BkUi, 1);
    GtkWidget *body, *foot;
    AdwDialog *d;
    GtkWidget *l, *hint, *ok;
    (void)btn;

    b->v = v;
    d = hb_modal_new("导出加密备份", &body, &foot);
    b->dlg = d;
    l = gtk_label_new("备份包含全部主机、密钥与密文；密码用于"
                      " Argon2id 加密，遗失无法恢复。");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(body), l);
    b->pw1 = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(
        GTK_PASSWORD_ENTRY(b->pw1), TRUE);
        gtk_box_append(GTK_BOX(body), b->pw1);
    b->pw2 = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(
        GTK_PASSWORD_ENTRY(b->pw2), TRUE);
        gtk_box_append(GTK_BOX(body), b->pw2);
    hint = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "hb-dim");
    gtk_box_append(GTK_BOX(body), hint);
    b->hint = hint;
    g_signal_connect(b->pw1, "changed", G_CALLBACK(bk_pw_changed), b);
    g_signal_connect(b->pw2, "changed", G_CALLBACK(bk_pw_changed), b);
    ok = gtk_button_new_with_label("导出…");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", G_CALLBACK(bk_export_go), b);
    gtk_box_append(GTK_BOX(foot), ok);
    b->ok = ok;
    g_signal_connect_swapped(d, "closed",
                             G_CALLBACK(bkui_closed), b);
    adw_dialog_present(d, GTK_WIDGET(v));
    bk_cur = b;
}

static void on_bk_import(GtkButton *btn, gpointer ud)
{
    HbSettingsView *v = ud;
    BkUi *b = g_new0(BkUi, 1);
    GtkFileDialog *d;
    (void)btn;
    b->v = v;
    d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "导入加密备份");
    gtk_file_dialog_open(d, NULL, NULL, bk_file_cb, b);
    g_object_unref(d);
}

void hb_settings_view_export_backup(HbSettingsView *v)
{
    on_bk_export(NULL, v);
}

void hb_settings_view_import_backup(HbSettingsView *v)
{
    on_bk_import(NULL, v);
}

static GtkWidget *build_backup_panel(HbSettingsView *v)
{
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *row, *be, *bi;

    gtk_widget_add_css_class(panel, "panel");
    gtk_box_append(GTK_BOX(panel),
        panel_head("keyring", "数据",
                   "加密备份包含主机、密钥、密文与设置（.harbor-backup）。"));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    be = gtk_button_new_with_label("导出加密备份…");
    bi = gtk_button_new_with_label("导入备份…");
    gtk_widget_add_css_class(be, "hb-btn");
    gtk_widget_add_css_class(bi, "hb-btn");
    g_signal_connect(be, "clicked", G_CALLBACK(on_bk_export), v);
    g_signal_connect(bi, "clicked", G_CALLBACK(on_bk_import), v);
    gtk_box_append(GTK_BOX(row), be);
    gtk_box_append(GTK_BOX(row), bi);
    gtk_widget_set_halign(row, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(panel), row);
    return panel;
}

static void hb_settings_view_dispose(GObject *o)
{
    HbSettingsView *v = HB_SETTINGS_VIEW(o);
    if (v->save_src) {
        g_source_remove(v->save_src);
        v->save_src = 0;
    }
    if (v->changed_handler && v->store)
        g_signal_handler_disconnect(v->store, v->changed_handler);
    v->changed_handler = 0;
    G_OBJECT_CLASS(hb_settings_view_parent_class)->dispose(o);
}

static void hb_settings_view_finalize(GObject *o)
{
    HbSettingsView *v = HB_SETTINGS_VIEW(o);
    g_free(v->pv_theme_id);
    g_clear_pointer(&v->sc_rows, g_ptr_array_unref);
    G_OBJECT_CLASS(hb_settings_view_parent_class)->finalize(o);
}

static void hb_settings_view_class_init(HbSettingsViewClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->dispose = hb_settings_view_dispose;
    oc->finalize = hb_settings_view_finalize;
}

GtkWidget *hb_settings_view_new(void)
{
    return g_object_new(HB_TYPE_SETTINGS_VIEW, NULL);
}

/* ---- dev-action 驱动钩子（截图/自动化验证用；经 dev-ui 调用） ---- */

/* 快捷键行编辑驱动：accel==NULL 进入捕获；之后 accel 模拟一次按键，
 * 走真实 sc_capture_key（Esc 取消 / Backspace 禁用 / 冲突 / 警告）。 */
gboolean hb_settings_view_sc_dev(HbSettingsView *v, size_t idx,
                                 const char *accel)
{
    ScRow *r;
    guint k = 0;
    GdkModifierType m = 0;
    if (!v->sc_rows || idx >= v->sc_rows->len)
        return FALSE;
    r = g_ptr_array_index(v->sc_rows, idx);
    if (!accel || !*accel) {
        on_sc_btn(NULL, r);
        return TRUE;
    }
    if (!strcmp(accel, "#reset")) {
        sc_reset_row(r);
        return TRUE;
    }
    if (!strcmp(accel, "#resetall")) {
        on_sc_reset_all(NULL, v);
        return TRUE;
    }
    if (!r->capturing)
        return FALSE;
    if (!gtk_accelerator_parse(accel, &k, &m) || !k)
        return FALSE;
    return sc_capture_key(NULL, k, 0, m, r);
}

void hb_settings_view_bk_dev_out(const char *path)
{
    g_free(bk_dev_out);
    bk_dev_out = g_strdup(path);
}

void hb_settings_view_bk_dev_fill(const char *txt)
{
    BkUi *b = bk_cur;
    if (!b) return;
    if (b->pw1)
        gtk_editable_set_text(GTK_EDITABLE(b->pw1), txt ? txt : "");
    if (b->pw2)
        gtk_editable_set_text(GTK_EDITABLE(b->pw2), txt ? txt : "");
}

void hb_settings_view_bk_dev_press(void)
{
    if (bk_cur && bk_cur->ok)
        gtk_widget_activate(bk_cur->ok);
}

static GtkWidget *find_button_lbl(GtkWidget *w, const char *lbl)
{
    GtkWidget *c;
    if (GTK_IS_BUTTON(w)) {
        const char *t = gtk_button_get_label(GTK_BUTTON(w));
        if (t && lbl && !strcmp(t, lbl))
            return w;
    }
    for (c = gtk_widget_get_first_child(w); c;
         c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *r = find_button_lbl(c, lbl);
        if (r) return r;
    }
    return NULL;
}

/* 确认/取消：走真按钮，cfm_closed 会以 accepted 回调并负责回收 */
void hb_settings_view_bk_dev_confirm(gboolean yes)
{
    BkUi *b = bk_cur;
    GtkWidget *btn;
    if (!b || !b->dlg) return;
    btn = find_button_lbl(GTK_WIDGET(b->dlg), yes ? "导入" : "取消");
    if (btn) gtk_widget_activate(btn);
    else if (!yes) adw_dialog_close(b->dlg);   /* pw 阶段直接关窗 */
}

void hb_settings_view_import_backup_dev(HbSettingsView *v,
                                        const char *path)
{
    BkUi *b = g_new0(BkUi, 1);
    char *contents = NULL;
    gsize len = 0;
    b->v = v;
    if (!path ||
        !g_file_get_contents(path, &contents, &len, NULL)) {
        hb_toast(GTK_WIDGET(v), "无法读取备份文件");
        bkui_free(b);
        return;
    }
    b->data = (uint8_t *)contents;
    b->data_len = len;
    bk_import_pw_dialog(b);
}
