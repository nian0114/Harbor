#include "hb-shortcuts.h"
#include "hb-term-page.h"
#include "hb-ssh-page.h"
#include "hb-terminal.h"
#include "hb-palette.h"
#include "hb-session-page.h"
#include "hb-workspace.h"

#include <string.h>

#define SC_CTRL   GDK_CONTROL_MASK
#define SC_SHIFT  GDK_SHIFT_MASK
#define SC_ALT    GDK_ALT_MASK
#define SC_MASK   (SC_CTRL | SC_SHIFT | SC_ALT)

/* 单一快捷键表：窗口 capture controller 按它派发；设置页可遍历展示。
 * keyval 一律小写（匹配前对事件 keyval 取 gdk_keyval_to_lower）。 */
static const HbShortcut SC[] = {
    { "palette",    GDK_KEY_k,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+K",   "命令面板",              0 },
    { "palette",    GDK_KEY_k,    SC_CTRL,          "Ctrl+K",         "命令面板（终端外）",    HB_SC_NOT_IN_TERM },
    { "find",       GDK_KEY_f,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+F",   "在 scrollback 中搜索",  0 },
    { "find",       GDK_KEY_f,    SC_CTRL,          "Ctrl+F",         "搜索（终端外）",        HB_SC_NOT_IN_TERM },
    { "new-local",  GDK_KEY_t,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+T",   "新建本地终端",          0 },
    { "close-tab",  GDK_KEY_w,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+W",   "关闭当前标签",          0 },
    { "close-tab",  GDK_KEY_w,    SC_CTRL,          "Ctrl+W",         "关闭当前标签",          0 },
    { "tab-next",   GDK_KEY_Page_Down, SC_CTRL,     "Ctrl+PgDn",      "下一个标签",            0 },
    { "tab-prev",   GDK_KEY_Page_Up,   SC_CTRL,     "Ctrl+PgUp",      "上一个标签",            0 },
    { "tab-next",   GDK_KEY_Tab,  SC_CTRL,          "Ctrl+Tab",       "下一个标签",            0 },
    { "tab-prev",   GDK_KEY_ISO_Left_Tab, SC_CTRL|SC_SHIFT, "Ctrl+Shift+Tab", "上一个标签",    0 },
    { "split-right",GDK_KEY_d,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+D",   "向右分屏",              0 },
    { "split-down", GDK_KEY_e,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+E",   "向下分屏",              0 },
    { "focus-mode", GDK_KEY_m,    SC_CTRL|SC_SHIFT, "Ctrl+Shift+M",   "Focus 模式",            0 },
    { "pane-left",  GDK_KEY_Left, SC_ALT,           "Alt+←",          "焦点移到左侧 pane",     0 },
    { "pane-right", GDK_KEY_Right,SC_ALT,           "Alt+→",          "焦点移到右侧 pane",     0 },
    { "pane-up",    GDK_KEY_Up,   SC_ALT,           "Alt+↑",          "焦点移到上方 pane",     0 },
    { "pane-down",  GDK_KEY_Down, SC_ALT,           "Alt+↓",          "焦点移到下方 pane",     0 },
    { "tab-n",      GDK_KEY_1,    SC_ALT,           "Alt+1…9",        "跳到标签 N（1=Vault）", 0 },
    { "tab-n",      GDK_KEY_1,    SC_CTRL,          "Ctrl+1…9",       "跳到标签 N（1=Vault）", 0 },
};

const HbShortcut *hb_shortcuts(size_t *n)
{
    if (n)
        *n = G_N_ELEMENTS(SC);
    return SC;
}

/* 焦点是否落在某个 HbTerminal 内（焦点可能在 find-bar 等覆盖件上，
 * 那时不算"在终端里"，Ctrl+K/Ctrl+F 仍然生效） */
static gboolean focus_in_terminal(HbWindow *win)
{
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(win));
    return f && gtk_widget_get_ancestor(f, HB_TYPE_TERMINAL) != NULL;
}

static gboolean dispatch(HbWindow *win, const char *id)
{
    GtkWidget *page;

    if (!strcmp(id, "palette")) {
        hb_palette_open(win);
        return TRUE;
    }
    if (!strcmp(id, "find")) {
        /* workspace → ws 的 find bar（绑焦点 pane）；普通页 → 页内 */
        page = hb_window_active_page(win);
        if (page && HB_IS_WORKSPACE(page)) {
            hb_workspace_open_find(HB_WORKSPACE(page));
            return TRUE;
        }
        page = hb_window_active_session_page(win);
        if (page && HB_IS_SESSION_PAGE(page)) {
            hb_session_page_open_find(HB_SESSION_PAGE(page));
            return TRUE;
        }
        return FALSE;   /* 非终端页：让事件继续传播 */
    }
    if (!strcmp(id, "split-right")) {
        hb_window_split_active(win, GTK_ORIENTATION_HORIZONTAL);
        return TRUE;
    }
    if (!strcmp(id, "split-down")) {
        hb_window_split_active(win, GTK_ORIENTATION_VERTICAL);
        return TRUE;
    }
    if (!strcmp(id, "focus-mode")) {
        hb_window_toggle_focus_mode(win);
        return TRUE;
    }
    if (!strcmp(id, "pane-left")) {
        hb_window_move_pane_focus(win, 0);
        return TRUE;
    }
    if (!strcmp(id, "pane-right")) {
        hb_window_move_pane_focus(win, 1);
        return TRUE;
    }
    if (!strcmp(id, "pane-up")) {
        hb_window_move_pane_focus(win, 2);
        return TRUE;
    }
    if (!strcmp(id, "pane-down")) {
        hb_window_move_pane_focus(win, 3);
        return TRUE;
    }
    if (!strcmp(id, "new-local")) {
        hb_window_open_local(win, NULL);
        return TRUE;
    }
    if (!strcmp(id, "close-tab")) {
        /* workspace → 关焦点 pane（剩 1 时塌回普通 tab）；否则关 tab */
        hb_window_close_pane_or_tab(win);
        return TRUE;
    }
    if (!strcmp(id, "tab-next")) {
        hb_window_switch_tab_rel(win, 1);
        return TRUE;
    }
    if (!strcmp(id, "tab-prev")) {
        hb_window_switch_tab_rel(win, -1);
        return TRUE;
    }
    return FALSE;
}

/* 焦点是否落在开了「快捷键透传」的远程桌面画布内：该画布向上标记了
 * hb-rd-passthrough，此时全部按键放行给远端。 */
static gboolean focus_in_rd_passthrough(HbWindow *win)
{
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(win));
    GtkWidget *w;
    for (w = f; w; w = gtk_widget_get_parent(w))
        if (g_object_get_data(G_OBJECT(w), "hb-rd-passthrough"))
            return TRUE;
    return FALSE;
}

static gboolean on_key(GtkEventControllerKey *kc, guint keyval,
                       guint code, GdkModifierType state, gpointer ud)
{
    HbWindow *win = ud;
    GdkModifierType m = state & SC_MASK;
    guint k = gdk_keyval_to_lower(keyval);
    const HbShortcut *s;
    size_t i;
    (void)kc; (void)code;

    /* 远程桌面快捷键透传：画布自己的 key controller 吃掉全部按键 */
    if (focus_in_rd_passthrough(win))
        return FALSE;

    /* Alt/Ctrl + 1…9：表里有展示行，派发在这里统一做 */
    if ((m == SC_ALT || m == SC_CTRL) &&
        k >= GDK_KEY_1 && k <= GDK_KEY_9) {
        hb_window_switch_tab_n(win, k - GDK_KEY_1);
        return TRUE;
    }

    for (i = 0; i < G_N_ELEMENTS(SC); i++) {
        s = &SC[i];
        if (s->keyval != k || s->mods != m)
            continue;
        if ((s->flags & HB_SC_NOT_IN_TERM) && focus_in_terminal(win))
            continue;   /* Ctrl+K/F 在终端里留给 shell */
        if (dispatch(win, s->id))
            return TRUE;
    }
    return FALSE;
}

void hb_shortcuts_install(HbWindow *win)
{
    GtkEventController *key = gtk_event_controller_key_new();
    /* capture：在终端/条目等焦点控件之前看到按键，终端吞不掉 */
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), win);
    gtk_widget_add_controller(GTK_WIDGET(win), key);
}
